#pragma once

#include "core/geometry.hpp"
#include "core/linear_source.hpp"
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace hdrshot::analysis {
// Statistics are immutable once published. Copying a ResultData shares their
// payload; a producer mutation detaches first, so previous UI/report snapshots
// remain stable without copying whole grids on every hover readout.
template <class T> class SharedSamples {
public:
  using Storage = std::vector<T>;
  SharedSamples() = default;
  SharedSamples(std::initializer_list<T> values)
      : values_(std::make_shared<Storage>(values)) {}
  std::size_t size() const { return values_ ? values_->size() : 0; }
  std::size_t capacity() const { return values_ ? values_->capacity() : 0; }
  bool empty() const { return size() == 0; }
  const T *data() const { return read().data(); }
  T *data() { return write().data(); }
  auto begin() const { return read().begin(); }
  auto end() const { return read().end(); }
  auto begin() { return write().begin(); }
  auto end() { return write().end(); }
  const T &operator[](std::size_t i) const { return read()[i]; }
  T &operator[](std::size_t i) { return write()[i]; }
  void resize(std::size_t n) { write().resize(n); }
  void reserve(std::size_t n) { write().reserve(n); }
  void clear() { values_.reset(); }
  void push_back(const T &v) { write().push_back(v); }
  template <class Iterator> void assign(Iterator first, Iterator last) {
    values_ = std::make_shared<Storage>(first, last);
  }
  bool shares_storage_with(const SharedSamples &other) const {
    return values_ == other.values_;
  }
  friend bool operator==(const SharedSamples &a, const SharedSamples &b) {
    return a.values_ == b.values_ || a.read() == b.read();
  }

private:
  const Storage &read() const {
    static const Storage empty;
    return values_ ? *values_ : empty;
  }
  Storage &write() {
    if (!values_)
      values_ = std::make_shared<Storage>();
    else if (values_.use_count() != 1)
      values_ = std::make_shared<Storage>(*values_);
    return *values_;
  }
  std::shared_ptr<Storage> values_;
};
inline constexpr std::uint32_t max_histogram_bins = 131072;
using Rgb = std::array<double, 3>;
enum class WorkingSpace { srgb_sdr, display_p3_sdr, display_p3_pq, bt2020_pq };
enum class WaveMode { intensity, rgb, parade_rgb, parade_intensity_rgb };
enum class HistogramMode {
  intensity,
  rgb,
  rgb_adobe,
  parade_rgb,
  parade_adobe,
  hue
};
enum class VectorMode {
  ycbcr,
  perceptual
}; // perceptual = SDR Lab D65 / HDR ITP
enum class MaskShape { rectangle, ellipse };
enum class ExportAction {
  save_analysis,
  copy_analysis,
  save_original,
  copy_original
};

struct Settings {
  WorkingSpace working_space{WorkingSpace::display_p3_pq};
  double reference_white_nits{203.0};
  double blur_sigma_px{}; // 0, .5, 1, 2, 4, 8 physical source pixels
  friend bool operator==(const Settings &, const Settings &) = default;
};
struct Rect {
  double x{}, y{}, width{}, height{};
  friend bool operator==(const Rect &, const Rect &) = default;
};
struct Mask {
  bool enabled{};
  MaskShape shape{MaskShape::rectangle};
  Rect bounds; // source physical coordinates, sample at pixel centers
  friend bool operator==(const Mask &, const Mask &) = default;
};
struct AxisView {
  double zoom{1.0};
  double pan{}; // visible domain starts here; display=(domain-pan)*zoom
  friend bool operator==(const AxisView &, const AxisView &) = default;
};
struct ScopeOptions {
  WaveMode wave_mode{WaveMode::intensity};
  HistogramMode histogram_mode{HistogramMode::intensity};
  VectorMode vector_mode{VectorMode::perceptual};
  bool colorize{true};
  bool waveform_visible{true}, histogram_visible{true}, vector_visible{true};
  AxisView amplitude_view, histogram_view;
  double vector_zoom{1.0};
  // Viewport center in color-plane units. Navigation only, not sample data.
  std::array<double, 2> vector_pan{};
  std::uint32_t wave_width{512}, wave_height{256};
  std::uint32_t vector_width{384}, vector_height{384};
  std::uint32_t histogram_bins{1024}; // full-domain bins, not viewport-anchored
  // Optional display-only fine viewport. Full-domain statistics stay retained;
  // this bounded refinement never becomes the input for numerical readouts.
  std::uint32_t wave_detail_width{}, wave_detail_height{};
  std::uint32_t vector_detail_width{}, vector_detail_height{};
  // Geometry only, never allocation dimensions. The real viewport aspect must
  // survive the independently capped detail grid. Zero/zero lets headless
  // callers use the detail grid as their explicitly chosen viewport.
  std::uint32_t vector_viewport_width{}, vector_viewport_height{};
};
struct SampleRequest {
  std::uint64_t id{}; // 0 = transient hover; nonzero = fixed Swatch
  std::int32_t x{}, y{};
  std::uint32_t side{1};
  bool respect_mask{true}; // false for fixed Swatches, always
};
struct Request {
  std::uint64_t revision{};
  Settings settings;
  ScopeOptions scopes;
  Mask mask;
  std::vector<SampleRequest> samples;
};
struct Input {
  LinearSourceRef
      source; // immutable clean ROI, Linear P3 EDR, includes annotations
  std::uint64_t revision{};
  bool is_hdr{}; // inherited original screenshot class, not settings/headroom
};
struct Readout {
  std::uint64_t valid_count{}, invalid_count{};
  Rgb source_rgb_edr{}, work_rgb_edr{}, signal_rgb{};
  Rgb work_rgb_nits{};
  double y_nits{}, intensity{}, intensity_nits{};
  std::array<double, 3> perceptual{}; // HDR I,T,P / SDR L*,a*,b*
  std::array<double, 2> chroma_plane{}, ycbcr{};
  std::optional<double> hue_degrees;
  double chroma{};
  std::array<float, 3>
      display_rgb{}; // declared sRGB UI swatch, not measurement input
};
struct ProjectedSample {
  double source_x{};  // normalized full source x, not mask-local
  double intensity{}; // SDR Y' / HDR I, already nonlinear
  Rgb signal_rgb{};
  std::array<double, 2>
      vector{}; // current Vector mode, unprojected color-plane coordinates
  std::array<double, 3>
      histogram{}; // normalized full-domain histogram positions
  bool hue_valid{};
};
struct SampleResult {
  SampleRequest request;
  Rect clipped_bounds;
  Readout mean; // average linear RGB first, then derive values
  ProjectedSample mean_position;
  SharedSamples<ProjectedSample>
      distribution; // bounded by sampling area, not entire image
};
struct DensityGrid {
  std::uint32_t width{}, height{};
  SharedSamples<std::uint32_t>
      counts; // exact participating counts, row 0 = low coordinate
  SharedSamples<std::array<float, 3>>
      color_sums; // optional UI-sRGB sums, not measurement
  std::uint32_t maximum{};
  // Waveform only: exact number of source columns represented by each x-bin.
  // Display density divides by this footprint; counts themselves stay exact.
  SharedSamples<float> column_coverage;
  double maximum_density{};
};
struct Histogram {
  SharedSamples<std::uint64_t>
      counts; // full-domain anchored, including endpoint clips
  SharedSamples<std::array<float, 3>>
      color_sums; // optional Hue source-color sums
  std::uint64_t maximum{};
};
struct AxisTick {
  double value{}; // nit / percent / degree or raw color-plane coordinate
  double
      position{}; // normalized visible viewport position; may be outside [0,1]
  std::string label;
  bool major{true};
  bool boundary{}; // Adobe 100% boundary
};
struct ReferenceTarget {
  std::array<double, 2> position{}; // unprojected color-plane coordinate
  std::array<float, 3> color{};
  std::string label;
};
struct VectorCalibration {
  double x_extent{0.5},
      y_extent{0.5}; // unzoomed half-span, same metric both axes
  // Nominal reference-axis half-spans, not a clipping box for analyzed pixels.
  // Independent of the aspect-correct viewport and the retained density domain.
  std::optional<std::array<double, 2>> reference_extent;
  std::string x_label, y_label;
  std::vector<AxisTick> x_ticks, y_ticks;
  std::vector<ReferenceTarget> full_targets, reduced_targets;
  std::array<double, 2> skin_direction{};
};
struct ResultData {
  std::uint64_t revision{};
  Settings settings;
  ScopeOptions scopes;
  std::uint64_t valid_count{}, invalid_count{}, hue_count{};
  // Intensity,R',G',B'. A mode may leave unused planes empty.
  std::array<DensityGrid, 4> waveform;
  DensityGrid vectorscope;
  // Fixed full-data-domain half spans; independent of Mask and view pan/zoom.
  // Presentation transforms this retained grid into the current calibration.
  double vector_grid_x_extent{0.5}, vector_grid_y_extent{0.5};
  std::array<DensityGrid, 4> waveform_detail;
  DensityGrid vectorscope_detail;
  AxisView waveform_detail_view;
  double vector_detail_x_extent{}, vector_detail_y_extent{},
      vector_detail_zoom{1};
  std::array<double, 2> vector_detail_center{};
  std::array<Histogram, 4> histograms;
  std::vector<AxisTick> amplitude_ticks, histogram_ticks;
  VectorCalibration vector_calibration;
  Readout mask_mean;
  ProjectedSample mask_mean_position;
  std::vector<SampleResult> samples;
  double prepare_ms{}, statistics_ms{}, sampling_ms{};
  double prepare_gpu_ms{}, statistics_gpu_ms{}, sampling_gpu_ms{};
  double detail_ms{}, detail_gpu_ms{};
  std::size_t retained_bytes{}, peak_bytes{};
};
using ResultRef = std::shared_ptr<const ResultData>;

struct UiImage;
struct SourceView {
  PixelSize target_size{}; // physical pixels of native surface/report rect
  double scale{1.0};       // physical target pixels per source pixel
  double offset_x{}, offset_y{}; // target origin + scale*source coordinate
  Settings
      settings; // original uses only SDR/HDR range; false color also uses W
  bool false_color{};
  Mask mask; // presentation darkening only; never changes S
  double mask_outside_factor{0.25};
  // Presentation only: viewport-sized transparent sRGB operation marks. Never
  // measurement input. Native executors composite these above the HDR source;
  // reports supply their own retained-only overlay through ReportPlan.
  std::shared_ptr<const UiImage> operation_overlay;
};
struct UiImage {
  PixelSize size{};
  std::vector<std::uint8_t>
      rgba; // straight-alpha sRGB8, row-major, top-left origin
};
struct ReportPlan {
  std::uint64_t revision{};
  UiImage underlay; // full visible client area, no transient hover
  UiImage overlay;  // same size, transparent except Source's retained UI marks
  PixelRect
      source_rect; // clipped visible Source surface in report pixel coordinates
  SourceView source_view;
  double ui_white_edr{1.0}; // SDR UI white; independent of analysis white
};
} // namespace hdrshot::analysis
