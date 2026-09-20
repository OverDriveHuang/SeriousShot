#include "platform/macos/macos_metal_export_pixel_processor.hpp"

#include "domain/color/extended_p3_mapper.hpp"
#include "platform/macos/macos_shared_source_buffer.hpp"
#include "platform/macos/macos_gpu_source.hpp"
#include "domain/annotation/annotation_compositing.hpp"
#include "domain/color/pq_reference_white_mapper.hpp"

#import <Metal/Metal.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <utility>

namespace hdrshot {
namespace {

Error processor_error(
    const ErrorCode code,
    const char* reason,
    std::map<std::string, std::string> context = {},
    std::source_location origin = std::source_location::current()) {
  context["reason"] = reason;
  const auto retryability = code == ErrorCode::presenter_failed
      ? Retryability::after_recreate
      : Retryability::never;
  return Error{code, "MacMetalExportPixelProcessor", retryability,
               std::move(context), origin};
}

std::string ns_string_to_utf8(NSString* value) {
  return value == nil ? std::string{} : std::string{value.UTF8String};
}

constexpr const char* kMetalSource = R"METAL(
#include <metal_stdlib>
using namespace metal;

struct SparseSample { float4 annotation; float4 source; };
kernel void compose_clean(device const SparseSample* input [[buffer(0)]],
    device float4* output [[buffer(1)]], uint gid [[thread_position_in_grid]]) {
  const SparseSample s = input[gid];
  output[gid] = float4(s.annotation.rgb + s.annotation.w * max(s.source.rgb, 0.0f), 0.0f);
}

struct Parameters {
  uint width;
  uint height;
  uint encode_pq;
  float diffuse_white_nits;
  uint pq_max_code;
  uint collect_content_light;
  uint source_is_linear;
  uint source_is_float32;
  uint source_offset_pixels;
  uint source_stride_pixels;
};

float3 read_source(texture2d<float, access::read> source, device const float4* linear_source,
    constant Parameters& params, uint2 position) {
  if (params.source_is_float32 == 2u) {
    const uint pixel = params.source_offset_pixels + position.y * params.source_stride_pixels + position.x;
    return source.read(uint2(pixel % params.source_stride_pixels, pixel / params.source_stride_pixels)).rgb;
  }
  return params.source_is_float32 == 1u
      ? linear_source[params.source_offset_pixels + position.y * params.source_stride_pixels + position.x].rgb
      : source.read(position).rgb;
}

float inverse_extended_srgb(float encoded) {
  const float magnitude = abs(encoded);
  const float linear = magnitude <= 0.04045f
      ? magnitude / 12.92f
      : pow((magnitude + 0.055f) / 1.055f, 2.4f);
  return encoded < 0.0f ? -linear : linear;
}

float3 source_linear(float3 sample, constant Parameters& params) {
  if (params.source_is_linear != 0u) return max(sample, 0.0f);
  return max(float3(inverse_extended_srgb(sample.r),
      inverse_extended_srgb(sample.g), inverse_extended_srgb(sample.b)), 0.0f);
}

float encode_extended_srgb(float linear) {
  const float magnitude = abs(linear);
  const float encoded = magnitude <= 0.0031308f
      ? 12.92f * magnitude
      : 1.055f * pow(magnitude, 1.0f / 2.4f) - 0.055f;
  return linear < 0.0f ? -encoded : encoded;
}

float st2084_oetf(float nits) {
  constexpr float m1 = 2610.0f / 16384.0f;
  constexpr float m2 = 2523.0f / 32.0f;
  constexpr float c1 = 3424.0f / 4096.0f;
  constexpr float c2 = 2413.0f / 128.0f;
  constexpr float c3 = 2392.0f / 128.0f;
  const float normalized = clamp(nits, 0.0f, 10000.0f) / 10000.0f;
  const float p = pow(normalized, m1);
  return pow((c1 + c2 * p) / (1.0f + c3 * p), m2);
}

kernel void extended_p3_to_png_rgb16(
    texture2d<float, access::read> source [[texture(0)]],
    device const uint* annotation_mask [[buffer(0)]],
    device const float4* annotation_colors [[buffer(1)]],
    device ushort* output_rgb [[buffer(2)]],
    device atomic_uint* counters [[buffer(3)]],
    constant Parameters& params [[buffer(4)]],
    device const float4* linear_source [[buffer(5)]],
    uint3 grid_position [[thread_position_in_grid]],
    uint3 thread_position [[thread_position_in_threadgroup]],
    uint3 threads_in_group [[threads_per_threadgroup]]) {
  const uint gid = grid_position.x;
  const uint thread_index = thread_position.x;
  const uint pixel_count = params.width * params.height;
  if (gid >= pixel_count) {
    return;
  }
  const uint2 position = uint2(gid % params.width, gid / params.width);
  const uint output = gid * 3u;
  const uint annotation_index = annotation_mask[gid];
  float3 linear = float3(0.0f);
  float3 encoded = float3(0.0f);
  if (annotation_index == 0u) {
    encoded = read_source(source, linear_source, params, position);
    if (!all(isfinite(encoded))) {
      atomic_fetch_add_explicit(&counters[0], 1u, memory_order_relaxed);
      encoded = float3(0.0f);
    }
    linear = source_linear(encoded, params);
  } else {
    const float4 sample = annotation_colors[annotation_index - 1u];
    linear = sample.rgb;
    if (sample.w > 0.0f) {
      const float3 underlying = read_source(source, linear_source, params, position);
      if (!all(isfinite(underlying))) {
        atomic_fetch_add_explicit(&counters[0], 1u, memory_order_relaxed);
      } else {
        linear += sample.w * source_linear(underlying, params);
      }
    }
  }
  // Linear source needs OETF only for SDR; HDR skips this unnecessary curve.
  if (params.encode_pq == 0u && (params.source_is_linear != 0u || annotation_index != 0u))
    encoded = float3(encode_extended_srgb(linear.r), encode_extended_srgb(linear.g),
                     encode_extended_srgb(linear.b));
  bool pixel_clipped = false;
  for (uint channel = 0u; channel < 3u; ++channel) {
    float value = clamp(encoded[channel], 0.0f, 1.0f);
    if (params.encode_pq != 0u) {
      const float nits = linear[channel] * params.diffuse_white_nits;
      if (nits > 10000.0f) {
        atomic_fetch_add_explicit(&counters[1], 1u, memory_order_relaxed);
        pixel_clipped = true;
      }
      value = st2084_oetf(nits);
    }
    const uint effective_code = uint(floor(
        clamp(value, 0.0f, 1.0f) * float(params.pq_max_code) + 0.5f));
    output_rgb[output + channel] = ushort(floor(
        float(effective_code) * 65535.0f / float(params.pq_max_code) + 0.5f));
  }
  if (pixel_clipped) {
    atomic_fetch_add_explicit(&counters[2], 1u, memory_order_relaxed);
  }
  if (params.collect_content_light != 0u) {
    const float3 output_nits = clamp(
        linear * params.diffuse_white_nits, 0.0f, 10000.0f);
    const uint luminance_x10000 = uint(floor(
        max(output_nits.r, max(output_nits.g, output_nits.b)) * 10000.0f + 0.5f));
    threadgroup uint group_max[256];
    threadgroup ulong group_sum[256];
    group_max[thread_index] = luminance_x10000;
    group_sum[thread_index] = ulong(luminance_x10000);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    uint active = threads_in_group.x;
    while (active > 1u) {
      const uint stride = (active + 1u) / 2u;
      if (thread_index < stride && thread_index + stride < active) {
        group_max[thread_index] = max(
            group_max[thread_index], group_max[thread_index + stride]);
        group_sum[thread_index] += group_sum[thread_index + stride];
      }
      threadgroup_barrier(mem_flags::mem_threadgroup);
      active = stride;
    }
    if (thread_index == 0u) {
      atomic_fetch_max_explicit(
          &counters[3], group_max[0], memory_order_relaxed);
      const uint sum_low = uint(group_sum[0]);
      uint sum_high = uint(group_sum[0] >> 32u);
      const uint previous_low = atomic_fetch_add_explicit(
          &counters[4], sum_low, memory_order_relaxed);
      if (sum_low != 0u && previous_low > 0xFFFFFFFFu - sum_low) {
        ++sum_high;
      }
      atomic_fetch_add_explicit(&counters[5], sum_high, memory_order_relaxed);
    }
  }
}

kernel void extended_p3_to_linear_rgba16f(
    texture2d<float, access::read> source [[texture(0)]],
    device const uint* annotation_mask [[buffer(0)]],
    device const float4* annotation_colors [[buffer(1)]],
    device half4* output_rgba [[buffer(2)]],
    device atomic_uint* counters [[buffer(3)]],
    constant Parameters& params [[buffer(4)]],
    device const float4* linear_source [[buffer(5)]],
    uint gid [[thread_position_in_grid]]) {
  const uint pixel_count = params.width * params.height;
  if (gid >= pixel_count) return;
  const uint2 position = uint2(gid % params.width, gid / params.width);
  const uint annotation_index = annotation_mask[gid];
  float3 linear;
  if (annotation_index == 0u) {
    const float3 encoded = read_source(source, linear_source, params, position);
    if (!all(isfinite(encoded))) {
      atomic_fetch_add_explicit(&counters[0], 1u, memory_order_relaxed);
      linear = float3(0.0f);
    } else {
      linear = source_linear(encoded, params);
    }
  } else {
    const float4 sample = annotation_colors[annotation_index - 1u];
    linear = sample.rgb;
    if (sample.w > 0.0f) {
      const float3 encoded = read_source(source, linear_source, params, position);
      if (!all(isfinite(encoded))) {
        atomic_fetch_add_explicit(&counters[0], 1u, memory_order_relaxed);
      } else {
        linear += sample.w * source_linear(encoded, params);
      }
    }
  }
  linear = min(linear, float3(10000.0f / 203.0f));
  const float maximum = max(linear.r, max(linear.g, linear.b));
  atomic_fetch_max_explicit(
      &counters[1], as_type<uint>(maximum), memory_order_relaxed);
  // Classification ignores every covered pixel, including partial AA.
  if (annotation_index == 0u) {
    atomic_fetch_max_explicit(&counters[2], as_type<uint>(maximum), memory_order_relaxed);
  }
  output_rgba[gid] = half4(half3(linear), half(1.0f));
}

// Packed source-visible spans: (absolute first pixel, length, packed start, 0).
// Only tiny statistics return to CPU; annotation-owned AA never participates.
kernel void probe_native_range(texture2d<float, access::read> source [[texture(0)]],
    device const uint4* spans [[buffer(0)]], device atomic_uint* counters [[buffer(1)]],
    constant uint& span_count [[buffer(2)]], uint gid [[thread_position_in_grid]],
    uint lane [[thread_position_in_threadgroup]], uint lanes [[threads_per_threadgroup]]) {
  uint flags = 0;
  uint lo = 0, hi = span_count;
  while (lo < hi) {
    const uint mid = lo + (hi - lo) / 2;
    const uint4 s = spans[mid];
    if (gid < s.z) hi = mid;
    else if (gid >= s.z + s.y) lo = mid + 1;
    else {
      const uint pixel = s.x + gid - s.z;
      const float3 v = source.read(uint2(pixel % source.get_width(), pixel / source.get_width())).rgb;
      if (any((as_type<uint3>(v) & uint3(0x7f800000)) == uint3(0x7f800000))) flags |= 1u;
      if (any(v > 1.0f)) flags |= 2u;
      break;
    }
  }
  threadgroup uint group[256];
  group[lane] = flags;
  threadgroup_barrier(mem_flags::mem_threadgroup);
  uint active = lanes;
  while (active > 1u) {
    const uint stride = (active + 1u) / 2u;
    if (lane < stride && lane + stride < active) group[lane] |= group[lane + stride];
    threadgroup_barrier(mem_flags::mem_threadgroup);
    active = stride;
  }
  if (lane == 0u) {
    if (group[0] & 1u) atomic_fetch_or_explicit(&counters[0], 1u, memory_order_relaxed);
    if (group[0] & 2u) atomic_fetch_or_explicit(&counters[1], 1u, memory_order_relaxed);
  }
}
)METAL";

struct Parameters {
  std::uint32_t width{};
  std::uint32_t height{};
  std::uint32_t encode_pq{};
  float diffuse_white_nits{};
  std::uint32_t pq_max_code{};
  std::uint32_t collect_content_light{};
  std::uint32_t source_is_linear{};
  std::uint32_t source_is_float32{};
  std::uint32_t source_offset_pixels{};
  std::uint32_t source_stride_pixels{};
};

struct MacAnnotationBuffers {
  id<MTLBuffer> mask{nil}, samples{nil};
};
Result<MacAnnotationBuffers, Error> annotation_buffers(id<MTLDevice> device,
    const AnnotationPixelPlan& plan) {
  using Output = Result<MacAnnotationBuffers, Error>;
  if (!AnnotationPixelPlanValidator::valid(plan)) return Output::failure(
      processor_error(ErrorCode::state_inconsistent, "invalid_pixel_plan"));
  LinearSampleRef native;
  for (const auto& span : plan.annotation_owned_spans) if (span.native_samples) {
    if (native && native != span.native_samples) return Output::failure(
        processor_error(ErrorCode::state_inconsistent, "mixed_native_sample_resources"));
    native = span.native_samples;
  }
  MacAnnotationBuffers out;
  if (native) {
    const auto width = static_cast<std::size_t>(plan.output_size_px.width);
    out.samples = macos_sample_buffer(native);
    out.mask = [device newBufferWithLength:width * static_cast<std::size_t>(plan.output_size_px.height) * sizeof(std::uint32_t)
        options:MTLResourceStorageModeShared];
    if (!out.mask || !out.samples) return Output::failure(processor_error(
        ErrorCode::presenter_failed, "native_annotation_binding_failed"));
    auto* indices = static_cast<std::uint32_t*>(out.mask.contents);
    std::memset(indices, 0, out.mask.length);
    for (const auto& span : plan.annotation_owned_spans) {
      if (span.native_samples != native || span.native_sample_offset + static_cast<std::size_t>(span.length) >= UINT32_MAX)
        return Output::failure(processor_error(ErrorCode::state_inconsistent, "invalid_native_span"));
      for (std::int32_t i = 0; i < span.length; ++i)
        indices[static_cast<std::size_t>(span.y) * width + static_cast<std::size_t>(span.x + i)] =
            static_cast<std::uint32_t>(span.native_sample_offset + static_cast<std::size_t>(i) + 1);
    }
  } else {
    auto upload = prepare_annotation_gpu_upload(plan);
    if (!upload) return Output::failure(upload.error());
    out.mask = [device newBufferWithBytes:upload.value().indices.data()
        length:upload.value().indices.size() * sizeof(std::uint32_t) options:MTLResourceStorageModeShared];
    out.samples = [device newBufferWithBytes:upload.value().samples.data()
        length:upload.value().samples.size() * sizeof(std::array<float,4>) options:MTLResourceStorageModeShared];
  }
  if (!out.mask || !out.samples) return Output::failure(processor_error(
      ErrorCode::presenter_failed, "annotation_allocation_failed"));
  return Output::success(std::move(out));
}

// Called after the consumer command completes, never an intermediate CPU wait.
Result<bool, Error> verify_native_inputs(const SelectionRoiView& source,
    const AnnotationPixelPlan& plan) {
  if (source.linear_source) {
    auto ready = source.linear_source->wait_until_ready();
    if (!ready) return ready;
  }
  LinearSampleRef last;
  for (const auto& span : plan.annotation_owned_spans) if (span.native_samples && span.native_samples != last) {
    auto ready = span.native_samples->wait_until_ready();
    if (!ready) return ready;
    last = span.native_samples;
  }
  return Result<bool, Error>::success(true);
}

}  // namespace

