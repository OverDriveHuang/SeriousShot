#pragma once

#include "core/linear_source.hpp"

#ifdef __OBJC__
#import <Metal/Metal.h>
#endif

namespace hdrshot {

// All native source consumers use this runtime's device/queue. A source is
// published only after its producer command has been submitted to that queue.
std::shared_ptr<SourceNormalizerPort> macos_source_normalizer();

Result<LinearSampleRef, Error> macos_compose_clean(
    LinearSourceRef source, std::span<const PositionedCleanSample> samples);

#ifdef __OBJC__
id<MTLDevice> macos_gpu_device();
id<MTLCommandQueue> macos_gpu_queue();
// Borrowed native bindings; callers retain the source/sample owner until their
// commands have been encoded. Default Metal command buffers retain resources.
id<MTLTexture> macos_source_texture(const LinearSourceRef& source);
id<MTLBuffer> macos_sample_buffer(const LinearSampleRef& samples);
// Publish a newly encoded private RGBA32F texture using the existing source
// implementation, so preview/export/read_region keep one native binding path.
// The command belongs to macos_gpu_queue and must not be committed yet; this
// function installs completion/error propagation and commits it exactly once.
Result<LinearSourceRef, Error> macos_wrap_linear_texture(
    id<MTLTexture> texture, id<MTLCommandBuffer> command,
    std::vector<LinearSourceRef> dependencies = {},
    std::vector<LinearSampleRef> sample_dependencies = {});
#endif

}  // namespace hdrshot
