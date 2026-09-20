#include "application/export_workflow.hpp"
#include "core/build_metadata.hpp"
#include "software_metadata_test_support.hpp"
#include "domain/color/extended_p3_mapper.hpp"
#include "test_support.hpp"

#include <array>
#include <map>
#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace hdrshot;

class FakeClipboard final : public ClipboardPort {
 public:
  std::vector<std::uint8_t> bytes;
  Result<ClipboardReceipt, Error> write(const ClipboardWriteRequest& request) override {
    bytes.assign(request.bytes.begin(), request.bytes.end());
    return Result<ClipboardReceipt, Error>::success(
        ClipboardReceipt{bytes.size(), request.mime_types});
  }
};

class FakeFileStore final : public FileStorePort {
 public:
  class Sink final : public AtomicFileSink {
   public:
    Sink(FakeFileStore& owner, std::string path, const bool overwrite)
        : owner_(owner), path_(std::move(path)), overwrite_(overwrite) {}

    Result<std::size_t, Error> write(
        const std::span<const std::uint8_t> bytes) override {
      ++owner_.stream_write_calls;
      if (owner_.fail_stream_after_write_calls.has_value() &&
          owner_.stream_write_calls > *owner_.fail_stream_after_write_calls) {
        return Result<std::size_t, Error>::failure(Error{
            ErrorCode::storage_full, "FakeFileStore", Retryability::after_user_action, {}});
      }
      pending_.insert(pending_.end(), bytes.begin(), bytes.end());
      return Result<std::size_t, Error>::success(bytes.size());
    }

    Result<FileReceipt, Error> commit() override {
      if (!overwrite_ && owner_.files.contains(path_)) {
        return Result<FileReceipt, Error>::failure(Error{
            ErrorCode::path_already_exists, "FakeFileStore", Retryability::same_input, {}});
      }
      owner_.files[path_] = pending_;
      committed_ = true;
      return Result<FileReceipt, Error>::success(FileReceipt{path_, pending_.size()});
    }

    void abort() noexcept override {
      if (!committed_) {
        ++owner_.abort_calls;
        pending_.clear();
      }
    }

   private:
    FakeFileStore& owner_;
    std::string path_;
    bool overwrite_{};
    bool committed_{};
    std::vector<std::uint8_t> pending_;
  };

  std::map<std::string, std::vector<std::uint8_t>> files;
  std::size_t stream_write_calls{};
  std::size_t abort_calls{};
  std::optional<std::size_t> fail_stream_after_write_calls;
  Result<FileReceipt, Error> write(const WriteFileRequest& request) override {
    if (files.contains(request.exact_path) && !request.overwrite) {
      return Result<FileReceipt, Error>::failure(Error{
          ErrorCode::path_already_exists, "FakeFileStore", Retryability::same_input, {}});
    }
    files[request.exact_path] = {request.bytes.begin(), request.bytes.end()};
    return Result<FileReceipt, Error>::success(
        FileReceipt{request.exact_path, request.bytes.size()});
  }

  Result<std::unique_ptr<AtomicFileSink>, Error> open_atomic(
      const OpenAtomicFileRequest& request) override {
    if (!request.overwrite && files.contains(request.exact_path)) {
      return Result<std::unique_ptr<AtomicFileSink>, Error>::failure(Error{
          ErrorCode::path_already_exists, "FakeFileStore", Retryability::same_input, {}});
    }
    return Result<std::unique_ptr<AtomicFileSink>, Error>::success(
        std::make_unique<Sink>(*this, request.exact_path, request.overwrite));
  }
};

class FixedClock final : public ClockPort {
 public:
  LocalDateTime now_local() const override {
    return LocalDateTime{2026, 8, 29, 0, 1, 2, 480};
  }
};

class FakeDialog final : public FileDialogPort {
 public:
  ChooseSavePathOutcome outcome{UserCancelled{}};
  std::optional<ChooseSavePathRequest> last_request;
  Result<ChooseSavePathOutcome, Error> choose_save_path(
      const ChooseSavePathRequest& request) override {
    last_request = request;
    return Result<ChooseSavePathOutcome, Error>::success(outcome);
  }
};

class SolidTextRasterizer final : public TextRasterizerPort {
 public:
  Result<TextCoverageMask, Error> rasterize(const TextRasterRequest& request) override {
    return Result<TextCoverageMask, Error>::success(TextCoverageMask{
        request.object_id,
        request.mask_size_px,
        std::vector<std::uint8_t>(
            static_cast<std::size_t>(request.mask_size_px.width) *
                static_cast<std::size_t>(request.mask_size_px.height),
            255),
        "test-font",
    });
  }
};

