#pragma once
#include <cstddef>
#include <memory>
#include "ports/linear_storage_port.hpp"
#include <limits>
#include <new>
#include <vector>
#include <type_traits>

namespace hdrshot {
// Portable aligned C++ storage; no device or OS object enters shared contracts.
// Native executors still validate their own page/VM requirements before aliasing.
template <typename T> struct AlignedPixelAllocator {
  using value_type = T;
  std::shared_ptr<LinearStoragePort> storage;
  AlignedPixelAllocator() noexcept = default;
  explicit AlignedPixelAllocator(std::shared_ptr<LinearStoragePort> port) : storage(std::move(port)) {}
  template <typename U> AlignedPixelAllocator(const AlignedPixelAllocator<U>& other) noexcept : storage(other.storage) {}
  [[nodiscard]] T* allocate(std::size_t count) {
    if (count > std::numeric_limits<std::size_t>::max() / sizeof(T)) throw std::bad_array_new_length();
    if (storage) return static_cast<T*>(storage->allocate(count * sizeof(T), 65536));
    return static_cast<T*>(::operator new(count * sizeof(T), std::align_val_t{65536}));
  }
  void deallocate(T* pointer, std::size_t count) noexcept {
    if (storage) { storage->deallocate(pointer, count * sizeof(T), 65536); return; }
    ::operator delete(pointer, std::align_val_t{65536});
  }
  template <typename U> bool operator==(const AlignedPixelAllocator<U>& other) const noexcept { return storage == other.storage; }
  using propagate_on_container_move_assignment = std::true_type;
};
using LinearFloatPixels = std::vector<float, AlignedPixelAllocator<float>>;
} // namespace hdrshot
