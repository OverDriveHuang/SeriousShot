#pragma once

#include "core/error.hpp"
#include "core/geometry.hpp"
#include "core/linear_pixel_storage.hpp"
#include "core/linear_source.hpp"
#include "core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace hdrshot {

struct CleanContentSnapshot;

enum class MacMetalSurfaceRange : std::uint8_t { sdr, edr };

struct MacMetalOverlayRequest {
  std::size_t width_px{};
  std::size_t height_px{};
  std::shared_ptr<const std::vector<std::uint16_t>> rgba_half_extended_p3;
  PixelRect selection_px{};
  float outside_linear_dim_factor{0.35F};
  float ui_white_edr{2.03F};
  std::int32_t ui_border_width_px{2};
  MacMetalSurfaceRange target_surface_range{MacMetalSurfaceRange::edr};
  std::uint64_t source_frame_key{};
  bool source_is_linear{}; // Production passes the frame transfer; old fixtures default encoded.
  std::shared_ptr<const LinearFloatPixels> rgba_float_linear_p3;
  std::shared_ptr<const CleanContentSnapshot> clean_content;
  LinearSourceRef linear_source;
};

struct MacMetalOverlayReadback {
  std::size_t width_px{};
  std::size_t height_px{};
  std::vector<float> rgba_linear_display_p3;
  std::string texture_format;
  std::string layer_color_space;
  double optical_output_scale_nits{};
  double gpu_elapsed_ms{};
  bool source_aliases_cpu{};
};

struct MacMetalPresentReceipt {
  std::size_t source_width_px{};
  std::size_t source_height_px{};
  std::size_t drawable_width_px{};
  std::size_t drawable_height_px{};
  std::string texture_format;
  std::string layer_color_space;
  double optical_output_scale_nits{};
};

class MacMetalEdrPresenter {
 public:
  static Result<std::unique_ptr<MacMetalEdrPresenter>, Error> create();

  ~MacMetalEdrPresenter();
  MacMetalEdrPresenter(MacMetalEdrPresenter&&) noexcept;
  MacMetalEdrPresenter& operator=(MacMetalEdrPresenter&&) noexcept;
  MacMetalEdrPresenter(const MacMetalEdrPresenter&) = delete;
  MacMetalEdrPresenter& operator=(const MacMetalEdrPresenter&) = delete;

  [[nodiscard]] Result<MacMetalOverlayReadback, Error> render_offscreen(
      const MacMetalOverlayRequest& request, bool fused = false, bool readback = true);

  // Configures CAMetalLayer properties. This must be called by the UI host on
  // the macOS main thread before any background presentation work starts.
  [[nodiscard]] Result<bool, Error> configure_target_layer(
      void* native_metal_layer,
      MacMetalSurfaceRange surface_range);

  // native_metal_layer must point to a CAMetalLayer owned by the UI host.
  [[nodiscard]] Result<MacMetalPresentReceipt, Error> present_to_layer(
      void* native_metal_layer,
      const MacMetalOverlayRequest& request);

 private:
  struct Impl;
  explicit MacMetalEdrPresenter(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace hdrshot
