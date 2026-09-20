#include "platform/freetype/freetype_text_rasterizer_port.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MULTIPLE_MASTERS_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace hdrshot {
namespace {

Error font_error(
    const ErrorCode code,
    const char* reason,
    std::map<std::string, std::string> context = {}) {
  context["reason"] = reason;
  return Error{code, "FreeTypeTextRasterizerPort", Retryability::never, std::move(context)};
}

Result<std::vector<std::uint32_t>, Error> decode_utf8(const std::string& text) {
  std::vector<std::uint32_t> codepoints;
  for (std::size_t index = 0; index < text.size();) {
    const auto first = static_cast<std::uint8_t>(text[index]);
    std::uint32_t value{};
    std::size_t length{};
    if ((first & 0x80U) == 0U) {
      value = first;
      length = 1;
    } else if ((first & 0xE0U) == 0xC0U) {
      value = first & 0x1FU;
      length = 2;
    } else if ((first & 0xF0U) == 0xE0U) {
      value = first & 0x0FU;
      length = 3;
    } else if ((first & 0xF8U) == 0xF0U) {
      value = first & 0x07U;
      length = 4;
    } else {
      return Result<std::vector<std::uint32_t>, Error>::failure(
          font_error(ErrorCode::invalid_input, "invalid_utf8"));
    }
    if (index + length > text.size()) {
      return Result<std::vector<std::uint32_t>, Error>::failure(
          font_error(ErrorCode::invalid_input, "truncated_utf8"));
    }
    for (std::size_t offset = 1; offset < length; ++offset) {
      const auto next = static_cast<std::uint8_t>(text[index + offset]);
      if ((next & 0xC0U) != 0x80U) {
        return Result<std::vector<std::uint32_t>, Error>::failure(
            font_error(ErrorCode::invalid_input, "invalid_utf8_continuation"));
      }
      value = (value << 6U) | (next & 0x3FU);
    }
    const bool overlong = (length == 2 && value < 0x80U) ||
        (length == 3 && value < 0x800U) || (length == 4 && value < 0x10000U);
    if (overlong || value > 0x10FFFFU || (value >= 0xD800U && value <= 0xDFFFU)) {
      return Result<std::vector<std::uint32_t>, Error>::failure(
          font_error(ErrorCode::invalid_input, "invalid_unicode_scalar"));
    }
    codepoints.push_back(value);
    index += length;
  }
  return Result<std::vector<std::uint32_t>, Error>::success(std::move(codepoints));
}

}  // namespace

struct FreeTypeTextRasterizerPort::Impl {
  std::mutex mutex;
  std::string font_path;
  std::string font_identity;
  FT_Library library{};
  FT_Face face{};
  std::optional<Error> initialization_error;

  Impl(std::string path, std::string identity)
      : font_path(std::move(path)), font_identity(std::move(identity)) {
    if (font_path.empty() || font_identity.empty()) {
      initialization_error = font_error(ErrorCode::invalid_input, "missing_font_contract");
      return;
    }
    auto status = FT_Init_FreeType(&library);
    if (status != 0) {
      initialization_error = font_error(
          ErrorCode::font_unavailable,
          "freetype_initialization_failed",
          {{"nativeCode", std::to_string(status)}});
      return;
    }
    status = FT_New_Face(library, font_path.c_str(), 0, &face);
    if (status != 0) {
      initialization_error = font_error(
          ErrorCode::font_unavailable,
          "font_load_failed",
          {{"nativeCode", std::to_string(status)}, {"path", font_path}});
      return;
    }

    FT_MM_Var* variation = nullptr;
    status = FT_Get_MM_Var(face, &variation);
    if (status != 0 || variation == nullptr) {
      initialization_error = font_error(
          ErrorCode::font_unavailable,
          "font_variation_axes_unavailable",
          {{"nativeCode", std::to_string(status)}, {"path", font_path}});
      return;
    }
    std::vector<FT_Fixed> coordinates(variation->num_axis);
    bool found_weight = false;
    constexpr FT_Fixed calibrated_weight = static_cast<FT_Fixed>(450L * 65536L);
    for (FT_UInt index = 0; index < variation->num_axis; ++index) {
      const auto& axis = variation->axis[index];
      coordinates[index] = axis.def;
      if (axis.tag == FT_MAKE_TAG('w', 'g', 'h', 't')) {
        coordinates[index] = std::clamp(calibrated_weight, axis.minimum, axis.maximum);
        found_weight = coordinates[index] == calibrated_weight;
      }
    }
    const auto variation_status = found_weight
        ? FT_Set_Var_Design_Coordinates(
              face, variation->num_axis, coordinates.data())
        : static_cast<FT_Error>(1);
    FT_Done_MM_Var(library, variation);
    if (!found_weight || variation_status != 0) {
      initialization_error = font_error(
          ErrorCode::font_unavailable,
          found_weight ? "font_calibrated_weight_rejected" : "font_weight_axis_missing",
          {{"nativeCode", std::to_string(variation_status)}, {"path", font_path}});
    }
  }