struct MacMetalExportPixelProcessor::Impl {
  id<MTLDevice> device{nil};
  id<MTLCommandQueue> command_queue{nil};
  id<MTLComputePipelineState> pipeline{nil};
  id<MTLComputePipelineState> ultra_hdr_pipeline{nil};
  id<MTLComputePipelineState> clean_pipeline{nil};
  id<MTLComputePipelineState> range_pipeline{nil};
  std::mutex mutex;
};

MacMetalExportPixelProcessor::MacMetalExportPixelProcessor(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

MacMetalExportPixelProcessor::~MacMetalExportPixelProcessor() = default;
MacMetalExportPixelProcessor::MacMetalExportPixelProcessor(
    MacMetalExportPixelProcessor&&) noexcept = default;
MacMetalExportPixelProcessor& MacMetalExportPixelProcessor::operator=(
    MacMetalExportPixelProcessor&&) noexcept = default;

Result<std::unique_ptr<MacMetalExportPixelProcessor>, Error>
MacMetalExportPixelProcessor::create() {
  auto impl = std::make_unique<Impl>();
  impl->device = macos_gpu_device();
  if (impl->device == nil) {
    return Result<std::unique_ptr<MacMetalExportPixelProcessor>, Error>::failure(
        processor_error(ErrorCode::presenter_failed, "metal_device_unavailable"));
  }
  impl->command_queue = macos_gpu_queue();
  if (impl->command_queue == nil) {
    return Result<std::unique_ptr<MacMetalExportPixelProcessor>, Error>::failure(
        processor_error(ErrorCode::presenter_failed, "command_queue_creation_failed"));
  }
  NSError* library_error = nil;
  id<MTLLibrary> library = [impl->device
      newLibraryWithSource:[NSString stringWithUTF8String:kMetalSource]
                   options:nil
                     error:&library_error];
  if (library == nil) {
    return Result<std::unique_ptr<MacMetalExportPixelProcessor>, Error>::failure(
        processor_error(
            ErrorCode::unsupported_encoding,
            "shader_compilation_failed",
            {{"nativeCode", std::to_string(library_error.code)},
         {"nativeDescription", ns_string_to_utf8(library_error.localizedDescription)}}));
  }
  NSError* pipeline_error = nil;
  id<MTLFunction> function = [library newFunctionWithName:@"extended_p3_to_png_rgb16"];
  impl->pipeline = [impl->device newComputePipelineStateWithFunction:function
                                                               error:&pipeline_error];
  if (impl->pipeline == nil) {
    return Result<std::unique_ptr<MacMetalExportPixelProcessor>, Error>::failure(
        processor_error(
            ErrorCode::unsupported_encoding,
            "pipeline_creation_failed",
            {{"nativeCode", std::to_string(pipeline_error.code)},
         {"nativeDescription", ns_string_to_utf8(pipeline_error.localizedDescription)}}));
  }
  NSError* ultra_hdr_pipeline_error = nil;
  id<MTLFunction> ultra_hdr_function =
      [library newFunctionWithName:@"extended_p3_to_linear_rgba16f"];
  impl->clean_pipeline = [impl->device newComputePipelineStateWithFunction:
      [library newFunctionWithName:@"compose_clean"] error:&pipeline_error];
  if (impl->clean_pipeline == nil) return Result<std::unique_ptr<MacMetalExportPixelProcessor>, Error>::failure(
      processor_error(ErrorCode::presenter_failed, "clean_pipeline_failed"));
  impl->range_pipeline = [impl->device newComputePipelineStateWithFunction:
      [library newFunctionWithName:@"probe_native_range"] error:&pipeline_error];
  if (!impl->range_pipeline) return Result<std::unique_ptr<MacMetalExportPixelProcessor>, Error>::failure(
      processor_error(ErrorCode::presenter_failed, "range_pipeline_failed"));
  impl->ultra_hdr_pipeline = [impl->device
      newComputePipelineStateWithFunction:ultra_hdr_function
                                    error:&ultra_hdr_pipeline_error];
  if (impl->ultra_hdr_pipeline == nil) {
    return Result<std::unique_ptr<MacMetalExportPixelProcessor>, Error>::failure(
        processor_error(
            ErrorCode::unsupported_encoding,
            "ultra_hdr_pipeline_creation_failed",
            {{"nativeCode", std::to_string(ultra_hdr_pipeline_error.code)},
         {"nativeDescription", ns_string_to_utf8(ultra_hdr_pipeline_error.localizedDescription)}}));
  }
  return Result<std::unique_ptr<MacMetalExportPixelProcessor>, Error>::success(
      std::unique_ptr<MacMetalExportPixelProcessor>(
          new MacMetalExportPixelProcessor(std::move(impl))));
}

Result<ExportPixelProcessResult, Error> MacMetalExportPixelProcessor::process(
    const ExportPixelProcessRequest& request) {
  @autoreleasepool {
  const std::scoped_lock lock(impl_->mutex);
  if (request.source == nullptr || request.pixel_plan == nullptr) {
    return Result<ExportPixelProcessResult, Error>::failure(
        processor_error(ErrorCode::invalid_input, "missing_request_input"));
  }
  const auto& source = *request.source;
  const auto& plan = *request.pixel_plan;
  if (!source.valid_storage() || source.size_px.width <= 0 || source.size_px.height <= 0 ||
      source.size_px != plan.output_size_px ||
      source.encoding.primaries != ColorPrimaries::display_p3 ||
      (source.encoding.transfer != TransferFunction::extended_srgb &&
       source.encoding.transfer != TransferFunction::linear) ||
      source.encoding.source_reference_white_nits != 0.0) {
    return Result<ExportPixelProcessResult, Error>::failure(
        processor_error(ErrorCode::invalid_color_contract, "unsupported_source_contract"));
  }
  const auto is_hdr = request.output_plan.encoding_intent == EncodingIntent::hdr_pq;
  if ((request.output_plan.encoding_intent != EncodingIntent::wide_gamut_sdr &&
       !is_hdr) ||
      request.output_plan.bit_depth != 16U ||
      (is_hdr && request.pq_diffuse_white != PqDiffuseWhite::nits_100 &&
       request.pq_diffuse_white != PqDiffuseWhite::nits_203) ||
      !valid_hdr_pq_precision(request.hdr_pq_precision)) {
    return Result<ExportPixelProcessResult, Error>::failure(
        processor_error(ErrorCode::invalid_input, "unsupported_output_contract"));
  }
  const auto width = static_cast<std::size_t>(source.size_px.width);
  const auto height = static_cast<std::size_t>(source.size_px.height);
  if (width > std::numeric_limits<std::size_t>::max() / height ||
      width * height > std::numeric_limits<std::size_t>::max() / 6U ||
      source.row_stride_samples < width * 4U ||
      source.first_sample_offset > source.sample_count() ||
      source.first_sample_offset + (height - 1U) * source.row_stride_samples + width * 4U >
          source.sample_count()) {
    return Result<ExportPixelProcessResult, Error>::failure(
        processor_error(ErrorCode::invalid_input, "invalid_source_shape"));
  }
  const auto pixel_count = width * height;
  auto annotation = annotation_buffers(impl_->device, plan);
  if (!annotation) return Result<ExportPixelProcessResult, Error>::failure(annotation.error());
  MTLTextureDescriptor* source_descriptor = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                  width:((source.rgba_float.empty() && !source.linear_source) ? width : 1)
                                 height:((source.rgba_float.empty() && !source.linear_source) ? height : 1)
                              mipmapped:NO];
  source_descriptor.storageMode = MTLStorageModeShared;
  source_descriptor.usage = MTLTextureUsageShaderRead;
  id<MTLTexture> source_texture = [impl_->device newTextureWithDescriptor:source_descriptor];
  if (source.linear_source) source_texture = macos_source_texture(source.linear_source);
  id<MTLBuffer> mask = annotation.value().mask;
  id<MTLBuffer> annotation_palette = annotation.value().samples;
  id<MTLBuffer> output = [impl_->device newBufferWithLength:pixel_count * 6U
                                                    options:MTLResourceStorageModeShared];
  id<MTLBuffer> counters = [impl_->device newBufferWithLength:6U * sizeof(std::uint32_t)
                                                      options:MTLResourceStorageModeShared];
  if (source_texture == nil || mask == nil || annotation_palette == nil ||
      output == nil || counters == nil) {
    return Result<ExportPixelProcessResult, Error>::failure(
        processor_error(ErrorCode::presenter_failed, "resource_allocation_failed"));
  }
  const auto binding = source.rgba_float.empty() ? MacSourceBufferBinding{}
      : bind_macos_source_buffer(impl_->device, source.rgba_float.data(),
          source.rgba_float.size_bytes(), std::max(source.rgba_float.size_bytes(),
              source.float_storage_capacity_samples * sizeof(float)));
  id<MTLBuffer> linear_source = source.rgba_float.empty()
      ? [impl_->device newBufferWithLength:16 options:MTLResourceStorageModeShared] : binding.buffer;
  if (!linear_source) return Result<ExportPixelProcessResult, Error>::failure(
      processor_error(ErrorCode::presenter_failed, "source_buffer_failed"));
  if (source.rgba_float.empty() && !source.linear_source) [source_texture
      replaceRegion:MTLRegionMake2D(0, 0, width, height)
        mipmapLevel:0
          withBytes:source.sample_data(source.first_sample_offset)
        bytesPerRow:source.row_stride_samples * source.sample_bytes()];
  std::memset(counters.contents, 0, 6U * sizeof(std::uint32_t));

  id<MTLCommandBuffer> command_buffer = [impl_->command_queue commandBuffer];
  id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
  if (command_buffer == nil || encoder == nil) {
    return Result<ExportPixelProcessResult, Error>::failure(
        processor_error(ErrorCode::presenter_failed, "command_encoder_creation_failed"));
  }
  const auto diffuse_white = pq_diffuse_white_nits(request.pq_diffuse_white);
  const Parameters parameters{
      static_cast<std::uint32_t>(width),
      static_cast<std::uint32_t>(height),
      request.output_plan.encoding_intent == EncodingIntent::hdr_pq ? 1U : 0U,
      static_cast<float>(diffuse_white),
      static_cast<std::uint32_t>((1U << hdr_pq_precision_bits(
          is_hdr ? request.hdr_pq_precision : HdrPqPrecision::bits_16)) - 1U),
      is_hdr ? 1U : 0U,
      source.encoding.transfer == TransferFunction::linear ? 1U : 0U,
      source.linear_source ? 2U : (source.rgba_float.empty() ? 0U : 1U),
      static_cast<std::uint32_t>(source.first_sample_offset / 4),
      static_cast<std::uint32_t>(source.row_stride_samples / 4),
  };
  [encoder setComputePipelineState:impl_->pipeline];
  [encoder setTexture:source_texture atIndex:0];
  [encoder setBuffer:mask offset:0 atIndex:0];
  [encoder setBuffer:annotation_palette offset:0 atIndex:1];
  [encoder setBuffer:output offset:0 atIndex:2];
  [encoder setBuffer:counters offset:0 atIndex:3];
  [encoder setBytes:&parameters length:sizeof(parameters) atIndex:4];
  [encoder setBuffer:linear_source offset:binding.offset atIndex:5];
  const auto thread_count = std::min<std::size_t>(
      256U, impl_->pipeline.maxTotalThreadsPerThreadgroup);
  [encoder dispatchThreads:MTLSizeMake(pixel_count, 1, 1)
       threadsPerThreadgroup:MTLSizeMake(thread_count, 1, 1)];
  [encoder endEncoding];
  [command_buffer commit];
  [command_buffer waitUntilCompleted];
  if (command_buffer.status != MTLCommandBufferStatusCompleted) {
    return Result<ExportPixelProcessResult, Error>::failure(processor_error(
        ErrorCode::presenter_failed,
        "gpu_execution_failed",
        {{"nativeCode", std::to_string(command_buffer.error.code)},
         {"nativeDescription", ns_string_to_utf8(command_buffer.error.localizedDescription)}}));
  }
  auto native_ready = verify_native_inputs(source, plan);
  if (!native_ready) return Result<ExportPixelProcessResult, Error>::failure(native_ready.error());
  const auto* counter_values = static_cast<const std::uint32_t*>(counters.contents);
  if (counter_values[0] != 0U) {
    return Result<ExportPixelProcessResult, Error>::failure(
        processor_error(ErrorCode::invalid_color_contract, "non_finite_source_component"));
  }
  ExportPixelProcessResult result;
  result.size_px = source.size_px;
  result.output_encoding = ColorEncoding{
      ColorPrimaries::display_p3,
      request.output_plan.encoding_intent == EncodingIntent::hdr_pq
          ? TransferFunction::pq
          : TransferFunction::srgb,
      AlphaMode::opaque,
      request.output_plan.encoding_intent == EncodingIntent::hdr_pq
          ? diffuse_white
          : 0.0,
  };
  result.rgb_u16.resize(pixel_count * 3U);
  std::memcpy(
      result.rgb_u16.data(), output.contents,
      result.rgb_u16.size() * sizeof(std::uint16_t));
  result.luminance_clip = LuminanceClipManifest{counter_values[2], counter_values[1]};
  if (is_hdr) {
    const auto luminance_sum =
        (static_cast<std::uint64_t>(counter_values[5]) << 32U) |
        static_cast<std::uint64_t>(counter_values[4]);
    result.content_light = ContentLightStatistics{
        counter_values[3], luminance_sum, pixel_count};
  }
  return Result<ExportPixelProcessResult, Error>::success(std::move(result));
  }
}

