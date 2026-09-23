#pragma once

#include <array>
#include <optional>

namespace hdrshot::analysis {

// The project stores HDR ITP as [I, T, P], where T is already Ct/2 and I is
// the normalized PQ intensity component. The BT.2124 scaling is intentionally
// kept here, at the shared domain boundary, rather than in a UI formatter.
std::optional<double> delta_e_itp(const std::array<double, 3> &a,
                                  const std::array<double, 3> &b);

// CIEDE2000 on D65 Lab (kL=kC=kH=1), with no chromatic adaptation or implicit
// rescaling. Returns nullopt for non-finite input or non-finite arithmetic.
std::optional<double> delta_e_2000(const std::array<double, 3> &a,
                                   const std::array<double, 3> &b);

} // namespace hdrshot::analysis
