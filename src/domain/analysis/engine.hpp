#pragma once
#include "core/error.hpp"
#include "core/result.hpp"
#include "domain/analysis/math.hpp"
#include <span>

namespace hdrshot::analysis {
using FloatImage = std::vector<std::array<float, 4>>;
Error analysis_error(
    const char *reason, ErrorCode code = ErrorCode::invalid_input,
    std::source_location origin = std::source_location::current());
Result<FloatImage, Error>
prepare_work(PixelSize size, std::span<const std::array<float, 4>> source,
             const Settings &settings);
Result<ResultData, Error>
analyze_cpu(PixelSize size, std::span<const std::array<float, 4>> source,
            std::span<const std::array<float, 4>> work, const Request &request);
// Explicit small ROI result reconstruction for native sample readback.
SampleResult summarize_sample(PixelSize full_size, const SampleRequest &sample,
                              const Request &request, Rect bounds,
                              std::span<const std::array<float, 4>> source,
                              std::span<const std::array<float, 4>> work);
bool same_statistics_request(const Request &a, const Request &b);
bool same_wave_statistics_request(const Request &a, const Request &b);
bool same_vector_statistics_request(const Request &a, const Request &b);
bool same_histogram_statistics_request(const Request &a, const Request &b);
bool same_detail_request(const Request &a, const Request &b);
bool same_wave_detail_request(const Request &a, const Request &b);
bool same_vector_detail_request(const Request &a, const Request &b);
void refine_cpu_scopes(ResultData &result, PixelSize size,
                       std::span<const std::array<float, 4>> source,
                       std::span<const std::array<float, 4>> work,
                       const Request &request, bool refine_wave = true,
                       bool refine_vector = true);
void refresh_cpu_samples(ResultData &result, PixelSize size,
                         std::span<const std::array<float, 4>> source,
                         std::span<const std::array<float, 4>> work,
                         const Request &request);
void refresh_result_projection(ResultData &result, const Request &request);
std::size_t result_bytes(const ResultData &result);
std::size_t additional_result_bytes(const ResultData &result,
                                    const ResultData &shared_with);
void set_waveform_column_coverage(DensityGrid &grid,
                                  std::uint32_t source_width);
} // namespace hdrshot::analysis
