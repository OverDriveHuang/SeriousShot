#pragma once
#include "domain/analysis/types.hpp"
#include <bit>
#include <cmath>
#include <cstdint>

namespace hdrshot::analysis_math {
inline float a_pow(float x, float y) { return std::pow(x, y); }
inline float a_sqrt(float x) { return std::sqrt(x); }
inline float a_atan2(float y, float x) { return std::atan2(y, x); }
inline float a_floor(float x) { return std::floor(x); }
inline float a_log2(float x) { return std::log2(x); }
inline float a_copysign(float x, float y) { return std::copysign(x, y); }
inline bool a_finite(float x) {
  return (std::bit_cast<std::uint32_t>(x) & 0x7f800000u) != 0x7f800000u;
}
#define ANALYSIS_REAL float
#define ANALYSIS_LITERAL(x) x##f
#define ANALYSIS_MATH_SOURCE(...) __VA_ARGS__
#include "domain/analysis/portable_math.inc"
#undef ANALYSIS_MATH_SOURCE
#undef ANALYSIS_LITERAL
#undef ANALYSIS_REAL
inline constexpr char metal_source[] =
#define ANALYSIS_STRINGIFY_EXPANDED(...) #__VA_ARGS__
#define ANALYSIS_MATH_SOURCE(...) ANALYSIS_STRINGIFY_EXPANDED(__VA_ARGS__)
#define ANALYSIS_REAL float
#define ANALYSIS_LITERAL(x) x##f
#include "domain/analysis/portable_math.inc"
#undef ANALYSIS_MATH_SOURCE
#undef ANALYSIS_STRINGIFY_EXPANDED
#undef ANALYSIS_LITERAL
#undef ANALYSIS_REAL
    ;
} // namespace hdrshot::analysis_math

namespace hdrshot::analysis_math_double {
struct A3 { double x; double y; double z; };
inline A3 a3(double x, double y, double z) { return A3{x, y, z}; }
inline A3 scale3(A3 a, double s) { return a3(a.x * s, a.y * s, a.z * s); }
inline double dot3(A3 a, A3 b) { return (a.x * b.x + a.y * b.y) + a.z * b.z; }
inline bool finite3(A3 v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
inline double bound(double x, double lo, double hi) {
  return x < lo ? lo : (x > hi ? hi : x);
}
inline double a_pow(double x, double y) { return std::pow(x, y); }
inline double a_sqrt(double x) { return std::sqrt(x); }
inline double a_atan2(double y, double x) { return std::atan2(y, x); }
inline double a_log2(double x) { return std::log2(x); }
inline double a_copysign(double x, double y) { return std::copysign(x, y); }
inline bool a_finite(double x) { return std::isfinite(x); }
#define ANALYSIS_REAL double
#define ANALYSIS_LITERAL(x) x
#define ANALYSIS_MATH_SOURCE(...) __VA_ARGS__
#include "domain/analysis/portable_color_math.inc"
#undef ANALYSIS_MATH_SOURCE
#undef ANALYSIS_LITERAL
#undef ANALYSIS_REAL
} // namespace hdrshot::analysis_math_double

namespace hdrshot::analysis {
bool is_hdr(WorkingSpace space);
bool valid_settings(const Settings &settings);
bool valid_request(const Request &request);
bool valid_source_view(const SourceView &view);
bool valid_report_plan(const ReportPlan &plan);
bool contains(const Mask &mask, double source_x, double source_y);
Rect sample_bounds(PixelSize size, const SampleRequest &request);
std::vector<float> gaussian_weights(double sigma);
double axis_value_to_domain(const Settings &settings, HistogramMode mode,
                            double value);
double axis_domain_to_value(const Settings &settings, HistogramMode mode,
                            double domain);
double project_axis(double domain, const AxisView &view);
std::vector<AxisTick> amplitude_ticks(const Settings &settings,
                                      const AxisView &view);
std::vector<AxisTick> histogram_ticks(const Settings &settings,
                                      HistogramMode mode, const AxisView &view);
VectorCalibration vector_calibration(const Settings &settings, VectorMode mode,
                                     std::uint32_t width, std::uint32_t height,
                                     double zoom, std::array<double, 2> center = {});
std::array<double, 2> vector_domain_extents(const Settings &settings,
                                           VectorMode mode, std::uint32_t width,
                                           std::uint32_t height);
// Half extents of the fine tile in color-plane coordinates, independent of
// its allocation budget. Shared by CPU, GPU, cache identity and report gates.
std::array<double, 2> vector_detail_extents(const Request &request);
std::array<double, 2> project_vector(std::array<double, 2> value,
                                     const VectorCalibration &calibration,
                                     double zoom, std::array<double, 2> center = {});
Readout make_readout(const Rgb &source_mean, const Rgb &work_mean,
                     std::uint64_t valid, std::uint64_t invalid,
                     const Settings &settings);
ProjectedSample project_sample(const Rgb &work, double normalized_source_x,
                               const Settings &settings,
                               const ScopeOptions &scopes);
std::array<float, 3> display_srgb(const Rgb &work, WorkingSpace space);
} // namespace hdrshot::analysis
