#pragma once
#include "domain/annotation/annotation_document.hpp"
#include "domain/annotation/annotation_units.hpp"
#include <algorithm>
#include <limits>
#include <memory>
#include <span>
namespace hdrshot {
// Immutable payload; descriptor copies and slices never copy coverage pixels.
class CoverageSamples {
public:
  CoverageSamples() = default;
  CoverageSamples(std::vector<std::uint8_t> data)
      : storage_(
            std::make_shared<const std::vector<std::uint8_t>>(std::move(data))),
        size_(storage_->size()) {}
  CoverageSamples(std::initializer_list<std::uint8_t> data)
      : CoverageSamples(std::vector<std::uint8_t>(data)) {}
  std::size_t size() const { return size_; }
  bool empty() const { return !size_; }
  const std::uint8_t *data() const {
    return storage_ ? storage_->data() + offset_ : nullptr;
  }
  const std::uint8_t *begin() const { return data(); }
  const std::uint8_t *end() const { return size_ ? data() + size_ : data(); }
  const std::uint8_t &operator[](std::size_t i) const {
    return (*storage_)[offset_ + i];
  }
  CoverageSamples slice(std::size_t offset, std::size_t size) const {
    CoverageSamples result = *this;
    result.offset_ += offset;
    result.size_ = size;
    return result;
  }
  bool valid_view() const {
    return storage_ ? offset_ <= storage_->size() &&
                          size_ <= storage_->size() - offset_
                    : offset_ == 0 && size_ == 0;
  }
  bool same_view(const CoverageSamples &b) const {
    return storage_ == b.storage_ && offset_ == b.offset_ && size_ == b.size_;
  }
  bool shares_storage(const CoverageSamples &b) const {
    return storage_ == b.storage_;
  }
  friend bool operator==(const CoverageSamples &a, const CoverageSamples &b) {
    if (a.size_ != b.size_)
      return false;
    if (a.storage_ == b.storage_ && a.offset_ == b.offset_)
      return true;
    return std::equal(a.begin(), a.end(), b.begin());
  }

private:
  std::shared_ptr<const std::vector<std::uint8_t>> storage_;
  std::size_t offset_{}, size_{};
};
struct CoverageSpan {
  std::int32_t y{}, x{};
  CoverageSamples coverage_u8;
  std::int32_t opaque_length{};
  std::size_t length() const {
    return opaque_length > 0 ? std::size_t(opaque_length) : coverage_u8.size();
  }
  bool is_opaque() const { return opaque_length > 0; }
  bool valid_view() const {
    return opaque_length >= 0 && coverage_u8.valid_view() &&
           (is_opaque() ? coverage_u8.empty() : !coverage_u8.empty()) &&
           length() <= std::size_t(std::numeric_limits<std::int32_t>::max());
  }
  std::uint8_t coverage_at(std::size_t i) const {
    return is_opaque() ? 255 : coverage_u8[i];
  }
  CoverageSpan slice(std::size_t offset, std::size_t n) const {
    if (!valid_view() || offset > length() || !n || n > length() - offset ||
        std::int64_t(x) + std::int64_t(offset) >
            std::numeric_limits<std::int32_t>::max())
      return {};
    if (is_opaque())
      return opaque(y, int(std::int64_t(x) + std::int64_t(offset)), int(n));
    return {y, int(std::int64_t(x) + std::int64_t(offset)),
            coverage_u8.slice(offset, n)};
  }
  static CoverageSpan opaque(std::int32_t y, std::int32_t x, std::int32_t n) {
    return {y, x, {}, n};
  }
  friend bool operator==(const CoverageSpan &, const CoverageSpan &) = default;
};
struct AnnotationCoverageLayer {
  ObjectId object_id{};
  AnnotationKind kind{AnnotationKind::rectangle};
  PixelRect bounds_px{};
  std::uint32_t color_srgb_rgb{};
  std::vector<CoverageSpan> spans;
  std::string raster_identity;
  friend bool operator==(const AnnotationCoverageLayer &,
                         const AnnotationCoverageLayer &) = default;
};
// Identity checks never traverse path points or coverage bytes, including for
// an untrusted slice whose offset/length are not safe to dereference.
inline bool same_annotation_source(const AnnotationObject &a,
                                   const AnnotationObject &b) {
  if (a.brush_geometry.has_value() != b.brush_geometry.has_value())
    return false;
  if (a.brush_geometry && a.brush_geometry->path != b.brush_geometry->path)
    return false;
  return a == b;
}
inline bool same_coverage_source(const AnnotationCoverageLayer &a,
                                 const AnnotationCoverageLayer &b) {
  if (a.object_id != b.object_id || a.kind != b.kind ||
      a.bounds_px != b.bounds_px || a.color_srgb_rgb != b.color_srgb_rgb ||
      a.raster_identity != b.raster_identity ||
      a.spans.size() != b.spans.size())
    return false;
  for (std::size_t i = 0; i < a.spans.size(); ++i) {
    const auto &x = a.spans[i];
    const auto &y = b.spans[i];
    if (x.x != y.x || x.y != y.y || x.opaque_length != y.opaque_length ||
        !x.coverage_u8.same_view(y.coverage_u8))
      return false;
  }
  return true;
}
class ReusableAnnotationLayer {
public:
  // Raw candidates are intentionally unproven. Only a sealed builder can
  // establish the binding between an actual final object and its coverage.
  ReusableAnnotationLayer(AnnotationObject object,
                          AnnotationCoverageLayer layer, PixelRect clip,
                          AnnotationScale scale,
                          std::uint64_t trajectory_revision)
      : object(std::move(object)), layer(std::move(layer)), clip(clip),
        scale(scale), trajectory_revision(trajectory_revision) {}
  AnnotationObject object;
  AnnotationCoverageLayer layer;
  PixelRect clip;
  AnnotationScale scale;
  std::uint64_t trajectory_revision{};
  bool has_valid_provenance() const {
    return provenance_ && same_annotation_source(object, provenance_->object) &&
           clip == provenance_->clip && scale == provenance_->scale &&
           trajectory_revision == provenance_->trajectory_revision &&
           same_coverage_source(layer, provenance_->layer);
  }

private:
  struct Provenance {
    AnnotationObject object;
    AnnotationCoverageLayer layer;
    PixelRect clip;
    AnnotationScale scale;
    std::uint64_t trajectory_revision;
  };
  std::shared_ptr<const Provenance> provenance_;
  void seal() {
    provenance_ = std::make_shared<const Provenance>(
        Provenance{object, layer, clip, scale, trajectory_revision});
  }
  friend class BrushStrokeBuilder;
};
struct BrushCoverageDelta {
  std::size_t segments_processed{}, sample_evaluations{}, frozen_rows{},
      copied_samples{}, opaque_pixels_skipped{},
      internal_samples_materialized{};
  std::uint64_t trajectory_revision{};
};
struct BrushPreview {
  AnnotationCoverageLayer layer;
  ObjectId id;
  BrushStyle style;
  PixelRect clip;
  AnnotationScale scale;
  std::uint64_t trajectory_revision{};
};
} // namespace hdrshot
