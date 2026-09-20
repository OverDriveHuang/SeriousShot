#include "platform/macos/metal_edr_presenter.hpp"
#include "application/clean_content_cache.hpp"

#import <CoreGraphics/CoreGraphics.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <cmath>
#include "platform/macos/macos_shared_source_buffer.hpp"
#include "platform/macos/macos_gpu_source.hpp"
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace hdrshot {
namespace {

constexpr double kOpticalOutputScaleNits = 100.0;
constexpr const char* kTextureFormat = "MTLPixelFormatRGBA16Float";
constexpr const char* kLayerColorSpace = "kCGColorSpaceExtendedLinearDisplayP3";

Error presenter_error(
    const ErrorCode code,
    const Retryability retryability,
    std::map<std::string, std::string> context = {},
    std::source_location origin = std::source_location::current()) {
  return Error{code, "MacMetalEdrPresenter", retryability, std::move(context), origin};
}

std::string ns_string_to_utf8(NSString* value) {
  return value == nil ? std::string{} : std::string{value.UTF8String};
}

Result<bool, Error> validate_request(const MacMetalOverlayRequest& request) {
  constexpr std::size_t channels = 4;
  if (request.width_px == 0 || request.height_px == 0 ||
      request.width_px > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()) ||
      request.height_px > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()) ||
      request.width_px > std::numeric_limits<std::size_t>::max() / channels ||
      request.width_px * channels >
          std::numeric_limits<std::size_t>::max() / request.height_px) {
    return Result<bool, Error>::failure(presenter_error(
        ErrorCode::invalid_input,
        Retryability::never,
        {{"reason", "invalid_frame_shape"}}));
  }
  const auto count = request.width_px * request.height_px * channels;
  const auto storages = static_cast<int>(static_cast<bool>(request.linear_source)) +
      static_cast<int>(static_cast<bool>(request.rgba_float_linear_p3)) +
      static_cast<int>(static_cast<bool>(request.rgba_half_extended_p3));
  if (storages != 1 ||
      (request.linear_source && (!request.source_is_linear ||
          count > std::numeric_limits<std::size_t>::max() / sizeof(float) ||
          request.linear_source->byte_count() != count * sizeof(float) ||
          request.linear_source->size_px() != PixelSize{
              static_cast<int>(request.width_px), static_cast<int>(request.height_px)})) ||
      (request.rgba_float_linear_p3 && (!request.source_is_linear ||
          request.rgba_float_linear_p3->size() != count)) ||
      (request.rgba_half_extended_p3 && request.rgba_half_extended_p3->size() != count)) {
    return Result<bool, Error>::failure(presenter_error(
        ErrorCode::invalid_input, Retryability::never, {{"reason", "invalid_source_storage"}}));
  }
  const auto frame_width = static_cast<std::int32_t>(request.width_px);
  const auto frame_height = static_cast<std::int32_t>(request.height_px);
  const bool has_selection = !request.selection_px.empty();
  const bool canonical_empty_selection = request.selection_px == PixelRect{};
  if ((!has_selection && !canonical_empty_selection) ||
      (has_selection &&
       (request.selection_px.x < 0 || request.selection_px.y < 0 ||
        request.selection_px.right() > frame_width ||
        request.selection_px.bottom() > frame_height))) {
    return Result<bool, Error>::failure(presenter_error(
        ErrorCode::invalid_input,
        Retryability::never,
        {{"reason", "selection_out_of_bounds"}}));
  }
  if (!std::isfinite(request.outside_linear_dim_factor) ||
      request.outside_linear_dim_factor < 0.0F ||
      request.outside_linear_dim_factor > 1.0F || !std::isfinite(request.ui_white_edr) ||
      request.ui_white_edr < 0.0F || request.ui_border_width_px < 0) {
    return Result<bool, Error>::failure(presenter_error(
        ErrorCode::invalid_color_contract,
        Retryability::never,
        {{"reason", "unsupported_overlay_contract"}}));
  }
  return Result<bool, Error>::success(true);
}

Result<bool, Error> verify_native_content(const MacMetalOverlayRequest& request) {
  if (request.linear_source) {
    auto ready = request.linear_source->wait_until_ready();
    if (!ready) return ready;
  }
  LinearSampleRef last;
  if (request.clean_content) for (const auto& span : request.clean_content->owned)
    if (span.native_samples && last != span.native_samples) {
      auto ready = span.native_samples->wait_until_ready();
      if (!ready) return ready;
      last = span.native_samples;
    }
  return Result<bool, Error>::success(true);
}

