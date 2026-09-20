#include "platform/macos/macos_gpu_source.hpp"

#include "domain/color/extended_p3_mapper.hpp"
#include "platform/macos/macos_linear_storage.hpp"

#import <Foundation/Foundation.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <source_location>
#include <string>
#include <utility>

namespace hdrshot {
namespace {

Error source_error(ErrorCode code, const char* reason,
    std::source_location origin = std::source_location::current()) {
  return {code, "MacGpuSource",
      code == ErrorCode::presenter_failed ? Retryability::after_recreate : Retryability::never,
      {{"reason", reason}}, origin};
}

Error native_error(const char* reason, NSError* error,
    std::source_location origin = std::source_location::current()) {
  auto result = source_error(ErrorCode::presenter_failed, reason, origin);
  if (error != nil) {
    result.safe_context["nativeCode"] = std::to_string(error.code);
    const char* description = error.localizedDescription.UTF8String;
    if (description) result.safe_context["nativeDescription"] = description;
  }
  return result;
}

// No command buffer or input owner is held here. In particular, a source must
// not keep the normalization command alive after its final GPU resource use.
class CompletionState {
 public:
  void finish(std::optional<Error> error = {}) {
    {
      std::scoped_lock lock(mutex_);
      if (complete_) return;
      error_ = std::move(error);
      complete_ = true;
    }
    completed_.notify_all();
  }

  Result<bool, Error> wait() const {
    std::unique_lock lock(mutex_);
    completed_.wait(lock, [this] { return complete_; });
    return error_ ? Result<bool, Error>::failure(*error_)
                  : Result<bool, Error>::success(true);
  }

 private:
  mutable std::mutex mutex_;
  mutable std::condition_variable completed_;
  bool complete_{};
  std::optional<Error> error_;
};

constexpr const char* kSourceShader = R"METAL(
#include <metal_stdlib>
using namespace metal;

kernel void normalize_extended_p3(
    device const ushort4* input [[buffer(0)]],
    device const float* inverse [[buffer(1)]],
    device atomic_uint* invalid [[buffer(2)]],
    texture2d<float, access::write> output [[texture(0)]],
    uint2 p [[thread_position_in_grid]]) {
  if (p.x >= output.get_width() || p.y >= output.get_height()) return;
  const ushort4 bits = input[p.y * output.get_width() + p.x];
  // The capture contract ignores alpha, including nonfinite alpha. RGB is
  // inspected by its raw exponent so fast-math cannot remove the validity gate.
  if (any((bits.rgb & ushort3(0x7c00)) == ushort3(0x7c00))) {
    atomic_store_explicit(invalid, 1u, memory_order_relaxed);
    output.write(float4(0.0f, 0.0f, 0.0f, 1.0f), p);
    return;
  }
  output.write(float4(inverse[bits.r], inverse[bits.g], inverse[bits.b], 1.0f), p);
}

// Matches the portable PositionedCleanSample's 24-byte layout, without
// Metal float4's native 16-byte alignment padding the struct to 32 bytes.
struct PositionedSample {
  packed_float4 annotation;
  uint x;
  uint y;
};

kernel void compose_positioned_clean(
    texture2d<float, access::read> source [[texture(0)]],
    device const PositionedSample* samples [[buffer(0)]],
    device float4* output [[buffer(1)]],
    device atomic_uint* invalid [[buffer(2)]],
    constant uint& count [[buffer(3)]],
    uint index [[thread_position_in_grid]]) {
  if (index >= count) return;
  const PositionedSample sample = samples[index];
  const float4 ink = float4(sample.annotation);
  float3 value = ink.rgb;
  if (ink.w > 0.0f) {
    value += ink.w * max(source.read(uint2(sample.x, sample.y)).rgb, 0.0f);
  }
  const uint3 exponent = as_type<uint3>(value) & uint3(0x7f800000);
  if (any(exponent == uint3(0x7f800000)) || any(value < 0.0f)) {
    atomic_store_explicit(invalid, 1u, memory_order_relaxed);
    value = float3(0.0f);
  }
  output[index] = float4(value, 0.0f);
}

kernel void read_linear_region(
    texture2d<float, access::read> source [[texture(0)]],
    device float4* output [[buffer(0)]],
    constant uint4& rect [[buffer(1)]],
    uint2 p [[thread_position_in_grid]]) {
  if (p.x >= rect.z || p.y >= rect.w) return;
  output[p.y * rect.z + p.x] = source.read(p + rect.xy);
}
)METAL";

static_assert(sizeof(PositionedCleanSample) == 24);
static_assert(offsetof(PositionedCleanSample, x) == 16);
static_assert(offsetof(PositionedCleanSample, y) == 20);

struct Runtime {
  id<MTLDevice> device{nil};
  id<MTLCommandQueue> queue{nil};
  id<MTLBuffer> inverse{nil};
  id<MTLComputePipelineState> normalize{nil};
  id<MTLComputePipelineState> compose{nil};
  id<MTLComputePipelineState> read_region{nil};
  std::optional<Error> error;

