#pragma once
#include "core/linear_source.hpp"
#include "core/result.hpp"
#include <cstdint>
#include <memory>
#include <vector>

namespace hdrshot {
class WindowsIccProfile;
// WGC RGBA16F uses scRGB semantics without a profile, or Legacy device
// semantics with one. Both produce immutable RGBA32F Linear Display P3;
// alpha is set to one. Legacy requires scrgb_to_edr_scale == 1.
Result<LinearSourceRef, Error>
windows_normalize_scrgb_source(PixelSize size,
                               std::vector<std::uint16_t> rgba_scrgb,
                               float scrgb_to_edr_scale, double gain,
                               std::shared_ptr<const WindowsIccProfile> legacy_profile = {});
} // namespace hdrshot
