#include "application/export_workflow.hpp"
#include "core/build_metadata.hpp"

#include "core/byte_sink.hpp"
#include "domain/color/display_p3_icc_profile.hpp"
#include "domain/frame/frame_cropper.hpp"
#include "domain/output/output_classifier.hpp"
#include "domain/output/output_row_producer.hpp"
#include "domain/output/source_range_probe.hpp"
#include "domain/png/streaming_png_encoder.hpp"
#include "domain/annotation/annotation_pixel_plan_validator.hpp"
#include "domain/annotation/annotation_compositing.hpp"

#include <cstdint>
#include <cmath>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace hdrshot {
namespace {

Error missing_frozen_desktop() {
  return Error{ErrorCode::invalid_input, "ExportWorkflow", Retryability::never,
               {{"field", "frozen_desktop"}}};
}

class VectorByteSink final : public ByteSink {
 public:
  Result<std::size_t, Error> write(const std::span<const std::uint8_t> bytes) override {
    bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
    return Result<std::size_t, Error>::success(bytes.size());
  }
  std::vector<std::uint8_t> take() { return std::move(bytes_); }

 private:
  std::vector<std::uint8_t> bytes_;
};

struct EncodedSnapshotReceipt {
  std::size_t bytes_written{};
  FrameId source_frame_id{};
  SelectionRevision source_selection_revision{};
  DocumentRevision source_document_revision{};
  ColorEncoding output_encoding{};
  LuminanceClipManifest luminance_clip{};
};

ExportReceipt receipt(
    std::variant<ClipboardReceipt, FileReceipt, UserCancelled> destination,
    const PreparedExport& prepared) {
  return ExportReceipt{std::move(destination), prepared.source_frame_id,
                       prepared.source_selection_revision,
                       prepared.source_document_revision, prepared.output_encoding,
                       prepared.luminance_clip};
}

ExportReceipt receipt(
    std::variant<ClipboardReceipt, FileReceipt, UserCancelled> destination,
    const EncodedSnapshotReceipt& encoded) {
  return ExportReceipt{std::move(destination), encoded.source_frame_id,
                       encoded.source_selection_revision,
                       encoded.source_document_revision, encoded.output_encoding,
                       encoded.luminance_clip};
}

Result<SelectionRoiView, Error> selection_view(const ExportSnapshot& snapshot) {
  if (snapshot.frozen_desktop == nullptr) {
    return Result<SelectionRoiView, Error>::failure(missing_frozen_desktop());
  }
  return snapshot.target_display_id.value == 0U
      ? FrameCropper::view(*snapshot.frozen_desktop, snapshot.selection)
      : FrameCropper::view_display(
            *snapshot.frozen_desktop, snapshot.target_display_id, snapshot.selection);
}

Result<EncodedSnapshotReceipt, Error> encode_snapshot_to_sink(
    const ExportSnapshot& snapshot,
    TextRasterizerPort* const text_rasterizer,
    ExportPixelProcessorPort* const pixel_processor,
    UltraHdrInputRendererPort* const ultra_hdr_renderer,
    UltraHdrEncoderPort* const ultra_hdr_encoder,
    ByteSink& sink,
    SourceRangeProbePort* range_probe) {
  auto frame = selection_view(snapshot);
  if (!frame) {
    return Result<EncodedSnapshotReceipt, Error>::failure(frame.error());
  }
  if (snapshot.save_format != SaveFormat::png_display_p3_dual_range &&
      snapshot.save_format != SaveFormat::ultra_hdr_jpeg) {
    return Result<EncodedSnapshotReceipt, Error>::failure(Error{
        ErrorCode::unsupported_encoding,
        "ExportWorkflow",
        Retryability::never,
        {{"reason", "unsupported_save_format"}}});
  }
  if (snapshot.annotation_render_plan != nullptr &&
      (snapshot.annotation_render_plan->source_document_revision != snapshot.annotations.revision ||
       snapshot.annotation_render_plan->source_selection_rect_px !=
           snapshot.selection.desktop_rect)) {
    return Result<EncodedSnapshotReceipt, Error>::failure(Error{
        ErrorCode::state_inconsistent, "ExportWorkflow", Retryability::never,
        {{"reason", "stale_annotation_render_plan"}}});
  }
  std::optional<AnnotationRenderPlan> built_plan;
  const AnnotationRenderPlan* render_plan = snapshot.annotation_render_plan.get();
  if (render_plan == nullptr) {
    auto result = AnnotationRenderPlanner::build(
        snapshot.annotations, snapshot.selection.desktop_rect,
        frame.value().point_pixel_scale, text_rasterizer);
    if (!result) {
      return Result<EncodedSnapshotReceipt, Error>::failure(result.error());
    }
    built_plan = std::move(result.value());
    render_plan = &*built_plan;
  }
  if (snapshot.clean_content &&
      (snapshot.clean_content->source != snapshot.frozen_desktop ||
       snapshot.clean_content->display_id != snapshot.target_display_id ||
       snapshot.clean_document_revision != snapshot.annotations.revision)) {
    return Result<EncodedSnapshotReceipt, Error>::failure(Error{
        ErrorCode::state_inconsistent, "ExportWorkflow", Retryability::never,
        {{"reason", "stale_clean_content"}}});
  }
  auto pixel_plan = snapshot.clean_content
      ? snapshot.clean_content->roi_plan(snapshot.selection.desktop_rect, snapshot.annotations.revision)
      : AnnotationRenderPlanner::build_pixel_plan(*render_plan);
  if (!pixel_plan) {
    return Result<EncodedSnapshotReceipt, Error>::failure(pixel_plan.error());
  }
  if (!AnnotationPixelPlanValidator::valid(pixel_plan.value())) {
    return Result<EncodedSnapshotReceipt, Error>::failure(Error{
        ErrorCode::state_inconsistent, "ExportWorkflow", Retryability::never,
        {{"reason", "invalid_pixel_ownership"}}});
  }
  // Explicit CPU fallback owns one selected ROI only. The Mac production ports
  // consume native resources and never enter this branch.
  std::optional<CanonicalFrameView> cpu_roi;
  if (snapshot.save_format == SaveFormat::png_display_p3_dual_range && pixel_processor == nullptr) {
    if (frame.value().linear_source) {
      auto read = FrameCropper::read_cpu_region(frame.value());
      if (!read) return Result<EncodedSnapshotReceipt, Error>::failure(read.error());
      cpu_roi = std::move(read.value());
      frame = Result<SelectionRoiView, Error>::success(FrameCropper::view(*cpu_roi));
    }
    auto cpu_plan = materialize_annotation_plan(pixel_plan.value());
    if (!cpu_plan) return Result<EncodedSnapshotReceipt, Error>::failure(cpu_plan.error());
    pixel_plan = std::move(cpu_plan);
  }
  if (snapshot.save_format == SaveFormat::ultra_hdr_jpeg) {
    if (ultra_hdr_renderer == nullptr || ultra_hdr_encoder == nullptr ||
        !valid_ultra_hdr_jpeg_quality(snapshot.ultra_hdr_jpeg_quality)) {
      return Result<EncodedSnapshotReceipt, Error>::failure(Error{
          ErrorCode::unsupported_encoding,
          "ExportWorkflow",
          Retryability::never,
          {{"reason", "ultra_hdr_ports_or_quality_unavailable"}}});
    }
    auto linear = ultra_hdr_renderer->render(UltraHdrInputRenderRequest{
        &frame.value(), &pixel_plan.value(), kUltraHdrReferenceWhiteNits});
    if (!linear) {
      return Result<EncodedSnapshotReceipt, Error>::failure(linear.error());
    }
    const auto expected_samples =
        static_cast<std::size_t>(frame.value().size_px.width) *
        static_cast<std::size_t>(frame.value().size_px.height) * 4U;
    if (linear.value().size_px != frame.value().size_px ||
        linear.value().rgba_half.size() != expected_samples ||
        linear.value().reference_white_nits != kUltraHdrReferenceWhiteNits ||
        !std::isfinite(linear.value().maximum_linear_component) ||
        linear.value().maximum_linear_component < 0.0 ||
        (linear.value().source_visible_maximum_linear_component &&
         (!std::isfinite(*linear.value().source_visible_maximum_linear_component) ||
          *linear.value().source_visible_maximum_linear_component < 0.0 ||
          *linear.value().source_visible_maximum_linear_component > linear.value().maximum_linear_component))) {
      return Result<EncodedSnapshotReceipt, Error>::failure(Error{
          ErrorCode::state_inconsistent,
          "ExportWorkflow",
          Retryability::never,
          {{"reason", "ultra_hdr_renderer_contract_mismatch"}}});
    }
    auto encoded = ultra_hdr_encoder->encode(UltraHdrEncodeRequest{
        &linear.value(), snapshot.ultra_hdr_jpeg_quality, software_identifier()});
    if (!encoded ||
        encoded.value().kind != jpeg_output_kind(linear.value()) ||
        encoded.value().bytes.size() < 4U ||
        encoded.value().bytes[0] != 0xFFU || encoded.value().bytes[1] != 0xD8U) {
      return !encoded
          ? Result<EncodedSnapshotReceipt, Error>::failure(encoded.error())
          : Result<EncodedSnapshotReceipt, Error>::failure(Error{
                ErrorCode::state_inconsistent,
                "ExportWorkflow",
                Retryability::never,
                {{"reason", "ultra_hdr_encoder_contract_mismatch"}}});
    }
    std::size_t written = 0U;
    while (written < encoded.value().bytes.size()) {
      auto accepted = sink.write(std::span<const std::uint8_t>{
          encoded.value().bytes.data() + written,
          encoded.value().bytes.size() - written});
      if (!accepted) {
        return Result<EncodedSnapshotReceipt, Error>::failure(accepted.error());
      }
      if (accepted.value() == 0U ||
          accepted.value() > encoded.value().bytes.size() - written) {
        return Result<EncodedSnapshotReceipt, Error>::failure(Error{
            ErrorCode::state_inconsistent,
            "ExportWorkflow",
            Retryability::never,
            {{"reason", "invalid_sink_progress"}}});
      }
      written += accepted.value();
    }
    const bool jpeg_is_hdr = encoded.value().kind == JpegOutputKind::ultra_hdr;
    return Result<EncodedSnapshotReceipt, Error>::success(EncodedSnapshotReceipt{
        written, frame.value().source_frame_id,
        frame.value().selection_revision, render_plan->source_document_revision,
        ColorEncoding{ColorPrimaries::display_p3,
                      jpeg_is_hdr ? TransferFunction::pq : TransferFunction::srgb,
                      AlphaMode::opaque,
                      jpeg_is_hdr ? kUltraHdrReferenceWhiteNits : 0.0}, {}});
  }
  const RangeProbeOptimization optimization{
      frame.value().display_dynamic_range == DisplayDynamicRange::sdr};
  auto range_fit = range_probe != nullptr
      ? range_probe->probe(frame.value(), pixel_plan.value(), optimization)
      : SourceRangeProbe::probe(frame.value(), pixel_plan.value(), optimization);
  if (!range_fit) {
    return Result<EncodedSnapshotReceipt, Error>::failure(range_fit.error());
  }
  const auto output_plan = OutputClassifier::classify(ClassifyOutputRequest{
      range_fit.value()});
  if (!output_plan) {
    return Result<EncodedSnapshotReceipt, Error>::failure(output_plan.error());
  }
  const auto is_hdr =
      output_plan.value().encoding_intent == EncodingIntent::hdr_pq;
  if (!valid_hdr_pq_precision(snapshot.hdr_pq_precision)) {
    return Result<EncodedSnapshotReceipt, Error>::failure(Error{
        ErrorCode::invalid_input,
        "ExportWorkflow",
        Retryability::never,
        {{"field", "hdr_pq_precision"}}});
  }
  const auto target_diffuse_white_nits = pq_diffuse_white_nits(
      snapshot.pq_diffuse_white);
  LuminanceClipManifest luminance_clip{};
  std::optional<ExportPixelProcessResult> processed;
  if (pixel_processor != nullptr) {
    auto result = pixel_processor->process(ExportPixelProcessRequest{
        &frame.value(), &pixel_plan.value(), output_plan.value(),
        snapshot.pq_diffuse_white, snapshot.hdr_pq_precision});
    if (!result) {
      return Result<EncodedSnapshotReceipt, Error>::failure(result.error());
    }
    const auto expected_samples = static_cast<std::size_t>(frame.value().size_px.width) *
        static_cast<std::size_t>(frame.value().size_px.height) * 3U;
    const auto expected_reference_white =
        is_hdr
        ? target_diffuse_white_nits
        : 0.0;
    if (result.value().size_px != frame.value().size_px ||
        result.value().rgb_u16.size() != expected_samples ||
        result.value().output_encoding.primaries != ColorPrimaries::display_p3 ||
        result.value().output_encoding.alpha != AlphaMode::opaque ||
        result.value().output_encoding.source_reference_white_nits !=
            expected_reference_white ||
        result.value().output_encoding.transfer !=
            (is_hdr
                ? TransferFunction::pq
                : TransferFunction::srgb) ||
        (is_hdr &&
         (!result.value().content_light.has_value() ||
          result.value().content_light->pixel_count !=
              static_cast<std::uint64_t>(frame.value().size_px.width) *
                  static_cast<std::uint64_t>(frame.value().size_px.height))) ||
        (!is_hdr && result.value().content_light.has_value())) {
      return Result<EncodedSnapshotReceipt, Error>::failure(Error{
          ErrorCode::state_inconsistent,
          "ExportWorkflow",
          Retryability::never,
          {{"reason", "pixel_processor_contract_mismatch"}}});
    }
    processed = std::move(result.value());
    luminance_clip = processed->luminance_clip;
  } else if (is_hdr) {
    auto result = OutputRowProducer::produce_all(
        frame.value(), pixel_plan.value(), output_plan.value(),
        target_diffuse_white_nits, snapshot.hdr_pq_precision);
    if (!result) {
      return Result<EncodedSnapshotReceipt, Error>::failure(result.error());
    }
    processed = ExportPixelProcessResult{
        result.value().size_px,
        result.value().encoding,
        std::move(result.value().rgb_u16),
        LuminanceClipManifest{
            result.value().clipped_pixel_count,
            result.value().clipped_channel_count},
        result.value().content_light,
    };
    luminance_clip = processed->luminance_clip;
  }
  std::optional<ContentLightLevelInfo> content_light;
  if (is_hdr) {
    if (!processed.has_value() || !processed->content_light.has_value()) {
      return Result<EncodedSnapshotReceipt, Error>::failure(Error{
          ErrorCode::state_inconsistent,
          "ExportWorkflow",
          Retryability::never,
          {{"reason", "missing_hdr_content_light_statistics"}}});
    }
    content_light = finalize_content_light(*processed->content_light);
    if (!content_light.has_value()) {
      return Result<EncodedSnapshotReceipt, Error>::failure(Error{
          ErrorCode::invalid_color_contract,
          "ExportWorkflow",
          Retryability::never,
          {{"reason", "invalid_hdr_content_light_statistics"}}});
    }
  }
  const auto color_metadata = is_hdr
      ? PngColorMetadata{kDisplayP3PqFullRange, std::nullopt, content_light}
      : PngColorMetadata{
            kDisplayP3SrgbFullRange,
            IccProfilePayload{"Display P3", DisplayP3IccProfile::bytes()},
            std::nullopt};
  auto png = StreamingPngEncoder::encode_16bit_rgb_to_sink(
      EncodeStreamingPng16Request{
          static_cast<std::uint32_t>(frame.value().size_px.width),
          static_cast<std::uint32_t>(frame.value().size_px.height),
          color_metadata,
          [&](const std::uint32_t y) -> Result<std::vector<std::uint16_t>, Error> {
            if (processed.has_value()) {
              const auto row_samples = static_cast<std::size_t>(
                  frame.value().size_px.width) * 3U;
              const auto offset = static_cast<std::size_t>(y) * row_samples;
              return Result<std::vector<std::uint16_t>, Error>::success(
                  std::vector<std::uint16_t>{
                      processed->rgb_u16.begin() + static_cast<std::ptrdiff_t>(offset),
                      processed->rgb_u16.begin() +
                          static_cast<std::ptrdiff_t>(offset + row_samples)});
            }
            auto row = OutputRowProducer::produce_row(
                frame.value(), pixel_plan.value(), output_plan.value(),
                static_cast<std::int32_t>(y), target_diffuse_white_nits,
                snapshot.hdr_pq_precision);
            if (!row) {
              return Result<std::vector<std::uint16_t>, Error>::failure(row.error());
            }
            luminance_clip.clipped_pixel_count += row.value().clipped_pixel_count;
            luminance_clip.clipped_channel_count += row.value().clipped_channel_count;
            return Result<std::vector<std::uint16_t>, Error>::success(
                std::move(row.value().rgb_u16));
          }, software_identifier(),
          is_hdr ? hdr_pq_precision_bits(snapshot.hdr_pq_precision) : std::uint8_t{16}},
      sink);
  if (!png) {
    return Result<EncodedSnapshotReceipt, Error>::failure(png.error());
  }
  return Result<EncodedSnapshotReceipt, Error>::success(EncodedSnapshotReceipt{
      png.value().bytes_written, frame.value().source_frame_id,
      frame.value().selection_revision, render_plan->source_document_revision,
      ColorEncoding{
          ColorPrimaries::display_p3,
          is_hdr
              ? TransferFunction::pq
              : TransferFunction::srgb,
          AlphaMode::opaque,
          is_hdr
              ? target_diffuse_white_nits
              : 0.0},
      luminance_clip});
}

Result<ExportReceipt, Error> encode_and_commit(
    const ExportSnapshot& snapshot,
    TextRasterizerPort* const text_rasterizer,
    ExportPixelProcessorPort* const pixel_processor,
    UltraHdrInputRendererPort* const ultra_hdr_renderer,
    UltraHdrEncoderPort* const ultra_hdr_encoder,
    std::unique_ptr<AtomicFileSink> sink,
    SourceRangeProbePort* range_probe) {
  auto encoded = encode_snapshot_to_sink(
      snapshot, text_rasterizer, pixel_processor, ultra_hdr_renderer,
      ultra_hdr_encoder, *sink, range_probe);
  if (!encoded) {
    sink->abort();
    return Result<ExportReceipt, Error>::failure(encoded.error());
  }
  auto committed = sink->commit();
  if (!committed) {
    sink->abort();
    return Result<ExportReceipt, Error>::failure(committed.error());
  }
  if (committed.value().bytes_written != encoded.value().bytes_written) {
    return Result<ExportReceipt, Error>::failure(Error{
        ErrorCode::state_inconsistent, "ExportWorkflow", Retryability::never,
        {{"reason", "file_receipt_byte_count_mismatch"}}});
  }
  return Result<ExportReceipt, Error>::success(
      receipt(committed.value(), encoded.value()));
}

}  // namespace