  Runtime() {
    @autoreleasepool {
      device = MTLCreateSystemDefaultDevice();
      if (!device) {
        error = source_error(ErrorCode::presenter_failed, "metal_device_unavailable");
        return;
      }
      queue = [device newCommandQueue];
      if (!queue) {
        error = source_error(ErrorCode::presenter_failed, "source_queue_creation_failed");
        return;
      }
      queue.label = @"SeriousShot shared source and consumers";
      NSError* native = nil;
      id<MTLLibrary> library = [device newLibraryWithSource:
          [NSString stringWithUTF8String:kSourceShader] options:nil error:&native];
      if (!library) {
        error = native_error("source_shader_compilation_failed", native);
        return;
      }
      const auto pipeline = [&](NSString* name) -> id<MTLComputePipelineState> {
        NSError* failure = nil;
        id<MTLFunction> function = [library newFunctionWithName:name];
        if (!function) {
          error = source_error(ErrorCode::presenter_failed, "source_function_unavailable");
          return nil;
        }
        auto value = [device newComputePipelineStateWithFunction:function error:&failure];
        if (!value) error = native_error("source_pipeline_creation_failed", failure);
        return value;
      };
      normalize = pipeline(@"normalize_extended_p3");
      if (!normalize) return;
      compose = pipeline(@"compose_positioned_clean");
      if (!compose) return;
      read_region = pipeline(@"read_linear_region");
      if (!read_region) return;

      std::array<float, 65536> lut{};
      for (std::size_t code = 0; code < lut.size(); ++code) {
        const auto value = ExtendedP3Mapper::decode_binary16(static_cast<std::uint16_t>(code));
        lut[code] = value ? ExtendedP3Mapper::inverse_extended_srgb(value.value())
                         : std::numeric_limits<float>::quiet_NaN();
      }
      inverse = [device newBufferWithBytes:lut.data() length:sizeof(lut)
          options:MTLResourceStorageModeShared];
      if (!inverse) error = source_error(ErrorCode::presenter_failed, "inverse_lut_allocation_failed");
    }
  }
};

std::shared_ptr<Runtime> runtime() {
  // Function-local static initialization is thread safe. The device, queue,
  // kernels and exact inverse table are shared across captures and displays.
  static const auto value = std::make_shared<Runtime>();
  return value;
}

std::optional<std::size_t> pixel_count(PixelSize size) {
  if (size.width <= 0 || size.height <= 0 || size.width > 16384 || size.height > 16384) return {};
  const auto width = static_cast<std::size_t>(size.width);
  const auto height = static_cast<std::size_t>(size.height);
  if (width > std::numeric_limits<std::size_t>::max() / height / 16U ||
      width * height > std::numeric_limits<std::uint32_t>::max()) return {};
  return width * height;
}

void dispatch_image(id<MTLComputeCommandEncoder> encoder,
    id<MTLComputePipelineState> pipeline, PixelSize size) {
  const auto width = pipeline.threadExecutionWidth;
  const auto height = std::max<NSUInteger>(1, std::min<NSUInteger>(8,
      pipeline.maxTotalThreadsPerThreadgroup / width));
  [encoder dispatchThreads:MTLSizeMake(size.width, size.height, 1)
      threadsPerThreadgroup:MTLSizeMake(width, height, 1)];
}

void record_completion(id<MTLCommandBuffer> command,
    const std::shared_ptr<CompletionState>& state, id<MTLBuffer> invalid,
    const char* command_reason, const char* invalid_reason) {
  // Capture an owned C++ value, never the helper's reference parameter.
  const auto state_owner = state;
  [command addCompletedHandler:^(id<MTLCommandBuffer> finished) {
    @autoreleasepool {
      if (finished.status != MTLCommandBufferStatusCompleted) {
        state_owner->finish(native_error(command_reason, finished.error));
      } else if (invalid && *static_cast<const std::uint32_t*>(invalid.contents) != 0U) {
        state_owner->finish(source_error(ErrorCode::invalid_color_contract, invalid_reason));
      } else {
        state_owner->finish();
      }
    }
  }];
}

class MacLinearSource final : public LinearSource {
 public:
  MacLinearSource(PixelSize size, id<MTLTexture> texture,
      std::shared_ptr<CompletionState> ready)
      : size_(size), texture_(texture), ready_(std::move(ready)) {}

