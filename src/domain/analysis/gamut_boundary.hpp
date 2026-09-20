#pragma once
#include "domain/analysis/types.hpp"
#include <span>

namespace hdrshot::analysis {
// Full RGB volume projected into Lab D65 a*/b* or BT.2124 T/P.
// Fixed D65 and channel range (SDR 0..1, HDR 0..10000 nit); independent of
// content, reference-white selection, mask, gain and viewport. This is a
// reference silhouette, NOT a per-pixel legal-gamut/clipping predicate.
// Immutable static storage, shared by every window/backend. Closed contour.
std::span<const std::array<float, 2>> perceptual_gamut_boundary(WorkingSpace);
} // namespace hdrshot::analysis
