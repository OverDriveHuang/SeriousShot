#pragma once
#import <Metal/Metal.h>
#include "platform/macos/macos_linear_storage.hpp"
#include <mach/mach_vm.h>
#include <unistd.h>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace hdrshot {
struct MacSourceBufferBinding {
  id<MTLBuffer> buffer{nil};
  bool aliases_source{};
  std::size_t offset{};
};
// Caller holds the immutable CPU owner until all submitted GPU work completes.
// Never transfer allocation ownership to Metal. Check every no-copy precondition;
// unsupported allocator layouts use an equally precise upload buffer.
inline MacSourceBufferBinding bind_macos_source_buffer(id<MTLDevice> device,
    const void* data, std::size_t bytes, std::size_t allocated_bytes) {
  if (!data || bytes == 0 || bytes > device.maxBufferLength) return {};
  const auto native = macos_linear_storage_binding(data, bytes);
  if (native.buffer && native.buffer.device.registryID == device.registryID)
    return {native.buffer, true, native.offset};
  const auto page = static_cast<std::size_t>(getpagesize());
  if (bytes > std::numeric_limits<std::size_t>::max() - page) return {};
  const auto aligned = (bytes + page - 1) / page * page;
  const auto pointer = reinterpret_cast<std::uintptr_t>(data);
  if (pointer % page == 0 && aligned <= allocated_bytes && aligned <= device.maxBufferLength) {
    mach_vm_address_t address = pointer;
    mach_vm_size_t region_size{};
    vm_region_basic_info_data_64_t info{};
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    const auto status = mach_vm_region(mach_task_self(), &address, &region_size,
        VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&info), &count, &object);
    if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
    if (status == KERN_SUCCESS && address <= pointer && pointer - address <= region_size &&
        aligned <= region_size - (pointer - address)) {
      auto buffer = [device newBufferWithBytesNoCopy:const_cast<void*>(data) length:aligned
          options:MTLResourceStorageModeShared deallocator:nil];
      if (buffer) return {buffer, true};
    }
  }
  return {[device newBufferWithBytes:data length:bytes options:MTLResourceStorageModeShared], false};
}
} // namespace hdrshot