Result<PreparedExport, Error> ExportWorkflow::prepare(
    const ExportSnapshot& snapshot,
    TextRasterizerPort* const text_rasterizer,
    ExportPixelProcessorPort* const pixel_processor,
    UltraHdrInputRendererPort* const ultra_hdr_renderer,
    UltraHdrEncoderPort* const ultra_hdr_encoder,
    SourceRangeProbePort* range_probe) {
  VectorByteSink sink;
  auto encoded = encode_snapshot_to_sink(
      snapshot, text_rasterizer, pixel_processor, ultra_hdr_renderer,
      ultra_hdr_encoder, sink, range_probe);
  if (!encoded) {
    return Result<PreparedExport, Error>::failure(encoded.error());
  }
  auto bytes = sink.take();
  if (bytes.size() != encoded.value().bytes_written) {
    return Result<PreparedExport, Error>::failure(Error{
        ErrorCode::state_inconsistent, "ExportWorkflow", Retryability::never,
        {{"reason", "memory_sink_byte_count_mismatch"}}});
  }
  return Result<PreparedExport, Error>::success(PreparedExport{
      EncodedArtifact{
          std::move(bytes),
          snapshot.save_format,
          snapshot.save_format == SaveFormat::ultra_hdr_jpeg
              ? "image/jpeg"
              : "image/png",
          snapshot.save_format == SaveFormat::ultra_hdr_jpeg ? ".jpg" : ".png"},
      encoded.value().source_frame_id,
      encoded.value().source_selection_revision,
      encoded.value().source_document_revision, encoded.value().output_encoding,
      encoded.value().luminance_clip});
}

