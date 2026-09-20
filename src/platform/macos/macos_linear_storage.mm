#include "platform/macos/macos_linear_storage.hpp"
#import <Metal/Metal.h>
#include <map>
#include <mutex>
#include <limits>
#include <new>
namespace hdrshot {
namespace {
class MacLinearStorage final : public LinearStoragePort {
 public:
  void* allocate(std::size_t bytes, std::size_t alignment) override {
    if (bytes > std::numeric_limits<std::size_t>::max() - alignment) throw std::bad_alloc();
    std::scoped_lock lock(mutex_);
    if (!device_) device_ = MTLCreateSystemDefaultDevice();
    if (!device_) return ::operator new(bytes, std::align_val_t{alignment});
    if (bytes + alignment > device_.maxBufferLength) throw std::bad_alloc();
    id<MTLBuffer> buffer = [device_ newBufferWithLength:bytes + alignment options:MTLResourceStorageModeShared];
    if (!buffer) throw std::bad_alloc();
    const auto base = reinterpret_cast<std::uintptr_t>(buffer.contents);
    const auto aligned = (base + alignment - 1) / alignment * alignment;
    void* pointer = reinterpret_cast<void*>(aligned);
    allocations_.emplace(pointer, MacLinearStorageBinding{buffer, aligned - base});
    return pointer;
  }
  void deallocate(void* pointer, std::size_t, std::size_t alignment) noexcept override {
    std::scoped_lock lock(mutex_);
    if (allocations_.erase(pointer) == 0) ::operator delete(pointer, std::align_val_t{alignment});
  }
  MacLinearStorageBinding find(const void* pointer, std::size_t bytes) {
    std::scoped_lock lock(mutex_);
    const auto found = allocations_.find(pointer);
    if (found == allocations_.end() || bytes > found->second.buffer.length - found->second.offset) return {};
    return found->second;
  }
 private:
  id<MTLDevice> device_{nil};
  std::mutex mutex_;
  std::map<const void*, MacLinearStorageBinding> allocations_;
};
std::shared_ptr<MacLinearStorage> storage() {
  static const auto instance = std::make_shared<MacLinearStorage>();
  return instance;
}
}
std::shared_ptr<LinearStoragePort> macos_linear_storage() { return storage(); }
MacLinearStorageBinding macos_linear_storage_binding(const void* pointer, std::size_t bytes) {
  return storage()->find(pointer, bytes);
}
} // namespace hdrshot
