#include "ui/qt/analyzer_scope_plot.hpp"
#include "domain/analysis/math.hpp"
#include "domain/analysis/gamut_boundary.hpp"
#include <QImage>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

namespace hdrshot {
namespace {
const QColor ink("#e9ebef");
const QColor sample_ink("#ffe3a3"), distribution_ink("#57d5df");
constexpr double sample_highlight_multiplier = 4.;
const QColor channels[]{QColor("#e9ebef"), QColor("#eb6472"), QColor("#4bb58b"),
                        QColor("#619df0")};
QColor ui_color(double red, double green, double blue) {
  return QColor::fromRgbF(float(std::clamp(red, 0., 1.)),
                          float(std::clamp(green, 0., 1.)),
                          float(std::clamp(blue, 0., 1.)));
}
bool hdr(analysis::WorkingSpace s) {
  return s == analysis::WorkingSpace::display_p3_pq ||
         s == analysis::WorkingSpace::bt2020_pq;
}
double project(double domain, analysis::AxisView view) {
  return (domain - view.pan) * view.zoom;
}
bool parade(analysis::WaveMode mode) {
  return mode == analysis::WaveMode::parade_rgb ||
         mode == analysis::WaveMode::parade_intensity_rgb;
}
std::vector<int> wave_channels(analysis::WaveMode mode) {
  if (mode == analysis::WaveMode::intensity)
    return {0};
  if (mode == analysis::WaveMode::parade_intensity_rgb)
    return {0, 1, 2, 3};
  return {1, 2, 3};
}
void density_image(QImage &image, const analysis::DensityGrid &grid,
                   QColor color, bool colorize, double gain = 1.) {
  if (!grid.width || !grid.height ||
      grid.counts.size() != std::size_t(grid.width) * grid.height) {
    image = {};
    return;
  }
  if (image.size() != QSize(int(grid.width), int(grid.height)))
    image = QImage(int(grid.width), int(grid.height),
                   QImage::Format_ARGB32_Premultiplied);
  image.fill(Qt::transparent);
  const bool normalized_columns = grid.column_coverage.size() == grid.width;
  const double norm = std::log1p(normalized_columns
      ? std::max(1e-12, grid.maximum_density) : double(std::max(1u, grid.maximum)));
  for (std::uint32_t y = 0; y < grid.height; ++y)
    for (std::uint32_t x = 0; x < grid.width; ++x) {
      const auto i = std::size_t(y) * grid.width + x;
      const auto count = grid.counts[i];
      if (!count)
        continue;
      QColor c = color;
      const double density = normalized_columns
          ? double(count) / std::max(1.f, grid.column_coverage[x])
          : double(count);
      if (colorize && i < grid.color_sums.size())
        c = ui_color(double(grid.color_sums[i][0]) / double(count),
                     double(grid.color_sums[i][1]) / double(count),
                     double(grid.color_sums[i][2]) / double(count));
      c.setAlphaF(float(std::clamp(
          gain * (.18 + .82 * std::log1p(density) / norm), 0., 1.)));
      image.setPixelColor(int(x), int(grid.height - y - 1), c);
    }
}
bool same_density(const analysis::DensityGrid &a, const analysis::DensityGrid &b) {
  return a.width == b.width && a.height == b.height &&
         a.counts.shares_storage_with(b.counts) &&
         a.color_sums.shares_storage_with(b.color_sums) &&
         a.column_coverage.shares_storage_with(b.column_coverage);
}
QImage highlighted_trace(const QImage &base, bool monochrome) {
  if (base.isNull()) return {};
  QImage image(base.size(), QImage::Format_ARGB32_Premultiplied);
  for (int y = 0; y < base.height(); ++y) {
    const auto *in = reinterpret_cast<const QRgb *>(base.constScanLine(y));
    auto *out = reinterpret_cast<QRgb *>(image.scanLine(y));
    for (int x = 0; x < base.width(); ++x) {
      const int old_alpha = qAlpha(in[x]);
      if (!old_alpha) { out[x] = 0; continue; }
      const int alpha = std::min(255, int(old_alpha * sample_highlight_multiplier));
      // Amplify premultiplied intensity with one common ratio, retaining hue.
      // Saturation is the display alpha ceiling, never the Gain input limit 3.
      if (monochrome)
        out[x] = qRgba(distribution_ink.red() * alpha / 255,
                       distribution_ink.green() * alpha / 255,
                       distribution_ink.blue() * alpha / 255, alpha);
      else
        out[x] = qRgba(qRed(in[x]) * alpha / old_alpha,
                       qGreen(in[x]) * alpha / old_alpha,
                       qBlue(in[x]) * alpha / old_alpha, alpha);
    }
  }
  return image;
}
// A retained detail tile replaces (never alpha-adds to) the base within its
// domain. Panning exposes the base around it while the next tile is refined.
void draw_density_layers(QPainter &p, QRectF clip, QRectF base_rect,
                         const QImage &base, QRectF detail_rect,
                         const QImage &detail) {
  const bool fine = !detail.isNull() && detail_rect.isValid() &&
                    detail_rect.intersects(clip);
  p.save();
  p.setClipRect(clip, Qt::IntersectClip);
  if (fine) {
    QPainterPath outside, tile;
    outside.addRect(clip);
    tile.addRect(detail_rect);
    p.setClipPath(outside.subtracted(tile), Qt::IntersectClip);
  }
  p.drawImage(base_rect, base);
  p.restore();
  if (fine) {
    p.save();
    p.setClipRect(clip, Qt::IntersectClip);
    p.drawImage(detail_rect, detail);
    p.restore();
  }
}
void focus(QPainter &p, QPointF point, QColor c, bool mean) {
  p.setOpacity(1);
  p.setPen(QPen(QColor("#17202a"), 4));
  p.setBrush(mean ? c : Qt::NoBrush);
  p.drawEllipse(point, mean ? 5 : 4, mean ? 5 : 4);
  p.setPen(QPen(c, mean ? 2 : 1.6));
  p.drawEllipse(point, mean ? 5 : 4, mean ? 5 : 4);
  if (!mean) {
    p.drawLine(point - QPointF(7, 0), point + QPointF(7, 0));
    p.drawLine(point - QPointF(0, 7), point + QPointF(0, 7));
  }
}
} // namespace
std::vector<analysis::AxisTick> analyzer_axis_label_ticks(
    std::vector<analysis::AxisTick> ticks, double span, double label_height) {
  std::stable_sort(ticks.begin(), ticks.end(), [](const auto &a, const auto &b) {
    return (a.boundary ? 3 : a.major ? 2 : 1) >
           (b.boundary ? 3 : b.major ? 2 : 1);
  });
  std::vector<analysis::AxisTick> selected;
  std::vector<double> centers;
  if (!std::isfinite(span) || span <= 0 || label_height <= 0)
    return selected;
  const double half = std::min(label_height * .5, span * .5);
  for (const auto &tick : ticks) {
    if (!std::isfinite(tick.position) || tick.position < 0 || tick.position > 1)
      continue;
    const double center = std::clamp(tick.position * span, half, span - half);
    if (std::any_of(centers.begin(), centers.end(), [&](double previous) {
          return std::abs(previous - center) < label_height + 3.;
        }))
      continue;
    selected.push_back(tick);
    centers.push_back(center);
  }
  return selected;
}
QRectF analyzer_vector_target_label_bounds(QRectF plot, QPointF target,
                                           QSizeF text_size) {
  const QRectF inner = plot.adjusted(2, 2, -2, -2);
  if (!plot.contains(target) || text_size.width() > inner.width() ||
      text_size.height() > inner.height())
    return {};
  const auto center = plot.center();
  const double left = target.x() < center.x()
                          ? target.x() - text_size.width() - 7
                          : target.x() + 7;
  const double top = target.y() < center.y()
                         ? target.y() - text_size.height() - 4
                         : target.y() + 4;
  return {
      QPointF(
          std::clamp(left, inner.left(), inner.right() - text_size.width()),
          std::clamp(top, inner.top(), inner.bottom() - text_size.height())),
      text_size};
}
AnalyzerScopePlot::AnalyzerScopePlot(Kind kind, QWidget *parent)
    : QWidget(parent), kind_(kind) {
  setObjectName(kind == Kind::waveform    ? "analyzerWavePlot"
                : kind == Kind::histogram ? "analyzerHistogramPlot"
                                          : "analyzerVectorPlot");
  setMinimumSize(1, 1);
  setMouseTracking(true);
  setFocusPolicy(Qt::StrongFocus);
  setCursor(kind == Kind::vectorscope ? Qt::ArrowCursor : Qt::OpenHandCursor);
}
void AnalyzerScopePlot::set_result(analysis::ResultRef result,
                                   bool rebuild_density) {
  bool rebuild_detail = !result_ || !result;
  if (result_ && result) {
    if (kind_ == Kind::waveform)
      for (std::size_t ch = 0; ch < 4; ++ch) {
        rebuild_density = rebuild_density || !same_density(result_->waveform[ch], result->waveform[ch]);
        rebuild_detail = rebuild_detail || !same_density(result_->waveform_detail[ch], result->waveform_detail[ch]);
      }
    if (kind_ == Kind::vectorscope) {
      rebuild_density = rebuild_density || !same_density(result_->vectorscope, result->vectorscope);
      rebuild_detail = rebuild_detail || !same_density(result_->vectorscope_detail, result->vectorscope_detail);
    }
  }
  result_ = std::move(result);
  if (rebuild_density || rebuild_detail)
    rebuild_density_images(rebuild_density, rebuild_detail);
  update();
}
void AnalyzerScopePlot::set_options(analysis::ScopeOptions options) {
  const bool color_changed = options_.colorize != options.colorize ||
      parade(options_.wave_mode) != parade(options.wave_mode);
  options_ = options;
  if (color_changed)
    rebuild_density_images();
  update();
}
void AnalyzerScopePlot::set_mask_enabled(bool enabled) {
  mask_enabled_ = enabled;
  update();
}
void AnalyzerScopePlot::set_pending(bool pending) {
  if (pending_ == pending)
    return;
  pending_ = pending;
  update();
}
void AnalyzerScopePlot::set_display_gain(double gain) {
  if (kind_ == Kind::histogram || !std::isfinite(gain) || gain < .1 || gain > 3. || gain == display_gain_)
    return;
  display_gain_ = gain;
  rebuild_density_images();
  update();
}
void AnalyzerScopePlot::rebuild_density_images(bool base, bool detail) {
  highlight_dirty_ = true;
  if (!result_)
    return;
  if (kind_ == Kind::waveform)
    for (std::size_t ch = 0; ch < 4; ++ch) {
      if (base) density_image(wave_images_[ch], result_->waveform[ch], channels[ch],
                    ch == 0 && options_.colorize &&
                        !parade(options_.wave_mode), display_gain_);
      if (detail) density_image(wave_detail_images_[ch], result_->waveform_detail[ch], channels[ch],
                    ch == 0 && options_.colorize && !parade(options_.wave_mode), display_gain_);
    }
  if (kind_ == Kind::vectorscope) {
    if (base) density_image(vector_image_, result_->vectorscope, ink, true, display_gain_);
    if (detail) density_image(vector_detail_image_, result_->vectorscope_detail, ink, true, display_gain_);
  }
}
void AnalyzerScopePlot::set_references(
    std::vector<AnalyzerReferenceLine> refs) {
  references_ = std::move(refs);
  update();
}
void AnalyzerScopePlot::set_transient_hidden(bool hidden) {
  if (transient_hidden_ == hidden)
    return;
  transient_hidden_ = hidden;
  update();
}
QRectF AnalyzerScopePlot::plot_rect() const {
  if (kind_ == Kind::waveform)
    return QRectF(47, 10, std::max(1, width() - 60),
                  std::max(1, height() - 36));
  if (kind_ == Kind::histogram)
    return QRectF(12, 14, std::max(1, width() - 25),
                  std::max(1, height() - (options_.histogram_mode == analysis::HistogramMode::hue ? 46 : 64)));
  return QRectF(9, 9, std::max(1, width() - 18), std::max(1, height() - 18));
}
void AnalyzerScopePlot::resizeEvent(QResizeEvent *) {
  if (surface_resized)
    surface_resized();
}
void AnalyzerScopePlot::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  p.fillRect(rect(), kind_ == Kind::waveform && result_ &&
                             hdr(result_->settings.working_space)
                         ? Qt::black
                         : QColor("#040506"));
  p.setPen(ink);
  QFont font = p.font();
  font.setPixelSize(11);
  p.setFont(font);
  if (!result_) {
    // Do not flash a placeholder for a short first calculation. Errors are
    // reported by the owning window, not hidden behind an eternal spinner.
    return;
  }
  if (kind_ == Kind::waveform)
    draw_wave(p);
  else if (kind_ == Kind::histogram)
    draw_histogram(p);
  else
    draw_vector(p);
  if (!result_->valid_count) {
    p.setPen(ink);
    p.drawText(plot_rect().adjusted(8, 6, 0, 0), Qt::AlignTop | Qt::AlignLeft,
               "无有效样本");
  } else if (kind_ == Kind::histogram &&
             options_.histogram_mode == analysis::HistogramMode::hue &&
             !result_->hue_count)
    p.drawText(plot_rect(), Qt::AlignCenter, "无有效色相样本");
  if (!pending_)
    draw_markers(p);
}
void AnalyzerScopePlot::draw_trace_layers(QPainter &p, bool highlight) {
  const auto r = plot_rect();
  p.save();
  p.setClipRect(r, Qt::IntersectClip);
  p.setRenderHint(QPainter::SmoothPixmapTransform, true);
  if (kind_ == Kind::vectorscope) {
    const auto c = view_calibration();
    const auto extent_rect = [&](double x, double y, std::array<double, 2> center) {
      const QSizeF size(r.width() * x / c.x_extent * options_.vector_zoom,
                        r.height() * y / c.y_extent * options_.vector_zoom);
      const auto pos = analysis::project_vector(center, c, options_.vector_zoom, options_.vector_pan);
      return QRectF(QPointF(r.left() + pos[0] * r.width() - size.width() / 2,
                            r.bottom() - pos[1] * r.height() - size.height() / 2), size);
    };
    draw_density_layers(p, r,
        extent_rect(result_->vector_grid_x_extent, result_->vector_grid_y_extent, {}),
        highlight ? highlight_vector_ : vector_image_,
        extent_rect(result_->vector_detail_x_extent, result_->vector_detail_y_extent, result_->vector_detail_center),
        highlight ? highlight_vector_detail_ : vector_detail_image_);
    p.restore();
    return;
  }
  const auto chs = wave_channels(options_.wave_mode);
  const bool separate = parade(options_.wave_mode);
  for (std::size_t lane = 0; lane < chs.size(); ++lane) {
    const int ch = chs[lane];
    const QRectF lane_rect =
        separate ? QRectF(r.x() + double(lane) * r.width() / double(chs.size()),
                          r.y(), r.width() / double(chs.size()), r.height())
                 : r;
    const auto view = options_.amplitude_view;
    const QRectF projected(lane_rect.left(),
                           lane_rect.bottom() - (1. - view.pan) * view.zoom *
                                                    lane_rect.height(),
                           lane_rect.width(), view.zoom * lane_rect.height());
    const auto fine_view = result_->waveform_detail_view;
    const QRectF fine_rect(lane_rect.left(),
        lane_rect.bottom() - (fine_view.pan + 1. / fine_view.zoom - view.pan) * view.zoom * lane_rect.height(),
        lane_rect.width(), view.zoom / fine_view.zoom * lane_rect.height());
    draw_density_layers(p, lane_rect, projected,
                        highlight ? highlight_wave_[std::size_t(ch)] : wave_images_[std::size_t(ch)],
                        fine_rect,
                        highlight ? highlight_wave_detail_[std::size_t(ch)] : wave_detail_images_[std::size_t(ch)]);
  }
  p.restore();
}
void AnalyzerScopePlot::draw_wave(QPainter &p) {
  draw_trace_layers(p);
  const auto r = plot_rect();
  const auto chs = wave_channels(options_.wave_mode);
  const bool separate = parade(options_.wave_mode);
  const auto ticks = analysis::amplitude_ticks(result_->settings,
                                              options_.amplitude_view);
  for (const auto &tick : ticks) {
    if (tick.position < 0 || tick.position > 1)
      continue;
    const double y = r.bottom() - tick.position * r.height();
    p.setPen(QPen(QColor(233, 235, 239, 45), 1));
    p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
  }
  const double label_height = p.fontMetrics().height();
  for (const auto &tick : analyzer_axis_label_ticks(ticks, r.height(), label_height)) {
    const double y = r.bottom() - tick.position * r.height();
    p.setPen(ink);
    p.drawText(QRectF(2, std::clamp(y - label_height * .5, r.top(),
                                   std::max(r.top(), r.bottom() - label_height)),
                      42, label_height), Qt::AlignRight | Qt::AlignVCenter,
               QString::fromStdString(tick.label));
  }
  for (std::size_t lane = 0; lane < (separate ? chs.size() : 1); ++lane) {
    const double left = separate ? r.left() + double(lane) * r.width() /
                                                  double(chs.size())
                                 : r.left(),
                 right = separate ? left + r.width() / double(chs.size())
                                  : r.right();
    for (const auto &ref : references_) {
      if (!ref.enabled)
        continue;
      if (separate && ref.intensity != (chs[lane] == 0))
        continue;
      const double v = project(ref.domain, options_.amplitude_view);
      if (v < 0 || v > 1)
        continue;
      const double y = r.bottom() - v * r.height();
      p.setPen(QPen(ink, 1, Qt::DashLine));
      p.drawLine(QPointF(left, y), QPointF(right, y));
      p.drawText(QRectF(left, std::max(r.top(), y - 15), right - left - 4, 14),
                 Qt::AlignRight, ref.label);
    }
    if (separate) {
      p.setPen(channels[chs[lane]]);
      p.drawText(
          QRectF(left, r.bottom() + 5, right - left, 17), Qt::AlignCenter,
          chs[lane] == 0 ? (hdr(result_->settings.working_space) ? "I" : "Y′")
                         : QString("RGB").mid(chs[lane] - 1, 1) + "′");
    }
  }
  if (!separate) {
    p.setPen(ink);
    p.drawText(QPointF(r.left(), height() - 7),
               options_.wave_mode == analysis::WaveMode::intensity
                   ? (hdr(result_->settings.working_space) ? "I · nit（等效）"
                                                           : "Y′ · %")
                   : (hdr(result_->settings.working_space)
                          ? "R′G′B′ · PQ · nit"
                          : "R′G′B′ · sRGB · %"));
  }
}
void AnalyzerScopePlot::draw_histogram(QPainter &p) {
  const auto r = plot_rect();
  const auto mode = options_.histogram_mode;
  const bool separate = mode == analysis::HistogramMode::parade_rgb ||
                        mode == analysis::HistogramMode::parade_adobe;
  const bool rgb = mode == analysis::HistogramMode::rgb ||
                   mode == analysis::HistogramMode::rgb_adobe;
  const bool hue = mode == analysis::HistogramMode::hue;
  const int lanes = separate ? 3 : 1;
  const auto view = options_.histogram_view;
  std::uint64_t peak = 1;
  for (const auto &h : result_->histograms)
    peak = std::max(peak, h.maximum);
  for (int lane = 0; lane < lanes; ++lane) {
    const QRectF lane_rect(r.x() + double(lane) * r.width() / lanes, r.y(),
                           r.width() / lanes - (separate ? 8 : 0), r.height());
    p.save();
    p.setClipRect(lane_rect);
    const auto &h =
        result_->histograms[std::size_t((rgb || separate) ? lane + 1 : 0)];
    const auto n = h.counts.size();
    for (std::size_t i = 0; i < n; ++i) {
      const double x = lane_rect.x() +
                       project(double(i) / double(n), view) * lane_rect.width(),
                   bar_width = lane_rect.width() * view.zoom / double(n);
      if (x + bar_width < lane_rect.left() || x > lane_rect.right())
        continue;
      if (rgb) {
        std::array<double, 3> heights{};
        std::vector<double> levels{0};
        for (int ch = 0; ch < 3; ++ch) {
          const auto &bins = result_->histograms[std::size_t(ch + 1)].counts;
          heights[std::size_t(ch)] =
              i < bins.size() ? std::sqrt(double(bins[i]) / double(peak)) : 0;
          levels.push_back(heights[std::size_t(ch)]);
        }
        std::sort(levels.begin(), levels.end());
        for (std::size_t b = 1; b < levels.size(); ++b) {
          if (levels[b] <= levels[b - 1])
            continue;
          const QColor c(heights[0] >= levels[b] ? 145 : 0,
                         heights[1] >= levels[b] ? 145 : 0,
                         heights[2] >= levels[b] ? 145 : 0);
          p.fillRect(QRectF(x,
                            lane_rect.bottom() - levels[b] * lane_rect.height(),
                            std::max(.6, bar_width),
                            (levels[b] - levels[b - 1]) * lane_rect.height()),
                     c);
        }
      } else {
        const double v = std::sqrt(double(h.counts[i]) / double(peak));
        QColor c = channels[separate ? lane + 1 : 0];
        if (hue && h.counts[i] && i < h.color_sums.size()) {
          const double count = double(h.counts[i]);
          c = ui_color(h.color_sums[i][0] / count, h.color_sums[i][1] / count,
                       h.color_sums[i][2] / count);
        }
        p.fillRect(QRectF(x, lane_rect.bottom() - v * lane_rect.height(),
                          std::max(.6, bar_width), v * lane_rect.height()),
                   c);
      }
    }
    p.restore();
    std::array<std::vector<QRectF>, 2> occupied;
    auto ticks = analysis::histogram_ticks(result_->settings, mode, view);
    std::stable_sort(ticks.begin(), ticks.end(),
                     [](const auto &a, const auto &b) {
                       return (a.boundary ? 3
                               : a.major  ? 2
                                          : 1) > (b.boundary ? 3
                                                 : b.major  ? 2
                                                            : 1);
                     });
    for (const auto &tick : ticks) {
      if (tick.position < 0 || tick.position > 1)
        continue;
      const double x = lane_rect.x() + tick.position * lane_rect.width();
      p.setPen(QPen(QColor(233, 235, 239,
                           tick.boundary ? 210
                           : tick.major  ? 110
                                         : 50),
                    tick.boundary ? 1.5 : 1, Qt::DashLine));
      p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
      const QString label = QString::fromStdString(tick.label);
      const double tw = p.fontMetrics().horizontalAdvance(label);
      if (tw > lane_rect.width())
        continue;
      const double left =
          std::clamp(x - tw * .5, lane_rect.left(), lane_rect.right() - tw);
      for (int row = 0; row < 2; ++row) {
        const QRectF candidate(left - 3, r.bottom() + 6 + row * 14, tw + 6, 14);
        if (std::none_of(occupied[std::size_t(row)].begin(),
                         occupied[std::size_t(row)].end(), [&](auto other) {
                           return other.intersects(candidate);
                         })) {
          occupied[std::size_t(row)].push_back(candidate);
          p.setPen(ink);
          p.drawText(candidate, Qt::AlignCenter, label);
          break;
        }
      }
    }
    for (const auto &ref : references_) {
      if (!ref.enabled)
        continue;
      const double v = project(ref.domain, view);
      if (v < 0 || v > 1)
        continue;
      const double x = lane_rect.x() + v * lane_rect.width();
      p.setPen(QPen(ink, 1, Qt::DashLine));
      p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
      p.drawText(
          QPointF(std::clamp(x + 3, lane_rect.left(),
                             std::max(lane_rect.left(),
                                      lane_rect.right() -
                                          p.fontMetrics().horizontalAdvance(
                                              ref.label))),
                  r.top() + 12),
          ref.label);
    }
    if (separate) {
      p.setPen(channels[lane + 1]);
      p.drawText(QRectF(lane_rect.x(), 0, lane_rect.width(), 14),
                 Qt::AlignCenter, QString("RGB").mid(lane, 1) + "′");
    }
  }
  p.setPen(ink);
  const bool adobe = mode == analysis::HistogramMode::rgb_adobe ||
                     mode == analysis::HistogramMode::parade_adobe;
  if (!adobe && !hue)
    p.drawText(
        QPointF(r.left(), height() - 5),
        (mode == analysis::HistogramMode::intensity
                   ? (hdr(result_->settings.working_space) ? "I · nit（等效）"
                                                           : "Y′ · %")
                   : (hdr(result_->settings.working_space) ? "PQ · nit"
                                                           : "sRGB · %")));
}
analysis::VectorCalibration AnalyzerScopePlot::view_calibration() const {
  const auto r = plot_rect();
  return analysis::vector_calibration(
      result_->settings, options_.vector_mode,
      std::uint32_t(std::max(1., std::ceil(r.width() * devicePixelRatioF()))),
      std::uint32_t(std::max(1., std::ceil(r.height() * devicePixelRatioF()))),
      options_.vector_zoom, options_.vector_pan);
}
void AnalyzerScopePlot::draw_vector(QPainter &p) {
  const auto r = plot_rect();
  const auto c = view_calibration();
  const auto at = [&](std::array<double, 2> value) {
    const auto pos = analysis::project_vector(value, c, options_.vector_zoom, options_.vector_pan);
    return QPointF(r.x() + pos[0] * r.width(),
                   r.bottom() - pos[1] * r.height());
  };
  const auto center = at({0, 0});
  p.save();
  p.setClipRect(r);
  p.setPen(QPen(QColor(233, 235, 239, 60), 1));
  const auto reference = c.reference_extent.value_or(std::array<double, 2>{
      std::abs(options_.vector_pan[0]) + c.x_extent / options_.vector_zoom,
      std::abs(options_.vector_pan[1]) + c.y_extent / options_.vector_zoom});
  p.drawLine(at({-reference[0], 0}), at({reference[0], 0}));
  p.drawLine(at({0, -reference[1]}), at({0, reference[1]}));
  draw_trace_layers(p);
  if (options_.vector_mode == analysis::VectorMode::perceptual) {
    const auto space = result_->settings.working_space;
    if (gamut_space_ != space) {
      gamut_path_ = {};
      const auto boundary = analysis::perceptual_gamut_boundary(space);
      if (!boundary.empty()) {
        gamut_path_.moveTo(boundary.front()[0], boundary.front()[1]);
        for (std::size_t i = 1; i < boundary.size(); ++i)
          gamut_path_.lineTo(boundary[i][0], boundary[i][1]);
        gamut_path_.closeSubpath();
      }
      gamut_space_ = space;
    }
    // Exactly the trace calibration; never normalize T and P separately.
    const QTransform transform(r.width() * options_.vector_zoom / (2 * c.x_extent), 0,
                               0, -r.height() * options_.vector_zoom / (2 * c.y_extent),
                               center.x(), center.y());
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(QColor(233, 235, 239, 165), 1.);
    pen.setDashPattern({5., 4.});
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawPath(transform.map(gamut_path_));
    p.restore();
  }
  std::vector<std::pair<QPointF, QString>> target_labels;
  for (int reduced = 0; reduced < 2; ++reduced) {
    const auto &targets = reduced ? c.reduced_targets : c.full_targets;
    if (targets.empty())
      continue;
    QPainterPath path;
    path.moveTo(at(targets[0].position));
    for (std::size_t i = 1; i < targets.size(); ++i)
      path.lineTo(at(targets[i].position));
    path.closeSubpath();
    p.setPen(QPen(QColor(233, 235, 239, reduced ? 75 : 150), 1,
                  reduced ? Qt::DashLine : Qt::SolidLine));
    p.drawPath(path);
    if (!reduced)
      for (const auto &target : targets) {
        auto pt = at(target.position);
        p.drawRect(QRectF(pt - QPointF(4, 4), QSizeF(8, 8)));
        target_labels.emplace_back(pt, QString::fromStdString(target.label));
      }
  }
  const double norm = std::hypot(c.skin_direction[0], c.skin_direction[1]);
  if (norm > 0) {
    const double dx = c.skin_direction[0] / norm;
    const double dy = c.skin_direction[1] / norm;
    const auto skin_bounds = c.reference_extent.value_or(
        std::array<double, 2>{c.x_extent, c.y_extent});
    const double extent =
        .96 * std::min(skin_bounds[0] / std::max(1e-12, std::abs(dx)),
                       skin_bounds[1] / std::max(1e-12, std::abs(dy)));
    auto endpoint = at({dx * extent, dy * extent});
    p.setPen(QPen(QColor(233, 235, 239, 150), 1, Qt::DashLine));
    p.drawLine(center, endpoint);
    const QString label = options_.vector_mode == analysis::VectorMode::ycbcr
                              ? "I-line"
                              : "Skin ref";
    const double text_width = p.fontMetrics().horizontalAdvance(label);
    const double label_x = endpoint.x() < center.x()
                               ? endpoint.x() - text_width - 7
                               : endpoint.x() + 7;
    p.drawText(
        QRectF(std::clamp(label_x, r.left() + 3,
                          std::max(r.left() + 3, r.right() - text_width - 3)),
               std::clamp(endpoint.y() + 4, r.top() + 2,
                          std::max(r.top() + 2, r.bottom() - 16)),
               text_width, 14),
        Qt::AlignCenter, label);
  }
  p.setPen(ink);
  const QPointF x_name(std::min(r.right() - 20, at({reference[0], 0}).x()), center.y() - 8);
  const QPointF y_name(center.x() + 6, std::max(r.top() + 12, at({0, reference[1]}).y() - 6));
  p.drawText(x_name, QString::fromStdString(c.x_label));
  p.drawText(y_name, QString::fromStdString(c.y_label));
  p.drawText(center + QPointF(5, 14), "0");
  std::vector<QRectF> occupied_x, occupied_y;
  for (const auto &tick : c.x_ticks) {
    auto pt = at({tick.value, 0});
    if (pt.x() < r.left() || pt.x() > r.right() ||
        (!tick.boundary && (pt.x() < r.left() + 16 || pt.x() > r.right() - 16)) ||
        std::abs(pt.x() - center.x()) < 24)
      continue;
    p.drawLine(pt - QPointF(0, 3), pt + QPointF(0, 3));
    const QString label = QString::fromStdString(tick.label);
    const double text_width = p.fontMetrics().horizontalAdvance(label) + 8;
    const QRectF bounds(std::clamp(pt.x() - text_width * .5, r.left(),
                                   std::max(r.left(), r.right() - text_width)),
                        pt.y() + 5, text_width, 14);
    if (std::none_of(
            occupied_x.begin(), occupied_x.end(),
            [&](const auto &other) { return bounds.intersects(other); })) {
      occupied_x.push_back(bounds);
      p.drawText(bounds, Qt::AlignCenter, label);
    }
  }
  for (const auto &tick : c.y_ticks) {
    auto pt = at({0, tick.value});
    if (pt.y() < r.top() || pt.y() > r.bottom() ||
        (!tick.boundary && (pt.y() < r.top() + 22 || pt.y() > r.bottom() - 14)) ||
        std::abs(pt.y() - center.y()) < 24)
      continue;
    p.drawLine(pt - QPointF(3, 0), pt + QPointF(3, 0));
    const QString label = QString::fromStdString(tick.label);
    const double text_width = p.fontMetrics().horizontalAdvance(label) + 4;
    const bool edge = pt.y() < r.top() + 22 || pt.y() > r.bottom() - 14;
    const QRectF bounds(edge ? pt.x() - text_width - 6 : pt.x() + 6,
                        std::clamp(pt.y() - 7, r.top(), std::max(r.top(), r.bottom() - 16)),
                        text_width, 16);
    if (std::none_of(
            occupied_y.begin(), occupied_y.end(),
            [&](const auto &other) { return bounds.intersects(other); })) {
      occupied_y.push_back(bounds);
      p.drawText(bounds, Qt::AlignVCenter, label);
    }
  }
  // Keep target names legible near the bounded axis endpoints at small zooms.
  // Numeric calibration has priority; a target box is retained even if its
  // optional name cannot fit without overlapping another label.
  std::vector<QRectF> occupied = occupied_x;
  occupied.insert(occupied.end(), occupied_y.begin(), occupied_y.end());
  occupied.push_back(QRectF(center + QPointF(3, 2), QSizeF(15, 16)));
  occupied.push_back(QRectF(x_name - QPointF(0, 12), QSizeF(22, 14)));
  occupied.push_back(QRectF(y_name - QPointF(0, 12), QSizeF(22, 14)));
  for (const auto &[point, label] : target_labels) {
    const auto bounds = analyzer_vector_target_label_bounds(
        r, point, QSizeF(p.fontMetrics().horizontalAdvance(label) + 2,
                        p.fontMetrics().height()));
    if (bounds.isEmpty()) continue;
    for (double offset : {0., 16., -16., 32., -32.}) {
      const auto candidate = bounds.translated(0, offset);
      if (!r.contains(candidate) || std::any_of(occupied.begin(), occupied.end(),
          [&](const auto &other) { return candidate.adjusted(-2, -2, 2, 2).intersects(other); })) continue;
      occupied.push_back(candidate);
      p.drawText(candidate, Qt::AlignCenter, label);
      break;
    }
  }
  p.restore();
}
void AnalyzerScopePlot::rebuild_highlight_images() {
  if (!highlight_dirty_ || !result_) return;
  // Derive from the already colored premultiplied trace; no second logarithm,
  // color conversion or full-statistics pass on first hover after refinement.
  // This is user Gain * 2 (up to normal 8-bit alpha quantization/saturation).
  if (kind_ == Kind::waveform) {
    for (std::size_t ch = 0; ch < 4; ++ch) {
      const bool colored = ch == 0 && options_.colorize && !parade(options_.wave_mode);
      highlight_wave_[ch] = highlighted_trace(wave_images_[ch], ch == 0 && !colored);
      highlight_wave_detail_[ch] = highlighted_trace(wave_detail_images_[ch], ch == 0 && !colored);
    }
  } else if (kind_ == Kind::vectorscope) {
    highlight_vector_ = highlighted_trace(vector_image_, false);
    highlight_vector_detail_ = highlighted_trace(vector_detail_image_, false);
  }
  highlight_dirty_ = false;
}
void AnalyzerScopePlot::draw_distribution(QPainter &p, const analysis::SampleResult &sample) {
  if (sample.distribution.empty()) return;
  const auto r = plot_rect();
  if (kind_ == Kind::histogram) {
    const auto mode = options_.histogram_mode;
    const bool hue = mode == analysis::HistogramMode::hue;
    const bool single = hue || mode == analysis::HistogramMode::intensity;
    const bool sep = mode == analysis::HistogramMode::parade_rgb || mode == analysis::HistogramMode::parade_adobe;
    const auto n = std::uint32_t(result_->histograms[single ? 0 : 1].counts.size());
    if (!n) return;
    std::array<std::vector<unsigned>, 3> bins;
    const int channels_count = single ? 1 : 3;
    for (int ch = 0; ch < channels_count; ++ch) bins[std::size_t(ch)].resize(n);
    unsigned peak = 1;
    for (const auto &point : sample.distribution) {
      if (hue && !point.hue_valid) continue;
      for (int ch = 0; ch < channels_count; ++ch) {
        const double value = point.histogram[std::size_t(ch)];
        if (!std::isfinite(value)) continue;
        const auto bin = analysis_math::signal_bin(float(value), n);
        peak = std::max(peak, ++bins[std::size_t(ch)][bin]);
      }
    }
    // One bounded-height, full-domain-normalized region histogram. Accumulate
    // RGB into a transparent strip first so overlaps stay neutral; never add
    // per-sample full-height lines onto the existing full-image histogram.
    const double strip_height = std::min(24., r.height() * .18);
    const double dpr = devicePixelRatioF();
    QImage strip(QSize(int(std::ceil(r.width() * dpr)), int(std::ceil(strip_height * dpr))), QImage::Format_ARGB32_Premultiplied);
    strip.setDevicePixelRatio(dpr); strip.fill(Qt::transparent);
    QPainter layer(&strip);
    layer.setCompositionMode(QPainter::CompositionMode_Plus);
    for (int ch = 0; ch < channels_count; ++ch) {
      const double width = sep ? r.width() / 3. - 8. : r.width();
      const double left = sep ? double(ch) * r.width() / 3. : 0.;
      layer.setClipRect(QRectF(left, 0, width, strip_height));
      const QColor color = single ? distribution_ink
          : (ch == 0 ? QColor(210, 0, 0) : ch == 1 ? QColor(0, 210, 0) : QColor(0, 0, 210));
      for (std::uint32_t b = 0; b < n; ++b) {
        const auto count = bins[std::size_t(ch)][b];
        if (!count) continue;
        const double x = left + project(double(b) / n, options_.histogram_view) * width;
        const double bw = width * options_.histogram_view.zoom / n;
        if (x + bw < left || x > left + width) continue;
        const double height = strip_height * double(count) / peak;
        layer.fillRect(QRectF(x, strip_height - height, bw, height), color);
      }
    }
    layer.end();
    p.drawImage(QPointF(r.left(), r.bottom() - strip_height), strip);
    return;
  }
  rebuild_highlight_images();
  const auto calibration = kind_ == Kind::vectorscope ? view_calibration() : analysis::VectorCalibration{};
  const auto chs = wave_channels(options_.wave_mode);
  const bool sep = parade(options_.wave_mode);
  std::vector<QPointF> points;
  points.reserve(sample.distribution.size() * (kind_ == Kind::waveform ? chs.size() : 1));
  QRectF bounds;
  const auto add = [&](QPointF point) {
    if (!std::isfinite(point.x()) || !std::isfinite(point.y()) || !r.contains(point)) return;
    points.push_back(point);
    const QRectF dot(point - QPointF(1, 1), QSizeF(2, 2));
    bounds = bounds.isEmpty() ? dot : bounds.united(dot);
  };
  for (const auto &point : sample.distribution) {
    if (kind_ == Kind::vectorscope) {
      const auto pos = analysis::project_vector(point.vector, calibration, options_.vector_zoom, options_.vector_pan);
      add(QPointF(r.x() + pos[0] * r.width(), r.bottom() - pos[1] * r.height()));
    } else for (std::size_t lane = 0; lane < chs.size(); ++lane) {
      const auto ch = chs[lane];
      const double value = ch == 0 ? point.intensity : point.signal_rgb[std::size_t(ch - 1)];
      const double width = sep ? r.width() / double(chs.size()) : r.width();
      const double left = r.x() + (sep ? double(lane) * width : 0.);
      add(QPointF(left + point.source_x * width, r.bottom() - project(value, options_.amplitude_view) * r.height()));
    }
  }
  if (points.empty()) return;
  const double dpr = devicePixelRatioF();
  const QRect tile_bounds = bounds.intersected(r).toAlignedRect();
  const QSize physical(int(std::ceil(tile_bounds.width() * dpr)), int(std::ceil(tile_bounds.height() * dpr)));
  QImage mask(physical, QImage::Format_ARGB32_Premultiplied);
  mask.setDevicePixelRatio(dpr); mask.fill(Qt::transparent);
  // Idempotent occupancy raster: collisions set the same cell, never accumulate
  // opacity. The original trace supplies density/color, not the hover point count.
  for (const auto &point : points) {
    const int x0 = std::max(0, int(std::floor((point.x() - 1 - tile_bounds.x()) * dpr)));
    const int y0 = std::max(0, int(std::floor((point.y() - 1 - tile_bounds.y()) * dpr)));
    const int x1 = std::min(physical.width(), int(std::ceil((point.x() + 1 - tile_bounds.x()) * dpr)));
    const int y1 = std::min(physical.height(), int(std::ceil((point.y() + 1 - tile_bounds.y()) * dpr)));
    for (int y = y0; y < y1; ++y) {
      auto *row = reinterpret_cast<QRgb *>(mask.scanLine(y));
      std::fill(row + x0, row + x1, qRgba(255, 255, 255, 255));
    }
  }
  QImage tile(physical, QImage::Format_ARGB32_Premultiplied);
  tile.setDevicePixelRatio(dpr); tile.fill(Qt::black);
  QPainter layer(&tile);
  layer.translate(-tile_bounds.topLeft());
  draw_trace_layers(layer, true);
  layer.resetTransform();
  layer.setCompositionMode(QPainter::CompositionMode_DestinationIn);
  layer.drawImage(QPointF(), mask);
  layer.end();
  // Opaque occupied cells replace the original trace; G + 4G is not the desired
  // gain. Unoccupied pixels remain transparent, preserving the surrounding plot.
  p.drawImage(tile_bounds.topLeft(), tile);
}
void AnalyzerScopePlot::draw_markers(QPainter &p) {
  if (!result_)
    return;
  const auto r = plot_rect();
  const auto calibration = kind_ == Kind::vectorscope
                               ? view_calibration()
                               : analysis::VectorCalibration{};
  p.save();
  p.setClipRect(r);
  const auto draw = [&](const analysis::ProjectedSample &sample, bool mean) {
    if (kind_ == Kind::waveform) {
      const auto chs = wave_channels(options_.wave_mode);
      const bool separate = parade(options_.wave_mode);
      for (std::size_t lane = 0; lane < chs.size(); ++lane) {
        const auto ch = chs[lane];
        const double value =
            ch == 0 ? sample.intensity : sample.signal_rgb[std::size_t(ch - 1)];
        const double left = separate ? r.x() + double(lane) * r.width() /
                                                   double(chs.size())
                                     : r.x(),
                     lane_width =
                         separate ? r.width() / double(chs.size()) : r.width();
        const QPointF pt(left + sample.source_x * lane_width,
                         r.bottom() - project(value, options_.amplitude_view) *
                                          r.height());
        QColor guide = sample_ink;
        guide.setAlphaF(.6f);
        p.setPen(QPen(guide, 1, Qt::DashLine));
        p.drawLine(QPointF(left, pt.y()), QPointF(left + lane_width, pt.y()));
        focus(p, pt, sample_ink, mean);
      }
    } else if (kind_ == Kind::histogram) {
      const auto mode = options_.histogram_mode;
      const bool sep = mode == analysis::HistogramMode::parade_rgb ||
                       mode == analysis::HistogramMode::parade_adobe;
      const bool single = mode == analysis::HistogramMode::intensity ||
                          mode == analysis::HistogramMode::hue;
      if (mode == analysis::HistogramMode::hue && !sample.hue_valid)
        return;
      for (int ch = 0; ch < (single ? 1 : 3); ++ch) {
        const double x = r.x() + (sep ? ch * r.width() / 3 : 0) +
                         project(sample.histogram[std::size_t(ch)],
                                 options_.histogram_view) *
                             (sep ? r.width() / 3 - 8 : r.width());
        p.setPen(QPen(sample_ink, mean ? 2.5 : 1.5));
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        focus(p, QPointF(x, r.bottom() - 7), sample_ink, mean);
      }
    } else {
      const auto pos = analysis::project_vector(
          sample.vector, calibration, options_.vector_zoom, options_.vector_pan);
      const QPointF pt(r.x() + pos[0] * r.width(),
                       r.bottom() - pos[1] * r.height());
      QColor guide = sample_ink;
      guide.setAlphaF(.6f);
      p.setPen(QPen(guide, 1, Qt::DashLine));
      const auto reference = calibration.reference_extent.value_or(std::array<double, 2>{
          std::abs(options_.vector_pan[0]) + calibration.x_extent / options_.vector_zoom,
          std::abs(options_.vector_pan[1]) + calibration.y_extent / options_.vector_zoom});
      const double half_width = reference[0] * options_.vector_zoom * r.width() / (2 * calibration.x_extent);
      const double half_height = reference[1] * options_.vector_zoom * r.height() / (2 * calibration.y_extent);
      const auto origin = analysis::project_vector({0, 0}, calibration, options_.vector_zoom, options_.vector_pan);
      const QPointF center(r.left() + origin[0] * r.width(), r.bottom() - origin[1] * r.height());
      if (std::abs(sample.vector[0]) <= reference[0] && std::abs(sample.vector[1]) <= reference[1]) {
        p.drawLine(QPointF(center.x() - half_width, pt.y()), QPointF(center.x() + half_width, pt.y()));
        p.drawLine(QPointF(pt.x(), center.y() - half_height), QPointF(pt.x(), center.y() + half_height));
      }
      focus(p, pt, sample_ink, mean);
    }
  };
  if (!transient_hidden_)
    for (const auto &sample : result_->samples)
      if (sample.request.id == 0 && sample.mean.valid_count) {
        draw_distribution(p, sample);
        draw(sample.mean_position, false);
      }
  if (mask_enabled_ && result_->mask_mean.valid_count)
    draw(result_->mask_mean_position, true);
  p.restore();
}
void AnalyzerScopePlot::notify_options() {
  if (options_changed)
    options_changed(options_);
  update();
}
void AnalyzerScopePlot::fit() {
  if (kind_ == Kind::waveform)
    options_.amplitude_view = {};
  else if (kind_ == Kind::histogram)
    options_.histogram_view = {};
  else {
    options_.vector_zoom = 1.;
    options_.vector_pan = {};
  }
  notify_options();
}
void AnalyzerScopePlot::zoom_at(QPointF position, double factor) {
  if (!std::isfinite(factor) || factor <= 0)
    return;
  if (kind_ == Kind::vectorscope) {
    options_.vector_zoom =
        std::clamp(options_.vector_zoom * factor, .5, 32.);
  } else {
    auto &v = kind_ == Kind::waveform ? options_.amplitude_view
                                      : options_.histogram_view;
    const auto r = plot_rect();
    double fraction = kind_ == Kind::waveform
                          ? (r.bottom() - position.y()) / r.height()
                          : (position.x() - r.left()) / r.width();
    if (kind_ == Kind::histogram &&
        (options_.histogram_mode == analysis::HistogramMode::parade_rgb ||
         options_.histogram_mode == analysis::HistogramMode::parade_adobe))
      fraction = std::fmod(fraction * 3., 1.);
    fraction = std::clamp(fraction, 0., 1.);
    const double anchor = v.pan + fraction / v.zoom;
    v.zoom = std::clamp(v.zoom * factor, 1., 64.);
    v.pan = std::clamp(anchor - fraction / v.zoom, 0., 1. - 1. / v.zoom);
  }
  notify_options();
}
void AnalyzerScopePlot::pan_by(QPointF pixels) {
  if (kind_ == Kind::vectorscope) {
    if (!result_ || !std::isfinite(pixels.x()) || !std::isfinite(pixels.y()))
      return;
    const auto r = plot_rect();
    const auto c = view_calibration();
    // Keep the color plane isotropic; positive gesture deltas move the image,
    // while the viewport center moves in the opposite domain direction.
    options_.vector_pan[0] = std::clamp(options_.vector_pan[0] -
        pixels.x() * 2 * c.x_extent / (r.width() * options_.vector_zoom), -1e6, 1e6);
    options_.vector_pan[1] = std::clamp(options_.vector_pan[1] +
        pixels.y() * 2 * c.y_extent / (r.height() * options_.vector_zoom), -1e6, 1e6);
    notify_options();
    return;
  }
  auto &v = kind_ == Kind::waveform ? options_.amplitude_view
                                    : options_.histogram_view;
  const auto r = plot_rect();
  double fraction = kind_ == Kind::waveform ? pixels.y() / r.height()
                                           : -pixels.x() / r.width();
  if (kind_ == Kind::histogram &&
      (options_.histogram_mode == analysis::HistogramMode::parade_rgb ||
       options_.histogram_mode == analysis::HistogramMode::parade_adobe))
    fraction *= 3.;
  const double next = std::clamp(v.pan + fraction / v.zoom, 0., 1. - 1. / v.zoom);
  if (next != v.pan) {
    v.pan = next;
    notify_options();
  }
}
void AnalyzerScopePlot::wheelEvent(QWheelEvent *e) {
  // Trackpad scrolling is translation, not a disguised magnification. Keep
  // wheel notches as zoom and handle actual pinch through NativeGesture.
  if ((!e->pixelDelta().isNull() || e->phase() != Qt::NoScrollPhase) &&
      !(e->modifiers() & Qt::ControlModifier)) {
    if (e->phase() != Qt::ScrollEnd)
      pan_by(e->pixelDelta().isNull() ? QPointF(e->angleDelta()) / 8.
                                     : QPointF(e->pixelDelta()));
  } else {
    const double delta = e->angleDelta().isNull() ? e->pixelDelta().y() * 8.
                                                 : e->angleDelta().y();
    if (delta != 0)
      zoom_at(e->position(), std::exp(delta * .003));
  }
  e->accept();
}
void AnalyzerScopePlot::mousePressEvent(QMouseEvent *e) {
  if (e->button() != Qt::LeftButton || kind_ == Kind::vectorscope)
    return;
  dragging_ = true;
  drag_start_ = e->position();
  old_options_ = options_;
  grabMouse();
  setCursor(Qt::ClosedHandCursor);
  e->accept();
}
void AnalyzerScopePlot::mouseMoveEvent(QMouseEvent *e) {
  if (!dragging_)
    return;
  auto &v = kind_ == Kind::waveform ? options_.amplitude_view
                                    : options_.histogram_view;
  const auto old = kind_ == Kind::waveform ? old_options_.amplitude_view
                                           : old_options_.histogram_view;
  const auto r = plot_rect();
  double fraction = kind_ == Kind::waveform
                        ? (e->position().y() - drag_start_.y()) / r.height()
                        : (drag_start_.x() - e->position().x()) / r.width();
  if (kind_ == Kind::histogram &&
      (options_.histogram_mode == analysis::HistogramMode::parade_rgb ||
       options_.histogram_mode == analysis::HistogramMode::parade_adobe))
    fraction *= 3;
  v.pan = std::clamp(old.pan + fraction / old.zoom, 0., 1. - 1. / v.zoom);
  notify_options();
  e->accept();
}
void AnalyzerScopePlot::mouseReleaseEvent(QMouseEvent *e) {
  if (dragging_) {
    dragging_ = false;
    releaseMouse();
    setCursor(Qt::OpenHandCursor);
  }
  e->accept();
}
void AnalyzerScopePlot::leaveEvent(QEvent *) {}
bool AnalyzerScopePlot::event(QEvent *e) {
  if (e->type() == QEvent::NativeGesture) {
    auto *gesture = static_cast<QNativeGestureEvent *>(e);
    switch (gesture->gestureType()) {
    case Qt::BeginNativeGesture:
      cancel_gesture();
      break;
    case Qt::ZoomNativeGesture:
      zoom_at(gesture->position(), std::max(.01, 1. + gesture->value()));
      break;
    case Qt::PanNativeGesture:
      pan_by(gesture->delta());
      break;
    case Qt::EndNativeGesture:
      break;
    default:
      return QWidget::event(e);
    }
    e->accept();
    return true;
  }
  if ((e->type() == QEvent::UngrabMouse || e->type() == QEvent::WindowDeactivate ||
       e->type() == QEvent::Hide) && dragging_)
    cancel_gesture();
  return QWidget::event(e);
}
bool AnalyzerScopePlot::cancel_gesture() {
  if (!dragging_)
    return false;
  dragging_ = false;
  options_ = old_options_;
  releaseMouse();
  setCursor(Qt::OpenHandCursor);
  notify_options();
  return true;
}
} // namespace hdrshot