Result<ExportReceipt, Error> ExportWorkflow::copy_prepared(
    const PreparedExport& prepared,
    ClipboardPort& clipboard) {
  const auto written = clipboard.write(
      ClipboardWriteRequest{
          prepared.artifact.bytes, {prepared.artifact.mime_type}});
  if (!written) {
    return Result<ExportReceipt, Error>::failure(written.error());
  }
  return Result<ExportReceipt, Error>::success(receipt(written.value(), prepared));
}

Result<ExportReceipt, Error> ExportWorkflow::save_default_prepared(
    const PreparedExport& prepared,
    const std::string& default_folder,
    const ClockPort& clock,
    FileStorePort& file_store) {
  const auto saved = DefaultSaveWorkflow::save(
      DefaultSaveRequest{
          prepared.artifact.bytes, default_folder, 10000,
          prepared.artifact.save_format},
      clock, file_store);
  if (!saved) {
    return Result<ExportReceipt, Error>::failure(saved.error());
  }
  return Result<ExportReceipt, Error>::success(receipt(saved.value(), prepared));
}

Result<ChooseSavePathOutcome, Error> ExportWorkflow::choose_save_as_path(
    const std::string& default_folder,
    const ClockPort& clock,
    FileDialogPort& file_dialog,
    const DisplayId owner_display_id,
    const SaveFormat save_format) {
  const auto suggested = FilenamePolicy::make_name(
      clock.now_local(), 0, save_format);
  if (!suggested) {
    return Result<ChooseSavePathOutcome, Error>::failure(suggested.error());
  }
  return file_dialog.choose_save_path(ChooseSavePathRequest{
      suggested.value(), default_folder, owner_display_id, save_format});
}