float half_to_float(const std::uint16_t bits) {
  _Float16 value{};
  static_assert(sizeof(value) == sizeof(bits));
  std::memcpy(&value, &bits, sizeof(bits));
  return static_cast<float>(value);
}

struct ShaderParameters {
  std::uint32_t width;
  std::uint32_t height;
  std::int32_t selection_x;
  std::int32_t selection_y;
  std::int32_t selection_width;
  std::int32_t selection_height;
  float outside_dim_factor;
  float ui_white_edr;
  std::int32_t ui_border_width;
  std::uint32_t clamp_to_sdr;
  std::uint32_t has_selection;
  std::uint32_t source_is_linear;
  std::uint32_t clean_span_count;
  std::uint32_t source_is_float32;
};

constexpr const char* kMetalSource = R"METAL(
#include <metal_stdlib>
using namespace metal;

struct ShaderParameters {
  uint width;
  uint height;
  int selection_x;
  int selection_y;
  int selection_width;
  int selection_height;
  float outside_dim_factor;
  float ui_white_edr;
  int ui_border_width;
  uint clamp_to_sdr;
  uint has_selection;
  uint source_is_linear;
  uint clean_span_count;
  uint source_is_float32;
};

float3 extended_srgb_to_linear(float3 encoded) {
  const float3 magnitude = abs(encoded);
  const float3 linear = select(
      magnitude / 12.92f,
      pow((magnitude + 0.055f) / 1.055f, float3(2.4f)),
      magnitude > 0.04045f);
  return select(linear, -linear, encoded < 0.0f);
}

float3 clean_color(texture2d<float, access::read> source,
    constant ShaderParameters& params, device const uint4* spans,
    device const float4* samples, device const uint2* rows, device const float4* float_source, uint2 gid) {
  const uint pixel = gid.y * params.width + gid.x;
  const uint2 row = rows[gid.y];
  uint lo = row.x, hi = row.x + row.y;
  while (lo < hi) {
    const uint mid = lo + (hi - lo) / 2;
    const uint4 span = spans[mid];
    if (pixel < span.x) hi = mid;
    else if (pixel >= span.x + span.y) lo = mid + 1;
    else return samples[span.z + pixel - span.x].rgb;
  }
  const float3 value = params.source_is_float32 != 0u ? float_source[pixel].rgb : source.read(gid).rgb;
  return params.source_is_linear != 0u ? value : extended_srgb_to_linear(value);
}
float3 apply_ui(float3 color, constant ShaderParameters& params, uint2 gid) {
  const int x = int(gid.x);
  const int y = int(gid.y);
  const int right = params.selection_x + params.selection_width;
  const int bottom = params.selection_y + params.selection_height;
  const bool inside = params.has_selection != 0 &&
                      x >= params.selection_x && x < right &&
                      y >= params.selection_y && y < bottom;
  if (!inside) {
    color *= params.outside_dim_factor;
  }

  const int border = params.has_selection != 0 ? params.ui_border_width : 0;
  const bool in_outer = border > 0 && x >= params.selection_x - border &&
                        x < right + border && y >= params.selection_y - border &&
                        y < bottom + border;
  const bool in_inner = x >= params.selection_x + border && x < right - border &&
                        y >= params.selection_y + border && y < bottom - border;
  if (in_outer && !in_inner) {
    color = float3(params.ui_white_edr);
  }
  if (params.clamp_to_sdr != 0) {
    color = clamp(color, 0.0f, 1.0f);
  }
  return color;
}
kernel void compose_overlay(
    texture2d<float, access::read> source [[texture(0)]],
    texture2d<half, access::write> output [[texture(1)]],
    constant ShaderParameters& params [[buffer(0)]],
    device const uint4* spans [[buffer(1)]],
    device const float4* samples [[buffer(2)]],
    device const uint2* rows [[buffer(3)]],
    device const float4* float_source [[buffer(4)]],
    uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= params.width || gid.y >= params.height) return;
  output.write(half4(half3(apply_ui(clean_color(source, params, spans, samples, rows, float_source, gid), params, gid)), 1.0h), gid);
}

struct VertexOut {
  float4 position [[position]];
  float2 uv;
};