  ~Impl() {
    if (face != nullptr) {
      FT_Done_Face(face);
    }
    if (library != nullptr) {
      FT_Done_FreeType(library);
    }
  }
};

FreeTypeTextRasterizerPort::FreeTypeTextRasterizerPort(
    std::string font_path,
    std::string font_identity)
    : impl_(std::make_unique<Impl>(std::move(font_path), std::move(font_identity))) {}

FreeTypeTextRasterizerPort::~FreeTypeTextRasterizerPort() = default;

Result<TextMetrics, Error> FreeTypeTextRasterizerPort::measure(
    const TextMeasureRequest& request) {
  const auto codepoints = decode_utf8(request.utf8_text);
  if (!codepoints) {
    return Result<TextMetrics, Error>::failure(codepoints.error());
  }
  if (request.font_size_pt == 0 ||
      !std::isfinite(request.pixels_per_point) || request.pixels_per_point <= 0.0 ||
      codepoints.value().empty()) {
    return Result<TextMetrics, Error>::failure(
        font_error(ErrorCode::invalid_input, "invalid_text_measure_request"));
  }
  const auto pixel_size = static_cast<unsigned int>(std::max(
      1.0,
      std::floor(
          static_cast<double>(request.font_size_pt) * request.pixels_per_point + 0.5)));

  const std::scoped_lock lock(impl_->mutex);
  if (impl_->initialization_error.has_value()) {
    return Result<TextMetrics, Error>::failure(*impl_->initialization_error);
  }
  const auto size_status = FT_Set_Pixel_Sizes(impl_->face, 0, pixel_size);
  if (size_status != 0) {
    return Result<TextMetrics, Error>::failure(font_error(
        ErrorCode::unsupported_encoding,
        "font_size_rejected",
        {{"nativeCode", std::to_string(size_status)}}));
  }

  const auto line_height = static_cast<std::int32_t>(
      std::max<FT_Pos>(64, impl_->face->size->metrics.height) / 64);
  std::int32_t maximum_width = 1;
  std::int32_t pen_x = 0;
  std::int32_t line_right = 0;
  std::int32_t line_count = 1;
  FT_UInt previous_glyph = 0;
  for (const auto codepoint : codepoints.value()) {
    if (codepoint == '\n') {
      maximum_width = std::max(maximum_width, std::max(pen_x, line_right));
      pen_x = 0;
      line_right = 0;
      previous_glyph = 0;
      ++line_count;
      continue;
    }
    const auto glyph_index = FT_Get_Char_Index(impl_->face, codepoint);
    if (glyph_index == 0) {
      return Result<TextMetrics, Error>::failure(font_error(
          ErrorCode::unsupported_encoding,
          "glyph_missing",
          {{"codepoint", std::to_string(codepoint)}}));
    }
    if (previous_glyph != 0 && FT_HAS_KERNING(impl_->face)) {
      FT_Vector kerning{};
      if (FT_Get_Kerning(
              impl_->face, previous_glyph, glyph_index, FT_KERNING_DEFAULT, &kerning) == 0) {
        pen_x += static_cast<std::int32_t>(kerning.x / 64);
      }
    }
    const auto load_status = FT_Load_Glyph(impl_->face, glyph_index, FT_LOAD_DEFAULT);
    if (load_status != 0) {
      return Result<TextMetrics, Error>::failure(font_error(
          ErrorCode::unsupported_encoding,
          "glyph_measurement_failed",
          {{"codepoint", std::to_string(codepoint)},
           {"nativeCode", std::to_string(load_status)}}));
    }
    const auto& glyph = *impl_->face->glyph;
    line_right = std::max(
        line_right,
        pen_x + static_cast<std::int32_t>(glyph.metrics.horiBearingX / 64) +
            static_cast<std::int32_t>((glyph.metrics.width + 63) / 64));
    pen_x += static_cast<std::int32_t>(glyph.advance.x / 64);
    previous_glyph = glyph_index;
  }
  maximum_width = std::max(maximum_width, std::max(pen_x, line_right));
  return Result<TextMetrics, Error>::success(TextMetrics{
      PixelSize{maximum_width, std::max(1, line_count * line_height)},
      impl_->font_identity,
  });
}