Result<ExportReceipt, Error> ExportWorkflow::save_as_prepared(
    const PreparedExport& prepared,
    const ChooseSavePathOutcome& destination,
    FileStorePort& file_store) {
  if (std::holds_alternative<UserCancelled>(destination)) {
    return Result<ExportReceipt, Error>::success(receipt(UserCancelled{}, prepared));
  }
  const auto& path = std::get<ChosenPath>(destination).exact_path;
  const auto saved = file_store.write(
      WriteFileRequest{prepared.artifact.bytes, path, true});
  if (!saved) {
    return Result<ExportReceipt, Error>::failure(saved.error());
  }
  return Result<ExportReceipt, Error>::success(receipt(saved.value(), prepared));
}

Result<ExportReceipt, Error> ExportWorkflow::cancel(const ExportSnapshot& snapshot) {
  if (snapshot.frozen_desktop == nullptr ||
      snapshot.frozen_desktop->canonical_segments.empty()) {
    return Result<ExportReceipt, Error>::failure(missing_frozen_desktop());
  }
  const auto& segment = snapshot.frozen_desktop->canonical_segments.front();
  return Result<ExportReceipt, Error>::success(ExportReceipt{
      UserCancelled{}, snapshot.frozen_desktop->frame_id, snapshot.selection.revision,
      snapshot.annotations.revision, segment.encoding, {}});
}