class RejectingPngPixelProcessor final : public ExportPixelProcessorPort {
 public:
  std::size_t calls{};

  Result<ExportPixelProcessResult, Error> process(
      const ExportPixelProcessRequest&) override {
    ++calls;
    return Result<ExportPixelProcessResult, Error>::failure(Error{
        ErrorCode::state_inconsistent,
        "RejectingPngPixelProcessor",
        Retryability::never,
        {{"reason", "png_path_must_not_run_for_ultra_hdr"}}});
  }
};

class FakeUltraHdrRenderer final : public UltraHdrInputRendererPort {
 public:
  std::size_t calls{};
  double received_reference_white{};
  double maximum_linear_component{2.0};

  Result<LinearDisplayP3HalfImage, Error> render(
      const UltraHdrInputRenderRequest& request) override {
    ++calls;
    HDRSHOT_CHECK(request.source != nullptr);
    HDRSHOT_CHECK(request.pixel_plan != nullptr);
    received_reference_white = request.reference_white_nits;
    const auto samples = static_cast<std::size_t>(request.source->size_px.width) *
        static_cast<std::size_t>(request.source->size_px.height) * 4U;
    return Result<LinearDisplayP3HalfImage, Error>::success(
        LinearDisplayP3HalfImage{
            request.source->size_px,
            std::vector<std::uint16_t>(samples, 0U),
            request.reference_white_nits,
            maximum_linear_component});
  }
};

class FakeUltraHdrEncoder final : public UltraHdrEncoderPort {
 public:
  std::size_t calls{};
  JpegOutputKind kind{JpegOutputKind::ultra_hdr};
  UltraHdrJpegQuality received_quality{UltraHdrJpegQuality::compact};

  Result<EncodedUltraHdrJpeg, Error> encode(
      const UltraHdrEncodeRequest& request) override {
    ++calls;
    HDRSHOT_CHECK(request.linear_display_p3 != nullptr);
    HDRSHOT_CHECK(request.linear_display_p3->reference_white_nits ==
                  kUltraHdrReferenceWhiteNits);
    received_quality = request.quality;
    HDRSHOT_CHECK(request.software == software_identifier());
    return Result<EncodedUltraHdrJpeg, Error>::success(
        EncodedUltraHdrJpeg{{0xFFU, 0xD8U, 0xFFU, 0xD9U}, kind});
  }
};

ExportSnapshot snapshot(
    const DisplayDynamicRange dynamic_range = DisplayDynamicRange::hdr,
    const std::uint16_t alpha_half = 0x3C00,
    const std::uint16_t rgb_half = 0x3810) {
  std::vector<std::uint16_t> pixels;
  for (int index = 0; index < 16; ++index) {
    pixels.insert(pixels.end(), {rgb_half, rgb_half, rgb_half, alpha_half});
  }
  auto frozen = std::make_shared<const FrozenDesktop>(FrozenDesktop{
      FrameId{1},
      2,
      LogicalRect{0, 0, 4, 4},
      {CanonicalFrameSegment{
          DisplayId{1},
          LogicalRect{0, 0, 4, 4},
          1.0,
          PixelSize{4, 4},
          PixelFormat::rgba16_float,
          ColorEncoding{
              ColorPrimaries::display_p3,
              TransferFunction::extended_srgb,
              AlphaMode::straight,
              0.0},
          dynamic_range,
          std::move(pixels),
      }},
  });
  return ExportSnapshot{
      SessionId{8},
      OperationId{9},
      frozen,
      SelectionSnapshot{3, PixelRect{1, 1, 2, 2}},
      AnnotationDocument::empty().snapshot(),
      nullptr,
      "/Pictures/SeriousShot",
      DisplayId{1},
  };
}