Result<LinearDisplayP3HalfImage, Error> MacMetalExportPixelProcessor::render(
    const UltraHdrInputRenderRequest& request) {
  @autoreleasepool {
  const std::scoped_lock lock(impl_->mutex);
  if (request.source == nullptr || request.pixel_plan == nullptr) {
    return Result<LinearDisplayP3HalfImage, Error>::failure(
        processor_error(ErrorCode::invalid_input, "missing_ultra_hdr_request_input"));
  }
  const auto& source = *request.source;
  const auto& plan = *request.pixel_plan;
  if (!source.valid_storage() || source.size_px.width <= 0 || source.size_px.height <= 0 ||
      source.size_px.width > kUltraHdrMaximumDimension ||
      source.size_px.height > kUltraHdrMaximumDimension ||
      source.size_px != plan.output_size_px ||
      source.encoding.primaries != ColorPrimaries::display_p3 ||
      (source.encoding.transfer != TransferFunction::extended_srgb &&
       source.encoding.transfer != TransferFunction::linear) ||
      source.encoding.source_reference_white_nits != 0.0 ||
      request.reference_white_nits != kUltraHdrReferenceWhiteNits) {
    return Result<LinearDisplayP3HalfImage, Error>::failure(
        processor_error(
            ErrorCode::invalid_color_contract, "unsupported_ultra_hdr_source_contract"));
  }
  const auto width = static_cast<std::size_t>(source.size_px.width);
  const auto height = static_cast<std::size_t>(source.size_px.height);
  if (width > std::numeric_limits<std::size_t>::max() / height ||
      width * height > std::numeric_limits<std::size_t>::max() / 8U ||
      source.row_stride_samples < width * 4U ||
      source.first_sample_offset > source.sample_count() ||
      source.first_sample_offset + (height - 1U) * source.row_stride_samples +
              width * 4U >
          source.sample_count()) {
    return Result<LinearDisplayP3HalfImage, Error>::failure(
        processor_error(ErrorCode::invalid_input, "invalid_ultra_hdr_source_shape"));
  }
  const auto pixel_count = width * height;
  auto annotation = annotation_buffers(impl_->device, plan);
  if (!annotation) return Result<LinearDisplayP3HalfImage, Error>::failure(annotation.error());
  MTLTextureDescriptor* source_descriptor = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                  width:((source.rgba_float.empty() && !source.linear_source) ? width : 1)
                                 height:((source.rgba_float.empty() && !source.linear_source) ? height : 1)
                              mipmapped:NO];
  source_descriptor.storageMode = MTLStorageModeShared;
  source_descriptor.usage = MTLTextureUsageShaderRead;
  id<MTLTexture> source_texture =
      [impl_->device newTextureWithDescriptor:source_descriptor];
  if (source.linear_source) source_texture = macos_source_texture(source.linear_source);
  id<MTLBuffer> mask = annotation.value().mask;
  id<MTLBuffer> annotation_palette = annotation.value().samples;
  id<MTLBuffer> output = [impl_->device newBufferWithLength:pixel_count * 8U
                                                    options:MTLResourceStorageModeShared];
  id<MTLBuffer> counters = [impl_->device newBufferWithLength:3U * sizeof(std::uint32_t)
                                                      options:MTLResourceStorageModeShared];
  if (source_texture == nil || mask == nil || annotation_palette == nil ||
      output == nil || counters == nil) {
    return Result<LinearDisplayP3HalfImage, Error>::failure(
        processor_error(ErrorCode::presenter_failed, "resource_allocation_failed"));
  }
  const auto binding = source.rgba_float.empty() ? MacSourceBufferBinding{}
      : bind_macos_source_buffer(impl_->device, source.rgba_float.data(),
          source.rgba_float.size_bytes(), std::max(source.rgba_float.size_bytes(),
              source.float_storage_capacity_samples * sizeof(float)));
  id<MTLBuffer> linear_source = source.rgba_float.empty()
      ? [impl_->device newBufferWithLength:16 options:MTLResourceStorageModeShared] : binding.buffer;
  if (!linear_source) return Result<LinearDisplayP3HalfImage, Error>::failure(
      processor_error(ErrorCode::presenter_failed, "source_buffer_failed"));
  if (source.rgba_float.empty() && !source.linear_source) [source_texture
      replaceRegion:MTLRegionMake2D(0, 0, width, height)
        mipmapLevel:0
          withBytes:source.sample_data(source.first_sample_offset)
        bytesPerRow:source.row_stride_samples * source.sample_bytes()];
  std::memset(counters.contents, 0, 3U * sizeof(std::uint32_t));

  id<MTLCommandBuffer> command_buffer = [impl_->command_queue commandBuffer];
  id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
  if (command_buffer == nil || encoder == nil) {
    return Result<LinearDisplayP3HalfImage, Error>::failure(
        processor_error(ErrorCode::presenter_failed, "command_encoder_creation_failed"));
  }
  const Parameters parameters{
      static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height),
      0U, static_cast<float>(kUltraHdrReferenceWhiteNits), 65535U, 0U,
      source.encoding.transfer == TransferFunction::linear ? 1U : 0U,
      source.linear_source ? 2U : (source.rgba_float.empty() ? 0U : 1U),
      static_cast<std::uint32_t>(source.first_sample_offset / 4),
      static_cast<std::uint32_t>(source.row_stride_samples / 4)};
  [encoder setComputePipelineState:impl_->ultra_hdr_pipeline];
  [encoder setTexture:source_texture atIndex:0];
  [encoder setBuffer:mask offset:0 atIndex:0];
  [encoder setBuffer:annotation_palette offset:0 atIndex:1];
  [encoder setBuffer:output offset:0 atIndex:2];
  [encoder setBuffer:counters offset:0 atIndex:3];
  [encoder setBytes:&parameters length:sizeof(parameters) atIndex:4];
  [encoder setBuffer:linear_source offset:binding.offset atIndex:5];
  const auto thread_count = std::min<std::size_t>(
      256U, impl_->ultra_hdr_pipeline.maxTotalThreadsPerThreadgroup);
  [encoder dispatchThreads:MTLSizeMake(pixel_count, 1, 1)
       threadsPerThreadgroup:MTLSizeMake(thread_count, 1, 1)];
  [encoder endEncoding];
  [command_buffer commit];
  [command_buffer waitUntilCompleted];
  if (command_buffer.status != MTLCommandBufferStatusCompleted) {
    return Result<LinearDisplayP3HalfImage, Error>::failure(processor_error(
        ErrorCode::presenter_failed,
        "gpu_execution_failed",
        {{"nativeCode", std::to_string(command_buffer.error.code)},
         {"nativeDescription", ns_string_to_utf8(command_buffer.error.localizedDescription)}}));
  }
  auto native_ready = verify_native_inputs(source, plan);
  if (!native_ready) return Result<LinearDisplayP3HalfImage, Error>::failure(native_ready.error());
  const auto* counter_values = static_cast<const std::uint32_t*>(counters.contents);
  if (counter_values[0] != 0U) {
    return Result<LinearDisplayP3HalfImage, Error>::failure(
        processor_error(ErrorCode::invalid_color_contract, "non_finite_source_component"));
  }
  LinearDisplayP3HalfImage result;
  result.size_px = source.size_px;
  result.reference_white_nits = request.reference_white_nits;
  result.source_visible_maximum_linear_component = static_cast<double>(
      std::bit_cast<float>(counter_values[2]));
  result.maximum_linear_component = static_cast<double>(
      std::bit_cast<float>(counter_values[1]));
  result.rgba_half.resize(pixel_count * 4U);
  std::memcpy(result.rgba_half.data(), output.contents,
              result.rgba_half.size() * sizeof(std::uint16_t));
  return Result<LinearDisplayP3HalfImage, Error>::success(std::move(result));
  }
}