vertex VertexOut fullscreen_vertex(uint vertex_id [[vertex_id]]) {
  constexpr float2 positions[] = {
      float2(-1.0f, -1.0f), float2(3.0f, -1.0f), float2(-1.0f, 3.0f)};
  constexpr float2 uvs[] = {
      float2(0.0f, 1.0f), float2(2.0f, 1.0f), float2(0.0f, -1.0f)};
  VertexOut out;
  out.position = float4(positions[vertex_id], 0.0f, 1.0f);
  out.uv = uvs[vertex_id];
  return out;
}

fragment half4 fullscreen_fragment(
    VertexOut in [[stage_in]], texture2d<float, access::read> source [[texture(0)]],
    constant ShaderParameters& params [[buffer(0)]], device const uint4* spans [[buffer(1)]],
    device const float4* samples [[buffer(2)]], device const uint2* rows [[buffer(3)]],
    device const float4* float_source [[buffer(4)]]) {
  const uint2 gid = min(uint2(in.uv * float2(params.width, params.height)),
                       uint2(params.width - 1, params.height - 1));
  return half4(half3(apply_ui(clean_color(source, params, spans, samples, rows, float_source, gid), params, gid)), 1.0h);
}

)METAL";

}  // namespace

struct MacMetalEdrPresenter::Impl {
  ~Impl() { cached_source_buffer = nil; }
  id<MTLDevice> device{nil};
  id<MTLCommandQueue> command_queue{nil};
  id<MTLComputePipelineState> compute_pipeline{nil};
  id<MTLRenderPipelineState> render_pipeline{nil};
  id<MTLTexture> cached_source_texture{nil};
  id<MTLBuffer> cached_source_buffer{nil};
  bool source_aliases_cpu{};
  bool source_uses_buffer{};
  std::size_t source_buffer_offset{};
  id<MTLTexture> cached_output_texture{nil};
  std::shared_ptr<const std::vector<std::uint16_t>> cached_source_owner;
  std::shared_ptr<const LinearFloatPixels> cached_float_owner;
  LinearSourceRef cached_native_owner;
  CleanContentRef cached_clean;
  id<MTLBuffer> clean_spans{nil}, clean_samples{nil}, clean_rows{nil};
  std::uint32_t clean_span_count{};
  ShaderParameters parameters{};
  std::uint64_t cached_source_frame_key{};
  std::size_t cached_width{};
  std::size_t cached_height{};
  CAMetalLayer* configured_layer{nil};
  MacMetalSurfaceRange configured_surface_range{MacMetalSurfaceRange::sdr};
  bool layer_is_configured{};

