#include "domain/annotation/brush_coverage_kernel.hpp"
#include "domain/annotation/annotation_allocation_checkpoint.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
namespace hdrshot {
namespace {
using Row = BrushCoverageAccumulator::Row;
struct BrushContainerOverflow {};
template <class T>
void prepare_sparse_capacity(std::vector<T> &values, std::size_t needed,
                             AnnotationAllocationStage stage =
                                 AnnotationAllocationStage::sparse_insert) {
  if (needed > values.max_size())
    throw BrushContainerOverflow{};
  if (needed <= values.capacity())
    return;
  auto capacity = std::max(std::size_t(1), values.capacity());
  while (capacity < needed) {
    if (capacity > values.max_size() / 2) {
      capacity = needed;
      break;
    }
    capacity *= 2;
  }
  annotation_allocation_checkpoint(stage);
  values.reserve(capacity);
}
void merge_opaque(Row &row, int a, int b, BrushCoverageDelta &delta) {
  if (a >= b)
    return;
  auto it = std::lower_bound(
      row.opaque.begin(), row.opaque.end(), a,
      [](auto interval, int x) { return interval.second < x; });
  while (it != row.opaque.end() && it->first <= b) {
    a = std::min(a, it->first);
    b = std::max(b, it->second);
    it = row.opaque.erase(it);
  }
  const auto index = std::size_t(it - row.opaque.begin());
  if (row.opaque.size() == row.opaque.max_size())
    throw BrushContainerOverflow{};
  prepare_sparse_capacity(row.opaque, row.opaque.size() + 1);
  row.opaque.insert(row.opaque.begin() + std::ptrdiff_t(index), {a, b});
  auto first =
      std::lower_bound(row.aa.begin(), row.aa.end(), a,
                       [](auto entry, int x) { return entry.first < x; });
  auto last = std::lower_bound(first, row.aa.end(), b, [](auto entry, int x) {
    return entry.first < x;
  });
  // Erasing covered AA compacts the retained tail; these are real old-AA
  // element copies, in addition to dirty clones and later merge copies.
  if (first != last)
    delta.copied_samples += std::size_t(row.aa.end() - last);
  row.aa.erase(first, last);
}
std::pair<double, double> capsule_row_intervals(StrokeSegment s, double py,
                                                double r) {
  double lo = INFINITY, hi = -INFINITY;
  auto add = [&](double a, double b) {
    lo = std::min(lo, a);
    hi = std::max(hi, b);
  };
  for (auto p : {s.start, s.end}) {
    double yy = py - p.y;
    if (std::abs(yy) <= r) {
      // Factored form avoids overflowing r*r for a finite, very large radius.
      double q = std::sqrt(std::max(0., r - std::abs(yy))) *
                 std::sqrt(r + std::abs(yy));
      add(p.x - q, p.x + q);
    }
  }
  double dx = s.end.x - s.start.x, dy = s.end.y - s.start.y,
         len = std::hypot(dx, dy);
  if (len > 0) {
    double nx = -dy / len * r, ny = dx / len * r;
    std::array<StrokePoint, 4> v{{{s.start.x + nx, s.start.y + ny},
                                  {s.end.x + nx, s.end.y + ny},
                                  {s.end.x - nx, s.end.y - ny},
                                  {s.start.x - nx, s.start.y - ny}}};
    for (int i = 0; i < 4; ++i) {
      auto a = v[i], b = v[(i + 1) % 4];
      if (py >= std::min(a.y, b.y) && py <= std::max(a.y, b.y)) {
        if (a.y == b.y)
          add(std::min(a.x, b.x), std::max(a.x, b.x));
        else {
          double x = a.x + (py - a.y) / (b.y - a.y) * (b.x - a.x);
          add(x, x);
        }
      }
    }
  }
  return {lo, hi};
}
int clipped(double x, int a, int b) {
  return int(std::clamp(x, double(a), double(b)));
}
} // namespace
Result<BrushCoverageDelta, Error>
accumulate_brush_segments(BrushCoverageAccumulator &acc,
                          std::span<const StrokeSegment> segments,
                          double radius, PixelRect clip) {
  auto invalid = [] {
    return Result<BrushCoverageDelta, Error>::failure(
        {ErrorCode::invalid_input, "BrushCoverage"});
  };
  if (!std::isfinite(radius) || radius <= 0 || !valid_annotation_clip(clip))
    return invalid();
  for (auto s : segments) {
    for (auto p : {s.start, s.end})
      if (!std::isfinite(p.x) || !std::isfinite(p.y))
        return invalid();
    double dx = s.end.x - s.start.x, dy = s.end.y - s.start.y;
    if (!std::isfinite(dx) || !std::isfinite(dy) ||
        !std::isfinite(dx * dx + dy * dy))
      return invalid();
    for (double x : {double(clip.x) + .5, double(clip.right()) - .5})
      for (double y : {double(clip.y) + .5, double(clip.bottom()) - .5})
        if (!std::isfinite((x - s.start.x) * dx + (y - s.start.y) * dy))
          return invalid();
  }
  try {
    // All throwing preparation is confined to dirty. Extracted map nodes and
    // Row swaps make the final publication nonallocating.
    std::map<int, Row> dirty;
    BrushCoverageDelta delta;
    // Transaction-local buffers may reuse capacity; published immutable row
    // payloads are never used as writable scratch storage.
    std::vector<std::pair<int, std::uint8_t>> boundary_candidates, merged;
    for (auto s : segments) {
      ++delta.segments_processed;
      double dx = s.end.x - s.start.x, dy = s.end.y - s.start.y,
             length = dx * dx + dy * dy, r = radius + .5;
      int top = clipped(std::floor(std::min(s.start.y, s.end.y) - r), clip.y,
                        clip.bottom());
      int bottom = clipped(std::ceil(std::max(s.start.y, s.end.y) + r), clip.y,
                           clip.bottom());
      for (int y = top; y < bottom; ++y) {
        auto outer = capsule_row_intervals(s, y + .5, r + 1e-7);
        if (outer.first > outer.second)
          continue;
        auto it = dirty.find(y);
        if (it == dirty.end()) {
          annotation_allocation_checkpoint(
              AnnotationAllocationStage::dirty_row);
          auto old = acc.rows.find(y);
          it = dirty.emplace(y, old == acc.rows.end() ? Row{} : old->second)
                   .first;
          delta.copied_samples += it->second.aa.size();
        }
        auto &row = it->second;
        if (radius > .5000001) {
          auto inner = capsule_row_intervals(s, y + .5, radius - .5 - 1e-7);
          if (inner.first <= inner.second)
            merge_opaque(
                row, clipped(std::ceil(inner.first - .5), clip.x, clip.right()),
                clipped(std::floor(inner.second - .5) + 1, clip.x,
                        clip.right()),
                delta);
        }
        int a = clipped(std::floor(outer.first - .5) - 1, clip.x, clip.right());
        int b = clipped(std::ceil(outer.second - .5) + 2, clip.x, clip.right());
        boundary_candidates.clear();
        // Preserve original distance/clamp/lround arithmetic at the boundary.
        auto boundary = [&](int x) {
          ++delta.sample_evaluations;
          double px = x + .5, py = y + .5;
          double t =
              length > 0
                  ? std::clamp(((px - s.start.x) * dx + (py - s.start.y) * dy) /
                                   length,
                               0., 1.)
                  : 0;
          auto c = std::uint8_t(
              std::lround(std::clamp(r - std::hypot(px - s.start.x - t * dx,
                                                    py - s.start.y - t * dy),
                                     0., 1.) *
                          255));
          if (c) {
            if (boundary_candidates.size() == boundary_candidates.max_size())
              throw BrushContainerOverflow{};
            prepare_sparse_capacity(boundary_candidates,
                                    boundary_candidates.size() + 1,
                                    AnnotationAllocationStage::boundary_growth);
            boundary_candidates.push_back({x, c});
          }
        };
        int x = a;
        for (auto full : row.opaque) {
          if (full.second <= x)
            continue;
          if (full.first >= b)
            break;
          while (x < std::min(full.first, b))
            boundary(x++);
          int end = std::min(b, full.second);
          delta.opaque_pixels_skipped += std::size_t(end - x);
          x = end;
        }
        while (x < b)
          boundary(x++);
        // Finish collecting candidates before changing opaque: the loop
        // above iterates row.opaque and must never invalidate its iterators.
        for (std::size_t i = 0; i < boundary_candidates.size();) {
          if (boundary_candidates[i].second != 255) {
            ++i;
            continue;
          }
          const int left = boundary_candidates[i].first;
          int right = left + 1;
          ++i;
          while (i < boundary_candidates.size() &&
                 boundary_candidates[i].first == right &&
                 boundary_candidates[i].second == 255) {
            ++right;
            ++i;
          }
          merge_opaque(row, left, right, delta);
        }
        merged.clear();
        if (row.aa.size() > merged.max_size() ||
            boundary_candidates.size() > merged.max_size() - row.aa.size())
          throw BrushContainerOverflow{};
        prepare_sparse_capacity(merged,
                                row.aa.size() + boundary_candidates.size(),
                                AnnotationAllocationStage::aa_merge_growth);
        std::size_t old_index = 0, new_index = 0, opaque_index = 0;
        while (old_index < row.aa.size() ||
               new_index < boundary_candidates.size()) {
          std::pair<int, std::uint8_t> value;
          bool copied_old = false;
          if (new_index == boundary_candidates.size() ||
              (old_index < row.aa.size() &&
               row.aa[old_index].first <
                   boundary_candidates[new_index].first)) {
            value = row.aa[old_index++];
            copied_old = true;
          } else if (old_index == row.aa.size() ||
                     boundary_candidates[new_index].first <
                         row.aa[old_index].first) {
            value = boundary_candidates[new_index++];
          } else {
            value = boundary_candidates[new_index++];
            if (row.aa[old_index].second >= value.second) {
              value.second = row.aa[old_index].second;
              copied_old = true;
            }
            ++old_index;
          }
          while (opaque_index < row.opaque.size() &&
                 row.opaque[opaque_index].second <= value.first)
            ++opaque_index;
          const bool opaque = opaque_index < row.opaque.size() &&
                              row.opaque[opaque_index].first <= value.first;
          if (!opaque && value.second > 0 && value.second < 255) {
            merged.push_back(value);
            if (copied_old)
              ++delta.copied_samples;
          }
        }
        row.aa.swap(merged);
      }
    }
    for (auto &[y, row] : dirty) {
      annotation_allocation_checkpoint(AnnotationAllocationStage::row_payload);
      row.spans.clear();
      for (auto [a, b] : row.opaque)
        row.spans.push_back(CoverageSpan::opaque(y, a, b - a));
      auto it = row.aa.begin();
      while (it != row.aa.end()) {
        int x = it->first, next = x;
        std::vector<std::uint8_t> samples;
        while (it != row.aa.end() && it->first == next) {
          if (it->second == 255)
            ++delta.internal_samples_materialized;
          samples.push_back(it->second);
          ++delta.copied_samples; // actual AA value copy into immutable payload
          ++next;
          ++it;
        }
        row.spans.push_back({y, x, CoverageSamples(std::move(samples))});
      }
      std::sort(row.spans.begin(), row.spans.end(),
                [](const auto &a, const auto &b) { return a.x < b.x; });
      ++delta.frozen_rows;
    }
    annotation_allocation_checkpoint(
        AnnotationAllocationStage::row_publication);
    while (!dirty.empty()) {
      auto node = dirty.extract(dirty.begin());
      auto old = acc.rows.find(node.key());
      if (old == acc.rows.end())
        acc.rows.insert(std::move(node));
      else {
        using std::swap;
        swap(old->second, node.mapped());
      }
    }
    acc.counters.segments_processed += delta.segments_processed;
    acc.counters.sample_evaluations += delta.sample_evaluations;
    acc.counters.frozen_rows += delta.frozen_rows;
    acc.counters.copied_samples += delta.copied_samples;
    acc.counters.opaque_pixels_skipped += delta.opaque_pixels_skipped;
    acc.counters.internal_samples_materialized +=
        delta.internal_samples_materialized;
    return Result<BrushCoverageDelta, Error>::success(delta);
  } catch (const BrushContainerOverflow &) {
    return invalid();
  } catch (const std::bad_alloc &) {
    return Result<BrushCoverageDelta, Error>::failure(
        {ErrorCode::state_inconsistent,
         "BrushCoverage",
         Retryability::same_input,
         {{"reason", "allocation_failed"}}});
  }
}
AnnotationCoverageLayer
freeze_brush_coverage(const BrushCoverageAccumulator &acc, ObjectId id,
                      std::uint32_t color, PixelRect clip) {
  AnnotationCoverageLayer result{id, AnnotationKind::brush,      {}, color,
                                 {}, "hdrshot-brush-coverage-v2"};
  if (!valid_annotation_clip(clip))
    return result;
  int left = clip.right(), top = clip.bottom(), right = clip.x, bottom = clip.y;
  for (const auto &[y, row] : acc.rows)
    for (const auto &span : row.spans) {
      result.spans.push_back(span);
      left = std::min(left, span.x);
      top = std::min(top, span.y);
      right = std::max(right, span.x + int(span.length()));
      bottom = std::max(bottom, span.y + 1);
    }
  if (!result.spans.empty())
    result.bounds_px = {left, top, right - left, bottom - top};
  return result;
}
} // namespace hdrshot