  PixelSize size_px() const override { return size_; }
  std::size_t byte_count() const override {
    return static_cast<std::size_t>(size_.width) * static_cast<std::size_t>(size_.height) * 16U;
  }
  Result<bool, Error> wait_until_ready() const override { return ready_->wait(); }
  id<MTLTexture> texture() const { return texture_; }

  Result<LinearFloatPixels, Error> read_region(PixelRect rect) const override {
    using Output = Result<LinearFloatPixels, Error>;
    if (rect.x < 0 || rect.y < 0 || rect.width <= 0 || rect.height <= 0 ||
        static_cast<std::int64_t>(rect.x) + rect.width > size_.width ||
        static_cast<std::int64_t>(rect.y) + rect.height > size_.height) {
      return Output::failure(source_error(ErrorCode::invalid_input, "source_read_region_out_of_bounds"));
    }
    auto available = wait_until_ready();
    if (!available) return Output::failure(available.error());
    try {
      @autoreleasepool {
        const auto gpu = runtime();
        if (gpu->error) return Output::failure(*gpu->error);
        const auto count = static_cast<std::size_t>(rect.width) * rect.height * 4U;
        // Allocate the returned object itself in CPU-readable Metal storage.
        // No second full ROI readback buffer or conversion vector is created.
        LinearFloatPixels result{AlignedPixelAllocator<float>{macos_linear_storage()}};
        result.resize(count);
        const auto binding = macos_linear_storage_binding(result.data(), count * sizeof(float));
        if (!binding.buffer || binding.buffer.device != gpu->device) {
          return Output::failure(source_error(ErrorCode::presenter_failed, "roi_readback_binding_failed"));
        }
        id<MTLCommandBuffer> command = [gpu->queue commandBuffer];
        if (!command) return Output::failure(source_error(ErrorCode::presenter_failed, "roi_command_failed"));
        id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
        if (!encoder) return Output::failure(source_error(ErrorCode::presenter_failed, "roi_encoder_failed"));
        const std::array<std::uint32_t, 4> region{
            static_cast<std::uint32_t>(rect.x), static_cast<std::uint32_t>(rect.y),
            static_cast<std::uint32_t>(rect.width), static_cast<std::uint32_t>(rect.height)};
        [encoder setComputePipelineState:gpu->read_region];
        [encoder setTexture:texture_ atIndex:0];
        [encoder setBuffer:binding.buffer offset:binding.offset atIndex:0];
        [encoder setBytes:region.data() length:sizeof(region) atIndex:1];
        dispatch_image(encoder, gpu->read_region, {rect.width, rect.height});
        [encoder endEncoding];
        [command commit];
        [command waitUntilCompleted];
        if (command.status != MTLCommandBufferStatusCompleted) {
          return Output::failure(native_error("roi_readback_execution_failed", command.error));
        }
        return Output::success(std::move(result));
      }
    } catch (const std::bad_alloc&) {
      return Output::failure(source_error(ErrorCode::presenter_failed, "roi_readback_allocation_failed"));
    }
  }

