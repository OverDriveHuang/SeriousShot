#pragma once

#include "application/default_save_workflow.hpp"
#include "core/frame.hpp"
#include "application/clean_content_cache.hpp"
#include "domain/annotation/annotation_document.hpp"
#include "domain/annotation/annotation_renderer.hpp"
#include "domain/geometry/selection_model.hpp"
#include "domain/png/png_encoder.hpp"
#include "ports/export_ports.hpp"
#include "ports/export_pixel_processor_port.hpp"
#include "ports/ultra_hdr_ports.hpp"
#include "ports/source_range_probe_port.hpp"

#include <string>
#include <memory>
#include <variant>
#include <vector>

namespace hdrshot {

struct ExportSnapshot {
  SessionId session_id{};
  OperationId operation_id{};
  FrozenDesktopRef frozen_desktop;
  SelectionSnapshot selection{};
  AnnotationDocumentSnapshot annotations{};
  std::shared_ptr<const AnnotationRenderPlan> annotation_render_plan;
  std::string default_folder;
  DisplayId target_display_id{};
  SaveFormat save_format{SaveFormat::png_display_p3_dual_range};
  PqDiffuseWhite pq_diffuse_white{PqDiffuseWhite::nits_203};
  HdrPqPrecision hdr_pq_precision{HdrPqPrecision::bits_10};
  UltraHdrJpegQuality ultra_hdr_jpeg_quality{UltraHdrJpegQuality::balanced};
  CleanContentRef clean_content;
  DocumentRevision clean_document_revision{};
};

struct ExportReceipt {
  std::variant<ClipboardReceipt, FileReceipt, UserCancelled> destination;
  FrameId source_frame_id{};
  SelectionRevision source_selection_revision{};
  DocumentRevision source_document_revision{};
  ColorEncoding output_encoding{};
  LuminanceClipManifest luminance_clip{};
};

struct EncodedArtifact {
  std::vector<std::uint8_t> bytes;
  SaveFormat save_format{SaveFormat::png_display_p3_dual_range};
  std::string mime_type{"image/png"};
  std::string extension{".png"};
};

// Destination-independent output of crop -> annotation render -> color conversion
// -> selected-format encoding. Pixel transforms are supplied by platform GPU ports.
struct PreparedExport {
  EncodedArtifact artifact;
  FrameId source_frame_id{};
  SelectionRevision source_selection_revision{};
  DocumentRevision source_document_revision{};
  // PNG pixel encoding; for UHDR the reconstructed HDR alternate intent.
  // UHDR SDR base remains P3/sRGB, and encoder raw input remains linear P3.
  ColorEncoding output_encoding{};
  LuminanceClipManifest luminance_clip{};
};

class ExportWorkflow {
 public:
  [[nodiscard]] static Result<PreparedExport, Error> prepare(
      const ExportSnapshot& snapshot,
      TextRasterizerPort* text_rasterizer = nullptr,
      ExportPixelProcessorPort* pixel_processor = nullptr,
      UltraHdrInputRendererPort* ultra_hdr_renderer = nullptr,
      UltraHdrEncoderPort* ultra_hdr_encoder = nullptr,
      SourceRangeProbePort* range_probe = nullptr);

  [[nodiscard]] static Result<ExportReceipt, Error> copy_prepared(
      const PreparedExport& prepared,
      ClipboardPort& clipboard);

  [[nodiscard]] static Result<ExportReceipt, Error> save_default_prepared(
      const PreparedExport& prepared,
      const std::string& default_folder,
      const ClockPort& clock,
      FileStorePort& file_store);

  [[nodiscard]] static Result<ChooseSavePathOutcome, Error> choose_save_as_path(
      const std::string& default_folder,
      const ClockPort& clock,
      FileDialogPort& file_dialog,
      DisplayId owner_display_id = {},
      SaveFormat save_format = SaveFormat::png_display_p3_dual_range);

  [[nodiscard]] static Result<ExportReceipt, Error> save_as_prepared(
      const PreparedExport& prepared,
      const ChooseSavePathOutcome& destination,
      FileStorePort& file_store);

  [[nodiscard]] static Result<ExportReceipt, Error> cancel(
      const ExportSnapshot& snapshot);

  [[nodiscard]] static Result<ExportReceipt, Error> copy(
      const ExportSnapshot& snapshot,
      ClipboardPort& clipboard,
      TextRasterizerPort* text_rasterizer = nullptr,
      ExportPixelProcessorPort* pixel_processor = nullptr,
      UltraHdrInputRendererPort* ultra_hdr_renderer = nullptr,
      UltraHdrEncoderPort* ultra_hdr_encoder = nullptr,
      SourceRangeProbePort* range_probe = nullptr);

  [[nodiscard]] static Result<ExportReceipt, Error> save_default(
      const ExportSnapshot& snapshot,
      const ClockPort& clock,
      FileStorePort& file_store,
      TextRasterizerPort* text_rasterizer = nullptr,
      ExportPixelProcessorPort* pixel_processor = nullptr,
      UltraHdrInputRendererPort* ultra_hdr_renderer = nullptr,
      UltraHdrEncoderPort* ultra_hdr_encoder = nullptr,
      SourceRangeProbePort* range_probe = nullptr);

  [[nodiscard]] static Result<ExportReceipt, Error> save_as(
      const ExportSnapshot& snapshot,
      const ClockPort& clock,
      FileDialogPort& file_dialog,
      FileStorePort& file_store,
      TextRasterizerPort* text_rasterizer = nullptr,
      ExportPixelProcessorPort* pixel_processor = nullptr,
      UltraHdrInputRendererPort* ultra_hdr_renderer = nullptr,
      UltraHdrEncoderPort* ultra_hdr_encoder = nullptr,
      SourceRangeProbePort* range_probe = nullptr);

  [[nodiscard]] static Result<ExportReceipt, Error> save_as_to_destination(
      const ExportSnapshot& snapshot,
      const ChooseSavePathOutcome& destination,
      FileStorePort& file_store,
      TextRasterizerPort* text_rasterizer = nullptr,
      ExportPixelProcessorPort* pixel_processor = nullptr,
      UltraHdrInputRendererPort* ultra_hdr_renderer = nullptr,
      UltraHdrEncoderPort* ultra_hdr_encoder = nullptr,
      SourceRangeProbePort* range_probe = nullptr);
};

}  // namespace hdrshot
