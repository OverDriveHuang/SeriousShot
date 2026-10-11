#pragma once
#include "domain/analysis/types.hpp"
#include <algorithm>
#include <array>
namespace hdrshot {
struct MagnifierReadings {
  std::array<bool, 9> selected{true,  false, false, true, false,
                               false, false, false, false};
  friend bool operator==(const MagnifierReadings &,
                         const MagnifierReadings &) = default;
};
struct AnalyzerReadings {
  std::array<bool, 7> selected{false, false, true, false, false, false, false};
  friend bool operator==(const AnalyzerReadings &,
                         const AnalyzerReadings &) = default;
};
struct AnalysisDefaults {
  bool magnifier_enabled{true};
  int magnifier_zoom{8};
  analysis::WorkingSpace hdr_space{analysis::WorkingSpace::display_p3_pq};
  analysis::WorkingSpace sdr_space{analysis::WorkingSpace::display_p3_sdr};
  double reference_white_nits{203}, blur_sigma_px{};
  int sample_side{1};
  MagnifierReadings magnifier;
  AnalyzerReadings analyzer;
  friend bool operator==(const AnalysisDefaults &,
                         const AnalysisDefaults &) = default;
};
inline bool valid_analysis_defaults(const AnalysisDefaults &d) {
  const std::array zooms{4, 8, 12, 16};
  const std::array sides{1, 3, 5, 11, 31, 101};
  const std::array sigmas{0., .5, 1., 2., 4., 8.};
  return std::find(zooms.begin(), zooms.end(), d.magnifier_zoom) !=
             zooms.end() &&
         std::find(sides.begin(), sides.end(), d.sample_side) != sides.end() &&
         std::find(sigmas.begin(), sigmas.end(), d.blur_sigma_px) !=
             sigmas.end() &&
         (d.reference_white_nits == 100 || d.reference_white_nits == 203) &&
         (d.hdr_space == analysis::WorkingSpace::display_p3_pq ||
          d.hdr_space == analysis::WorkingSpace::bt2020_pq) &&
         (d.sdr_space == analysis::WorkingSpace::display_p3_sdr ||
          d.sdr_space == analysis::WorkingSpace::srgb_sdr);
}
inline analysis::Settings analysis_settings(const AnalysisDefaults &d,
                                            bool hdr) {
  return {hdr ? d.hdr_space : d.sdr_space, d.reference_white_nits,
          d.blur_sigma_px};
}
} // namespace hdrshot