 private:
  PixelSize size_{};
  id<MTLTexture> texture_{nil};
  std::shared_ptr<CompletionState> ready_;
};

class MacLinearSamples final : public LinearSampleStorage {
 public:
  MacLinearSamples(LinearSourceRef source, std::size_t count, id<MTLBuffer> buffer,
      std::shared_ptr<CompletionState> ready)
      : source_(std::move(source)), count_(count), buffer_(buffer), ready_(std::move(ready)) {}

  std::size_t sample_count() const override { return count_; }
  id<MTLBuffer> buffer() const { return buffer_; }
  Result<bool, Error> wait_until_ready() const override {
    // Preserve the producer failure even if the later GPU command can execute.
    const auto source_ready = source_->wait_until_ready();
    if (!source_ready) return source_ready;
    return ready_->wait();
  }
  Result<std::vector<std::array<float, 4>>, Error> read_samples(
      std::size_t offset, std::size_t count) const override {
    using Output = Result<std::vector<std::array<float, 4>>, Error>;
    if (offset > count_ || count > count_ - offset) {
      return Output::failure(source_error(ErrorCode::invalid_input, "clean_read_out_of_bounds"));
    }
    const auto ready = wait_until_ready();
    if (!ready) return Output::failure(ready.error());
    try {
      std::vector<std::array<float, 4>> result(count);
      if (count) std::memcpy(result.data(),
          static_cast<const std::array<float, 4>*>(buffer_.contents) + offset,
          count * sizeof(std::array<float, 4>));
      return Output::success(std::move(result));
    } catch (const std::bad_alloc&) {
      return Output::failure(source_error(ErrorCode::presenter_failed, "clean_read_allocation_failed"));
    }
  }

 private:
  LinearSourceRef source_;
  std::size_t count_{};
  id<MTLBuffer> buffer_{nil};
  std::shared_ptr<CompletionState> ready_;
};

class MacSourceNormalizer final : public SourceNormalizerPort {
 public:
  Result<LinearSourceRef, Error> normalize_extended_p3(
      PixelSize size, std::vector<std::uint16_t> rgba_half) override {
    using Output = Result<LinearSourceRef, Error>;
    const auto count = pixel_count(size);
    if (!count || rgba_half.size() != *count * 4U) {
      return Output::failure(source_error(ErrorCode::invalid_input, "source_normalization_shape_mismatch"));
    }
    try {
      @autoreleasepool {
        const auto gpu = runtime();
        if (gpu->error) return Output::failure(*gpu->error);
        if (rgba_half.size() > gpu->device.maxBufferLength / sizeof(std::uint16_t)) {
          return Output::failure(source_error(ErrorCode::presenter_failed, "source_input_too_large"));
        }
        const auto ready = std::make_shared<CompletionState>();
        auto descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
            width:size.width height:size.height mipmapped:NO];
        descriptor.storageMode = MTLStorageModePrivate;
        descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        descriptor.hazardTrackingMode = MTLHazardTrackingModeTracked;
        id<MTLTexture> texture = [gpu->device newTextureWithDescriptor:descriptor];
        if (!texture) return Output::failure(source_error(ErrorCode::presenter_failed, "linear_texture_allocation_failed"));
        texture.label = @"SeriousShot immutable FP32 Linear P3 source";
        auto result = std::make_shared<MacLinearSource>(size, texture, ready);
        {
          @autoreleasepool {
            id<MTLBuffer> input = [gpu->device newBufferWithBytes:rgba_half.data()
                length:rgba_half.size() * sizeof(std::uint16_t) options:MTLResourceStorageModeShared];
            // newBufferWithBytes owns a copy. Release the capture owner now,
            // before the GPU writes the larger authoritative FP32 source.
            std::vector<std::uint16_t>().swap(rgba_half);
            id<MTLBuffer> invalid = [gpu->device newBufferWithLength:sizeof(std::uint32_t)
                options:MTLResourceStorageModeShared];
            if (!input || !invalid) return Output::failure(source_error(ErrorCode::presenter_failed, "source_staging_allocation_failed"));
            *static_cast<std::uint32_t*>(invalid.contents) = 0U;
            id<MTLCommandBuffer> command = [gpu->queue commandBuffer];
            if (!command) return Output::failure(source_error(ErrorCode::presenter_failed, "source_command_failed"));
            command.label = @"SeriousShot one-time source normalization";
            id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
            if (!encoder) return Output::failure(source_error(ErrorCode::presenter_failed, "source_encoder_failed"));
            [encoder setComputePipelineState:gpu->normalize];
            [encoder setBuffer:input offset:0 atIndex:0];
            [encoder setBuffer:gpu->inverse offset:0 atIndex:1];
            [encoder setBuffer:invalid offset:0 atIndex:2];
            [encoder setTexture:texture atIndex:0];
            dispatch_image(encoder, gpu->normalize, size);
            [encoder endEncoding];
            record_completion(command, ready, invalid,
                "source_normalization_execution_failed", "non_finite_source_component");
            [command commit];
          }
        }
        // The source is already enqueued before any consumer can see it. Metal
        // retains transient inputs to their last GPU use, not to the first draw.
        return Output::success(std::move(result));
      }
    } catch (const std::bad_alloc&) {
      return Output::failure(source_error(ErrorCode::presenter_failed, "source_allocation_failed"));
    }
  }
};

}  // namespace