  Result<id<MTLTexture>, Error> compose(
      const MacMetalOverlayRequest& request,
      id<MTLCommandBuffer> command_buffer, bool dispatch = true) {
    const bool dimensions_changed = cached_width != request.width_px ||
        cached_height != request.height_px;
    const bool source_changed = dimensions_changed || cached_source_texture == nil ||
        cached_source_frame_key != request.source_frame_key ||
        cached_source_owner.get() != request.rgba_half_extended_p3.get() ||
        cached_float_owner.get() != request.rgba_float_linear_p3.get() ||
        cached_native_owner != request.linear_source;
    if (source_changed) {
      // FP32 is the sole software-normalized source. Large malloc allocations
      // that meet Metal's documented page alignment can be shared directly.
      // Other layouts use a bounded upload copy; never reduce precision.
      if (request.linear_source) {
        cached_source_texture = macos_source_texture(request.linear_source);
        cached_source_buffer = [device newBufferWithLength:16 options:MTLResourceStorageModeShared];
        source_aliases_cpu = false;
        source_buffer_offset = 0;
        source_uses_buffer = false;
        if (!cached_source_texture || !cached_source_buffer) return Result<id<MTLTexture>, Error>::failure(
            presenter_error(ErrorCode::presenter_failed, Retryability::after_recreate,
                {{"reason", "native_source_binding_failed"}}));
      } else {
      const bool fp32 = request.rgba_float_linear_p3 != nullptr;
      if (fp32) {
        const auto* data = request.rgba_float_linear_p3->data();
        const auto bytes = request.rgba_float_linear_p3->size() * sizeof(float);
        const auto binding = bind_macos_source_buffer(device, data, bytes,
            request.rgba_float_linear_p3->capacity() * sizeof(float));
        cached_source_buffer = binding.buffer;
        source_aliases_cpu = binding.aliases_source;
        source_buffer_offset = binding.offset;
      } else {
        source_buffer_offset = 0;
        cached_source_buffer = [device newBufferWithLength:16 options:MTLResourceStorageModeShared];
      }
      source_uses_buffer = fp32;
      const auto row_bytes = request.width_px * 4 * sizeof(float);
      const auto texture_alignment = [device minimumLinearTextureAlignmentForPixelFormat:MTLPixelFormatRGBA32Float];
      if (fp32 && row_bytes % texture_alignment == 0 && source_buffer_offset % texture_alignment == 0) {
        auto view_descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
            width:request.width_px height:request.height_px mipmapped:NO];
        view_descriptor.storageMode = MTLStorageModeShared;
        view_descriptor.usage = MTLTextureUsageShaderRead;
        cached_source_texture = [cached_source_buffer newTextureWithDescriptor:view_descriptor
            offset:source_buffer_offset bytesPerRow:row_bytes];
        source_uses_buffer = cached_source_texture == nil;
      } else cached_source_texture = nil;
      if (!cached_source_texture) {
        MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
            width:(fp32 ? 1 : request.width_px) height:(fp32 ? 1 : request.height_px) mipmapped:NO];
        descriptor.storageMode = MTLStorageModeShared;
        descriptor.usage = MTLTextureUsageShaderRead;
        cached_source_texture = [device newTextureWithDescriptor:descriptor];
      }
      if (!cached_source_texture || !cached_source_buffer) return Result<id<MTLTexture>, Error>::failure(
          presenter_error(ErrorCode::presenter_failed, Retryability::after_recreate,
              {{"reason", "source_resource_allocation_failed"}}));
      if (!fp32) [cached_source_texture replaceRegion:MTLRegionMake2D(0,0,request.width_px,request.height_px)
          mipmapLevel:0 withBytes:request.rgba_half_extended_p3->data()
          bytesPerRow:request.width_px * 4 * sizeof(std::uint16_t)];
      }
      cached_source_owner = request.rgba_half_extended_p3;
      cached_float_owner = request.rgba_float_linear_p3;
      cached_native_owner = request.linear_source;
      cached_source_frame_key = request.source_frame_key;
    }

    if (dimensions_changed) cached_output_texture = nil;
    if (dispatch && cached_output_texture == nil) {
      MTLTextureDescriptor* output_descriptor = [MTLTextureDescriptor
          texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                      width:request.width_px
                                     height:request.height_px
                                  mipmapped:NO];
      output_descriptor.storageMode = MTLStorageModeShared;
      output_descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget;
      cached_output_texture = [device newTextureWithDescriptor:output_descriptor];
      if (cached_output_texture == nil) {
        return Result<id<MTLTexture>, Error>::failure(presenter_error(
            ErrorCode::presenter_failed,
            Retryability::after_recreate,
            {{"reason", "output_texture_allocation_failed"}}));
      }
    }
    cached_width = request.width_px;
    cached_height = request.height_px;

    if (cached_clean != request.clean_content || clean_spans == nil || source_changed) {
      std::vector<std::array<std::uint32_t, 4>> spans;
      std::vector<std::array<float, 4>> samples;
      LinearSampleRef native_samples;
      std::vector<std::array<std::uint32_t, 2>> rows(request.height_px);
      if (request.clean_content) for (const auto& span : request.clean_content->owned) {
        if (span.y < 0 || span.y >= static_cast<int>(request.height_px) || span.x < 0 ||
            span.length <= 0 || static_cast<std::int64_t>(span.x) + span.length >
                static_cast<std::int64_t>(request.width_px))
          return Result<id<MTLTexture>, Error>::failure(presenter_error(ErrorCode::invalid_input,
              Retryability::never, {{"reason", "clean_span_out_of_bounds"}}));
        auto& row = rows[static_cast<std::size_t>(span.y)];
        if (row[1] == 0) row[0] = static_cast<std::uint32_t>(spans.size());
        ++row[1];
        if (span.native_samples) {
          if ((native_samples && native_samples != span.native_samples) || !samples.empty() ||
              !span.clean_composited || !span.edge_samples.empty() ||
              span.native_sample_offset > span.native_samples->sample_count() ||
              static_cast<std::size_t>(span.length) >
                  span.native_samples->sample_count() - span.native_sample_offset ||
              span.native_sample_offset > UINT32_MAX - static_cast<std::uint32_t>(span.length))
            return Result<id<MTLTexture>, Error>::failure(presenter_error(ErrorCode::invalid_input,
                Retryability::never, {{"reason", "invalid_native_samples"}}));
          native_samples = span.native_samples;
        } else if (native_samples || span.edge_samples.size() != static_cast<std::size_t>(span.length))
          return Result<id<MTLTexture>, Error>::failure(presenter_error(ErrorCode::invalid_input,
              Retryability::never, {{"reason", "invalid_clean_samples"}}));
        spans.push_back({static_cast<std::uint32_t>(static_cast<std::size_t>(span.y) *
                request.width_px + static_cast<std::size_t>(span.x)),
            static_cast<std::uint32_t>(span.length), static_cast<std::uint32_t>(
                span.native_samples ? span.native_sample_offset : samples.size()), 0});
        if (!span.native_samples) samples.insert(samples.end(), span.edge_samples.begin(), span.edge_samples.end());
      }
      clean_span_count = static_cast<std::uint32_t>(spans.size());
      if (spans.empty()) spans.push_back({0, 0, 0, 0});
      if (samples.empty()) samples.push_back({0, 0, 0, 0});
      clean_spans = [device newBufferWithBytes:spans.data() length:spans.size() * 16 options:MTLResourceStorageModeShared];
      clean_samples = native_samples ? macos_sample_buffer(native_samples)
          : [device newBufferWithBytes:samples.data() length:samples.size() * 16 options:MTLResourceStorageModeShared];
      clean_rows = [device newBufferWithBytes:rows.data() length:rows.size() * 8 options:MTLResourceStorageModeShared];
      if (!clean_spans || !clean_samples || !clean_rows) return Result<id<MTLTexture>, Error>::failure(
          presenter_error(ErrorCode::presenter_failed, Retryability::after_recreate, {{"reason", "clean_upload_failed"}}));
      cached_clean = request.clean_content;
    }
    parameters = ShaderParameters{
        static_cast<std::uint32_t>(request.width_px),
        static_cast<std::uint32_t>(request.height_px),
        request.selection_px.x,
        request.selection_px.y,
        request.selection_px.width,
        request.selection_px.height,
        request.outside_linear_dim_factor,
        request.ui_white_edr,
        request.ui_border_width_px,
        request.target_surface_range == MacMetalSurfaceRange::sdr ? 1U : 0U,
        request.selection_px.empty() ? 0U : 1U,
        request.source_is_linear ? 1U : 0U,
        clean_span_count,
        source_uses_buffer ? 1U : 0U,
    };
    if (!dispatch) return Result<id<MTLTexture>, Error>::success(cached_source_texture);
    id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
    if (encoder == nil) {
      return Result<id<MTLTexture>, Error>::failure(presenter_error(
          ErrorCode::presenter_failed,
          Retryability::after_recreate,
          {{"reason", "compute_encoder_creation_failed"}}));
    }
    [encoder setComputePipelineState:compute_pipeline];
    [encoder setTexture:cached_source_texture atIndex:0];
    [encoder setTexture:cached_output_texture atIndex:1];
    [encoder setBytes:&parameters length:sizeof(parameters) atIndex:0];
    [encoder setBuffer:clean_spans offset:0 atIndex:1];
    [encoder setBuffer:clean_samples offset:0 atIndex:2];
    [encoder setBuffer:clean_rows offset:0 atIndex:3];
    [encoder setBuffer:cached_source_buffer offset:source_buffer_offset atIndex:4];
    const auto thread_width = compute_pipeline.threadExecutionWidth;
    const auto thread_height = std::max<std::size_t>(
        1, compute_pipeline.maxTotalThreadsPerThreadgroup / thread_width);
    [encoder dispatchThreads:MTLSizeMake(request.width_px, request.height_px, 1)
         threadsPerThreadgroup:MTLSizeMake(thread_width, thread_height, 1)];
    [encoder endEncoding];
    return Result<id<MTLTexture>, Error>::success(cached_output_texture);
  }

  void configure_layer(
      CAMetalLayer* layer,
      const MacMetalSurfaceRange surface_range) {
    if (layer_is_configured && configured_layer == layer &&
        configured_surface_range == surface_range) {
      return;
    }
    layer.device = device;
    layer.pixelFormat = MTLPixelFormatRGBA16Float;
    layer.wantsExtendedDynamicRangeContent =
        surface_range == MacMetalSurfaceRange::edr ? YES : NO;
    layer.toneMapMode = CAToneMapModeNever;
    if (@available(macOS 26.0, *)) {
      layer.preferredDynamicRange = surface_range == MacMetalSurfaceRange::edr
          ? CADynamicRangeHigh
          : CADynamicRangeStandard;
    }
    CGColorSpaceRef color_space =
        CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearDisplayP3);
    layer.colorspace = color_space;
    CGColorSpaceRelease(color_space);
    configured_layer = layer;
    configured_surface_range = surface_range;
    layer_is_configured = true;
    std::cout << "metalSurfaceConfigured="
              << (surface_range == MacMetalSurfaceRange::edr ? "edr" : "sdr")
              << " sourceTextureCache=enabled layerConfigurationCache=enabled\n";
  }

  bool is_layer_configured(
      CAMetalLayer* layer,
      const MacMetalSurfaceRange surface_range) const {
    return layer_is_configured && configured_layer == layer &&
        configured_surface_range == surface_range;
  }
};

