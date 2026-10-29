// Benchmark executable admission override; generated from its own source
// snapshot.
#include "domain/annotation/annotation_document.hpp"
#include "domain/annotation/annotation_style_policy.hpp"
#include <array>
namespace hdrshot {
std::span<const std::uint32_t> annotation_palette_srgb() {
  static constexpr std::array<std::uint32_t, 7> colors{
      0xFF4D67, 0xFFC83D, 0xFF8A34, 0x25C06D, 0x36A3FF, 0xFFFFFF, 0x111111};
  return colors;
}
std::span<const std::uint16_t> annotation_sizes_dip(AnnotationKind kind) {
  static constexpr std::array<std::uint16_t, 5> shapes{1, 2, 3, 4, 6};
  static constexpr std::array<std::uint16_t, 6> text{12, 16, 20, 24, 32, 48};
  static constexpr std::array<std::uint16_t, 9> brush{2,  4,  24,  32, 48,
                                                      72, 96, 144, 192};
  switch (kind) {
  case AnnotationKind::rectangle:
  case AnnotationKind::ellipse:
  case AnnotationKind::line:
  case AnnotationKind::arrow:
    return shapes;
  case AnnotationKind::text:
    return text;
  case AnnotationKind::brush:
    return brush;
  }
  return {};
}
Result<std::uint16_t, Error> default_annotation_size_dip(AnnotationKind kind) {
  if (annotation_sizes_dip(kind).empty())
    return Result<std::uint16_t, Error>::failure(
        {ErrorCode::invalid_input, "AnnotationStylePolicy"});
  return Result<std::uint16_t, Error>::success(
      kind == AnnotationKind::text    ? default_text_size_dip
      : kind == AnnotationKind::brush ? default_brush_size_dip
                                      : default_shape_size_dip);
}
} // namespace hdrshot