std::shared_ptr<SourceNormalizerPort> macos_source_normalizer() {
  // Prewarm when called from the composition root; no per-capture shader build.
  (void)runtime();
  static const auto normalizer = std::make_shared<MacSourceNormalizer>();
  return normalizer;
}

id<MTLDevice> macos_gpu_device() {
  const auto gpu = runtime();
  return gpu->error ? nil : gpu->device;
}

id<MTLCommandQueue> macos_gpu_queue() {
  const auto gpu = runtime();
  return gpu->error ? nil : gpu->queue;
}

id<MTLTexture> macos_source_texture(const LinearSourceRef& source) {
  const auto* native = dynamic_cast<const MacLinearSource*>(source.get());
  return native ? native->texture() : nil;
}

id<MTLBuffer> macos_sample_buffer(const LinearSampleRef& samples) {
  const auto* native = dynamic_cast<const MacLinearSamples*>(samples.get());
  return native ? native->buffer() : nil;
}

Result<LinearSourceRef, Error> macos_wrap_linear_texture(
    id<MTLTexture> texture, id<MTLCommandBuffer> command,
    std::vector<LinearSourceRef> dependencies,
    std::vector<LinearSampleRef> sample_dependencies) {
  using Output = Result<LinearSourceRef, Error>;
  const auto gpu = runtime();
  if (gpu->error) return Output::failure(*gpu->error);
  if (!texture || texture.device != gpu->device ||
      texture.pixelFormat != MTLPixelFormatRGBA32Float ||
      texture.storageMode != MTLStorageModePrivate || texture.depth != 1 ||
      !command || command.commandQueue != gpu->queue ||
      command.status != MTLCommandBufferStatusNotEnqueued) {
    return Output::failure(source_error(ErrorCode::invalid_input, "invalid_wrapped_linear_texture"));
  }
  const PixelSize size{static_cast<int>(texture.width), static_cast<int>(texture.height)};
  if (!pixel_count(size)) return Output::failure(source_error(ErrorCode::invalid_input, "invalid_wrapped_texture_size"));
  const auto ready = std::make_shared<CompletionState>();
  auto result = std::make_shared<MacLinearSource>(size, texture, ready);
  [command addCompletedHandler:^(id<MTLCommandBuffer> finished) {
    if (finished.status != MTLCommandBufferStatusCompleted) {
      ready->finish(native_error("wrapped_texture_execution_failed", finished.error));
      return;
    }
    // Never wait for another completion handler on Metal's completion delivery
    // thread. Producers may finish GPU work before their CPU callbacks run.
    if (dependencies.empty() && sample_dependencies.empty()) { ready->finish(); return; }
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
      for (const auto& dependency : dependencies) {
        auto status = dependency->wait_until_ready();
        if (!status) { ready->finish(status.error()); return; }
      }
      for (const auto& dependency : sample_dependencies) {
        auto status = dependency->wait_until_ready();
        if (!status) { ready->finish(status.error()); return; }
      }
      ready->finish();
    });
  }];
  [command commit];
  return Output::success(std::move(result));
}