Result<ExportReceipt, Error> ExportWorkflow::copy(
    const ExportSnapshot& snapshot,
    ClipboardPort& clipboard,
    TextRasterizerPort* const text_rasterizer,
    ExportPixelProcessorPort* const pixel_processor,
    UltraHdrInputRendererPort* const ultra_hdr_renderer,
    UltraHdrEncoderPort* const ultra_hdr_encoder,
    SourceRangeProbePort* range_probe) {
  auto prepared = prepare(
      snapshot, text_rasterizer, pixel_processor, ultra_hdr_renderer,
      ultra_hdr_encoder, range_probe);
  if (!prepared) {
    return Result<ExportReceipt, Error>::failure(prepared.error());
  }
  return copy_prepared(prepared.value(), clipboard);
}

Result<ExportReceipt, Error> ExportWorkflow::save_default(
    const ExportSnapshot& snapshot,
    const ClockPort& clock,
    FileStorePort& file_store,
    TextRasterizerPort* const text_rasterizer,
    ExportPixelProcessorPort* const pixel_processor,
    UltraHdrInputRendererPort* const ultra_hdr_renderer,
    UltraHdrEncoderPort* const ultra_hdr_encoder,
    SourceRangeProbePort* range_probe) {
  constexpr std::size_t kMaximumCollisionAttempts = 10000U;
  const auto timestamp = clock.now_local();
  for (std::size_t collision_index = 0U;
       collision_index < kMaximumCollisionAttempts; ++collision_index) {
    auto name = FilenamePolicy::make_name(
        timestamp, collision_index, snapshot.save_format);
    if (!name) {
      return Result<ExportReceipt, Error>::failure(name.error());
    }
    auto path = FilenamePolicy::join_folder(snapshot.default_folder, name.value());
    if (!path) {
      return Result<ExportReceipt, Error>::failure(path.error());
    }
    auto sink = file_store.open_atomic(OpenAtomicFileRequest{path.value(), false});
    if (!sink) {
      if (sink.error().code == ErrorCode::path_already_exists) {
        continue;
      }
      return Result<ExportReceipt, Error>::failure(sink.error());
    }
    auto saved = encode_and_commit(
        snapshot, text_rasterizer, pixel_processor, ultra_hdr_renderer,
        ultra_hdr_encoder, std::move(sink.value()), range_probe);
    if (saved || saved.error().code != ErrorCode::path_already_exists) {
      return saved;
    }
  }
  return Result<ExportReceipt, Error>::failure(Error{
      ErrorCode::path_already_exists, "ExportWorkflow",
      Retryability::after_user_action,
      {{"attempts", std::to_string(kMaximumCollisionAttempts)}}});
}

