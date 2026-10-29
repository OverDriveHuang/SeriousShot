#pragma once
#include "core/error.hpp"
#include "core/geometry.hpp"
#include "core/result.hpp"
#include <cmath>
#include <limits>
namespace hdrshot {
// Reject unrepresentable endpoints before any PixelRect int32 arithmetic.
inline bool valid_annotation_clip(PixelRect clip, bool allow_empty = false) {
  if (clip.width < 0 || clip.height < 0 || (!allow_empty && clip.empty()))
    return false;
  const auto right = std::int64_t(clip.x) + clip.width;
  const auto bottom = std::int64_t(clip.y) + clip.height;
  return right <= std::numeric_limits<std::int32_t>::max() &&
         bottom <= std::numeric_limits<std::int32_t>::max();
}
class AnnotationScale {
public:
  static Result<AnnotationScale, Error> from(double value) {
    if (!std::isfinite(value) || value <= 0)
      return Result<AnnotationScale, Error>::failure(
          {ErrorCode::invalid_input, "AnnotationScale"});
    return Result<AnnotationScale, Error>::success(AnnotationScale(value));
  }
  double value() const { return value_; }
  friend bool operator==(const AnnotationScale &,
                         const AnnotationScale &) = default;

private:
  explicit AnnotationScale(double value) : value_(value) {}
  double value_;
};
inline Result<double, Error> to_capture_px(double dip, AnnotationScale scale) {
  const auto value = dip * scale.value();
  if (!std::isfinite(dip) || dip < 0 || !std::isfinite(value))
    return Result<double, Error>::failure(
        {ErrorCode::invalid_input, "AnnotationUnits"});
  return Result<double, Error>::success(value);
}
} // namespace hdrshot
