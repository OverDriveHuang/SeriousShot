#pragma once
#include <cstddef>
namespace hdrshot {
// Optional native allocation executor. Standard aligned allocation is the CPU
// fallback. Ownership travels with the vector allocator, including detached jobs.
class LinearStoragePort {
 public:
  virtual ~LinearStoragePort() = default;
  virtual void* allocate(std::size_t bytes, std::size_t alignment) = 0;
  virtual void deallocate(void* pointer, std::size_t bytes, std::size_t alignment) noexcept = 0;
};
} // namespace hdrshot