Result<ExportReceipt, Error> ExportWorkflow::save_as_to_destination(
    const ExportSnapshot& snapshot,
    const ChooseSavePathOutcome& destination,
    FileStorePort& file_store,
    TextRasterizerPort* const text_rasterizer,
    ExportPixelProcessorPort* const pixel_processor,
    UltraHdrInputRendererPort* const ultra_hdr_renderer,
    UltraHdrEncoderPort* const ultra_hdr_encoder,
    SourceRangeProbePort* range_probe) {
  if (std::holds_alternative<UserCancelled>(destination)) {
    return cancel(snapshot);
  }
  const auto& path = std::get<ChosenPath>(destination).exact_path;
  auto sink = file_store.open_atomic(OpenAtomicFileRequest{path, true});
  if (!sink) {
    return Result<ExportReceipt, Error>::failure(sink.error());
  }
  return encode_and_commit(
      snapshot, text_rasterizer, pixel_processor, ultra_hdr_renderer,
      ultra_hdr_encoder, std::move(sink.value()), range_probe);
}

Result<ExportReceipt, Error> ExportWorkflow::save_as(
    const ExportSnapshot& snapshot,
    const ClockPort& clock,
    FileDialogPort& file_dialog,
    FileStorePort& file_store,
    TextRasterizerPort* const text_rasterizer,
    ExportPixelProcessorPort* const pixel_processor,
    UltraHdrInputRendererPort* const ultra_hdr_renderer,
    UltraHdrEncoderPort* const ultra_hdr_encoder,
    SourceRangeProbePort* range_probe) {
  auto destination = choose_save_as_path(
      snapshot.default_folder, clock, file_dialog, snapshot.target_display_id,
      snapshot.save_format);
  if (!destination) {
    return Result<ExportReceipt, Error>::failure(destination.error());
  }
  return save_as_to_destination(
      snapshot, destination.value(), file_store, text_rasterizer, pixel_processor,
      ultra_hdr_renderer, ultra_hdr_encoder, range_probe);
}

}  // namespace hdrshot
