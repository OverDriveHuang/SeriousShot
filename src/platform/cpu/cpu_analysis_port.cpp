#include "platform/cpu/cpu_analysis_port.hpp"
#include "domain/annotation/annotation_compositing.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>

namespace hdrshot {
namespace {
using namespace analysis;
class CpuSource final : public LinearSource {
public:
  CpuSource(PixelSize size, FloatImage pixels)
      : size_(size), pixels_(std::move(pixels)) {}
  PixelSize size_px() const override { return size_; }
  std::size_t byte_count() const override { return pixels_.size() * 16; }
  Result<bool, Error> wait_until_ready() const override {
    return Result<bool, Error>::success(true);
  }
  Result<LinearFloatPixels, Error> read_region(PixelRect r) const override {
    using Out = Result<LinearFloatPixels, Error>;
    if (r.x < 0 || r.y < 0 || r.width <= 0 || r.height <= 0 ||
        std::int64_t(r.x) + r.width > size_.width ||
        std::int64_t(r.y) + r.height > size_.height)
      return Out::failure(analysis_error("source_region_out_of_bounds"));
    LinearFloatPixels output(std::size_t(r.width) * std::size_t(r.height) * 4);
    for (int y = 0; y < r.height; ++y)
      std::memcpy(output.data() + std::size_t(y) * std::size_t(r.width) * 4,
                  pixels_.data() +
                      std::size_t(y + r.y) * std::size_t(size_.width) + r.x,
                  std::size_t(r.width) * 16);
    return Out::success(std::move(output));
  }
  const FloatImage &pixels() const { return pixels_; }

private:
  PixelSize size_;
  FloatImage pixels_;
};
class CpuAnalysisPort final : public AnalysisPort {
public:
  Result<LinearSourceRef, Error>
  prepare(const SelectionRoiView &source,
          const AnnotationPixelPlan &annotations) override {
    using Out = Result<LinearSourceRef, Error>;
    if (!source.valid_storage() || annotations.output_size_px != source.size_px)
      return Out::failure(analysis_error("invalid_clean_analysis_input"));
    auto plan = materialize_annotation_plan(annotations);
    if (!plan)
      return Out::failure(plan.error());
    auto frame = FrameCropper::read_cpu_region(source);
    if (!frame)
      return Out::failure(frame.error());
    auto roi = FrameCropper::view(frame.value());
    FloatImage pixels(std::size_t(roi.size_px.width) *
                      std::size_t(roi.size_px.height));
    for (int y = 0; y < roi.size_px.height; ++y)
      for (int x = 0; x < roi.size_px.width; ++x) {
        auto &p = pixels[std::size_t(y) * std::size_t(roi.size_px.width) +
                         std::size_t(x)];
        p[3] = 1;
        for (std::size_t c = 0; c < 3; ++c) {
          auto offset = roi.first_sample_offset +
                        std::size_t(y) * roi.row_stride_samples +
                        std::size_t(x) * 4 + c;
          if (!roi.rgba_float.empty())
            p[c] = roi.rgba_float[offset];
          else {
            auto v = roi.sample(offset);
            if (!v)
              return Out::failure(v.error());
            p[c] = ExtendedP3Mapper::source_linear(v.value(),
                                                   roi.encoding.transfer);
          }
        }
      }
    for (auto &span : plan.value().annotation_owned_spans)
      for (int x = 0; x < span.length; ++x) {
        auto i = std::size_t(span.y) * std::size_t(roi.size_px.width) +
                 std::size_t(span.x) + std::size_t(x);
        auto ink = annotation_linear_sample(span, std::size_t(x));
        for (std::size_t c = 0; c < 3; ++c)
          pixels[i][c] =
              ink[c] +
              (ink[3] > 0 ? ink[3] * std::max(0.f, pixels[i][c]) : 0.f);
      }
    return make_cpu_analysis_source(roi.size_px, std::move(pixels));
  }
  Result<ResultRef, Error> analyze(const Input &input,
                                   const Request &request) override {
    using Out = Result<ResultRef, Error>;
    if (!input.source || !valid_request(request))
      return Out::failure(analysis_error("invalid_analysis_request"));
    auto work = prepare_cached(input, request.settings);
    if (!work)
      return Out::failure(work.error());
    Request statistics_request = request;
    statistics_request.scopes.wave_width =
        std::min(request.scopes.wave_width,
                 std::uint32_t(input.source->size_px().width));
    if (last_result_ && last_source_ == input.source && last_request_ &&
        same_statistics_request(*last_request_, statistics_request)) {
      auto result = std::make_shared<ResultData>(*last_result_);
      refresh_result_projection(*result, request);
      if (!same_detail_request(*last_request_, statistics_request))
        refine_cpu_scopes(
            *result, input.source->size_px(), source_pixels_, work_, request,
            !same_wave_detail_request(*last_request_, statistics_request),
            !same_vector_detail_request(*last_request_, statistics_request));
      else
        result->detail_ms = result->detail_gpu_ms = 0;
      auto start = std::chrono::steady_clock::now();
      refresh_cpu_samples(*result, input.source->size_px(), source_pixels_,
                          work_, request);
      result->prepare_ms = result->statistics_ms = 0;
      result->sampling_ms = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - start)
                                .count();
      result->retained_bytes = source_pixels_.size() * 16 + work_.size() * 16 +
                               result_bytes(*result);
      result->peak_bytes = result->retained_bytes +
                           additional_result_bytes(*last_result_, *result);
      last_result_ = result;
      last_request_ = statistics_request;
      return Out::success(std::move(result));
    }
    auto result =
        analyze_cpu(input.source->size_px(), source_pixels_, work_, request);
    if (!result)
      return Out::failure(result.error());
    result.value().prepare_ms = prepare_ms_;
    last_result_ = std::make_shared<ResultData>(std::move(result.value()));
    last_request_ = statistics_request;
    last_source_ = input.source;
    return Out::success(last_result_);
  }
  Result<LinearSourceRef, Error>
  compose_report(const Input &input, const ReportPlan &plan) override {
    using Out = Result<LinearSourceRef, Error>;
    if (!input.source || !valid_report_plan(plan))
      return Out::failure(analysis_error("invalid_report_plan"));
    auto prepared = prepare_cached(input, plan.source_view.settings);
    if (!prepared)
      return Out::failure(prepared.error());
    FloatImage output(std::size_t(plan.underlay.size.width) *
                      std::size_t(plan.underlay.size.height));
    auto source_size = input.source->size_px();
    auto &view = plan.source_view;
    using namespace analysis_math;
    auto pixel = [&](int x, int y) {
      x = std::clamp(x, 0, source_size.width - 1);
      y = std::clamp(y, 0, source_size.height - 1);
      auto v = source_pixels_[std::size_t(y) * std::size_t(source_size.width) +
                              std::size_t(x)];
      A3 c = a3(v[0], v[1], v[2]);
      if (!finite3(c))
        return a3(.35f, 0, .35f);
      return is_hdr(view.settings.working_space) ? c : clip3(c, 1);
    };
    auto mix = [](A3 a, A3 b, float t) {
      return add3(scale3(a, 1 - t), scale3(b, t));
    };
    for (int y = 0; y < plan.underlay.size.height; ++y)
      for (int x = 0; x < plan.underlay.size.width; ++x) {
        auto i = std::size_t(y) * std::size_t(plan.underlay.size.width) +
                 std::size_t(x);
        auto ui = [&](const UiImage &image) {
          auto *p = image.rgba.data() + i * 4;
          auto color = scale3(ui_rgb_to_linear_p3((unsigned(p[0]) << 16) |
                                                  (unsigned(p[1]) << 8) |
                                                  unsigned(p[2])),
                              float(plan.ui_white_edr));
          return std::array<float, 4>{color.x, color.y, color.z,
                                      float(p[3]) / 255};
        };
        auto base = ui(plan.underlay);
        A3 color = scale3(a3(base[0], base[1], base[2]), base[3]);
        int local_x = x - plan.source_rect.x, local_y = y - plan.source_rect.y;
        if (local_x >= 0 && local_y >= 0 && local_x < plan.source_rect.width &&
            local_y < plan.source_rect.height) {
          float px = (float(local_x) + .5f - float(view.offset_x)) /
                     float(view.scale),
                py = (float(local_y) + .5f - float(view.offset_y)) /
                     float(view.scale);
          color = a3(.004f, .006f, .008f);
          if (px >= 0 && py >= 0 && px < float(source_size.width) &&
              py < float(source_size.height)) {
            if (view.false_color) {
              auto w =
                  work_[std::size_t(int(py)) * std::size_t(source_size.width) +
                        std::size_t(int(px))];
              color = w[3] > 0 ? false_color_rgb(
                                     dot3(y_coefficients(working_gamut(unsigned(
                                              view.settings.working_space))),
                                          a3(w[0], w[1], w[2])))
                               : a3(.35f, 0, .35f);
            } else {
              float xx = px - .5f, yy = py - .5f;
              int lx = int(std::floor(xx)), ly = int(std::floor(yy));
              color = mix(
                  mix(pixel(lx, ly), pixel(lx + 1, ly), xx - float(lx)),
                  mix(pixel(lx, ly + 1), pixel(lx + 1, ly + 1), xx - float(lx)),
                  yy - float(ly));
            }
            if (!contains(view.mask, px, py))
              color = scale3(color, float(view.mask_outside_factor));
          }
        }
        auto top = ui(plan.overlay);
        color = mix(color, a3(top[0], top[1], top[2]), top[3]);
        output[i] = {color.x, color.y, color.z, 1};
      }
    return make_cpu_analysis_source(plan.underlay.size, std::move(output));
  }

private:
  Result<bool, Error> prepare_cached(const Input &input,
                                     const Settings &settings) {
    if (!input.source || !valid_settings(settings))
      return Result<bool, Error>::failure(
          analysis_error("invalid_analysis_input"));
    auto start = std::chrono::steady_clock::now();
    if (source_ != input.source) {
      auto size = input.source->size_px();
      auto data = input.source->read_region({0, 0, size.width, size.height});
      if (!data)
        return Result<bool, Error>::failure(data.error());
      source_pixels_.resize(std::size_t(size.width) * std::size_t(size.height));
      std::memcpy(source_pixels_.data(), data.value().data(),
                  source_pixels_.size() * 16);
      source_ = input.source;
      work_.clear();
    }
    if (work_.empty() || settings_ != settings) {
      auto data =
          prepare_work(input.source->size_px(), source_pixels_, settings);
      if (!data)
        return Result<bool, Error>::failure(data.error());
      work_ = std::move(data.value());
      settings_ = settings;
      prepare_ms_ = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    } else
      prepare_ms_ = 0;
    return Result<bool, Error>::success(true);
  }
  LinearSourceRef source_;
  FloatImage source_pixels_, work_;
  Settings settings_;
  double prepare_ms_{};
  LinearSourceRef last_source_;
  std::optional<Request> last_request_;
  ResultRef last_result_;
};
} // namespace
std::shared_ptr<AnalysisPort> make_cpu_analysis_port() {
  return std::make_shared<CpuAnalysisPort>();
}
Result<LinearSourceRef, Error>
make_cpu_analysis_source(PixelSize size, analysis::FloatImage pixels) {
  if (size.width <= 0 || size.height <= 0 ||
      std::uint64_t(size.width) * std::uint64_t(size.height) != pixels.size())
    return Result<LinearSourceRef, Error>::failure(
        analysis::analysis_error("invalid_cpu_source_shape"));
  return Result<LinearSourceRef, Error>::success(
      std::make_shared<CpuSource>(size, std::move(pixels)));
}
} // namespace hdrshot