MacMetalEdrPresenter::MacMetalEdrPresenter(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

MacMetalEdrPresenter::~MacMetalEdrPresenter() = default;
MacMetalEdrPresenter::MacMetalEdrPresenter(MacMetalEdrPresenter&&) noexcept = default;
MacMetalEdrPresenter& MacMetalEdrPresenter::operator=(MacMetalEdrPresenter&&) noexcept = default;

Result<std::unique_ptr<MacMetalEdrPresenter>, Error> MacMetalEdrPresenter::create() {
  auto impl = std::make_unique<Impl>();
  impl->device = macos_gpu_device();
  if (impl->device == nil) {
    return Result<std::unique_ptr<MacMetalEdrPresenter>, Error>::failure(presenter_error(
        ErrorCode::presenter_failed,
        Retryability::after_recreate,
        {{"reason", "metal_device_unavailable"}}));
  }
  impl->command_queue = macos_gpu_queue();
  if (impl->command_queue == nil) {
    return Result<std::unique_ptr<MacMetalEdrPresenter>, Error>::failure(presenter_error(
        ErrorCode::presenter_failed,
        Retryability::after_recreate,
        {{"reason", "command_queue_creation_failed"}}));
  }

  NSError* library_error = nil;
  id<MTLLibrary> library = [impl->device
      newLibraryWithSource:[NSString stringWithUTF8String:kMetalSource]
                   options:nil
                     error:&library_error];
  if (library == nil) {
    return Result<std::unique_ptr<MacMetalEdrPresenter>, Error>::failure(presenter_error(
        ErrorCode::presenter_failed,
        Retryability::never,
        {{"reason", "shader_compilation_failed"},
         {"nativeCode", std::to_string(library_error.code)},
         {"nativeDescription", ns_string_to_utf8(library_error.localizedDescription)}}));
  }

  NSError* pipeline_error = nil;
  id<MTLFunction> compute_function = [library newFunctionWithName:@"compose_overlay"];
  impl->compute_pipeline = [impl->device newComputePipelineStateWithFunction:compute_function
                                                                        error:&pipeline_error];
  if (impl->compute_pipeline == nil) {
    return Result<std::unique_ptr<MacMetalEdrPresenter>, Error>::failure(presenter_error(
        ErrorCode::presenter_failed,
        Retryability::never,
        {{"reason", "compute_pipeline_creation_failed"},
         {"nativeCode", std::to_string(pipeline_error.code)},
         {"nativeDescription", ns_string_to_utf8(pipeline_error.localizedDescription)}}));
  }

  MTLRenderPipelineDescriptor* render_descriptor = [[MTLRenderPipelineDescriptor alloc] init];
  render_descriptor.vertexFunction = [library newFunctionWithName:@"fullscreen_vertex"];
  render_descriptor.fragmentFunction = [library newFunctionWithName:@"fullscreen_fragment"];
  render_descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
  pipeline_error = nil;
  impl->render_pipeline = [impl->device newRenderPipelineStateWithDescriptor:render_descriptor
                                                                        error:&pipeline_error];
  if (impl->render_pipeline == nil) {
    return Result<std::unique_ptr<MacMetalEdrPresenter>, Error>::failure(presenter_error(
        ErrorCode::presenter_failed,
        Retryability::never,
        {{"reason", "render_pipeline_creation_failed"},
         {"nativeCode", std::to_string(pipeline_error.code)},
         {"nativeDescription", ns_string_to_utf8(pipeline_error.localizedDescription)}}));
  }
  return Result<std::unique_ptr<MacMetalEdrPresenter>, Error>::success(
      std::unique_ptr<MacMetalEdrPresenter>(new MacMetalEdrPresenter(std::move(impl))));
}

Result<MacMetalOverlayReadback, Error> MacMetalEdrPresenter::render_offscreen(
    const MacMetalOverlayRequest& request, bool fused, bool readback) {
  @autoreleasepool {
  const auto valid = validate_request(request);
  if (!valid) {
    return Result<MacMetalOverlayReadback, Error>::failure(valid.error());
  }
  id<MTLCommandBuffer> command_buffer = [impl_->command_queue commandBuffer];
  if (command_buffer == nil) {
    return Result<MacMetalOverlayReadback, Error>::failure(presenter_error(
        ErrorCode::presenter_failed,
        Retryability::after_recreate,
        {{"reason", "command_buffer_creation_failed"}}));
  }
  const auto composed = impl_->compose(request, command_buffer, !fused);
  if (!composed) {
    return Result<MacMetalOverlayReadback, Error>::failure(composed.error());
  }
  id<MTLTexture> target = composed.value();
  if (fused) {
    if (impl_->cached_output_texture == nil) {
      auto descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
          width:request.width_px height:request.height_px mipmapped:NO];
      descriptor.storageMode = MTLStorageModeShared;
      descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
      impl_->cached_output_texture = [impl_->device newTextureWithDescriptor:descriptor];
    }
    target = impl_->cached_output_texture;
    auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    auto encoder = [command_buffer renderCommandEncoderWithDescriptor:pass];
    if (!target || !encoder) return Result<MacMetalOverlayReadback, Error>::failure(
        presenter_error(ErrorCode::presenter_failed, Retryability::never, {{"reason", "offscreen_render_failed"}}));
    [encoder setRenderPipelineState:impl_->render_pipeline];
    [encoder setFragmentTexture:composed.value() atIndex:0];
    [encoder setFragmentBytes:&impl_->parameters length:sizeof(impl_->parameters) atIndex:0];
    [encoder setFragmentBuffer:impl_->clean_spans offset:0 atIndex:1];
    [encoder setFragmentBuffer:impl_->clean_samples offset:0 atIndex:2];
    [encoder setFragmentBuffer:impl_->clean_rows offset:0 atIndex:3];
    [encoder setFragmentBuffer:impl_->cached_source_buffer offset:impl_->source_buffer_offset atIndex:4];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
  }
  [command_buffer commit];
  [command_buffer waitUntilCompleted];
  if (command_buffer.status != MTLCommandBufferStatusCompleted) {
    return Result<MacMetalOverlayReadback, Error>::failure(presenter_error(
        ErrorCode::presenter_failed,
        Retryability::after_recreate,
        {{"reason", "gpu_execution_failed"},
         {"nativeCode", std::to_string(command_buffer.error.code)},
         {"nativeDescription", ns_string_to_utf8(command_buffer.error.localizedDescription)}}));
  }

  const double gpu_ms = (command_buffer.GPUEndTime - command_buffer.GPUStartTime) * 1000.0;
  auto native_ready = verify_native_content(request);
  if (!native_ready) return Result<MacMetalOverlayReadback, Error>::failure(native_ready.error());
  if (!readback) return Result<MacMetalOverlayReadback, Error>::success({
      request.width_px, request.height_px, {}, kTextureFormat, kLayerColorSpace, kOpticalOutputScaleNits, gpu_ms, impl_->source_aliases_cpu});
  std::vector<std::uint16_t> half_samples(request.width_px * request.height_px * 4U);
  [target getBytes:half_samples.data()
                 bytesPerRow:request.width_px * 4U * sizeof(std::uint16_t)
                  fromRegion:MTLRegionMake2D(0, 0, request.width_px, request.height_px)
                 mipmapLevel:0];
  std::vector<float> float_samples;
  float_samples.reserve(half_samples.size());
  for (const auto sample : half_samples) {
    float_samples.push_back(half_to_float(sample));
  }
  return Result<MacMetalOverlayReadback, Error>::success(MacMetalOverlayReadback{
      request.width_px,
      request.height_px,
      std::move(float_samples),
      kTextureFormat,
      kLayerColorSpace,
      kOpticalOutputScaleNits,
      gpu_ms,
      impl_->source_aliases_cpu,
  });
  }
}

