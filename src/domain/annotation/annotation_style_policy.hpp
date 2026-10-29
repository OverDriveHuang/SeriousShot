#pragma once
#include "core/error.hpp"
#include "core/result.hpp"
#include <cstdint>
#include <span>
namespace hdrshot {
enum class AnnotationKind : std::uint8_t;
inline constexpr std::uint16_t default_shape_size_dip = 2;
inline constexpr std::uint16_t default_text_size_dip = 20;
inline constexpr std::uint16_t default_brush_size_dip = 24;
std::span<const std::uint32_t> annotation_palette_srgb();
std::span<const std::uint16_t> annotation_sizes_dip(AnnotationKind);
Result<std::uint16_t, Error> default_annotation_size_dip(AnnotationKind);
} // namespace hdrshot