void source_encoding_is_interpreted_by_injected_ports_not_shared_policy() {
  class ScRgbProbe final : public SourceRangeProbePort {
   public:
    int calls{};
    Result<RangeFitResult, Error> probe(const SelectionRoiView& source,
        const AnnotationPixelPlan&, RangeProbeOptimization) override {
      ++calls;
      HDRSHOT_CHECK(source.encoding.primaries == ColorPrimaries::srgb_bt709);
      HDRSHOT_CHECK(source.encoding.transfer == TransferFunction::linear);
      return Result<RangeFitResult, Error>::success(
          {true, 4U, 0U, 12U, "injected_scRGB_test_interpreter"});
    }
  } probe;
  class ScRgbPixels final : public ExportPixelProcessorPort {
   public:
    int calls{};
    Result<ExportPixelProcessResult, Error> process(
        const ExportPixelProcessRequest& request) override {
      ++calls;
      HDRSHOT_CHECK(request.source->encoding.transfer == TransferFunction::linear);
      HDRSHOT_CHECK(request.output_plan.encoding_intent == EncodingIntent::wide_gamut_sdr);
      return Result<ExportPixelProcessResult, Error>::success({
          request.source->size_px,
          {ColorPrimaries::display_p3, TransferFunction::srgb, AlphaMode::opaque, 0.0},
          std::vector<std::uint16_t>(12U, 32768U), {}, std::nullopt});
    }
  } pixels;
  auto input = snapshot();
  auto desktop = std::make_shared<FrozenDesktop>(*input.frozen_desktop);
  desktop->canonical_segments[0].encoding = {
      ColorPrimaries::srgb_bt709, TransferFunction::linear, AlphaMode::opaque, 80.0};
  input.frozen_desktop = std::move(desktop);
  const auto prepared = ExportWorkflow::prepare(
      input, nullptr, &pixels, nullptr, nullptr, &probe);
  HDRSHOT_CHECK(prepared.has_value());
  HDRSHOT_CHECK(probe.calls == 1 && pixels.calls == 1);
  HDRSHOT_CHECK(prepared.value().artifact.mime_type == "image/png");
  HDRSHOT_CHECK(prepared.value().output_encoding.primaries == ColorPrimaries::display_p3);
}

std::optional<std::vector<std::uint8_t>> chunk_payload(
    const std::vector<std::uint8_t>& bytes,
    const std::array<std::uint8_t, 4>& type) {
  auto read_u32 = [&bytes](const std::size_t offset) {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
        (static_cast<std::uint32_t>(bytes[offset + 1U]) << 16U) |
        (static_cast<std::uint32_t>(bytes[offset + 2U]) << 8U) |
        static_cast<std::uint32_t>(bytes[offset + 3U]);
  };
  std::size_t offset = 8U;
  while (offset + 12U <= bytes.size()) {
    const auto size = static_cast<std::size_t>(read_u32(offset));
    HDRSHOT_CHECK(offset + 12U + size <= bytes.size());
    if (std::equal(type.begin(), type.end(), bytes.begin() +
            static_cast<std::ptrdiff_t>(offset + 4U))) {
      return std::vector<std::uint8_t>{
          bytes.begin() + static_cast<std::ptrdiff_t>(offset + 8U),
          bytes.begin() + static_cast<std::ptrdiff_t>(offset + 8U + size)};
    }
    offset += 12U + size;
  }
  return std::nullopt;
}

std::vector<std::uint8_t> u32_pair(
    const std::uint32_t first,
    const std::uint32_t second) {
  return {
      static_cast<std::uint8_t>((first >> 24U) & 0xFFU),
      static_cast<std::uint8_t>((first >> 16U) & 0xFFU),
      static_cast<std::uint8_t>((first >> 8U) & 0xFFU),
      static_cast<std::uint8_t>(first & 0xFFU),
      static_cast<std::uint8_t>((second >> 24U) & 0xFFU),
      static_cast<std::uint8_t>((second >> 16U) & 0xFFU),
      static_cast<std::uint8_t>((second >> 8U) & 0xFFU),
      static_cast<std::uint8_t>(second & 0xFFU),
  };
}

void copy_writes_valid_png_and_provenance() {
  FakeClipboard clipboard;
  const auto result = ExportWorkflow::copy(snapshot(), clipboard);
  HDRSHOT_CHECK(result.has_value());
  HDRSHOT_CHECK(clipboard.bytes.size() > 8);
  HDRSHOT_CHECK(clipboard.bytes[0] == 137 && clipboard.bytes[1] == 80);
  HDRSHOT_CHECK(clipboard.bytes.size() > 25U);
  HDRSHOT_CHECK(clipboard.bytes[24] == 16U);
  HDRSHOT_CHECK(clipboard.bytes[25] == 2U);
  HDRSHOT_CHECK(result.value().source_frame_id == FrameId{1});
  HDRSHOT_CHECK(result.value().source_selection_revision == 3);
  HDRSHOT_CHECK(std::holds_alternative<ClipboardReceipt>(result.value().destination));
}