Result<TextCoverageMask, Error> FreeTypeTextRasterizerPort::rasterize(
    const TextRasterRequest& request) {
  const auto codepoints = decode_utf8(request.utf8_text);
  if (!codepoints) {
    return Result<TextCoverageMask, Error>::failure(codepoints.error());
  }
  if (request.object_id.value == 0 || request.mask_size_px.width <= 0 ||
      request.mask_size_px.height <= 0 || request.font_size_pt == 0 ||
      !std::isfinite(request.pixels_per_point) || request.pixels_per_point <= 0.0 ||
      codepoints.value().empty()) {
    return Result<TextCoverageMask, Error>::failure(
        font_error(ErrorCode::invalid_input, "invalid_text_raster_request"));
  }

  const auto width = static_cast<std::size_t>(request.mask_size_px.width);
  const auto height = static_cast<std::size_t>(request.mask_size_px.height);
  if (width > std::numeric_limits<std::size_t>::max() / height) {
    return Result<TextCoverageMask, Error>::failure(
        font_error(ErrorCode::invalid_input, "mask_size_overflow"));
  }
  const auto pixel_size = static_cast<unsigned int>(std::max(
      1.0,
      std::floor(
          static_cast<double>(request.font_size_pt) * request.pixels_per_point + 0.5)));

  const std::scoped_lock lock(impl_->mutex);
  if (impl_->initialization_error.has_value()) {
    return Result<TextCoverageMask, Error>::failure(*impl_->initialization_error);
  }
  const auto size_status = FT_Set_Pixel_Sizes(impl_->face, 0, pixel_size);
  if (size_status != 0) {
    return Result<TextCoverageMask, Error>::failure(font_error(
        ErrorCode::unsupported_encoding,
        "font_size_rejected",
        {{"nativeCode", std::to_string(size_status)}}));
  }

  std::vector<std::uint8_t> coverage(width * height, 0);
  const auto line_height = std::max<FT_Pos>(64, impl_->face->size->metrics.height) / 64;
  const auto ascender = impl_->face->size->metrics.ascender / 64;
  const auto line_count = static_cast<std::int32_t>(
      1 + std::count(codepoints.value().begin(), codepoints.value().end(), '\n'));
  const auto block_height = line_count * static_cast<std::int32_t>(line_height);
  std::int32_t baseline = std::max(0, (request.mask_size_px.height - block_height) / 2) +
      static_cast<std::int32_t>(ascender);
  std::int32_t pen_x = 0;
  FT_UInt previous_glyph = 0;

  for (const auto codepoint : codepoints.value()) {
    if (codepoint == '\n') {
      pen_x = 0;
      baseline += static_cast<std::int32_t>(line_height);
      previous_glyph = 0;
      continue;
    }
    const auto glyph_index = FT_Get_Char_Index(impl_->face, codepoint);
    if (glyph_index == 0) {
      return Result<TextCoverageMask, Error>::failure(font_error(
          ErrorCode::unsupported_encoding,
          "glyph_missing",
          {{"codepoint", std::to_string(codepoint)}}));
    }
    if (previous_glyph != 0 && FT_HAS_KERNING(impl_->face)) {
      FT_Vector kerning{};
      if (FT_Get_Kerning(
              impl_->face, previous_glyph, glyph_index, FT_KERNING_DEFAULT, &kerning) == 0) {
        pen_x += static_cast<std::int32_t>(kerning.x / 64);
      }
    }
    const auto load_status = FT_Load_Glyph(
        impl_->face,
        glyph_index,
        FT_LOAD_DEFAULT);
    if (load_status != 0 || FT_Render_Glyph(impl_->face->glyph, FT_RENDER_MODE_NORMAL) != 0) {
      return Result<TextCoverageMask, Error>::failure(font_error(
          ErrorCode::unsupported_encoding,
          "glyph_rasterization_failed",
          {{"codepoint", std::to_string(codepoint)}, {"nativeCode", std::to_string(load_status)}}));
    }
    const auto& glyph = *impl_->face->glyph;
    const auto& bitmap = glyph.bitmap;
    if (bitmap.pixel_mode != FT_PIXEL_MODE_GRAY) {
      return Result<TextCoverageMask, Error>::failure(
          font_error(ErrorCode::unsupported_encoding, "non_grayscale_glyph"));
    }
    const auto glyph_left = pen_x + glyph.bitmap_left;
    const auto glyph_top = baseline - glyph.bitmap_top;
    for (unsigned int row = 0; row < bitmap.rows; ++row) {
      const auto target_y = glyph_top + static_cast<std::int32_t>(row);
      if (target_y < 0 || target_y >= request.mask_size_px.height) {
        continue;
      }
      const auto* source = bitmap.buffer +
          static_cast<std::ptrdiff_t>(row) * static_cast<std::ptrdiff_t>(bitmap.pitch);
      for (unsigned int column = 0; column < bitmap.width; ++column) {
        const auto target_x = glyph_left + static_cast<std::int32_t>(column);
        if (target_x < 0 || target_x >= request.mask_size_px.width) {
          continue;
        }
        auto& destination = coverage[
            static_cast<std::size_t>(target_y) * width + static_cast<std::size_t>(target_x)];
        destination = std::max(destination, source[column]);
      }
    }
    pen_x += static_cast<std::int32_t>(glyph.advance.x / 64);
    previous_glyph = glyph_index;
  }

  return Result<TextCoverageMask, Error>::success(TextCoverageMask{
      request.object_id,
      request.mask_size_px,
      std::move(coverage),
      impl_->font_identity,
  });
}

}  // namespace hdrshot