Result<bool, Error> MacMetalEdrPresenter::configure_target_layer(
    void* native_metal_layer,
    const MacMetalSurfaceRange surface_range) {
  if (native_metal_layer == nullptr) {
    return Result<bool, Error>::failure(presenter_error(
        ErrorCode::invalid_input,
        Retryability::never,
        {{"reason", "missing_metal_layer"}}));
  }
  if (![NSThread isMainThread]) {
    return Result<bool, Error>::failure(presenter_error(
        ErrorCode::presenter_failed,
        Retryability::after_recreate,
        {{"reason", "layer_configuration_requires_main_thread"}}));
  }
  CAMetalLayer* layer = (__bridge CAMetalLayer*)native_metal_layer;
  impl_->configure_layer(layer, surface_range);
  return Result<bool, Error>::success(true);
}

Result<MacMetalPresentReceipt, Error> MacMetalEdrPresenter::present_to_layer(
    void* native_metal_layer,
    const MacMetalOverlayRequest& request) {
  @autoreleasepool {
  const auto valid = validate_request(request);
  if (!valid) {
    return Result<MacMetalPresentReceipt, Error>::failure(valid.error());
  }
  if (native_metal_layer == nullptr) {
    return Result<MacMetalPresentReceipt, Error>::failure(presenter_error(
        ErrorCode::invalid_input,
        Retryability::never,
        {{"reason", "missing_metal_layer"}}));
  }
  CAMetalLayer* layer = (__bridge CAMetalLayer*)native_metal_layer;
  if (!impl_->is_layer_configured(layer, request.target_surface_range)) {
    return Result<MacMetalPresentReceipt, Error>::failure(presenter_error(
        ErrorCode::presenter_failed,
        Retryability::after_recreate,
        {{"reason", "layer_not_preconfigured_on_main_thread"}}));
  }
  // Production samples the already-normalized Linear P3 source and clean ink;
  // the fragment adds only display UI. Legacy encoded fixtures decode there.
  // EDR 1.0 remains 1.0; no PQ or BT.2020 interpretation belongs to this layer.

  id<CAMetalDrawable> drawable = [layer nextDrawable];
  if (drawable == nil) {
    return Result<MacMetalPresentReceipt, Error>::failure(presenter_error(
        ErrorCode::presenter_failed,
        Retryability::after_recreate,
        {{"reason", "drawable_unavailable"}}));
  }
  id<MTLTexture> drawable_texture = drawable.texture;
  const auto drawable_width = drawable_texture.width;
  const auto drawable_height = drawable_texture.height;
  id<MTLCommandBuffer> command_buffer = [impl_->command_queue commandBuffer];
  const auto composed = impl_->compose(request, command_buffer, false);
  if (!composed) {
    return Result<MacMetalPresentReceipt, Error>::failure(composed.error());
  }
  MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = drawable_texture;
  pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  id<MTLRenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:pass];
  if (encoder == nil) {
    return Result<MacMetalPresentReceipt, Error>::failure(presenter_error(
        ErrorCode::presenter_failed,
        Retryability::after_recreate,
        {{"reason", "render_encoder_creation_failed"}}));
  }
  [encoder setRenderPipelineState:impl_->render_pipeline];
  [encoder setFragmentTexture:composed.value() atIndex:0];
  [encoder setFragmentBytes:&impl_->parameters length:sizeof(impl_->parameters) atIndex:0];
  [encoder setFragmentBuffer:impl_->clean_spans offset:0 atIndex:1];
  [encoder setFragmentBuffer:impl_->clean_samples offset:0 atIndex:2];
    [encoder setFragmentBuffer:impl_->clean_rows offset:0 atIndex:3];
    [encoder setFragmentBuffer:impl_->cached_source_buffer offset:impl_->source_buffer_offset atIndex:4];
  [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  [encoder endEncoding];
  [command_buffer presentDrawable:drawable];
  [command_buffer commit];
  [command_buffer waitUntilCompleted];
  if (command_buffer.status != MTLCommandBufferStatusCompleted) {
    return Result<MacMetalPresentReceipt, Error>::failure(presenter_error(
        ErrorCode::presenter_failed,
        Retryability::after_recreate,
        {{"reason", "gpu_presentation_failed"},
         {"nativeCode", std::to_string(command_buffer.error.code)},
         {"nativeDescription", ns_string_to_utf8(command_buffer.error.localizedDescription)}}));
  }
  auto native_ready = verify_native_content(request);
  if (!native_ready) return Result<MacMetalPresentReceipt, Error>::failure(native_ready.error());
  return Result<MacMetalPresentReceipt, Error>::success(MacMetalPresentReceipt{
      request.width_px,
      request.height_px,
      drawable_width,
      drawable_height,
      kTextureFormat,
      kLayerColorSpace,
      kOpticalOutputScaleNits,
  });
  }
}

}  // namespace hdrshot
