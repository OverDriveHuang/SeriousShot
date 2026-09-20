#include "domain/analysis/gamut_boundary.hpp"

namespace hdrshot::analysis {
namespace {
#include "domain/analysis/gamut_boundary_data.inc"
}
std::span<const std::array<float, 2>> perceptual_gamut_boundary(WorkingSpace s) {
  switch (s) {
  case WorkingSpace::srgb_sdr: return gamut_space_0;
  case WorkingSpace::display_p3_sdr: return gamut_space_1;
  case WorkingSpace::display_p3_pq: return gamut_space_2;
  case WorkingSpace::bt2020_pq: return gamut_space_3;
  }
  return {};
}
} // namespace hdrshot::analysis