void ultra_hdr_copy_uses_only_its_renderer_and_codec_contract() {
  auto value = snapshot(DisplayDynamicRange::hdr);
  value.save_format = SaveFormat::ultra_hdr_jpeg;
  value.ultra_hdr_jpeg_quality = UltraHdrJpegQuality::balanced;
  value.pq_diffuse_white = PqDiffuseWhite::nits_100;
  value.hdr_pq_precision = HdrPqPrecision::bits_16;
  RejectingPngPixelProcessor png_processor;
  FakeUltraHdrRenderer renderer;
  FakeUltraHdrEncoder encoder;
  FakeClipboard clipboard;

  const auto result = ExportWorkflow::copy(
      value, clipboard, nullptr, &png_processor, &renderer, &encoder);
  HDRSHOT_CHECK(result.has_value());
  HDRSHOT_CHECK(png_processor.calls == 0U);
  HDRSHOT_CHECK(renderer.calls == 1U);
  HDRSHOT_CHECK(renderer.received_reference_white == 203.0);
  HDRSHOT_CHECK(encoder.calls == 1U);
  HDRSHOT_CHECK(encoder.received_quality == UltraHdrJpegQuality::balanced);
  HDRSHOT_CHECK(clipboard.bytes ==
                (std::vector<std::uint8_t>{0xFFU, 0xD8U, 0xFFU, 0xD9U}));
  const auto& receipt = std::get<ClipboardReceipt>(result.value().destination);
  HDRSHOT_CHECK(receipt.published_mime_types ==
                (std::vector<std::string>{"image/jpeg"}));
  HDRSHOT_CHECK(result.value().output_encoding.primaries ==
                ColorPrimaries::display_p3);
  HDRSHOT_CHECK(result.value().output_encoding.transfer ==
                TransferFunction::pq);
  HDRSHOT_CHECK(result.value().output_encoding.source_reference_white_nits ==
                203.0);
}

void sdr_jpeg_uses_shared_workflow_and_truthful_receipt() {
  auto value = snapshot(DisplayDynamicRange::hdr); // Screen capability isn't file class.
  value.save_format = SaveFormat::ultra_hdr_jpeg;
  RejectingPngPixelProcessor png_processor;
  FakeUltraHdrRenderer renderer;
  renderer.maximum_linear_component = 1.0;
  FakeUltraHdrEncoder encoder;
  encoder.kind = JpegOutputKind::display_p3_sdr;
  FakeClipboard clipboard;
  const auto result = ExportWorkflow::copy(
      value, clipboard, nullptr, &png_processor, &renderer, &encoder);
  HDRSHOT_CHECK(result.has_value() && png_processor.calls == 0U);
  HDRSHOT_CHECK(result.value().output_encoding.transfer == TransferFunction::srgb);
  HDRSHOT_CHECK(result.value().output_encoding.primaries == ColorPrimaries::display_p3);
  HDRSHOT_CHECK(result.value().output_encoding.source_reference_white_nits == 0.0);
  HDRSHOT_CHECK(std::get<ClipboardReceipt>(result.value().destination).published_mime_types ==
      (std::vector<std::string>{"image/jpeg"}));
  FixedClock clock;
  FakeFileStore files;
  const auto saved = ExportWorkflow::save_default(
      value, clock, files, nullptr, &png_processor, &renderer, &encoder);
  HDRSHOT_CHECK(saved.has_value());
  HDRSHOT_CHECK(files.files.at("/Pictures/SeriousShot/SeriousShot_2026-08-29_00-01-02.jpg") == clipboard.bytes);
  // A lying/incorrect encoder must fail before publishing either kind.
  for (const double maximum : {1.0, 2.0}) {
    renderer.maximum_linear_component = maximum;
    encoder.kind = maximum <= 1.0 ? JpegOutputKind::ultra_hdr : JpegOutputKind::display_p3_sdr;
    FakeClipboard untouched;
    const auto invalid = ExportWorkflow::copy(
        value, untouched, nullptr, &png_processor, &renderer, &encoder);
    HDRSHOT_CHECK(!invalid.has_value() && untouched.bytes.empty());
    HDRSHOT_CHECK(invalid.error().safe_context.at("reason") == "ultra_hdr_encoder_contract_mismatch");
  }
}

void ultra_hdr_save_as_request_uses_jpeg_name_and_format() {
  FixedClock clock;
  FakeDialog dialog;
  dialog.outcome = UserCancelled{};
  const auto result = ExportWorkflow::choose_save_as_path(
      "/Pictures/SeriousShot", clock, dialog, DisplayId{1},
      SaveFormat::ultra_hdr_jpeg);
  HDRSHOT_CHECK(result.has_value());
  HDRSHOT_CHECK(dialog.last_request.has_value());
  HDRSHOT_CHECK(dialog.last_request->suggested_name ==
                "SeriousShot_2026-08-29_00-01-02.jpg");
  HDRSHOT_CHECK(dialog.last_request->save_format ==
                SaveFormat::ultra_hdr_jpeg);
}