Result<LinearSampleRef, Error> macos_compose_clean(
    LinearSourceRef source, std::span<const PositionedCleanSample> samples) {
  using Output = Result<LinearSampleRef, Error>;
  const auto* native = dynamic_cast<const MacLinearSource*>(source.get());
  if (!native) return Output::failure(source_error(ErrorCode::invalid_input, "unsupported_native_linear_source"));
  const auto size = source->size_px();
  if (samples.size() > std::numeric_limits<std::uint32_t>::max()) {
    return Output::failure(source_error(ErrorCode::invalid_input, "clean_sample_count_overflow"));
  }
  for (const auto& sample : samples) {
    if (sample.x >= static_cast<std::uint32_t>(size.width) ||
        sample.y >= static_cast<std::uint32_t>(size.height)) {
      return Output::failure(source_error(ErrorCode::invalid_input, "clean_sample_out_of_bounds"));
    }
    for (float component : sample.annotation) {
      if (!std::isfinite(component) || component < 0.0F) {
        return Output::failure(source_error(ErrorCode::invalid_color_contract, "invalid_clean_annotation"));
      }
    }
    if (sample.annotation[3] > 1.0F) {
      return Output::failure(source_error(ErrorCode::invalid_color_contract, "invalid_clean_source_weight"));
    }
  }
  try {
    @autoreleasepool {
      const auto gpu = runtime();
      if (gpu->error) return Output::failure(*gpu->error);
      if (samples.size() > gpu->device.maxBufferLength / sizeof(PositionedCleanSample)) {
        return Output::failure(source_error(ErrorCode::presenter_failed, "clean_samples_too_large"));
      }
      const auto ready = std::make_shared<CompletionState>();
      id<MTLBuffer> output = [gpu->device newBufferWithLength:
          std::max<std::size_t>(16U, samples.size() * 16U) options:MTLResourceStorageModeShared];
      if (!output) return Output::failure(source_error(ErrorCode::presenter_failed, "clean_output_allocation_failed"));
      output.label = @"SeriousShot immutable sparse clean samples";
      auto result = std::make_shared<MacLinearSamples>(source, samples.size(), output, ready);
      if (samples.empty()) {
        std::memset(output.contents, 0, 16);
        ready->finish();
        return Output::success(std::move(result));
      }
      {
        @autoreleasepool {
          id<MTLBuffer> input = [gpu->device newBufferWithBytes:samples.data()
              length:samples.size_bytes() options:MTLResourceStorageModeShared];
          id<MTLBuffer> invalid = [gpu->device newBufferWithLength:sizeof(std::uint32_t)
              options:MTLResourceStorageModeShared];
          if (!input || !invalid) return Output::failure(source_error(ErrorCode::presenter_failed, "clean_staging_allocation_failed"));
          *static_cast<std::uint32_t*>(invalid.contents) = 0U;
          id<MTLCommandBuffer> command = [gpu->queue commandBuffer];
          if (!command) return Output::failure(source_error(ErrorCode::presenter_failed, "clean_command_failed"));
          command.label = @"SeriousShot sparse clean composition";
          id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
          if (!encoder) return Output::failure(source_error(ErrorCode::presenter_failed, "clean_encoder_failed"));
          const auto count = static_cast<std::uint32_t>(samples.size());
          [encoder setComputePipelineState:gpu->compose];
          [encoder setTexture:native->texture() atIndex:0];
          [encoder setBuffer:input offset:0 atIndex:0];
          [encoder setBuffer:output offset:0 atIndex:1];
          [encoder setBuffer:invalid offset:0 atIndex:2];
          [encoder setBytes:&count length:sizeof(count) atIndex:3];
          [encoder dispatchThreads:MTLSizeMake(samples.size(), 1, 1)
              threadsPerThreadgroup:MTLSizeMake(gpu->compose.threadExecutionWidth, 1, 1)];
          [encoder endEncoding];
          record_completion(command, ready, invalid,
              "clean_composition_execution_failed", "non_finite_clean_component");
          [command commit];
        }
      }
      return Output::success(std::move(result));
    }
  } catch (const std::bad_alloc&) {
    return Output::failure(source_error(ErrorCode::presenter_failed, "clean_allocation_failed"));
  }
}

}  // namespace hdrshot
