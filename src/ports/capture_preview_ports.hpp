#pragma once

#include "core/error.hpp"
#include "core/frame.hpp"
#include "core/result.hpp"
#include "domain/annotation/annotation_document.hpp"
#include "domain/geometry/selection_model.hpp"
#include "domain/geometry/window_selection.hpp"

#include <cstdint>
#include <functional>
#include <vector>

namespace hdrshot {

struct SnapshotDisplaysRequest {
  SessionId session_id{};
  OperationId operation_id{};
};

class DisplayCatalogPort {
 public:
  using Completion = std::function<void(Result<DisplaySnapshotSet, Error>)>;
  virtual ~DisplayCatalogPort() = default;
  virtual void snapshot_displays(const SnapshotDisplaysRequest& request, Completion completion) = 0;
};

struct CaptureBatchRequest {
  SessionId session_id{};
  OperationId operation_id{};
  DisplayGeneration display_generation{};
  std::vector<DisplayId> targets;
  PixelFormat desired_format{PixelFormat::rgba16_float};
  bool exclude_own_windows{true};
};

class CapturePort {
 public:
  using Completion = std::function<void(Result<NativeFrameBatch, Error>)>;
  virtual ~CapturePort() = default;
  virtual void capture(const CaptureBatchRequest& request, Completion completion) = 0;
  virtual void cancel(SessionId session_id, OperationId operation_id) = 0;
};

struct OverlayStyle {
  float outside_linear_dim_factor{0.35F};
  float ui_white_edr{2.03F};
  std::int32_t selection_border_width_px{2};
};

struct CleanContentSnapshot;

struct PreviewModel {
  SessionId session_id{};
  FrozenDesktopRef frozen_desktop;
  SelectionSnapshot selection{};
  AnnotationDocumentSnapshot annotation_document{};
  OverlayStyle overlay_style{};
  DisplayGeneration display_generation{};
  PreviewHighlight initial_highlight{};
  std::shared_ptr<const CleanContentSnapshot> clean_content;
};

// Hover/initial drag are display-only. They never become export selections or
// annotation plan input until the shared gesture commits a valid rectangle.
[[nodiscard]] inline PixelRect preview_selection_rect(const PreviewModel& model) {
  return !model.selection.desktop_rect.empty() ? model.selection.desktop_rect
      : model.initial_highlight.rect;
}

struct PresentPreviewRequest {
  OperationId operation_id{};
  PreviewModel model;
  // Zero means "fan out to every configured display" at the multi-display
  // coordinator boundary. A concrete surface adapter always receives a real ID.
  DisplayId target_display_id{};
};

struct PresentReceipt {
  SessionId session_id{};
  OperationId operation_id{};
  FrameId frame_id{};
  DisplayGeneration display_generation{};
  SelectionRevision selection_revision{};
  DocumentRevision document_revision{};
  std::uint64_t preview_revision{};

  friend bool operator==(const PresentReceipt&, const PresentReceipt&) = default;
};

class PreviewPresenterPort {
 public:
  using Completion = std::function<void(Result<PresentReceipt, Error>)>;
  virtual ~PreviewPresenterPort() = default;
  virtual void present(const PresentPreviewRequest& request, Completion completion) = 0;
  virtual void cancel(SessionId session_id, OperationId operation_id) = 0;
};

}  // namespace hdrshot