void default_save_uses_filename_policy_and_create_new() {
  FakeFileStore files;
  FixedClock clock;
  const auto result = ExportWorkflow::save_default(snapshot(), clock, files);
  HDRSHOT_CHECK(result.has_value());
  HDRSHOT_CHECK(files.files.contains("/Pictures/SeriousShot/SeriousShot_2026-08-29_00-01-02.png"));
  HDRSHOT_CHECK(files.stream_write_calls > 1U);
  HDRSHOT_CHECK(std::holds_alternative<FileReceipt>(result.value().destination));
}

void streaming_save_failure_aborts_without_publishing_partial_file() {
  FakeFileStore files;
  files.fail_stream_after_write_calls = 2U;
  FixedClock clock;
  const auto result = ExportWorkflow::save_default(snapshot(), clock, files);
  HDRSHOT_CHECK(!result.has_value());
  HDRSHOT_CHECK(result.error().code == ErrorCode::storage_full);
  HDRSHOT_CHECK(files.files.empty());
  HDRSHOT_CHECK(files.abort_calls == 1U);
}

void sdr_content_exports_display_p3_samples_with_icc_and_cicp() {
  FakeClipboard clipboard;
  const auto result = ExportWorkflow::copy(
      snapshot(DisplayDynamicRange::sdr), clipboard);
  HDRSHOT_CHECK(result.has_value());
  HDRSHOT_CHECK(result.value().output_encoding.primaries == ColorPrimaries::display_p3);
  HDRSHOT_CHECK(result.value().output_encoding.transfer == TransferFunction::srgb);
  HDRSHOT_CHECK(chunk_payload(
      clipboard.bytes, std::array<std::uint8_t, 4>{'c', 'I', 'C', 'P'}) ==
      (std::vector<std::uint8_t>{12, 13, 0, 1}));
  HDRSHOT_CHECK(chunk_payload(
      clipboard.bytes, std::array<std::uint8_t, 4>{'i', 'C', 'C', 'P'}).has_value());
  HDRSHOT_CHECK(!chunk_payload(
      clipboard.bytes, std::array<std::uint8_t, 4>{'c', 'L', 'L', 'I'}).has_value());
}

void over_edr_one_content_exports_display_p3_pq_samples_and_cicp() {
  FakeClipboard clipboard;
  const auto result = ExportWorkflow::copy(
      snapshot(DisplayDynamicRange::hdr, 0x3C00, 0x3E00), clipboard);
  HDRSHOT_CHECK(result.has_value());
  HDRSHOT_CHECK(result.value().output_encoding.primaries == ColorPrimaries::display_p3);
  HDRSHOT_CHECK(result.value().output_encoding.transfer == TransferFunction::pq);
  HDRSHOT_CHECK(chunk_payload(
      clipboard.bytes, std::array<std::uint8_t, 4>{'c', 'I', 'C', 'P'}) ==
      (std::vector<std::uint8_t>{12, 16, 0, 1}));
  HDRSHOT_CHECK(!chunk_payload(
      clipboard.bytes, std::array<std::uint8_t, 4>{'i', 'C', 'C', 'P'}).has_value());
  const auto linear_edr = ExtendedP3Mapper::inverse_extended_srgb(1.5F);
  const auto expected_clli = content_light_units(
      static_cast<double>(linear_edr * 203.0F));
  HDRSHOT_CHECK(chunk_payload(
      clipboard.bytes, std::array<std::uint8_t, 4>{'c', 'L', 'L', 'I'}) ==
      u32_pair(expected_clli, expected_clli));
}

void hdr_precision_changes_codes_but_not_content_light_metadata() {
  std::vector<std::vector<std::uint8_t>> pngs;
  std::optional<std::vector<std::uint8_t>> expected_clli;
  for (const auto precision : {
           HdrPqPrecision::bits_16,
           HdrPqPrecision::bits_12,
           HdrPqPrecision::bits_10,
       }) {
    auto value = snapshot(DisplayDynamicRange::hdr, 0x3C00, 0x3E00);
    value.hdr_pq_precision = precision;
    FakeClipboard clipboard;
    const auto result = ExportWorkflow::copy(value, clipboard);
    HDRSHOT_CHECK(result.has_value());
    const auto clli = chunk_payload(
        clipboard.bytes, std::array<std::uint8_t, 4>{'c', 'L', 'L', 'I'});
    HDRSHOT_CHECK(clli.has_value());
    if (!expected_clli.has_value()) {
      expected_clli = clli;
    } else {
      HDRSHOT_CHECK(clli == expected_clli);
    }
    pngs.push_back(std::move(clipboard.bytes));
  }
  HDRSHOT_CHECK(pngs[0] != pngs[1]);
  HDRSHOT_CHECK(pngs[1] != pngs[2]);
}

