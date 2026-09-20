#pragma once
#include "ports/linear_storage_port.hpp"
#include <memory>
#ifdef __OBJC__
#import <Metal/Metal.h>
#endif
namespace hdrshot {
// CPU-addressable native shared storage. Lifetime stays with the portable allocator.
std::shared_ptr<LinearStoragePort> macos_linear_storage();
#ifdef __OBJC__
struct MacLinearStorageBinding { id<MTLBuffer> buffer{nil}; std::size_t offset{}; };
MacLinearStorageBinding macos_linear_storage_binding(const void* pointer, std::size_t bytes);
#endif
} // namespace hdrshot
