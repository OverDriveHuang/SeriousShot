#pragma once

#include "core/error.hpp"
#include "core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace hdrshot {

// Platform-neutral destination for incrementally produced bytes. Implementations
// may accept a partial write; callers must continue until the span is consumed.
class ByteSink {
 public:
  virtual ~ByteSink() = default;
  [[nodiscard]] virtual Result<std::size_t, Error> write(
      std::span<const std::uint8_t> bytes) = 0;
};

}  // namespace hdrshot