void diffuse_white_changes_only_hdr_png() {
  auto sdr_100 = snapshot(DisplayDynamicRange::hdr, 0x3C00, 0x3800);
  auto sdr_203 = sdr_100;
  sdr_100.pq_diffuse_white = PqDiffuseWhite::nits_100;
  sdr_203.pq_diffuse_white = PqDiffuseWhite::nits_203;
  FakeClipboard sdr_clipboard_100;
  FakeClipboard sdr_clipboard_203;
  HDRSHOT_CHECK(ExportWorkflow::copy(sdr_100, sdr_clipboard_100).has_value());
  HDRSHOT_CHECK(ExportWorkflow::copy(sdr_203, sdr_clipboard_203).has_value());
  HDRSHOT_CHECK(sdr_clipboard_100.bytes == sdr_clipboard_203.bytes);

  auto hdr_100 = snapshot(DisplayDynamicRange::hdr, 0x3C00, 0x3E00);
  auto hdr_203 = hdr_100;
  hdr_100.pq_diffuse_white = PqDiffuseWhite::nits_100;
  hdr_203.pq_diffuse_white = PqDiffuseWhite::nits_203;
  FakeClipboard hdr_clipboard_100;
  FakeClipboard hdr_clipboard_203;
  const auto result_100 = ExportWorkflow::copy(hdr_100, hdr_clipboard_100);
  const auto result_203 = ExportWorkflow::copy(hdr_203, hdr_clipboard_203);
  HDRSHOT_CHECK(result_100.has_value());
  HDRSHOT_CHECK(result_203.has_value());
  HDRSHOT_CHECK(result_100.value().output_encoding.source_reference_white_nits == 100.0);
  HDRSHOT_CHECK(result_203.value().output_encoding.source_reference_white_nits == 203.0);
  HDRSHOT_CHECK(hdr_clipboard_100.bytes != hdr_clipboard_203.bytes);
}

void capture_alpha_is_not_exported() {
  FakeClipboard clipboard;
  const auto result = ExportWorkflow::copy(
      snapshot(DisplayDynamicRange::hdr, 0x3800), clipboard);
  HDRSHOT_CHECK(result.has_value());
  HDRSHOT_CHECK(clipboard.bytes.size() > 25U);
  HDRSHOT_CHECK(clipboard.bytes[25] == 2U);
  HDRSHOT_CHECK(result.value().output_encoding.alpha == AlphaMode::opaque);
}

void save_as_cancel_preserves_user_state_and_writes_nothing() {
  FakeFileStore files;
  FakeDialog dialog;
  FixedClock clock;
  const auto result = ExportWorkflow::save_as(snapshot(), clock, dialog, files);
  HDRSHOT_CHECK(result.has_value());
  HDRSHOT_CHECK(std::holds_alternative<UserCancelled>(result.value().destination));
  HDRSHOT_CHECK(files.files.empty());
}

void save_as_replaces_user_confirmed_destination() {
  FakeFileStore files;
  FakeDialog dialog;
  FixedClock clock;
  const std::string path = "/Pictures/existing.png";
  files.files[path] = {1U, 2U, 3U};
  dialog.outcome = ChosenPath{path};
  const auto result = ExportWorkflow::save_as(snapshot(), clock, dialog, files);
  HDRSHOT_CHECK(result.has_value());
  HDRSHOT_CHECK(files.files.at(path).size() > 8U);
  HDRSHOT_CHECK(files.files.at(path)[0] == 137U);
}

void text_annotation_failure_never_writes_destination() {
  auto value = snapshot();
  auto document = AnnotationDocument::apply(
      AnnotationDocument::empty(),
      CreateAnnotation{AnnotationObject{
          ObjectId{3}, AnnotationKind::text, PixelRect{1, 1, 2, 2},
          TextStyle{0xFFFFFF, 20}, "text", std::nullopt}});
  HDRSHOT_CHECK(document.has_value());
  value.annotations = document.value().snapshot();
  FakeClipboard clipboard;
  const auto result = ExportWorkflow::copy(value, clipboard);
  HDRSHOT_CHECK(!result.has_value());
  HDRSHOT_CHECK(result.error().code == ErrorCode::unsupported_encoding);
  HDRSHOT_CHECK(clipboard.bytes.empty());
}

