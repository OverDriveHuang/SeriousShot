#pragma once

#include "core/error.hpp"
#include "core/geometry.hpp"
#include "core/linear_pixel_storage.hpp"
#include "core/result.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace hdrshot {
// Immutable FP32 Linear Display P3 resource. Native implementations may be
// pending on their GPU queue; consumers must establish GPU ordering and verify
// completion before publishing a successful frame/export. No OS types here.
class LinearSource {
 public:
  virtual ~LinearSource() = default;
  virtual PixelSize size_px() const = 0;
  virtual std::size_t byte_count() const = 0;
  // Explicit CPU boundary, never called between normalization and first draw.
  virtual Result<bool, Error> wait_until_ready() const = 0;
  virtual Result<LinearFloatPixels, Error> read_region(PixelRect rect) const = 0;
};
using LinearSourceRef = std::shared_ptr<const LinearSource>;

class SourceNormalizerPort {
 public:
  virtual ~SourceNormalizerPort() = default;
  // Only called after the shared interpreter validates non-linear P3/FP16.
  // Takes ownership of the transient input. Preserve exact FP32 inverse values.
  virtual Result<LinearSourceRef, Error> normalize_extended_p3(
      PixelSize size, std::vector<std::uint16_t> rgba_half) = 0;
};

class LinearSampleStorage {
 public:
  virtual ~LinearSampleStorage() = default;
  virtual std::size_t sample_count() const = 0;
  virtual Result<bool, Error> wait_until_ready() const = 0;
  virtual Result<std::vector<std::array<float, 4>>, Error> read_samples(
      std::size_t offset, std::size_t count) const = 0;
};
using LinearSampleRef = std::shared_ptr<const LinearSampleStorage>;
struct PositionedCleanSample {
  std::array<float, 4> annotation; // premultiplied RGB + source weight
  std::uint32_t x{}, y{};         // display-local physical pixel coordinates
};
} // namespace hdrshot