Result<std::vector<std::array<float, 4>>, Error> MacMetalExportPixelProcessor::compose(
    std::span<const CleanCompositionSample> samples) {
  using Output = Result<std::vector<std::array<float, 4>>, Error>;
  if (samples.empty()) return Output::success({});
  std::scoped_lock lock(impl_->mutex);
  @autoreleasepool {
    id<MTLBuffer> input = [impl_->device newBufferWithBytes:samples.data()
        length:samples.size_bytes() options:MTLResourceStorageModeShared];
    id<MTLBuffer> output = [impl_->device newBufferWithLength:samples.size() * 16
        options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> command = [impl_->command_queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
    if (!input || !output || !command || !encoder) return Output::failure(
        processor_error(ErrorCode::presenter_failed, "clean_resource_failed"));
    [encoder setComputePipelineState:impl_->clean_pipeline];
    [encoder setBuffer:input offset:0 atIndex:0];
    [encoder setBuffer:output offset:0 atIndex:1];
    [encoder dispatchThreads:MTLSizeMake(samples.size(), 1, 1)
        threadsPerThreadgroup:MTLSizeMake(impl_->clean_pipeline.threadExecutionWidth, 1, 1)];
    [encoder endEncoding]; [command commit]; [command waitUntilCompleted];
    if (command.status == MTLCommandBufferStatusError) return Output::failure(
        processor_error(ErrorCode::presenter_failed, "clean_command_failed"));
    std::vector<std::array<float, 4>> result(samples.size());
    std::memcpy(result.data(), output.contents, result.size() * 16);
    for (const auto& value : result) for (std::size_t c = 0; c < 3; ++c)
      if (!std::isfinite(value[c])) return Output::failure(
          processor_error(ErrorCode::invalid_color_contract, "clean_nonfinite"));
    return Output::success(std::move(result));
  }
}

Result<LinearSampleRef, Error> MacMetalExportPixelProcessor::compose_native(
    LinearSourceRef source, std::span<const PositionedCleanSample> samples) {
  // Separate native runtime, not the exporter's mutex or synchronous readback.
  return macos_compose_clean(std::move(source), samples);
}

Result<RangeFitResult, Error> MacMetalExportPixelProcessor::probe(
    const SelectionRoiView& source, const AnnotationPixelPlan& plan,
    RangeProbeOptimization optimization) {
  using Output = Result<RangeFitResult, Error>;
  if (!source.linear_source) return SourceRangeProbe::probe(source, plan, optimization);
  @autoreleasepool {
    if (!source.valid_storage() || source.encoding.primaries != ColorPrimaries::display_p3 ||
        source.encoding.source_reference_white_nits != 0 || source.size_px != plan.output_size_px ||
        !AnnotationPixelPlanValidator::valid(plan) || source.row_stride_samples % 4 ||
        source.first_sample_offset % 4 || source.row_stride_samples / 4 !=
            static_cast<std::size_t>(source.linear_source->size_px().width))
      return Output::failure(processor_error(ErrorCode::invalid_input, "invalid_native_range_input"));
    std::vector<std::array<std::uint32_t, 4>> spans;
    std::size_t source_pixels = 0, annotation_pixels = 0;
    for (const auto& s : plan.source_visible_spans) {
      const auto start = source.first_sample_offset / 4 +
          static_cast<std::size_t>(s.y) * (source.row_stride_samples / 4) + static_cast<std::size_t>(s.x);
      if (start + static_cast<std::size_t>(s.length) > source.sample_count() / 4 || start > UINT32_MAX || source_pixels > UINT32_MAX)
        return Output::failure(processor_error(ErrorCode::invalid_input, "native_range_out_of_bounds"));
      spans.push_back({static_cast<std::uint32_t>(start), static_cast<std::uint32_t>(s.length),
          static_cast<std::uint32_t>(source_pixels), 0});
      source_pixels += static_cast<std::size_t>(s.length);
    }
    for (const auto& s : plan.annotation_owned_spans) annotation_pixels += static_cast<std::size_t>(s.length);
    if (source_pixels == 0 || optimization.source_visible_within_sdr_guaranteed)
      return Output::success({true, source_pixels, annotation_pixels, 0,
          source_pixels == 0 ? "all_pixels_annotation_owned" : "capture_contract_sdr_fast_path"});
    id<MTLTexture> texture = macos_source_texture(source.linear_source);
    id<MTLBuffer> input = [impl_->device newBufferWithBytes:spans.data()
        length:spans.size() * sizeof(spans[0]) options:MTLResourceStorageModeShared];
    id<MTLBuffer> stats = [impl_->device newBufferWithLength:8 options:MTLResourceStorageModeShared];
    auto command = [impl_->command_queue commandBuffer];
    auto encoder = [command computeCommandEncoder];
    if (!texture || !input || !stats || !command || !encoder)
      return Output::failure(processor_error(ErrorCode::presenter_failed, "range_resource_failed"));
    std::memset(stats.contents, 0, 8);
    const auto count = static_cast<std::uint32_t>(spans.size());
    [encoder setComputePipelineState:impl_->range_pipeline];
    [encoder setTexture:texture atIndex:0];
    [encoder setBuffer:input offset:0 atIndex:0];
    [encoder setBuffer:stats offset:0 atIndex:1];
    [encoder setBytes:&count length:sizeof(count) atIndex:2];
    [encoder dispatchThreads:MTLSizeMake(source_pixels,1,1)
        threadsPerThreadgroup:MTLSizeMake(256,1,1)];
    [encoder endEncoding]; [command commit]; [command waitUntilCompleted];
    if (command.status != MTLCommandBufferStatusCompleted)
      return Output::failure(processor_error(ErrorCode::presenter_failed, "range_command_failed"));
    auto ready = source.linear_source->wait_until_ready();
    if (!ready) return Output::failure(ready.error());
    const auto* values = static_cast<const std::uint32_t*>(stats.contents);
    if (values[0]) return Output::failure(processor_error(ErrorCode::invalid_color_contract, "range_nonfinite_source"));
    return Output::success({values[1] == 0, source_pixels, annotation_pixels, source_pixels * 3,
        values[1] ? "source_visible_component_above_edr_one" : "source_visible_components_fit_edr_one"});
  }
}

}  // namespace hdrshot