void text_annotation_with_rasterizer_exports_png() {
  auto value = snapshot();
  auto document = AnnotationDocument::apply(
      AnnotationDocument::empty(),
      CreateAnnotation{AnnotationObject{
          ObjectId{4}, AnnotationKind::text, PixelRect{1, 1, 2, 2},
          TextStyle{0xFFFFFF, 20}, "文字", std::nullopt}});
  HDRSHOT_CHECK(document.has_value());
  value.annotations = document.value().snapshot();
  FakeClipboard clipboard;
  SolidTextRasterizer rasterizer;
  const auto result = ExportWorkflow::copy(value, clipboard, &rasterizer);
  HDRSHOT_CHECK(result.has_value());
  HDRSHOT_CHECK(clipboard.bytes.size() > 8);
  HDRSHOT_CHECK(result.value().source_document_revision == 1);
}

void preparation_and_destination_public_phases_are_independently_testable() {
  FakeClipboard clipboard;
  FakeFileStore files;
  FakeDialog dialog;
  FixedClock clock;
  dialog.outcome = ChosenPath{"/Pictures/phased.png"};

  const auto prepared = ExportWorkflow::prepare(snapshot());
  HDRSHOT_CHECK(prepared.has_value());
  HDRSHOT_CHECK(!prepared.value().artifact.bytes.empty());
  HDRSHOT_CHECK(clipboard.bytes.empty());
  HDRSHOT_CHECK(files.files.empty());

  const auto destination = ExportWorkflow::choose_save_as_path(
      "/Pictures", clock, dialog, DisplayId{1});
  HDRSHOT_CHECK(destination.has_value());
  HDRSHOT_CHECK(dialog.last_request.has_value());
  HDRSHOT_CHECK(dialog.last_request->owner_display_id == DisplayId{1});
  const auto saved = ExportWorkflow::save_as_prepared(
      prepared.value(), destination.value(), files);
  HDRSHOT_CHECK(saved.has_value());
  HDRSHOT_CHECK(files.files.contains("/Pictures/phased.png"));

  const auto copied = ExportWorkflow::copy_prepared(prepared.value(), clipboard);
  HDRSHOT_CHECK(copied.has_value());
  HDRSHOT_CHECK(clipboard.bytes == prepared.value().artifact.bytes);
}

void stale_preview_plan_is_rejected_before_publish() {
  auto value = snapshot();
  const auto empty_plan = AnnotationRenderPlanner::build(
      value.annotations, value.selection.desktop_rect, 1.0);
  HDRSHOT_CHECK(empty_plan.has_value());
  value.annotation_render_plan = std::make_shared<const AnnotationRenderPlan>(
      empty_plan.value());
  const auto document = AnnotationDocument::apply(
      AnnotationDocument::empty(),
      CreateAnnotation{AnnotationObject{
          ObjectId{8},
          AnnotationKind::rectangle,
          PixelRect{0, 0, 2, 2},
          ShapeStyle{0xFF4D67, 2},
          {},
          std::nullopt,
      }});
  HDRSHOT_CHECK(document.has_value());
  value.annotations = document.value().snapshot();
  FakeClipboard clipboard;
  const auto result = ExportWorkflow::copy(value, clipboard);
  HDRSHOT_CHECK(!result.has_value());
  HDRSHOT_CHECK(result.error().code == ErrorCode::state_inconsistent);
  HDRSHOT_CHECK(result.error().safe_context.at("reason") ==
                "stale_annotation_render_plan");
  HDRSHOT_CHECK(clipboard.bytes.empty());
}

void multi_display_snapshot_exports_the_explicit_target_display() {
  auto value = snapshot(DisplayDynamicRange::hdr);
  auto mutable_frame = *value.frozen_desktop;
  auto second = mutable_frame.canonical_segments.front();
  second.display_id = DisplayId{2};
  second.display_dynamic_range = DisplayDynamicRange::sdr;
  second.desktop_frame_points.x = 4.0;
  mutable_frame.canonical_segments.push_back(std::move(second));
  mutable_frame.desktop_bounds_points.width = 8.0;
  value.frozen_desktop = std::make_shared<const FrozenDesktop>(std::move(mutable_frame));
  value.target_display_id = DisplayId{2};
  FakeClipboard clipboard;
  const auto result = ExportWorkflow::copy(value, clipboard);
  HDRSHOT_CHECK(result.has_value());
  HDRSHOT_CHECK(result.value().output_encoding.transfer == TransferFunction::srgb);
}

void all_completion_actions_share_compiled_software() {
  for (const bool hdr : {false, true}) {
    for (const bool jpeg : {false, true}) {
      auto value = snapshot(DisplayDynamicRange::hdr, hdr ? 0x4000 : 0x3800);
      value.save_format = jpeg ? SaveFormat::ultra_hdr_jpeg : SaveFormat::png_display_p3_dual_range;
      FakeUltraHdrRenderer renderer;
      renderer.maximum_linear_component = hdr ? 2.0 : 1.0;
      FakeUltraHdrEncoder encoder; // Checks Software at the shared encoder port.
      encoder.kind = hdr ? JpegOutputKind::ultra_hdr : JpegOutputKind::display_p3_sdr;
      FakeClipboard clipboard;
      FakeFileStore files;
      FakeDialog dialog;
      FixedClock clock;
      const auto suffix = jpeg ? ".jpg" : ".png";
      const auto save_as = std::string("/Pictures/metadata") + suffix;
      dialog.outcome = ChosenPath{save_as};
      HDRSHOT_CHECK(ExportWorkflow::copy(value, clipboard, nullptr, nullptr, &renderer, &encoder).has_value());
      HDRSHOT_CHECK(ExportWorkflow::save_default(value, clock, files, nullptr, nullptr, &renderer, &encoder).has_value());
      HDRSHOT_CHECK(ExportWorkflow::save_as(value, clock, dialog, files, nullptr, nullptr, &renderer, &encoder).has_value());
      HDRSHOT_CHECK(files.files.at(save_as) == clipboard.bytes);
      HDRSHOT_CHECK(files.files.at(std::string("/Pictures/SeriousShot/SeriousShot_2026-08-29_00-01-02") + suffix) == clipboard.bytes);
      if (jpeg) {
        HDRSHOT_CHECK(encoder.calls == 3);
      } else {
        HDRSHOT_CHECK(test::read_png_software(clipboard.bytes).software == software_identifier());
      }
    }
  }
}

}  // namespace

int main() {
  using hdrshot::test::TestCase;
  return hdrshot::test::run(std::vector<TestCase>{
      {"A3 all formats/ranges/actions share compiled Software", all_completion_actions_share_compiled_software},
      {"A3 injected scRGB ports share output policy", source_encoding_is_interpreted_by_injected_ports_not_shared_policy},
      {"A3 copy PNG end-to-end", copy_writes_valid_png_and_provenance},
      {"A3 Ultra HDR uses isolated renderer and codec",
       ultra_hdr_copy_uses_only_its_renderer_and_codec_contract},
      {"A3 SDR JPEG receipt and publication", sdr_jpeg_uses_shared_workflow_and_truthful_receipt},
      {"A3 Ultra HDR save-as requests JPEG",
       ultra_hdr_save_as_request_uses_jpeg_name_and_format},
      {"A3 default save end-to-end", default_save_uses_filename_policy_and_create_new},
      {"A3 streaming save aborts partial file on failure", streaming_save_failure_aborts_without_publishing_partial_file},
      {"A3 SDR content exports Display P3 ICC+cICP PNG", sdr_content_exports_display_p3_samples_with_icc_and_cicp},
      {"A3 over-range content exports Display P3 PQ PNG", over_edr_one_content_exports_display_p3_pq_samples_and_cicp},
      {"A3 HDR precision preserves cLLI", hdr_precision_changes_codes_but_not_content_light_metadata},
      {"A3 diffuse white changes only HDR", diffuse_white_changes_only_hdr_png},
      {"A3 capture alpha is not exported", capture_alpha_is_not_exported},
      {"A3 save-as cancel preserves state", save_as_cancel_preserves_user_state_and_writes_nothing},
      {"A3 save-as replaces confirmed destination", save_as_replaces_user_confirmed_destination},
      {"A3 text failure writes nothing", text_annotation_failure_never_writes_destination},
      {"A3 text with rasterizer exports", text_annotation_with_rasterizer_exports_png},
      {"A3 preparation and destination phases", preparation_and_destination_public_phases_are_independently_testable},
      {"A3 rejects stale preview plan", stale_preview_plan_is_rejected_before_publish},
      {"A3 exports explicit display from multi-display frame", multi_display_snapshot_exports_the_explicit_target_display},
  });
}
