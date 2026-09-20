#include "ui/qt/qt_overlay_host.hpp"
#include "ui/qt/capture_error_message.hpp"
#include "application/capture_interaction_session.hpp"
#include "qt_test_input_platform_adapter.hpp"
#include "test_support.hpp"
#include <QApplication>
#include <QLineEdit>
#include <QToolButton>

using namespace hdrshot;
namespace {
class Window final : public QtOverlayWindowPort {
 public:
  int prepares{}, fronts{}, dialogs{}, resizes{}, recoveries{};
  std::function<void()> interrupted;
  void prepare(QWidget&) override { ++prepares; }
  void configure(QWidget&, bool) override { ++fronts; }
  void resize(QWidget&) override { ++resizes; }
  void set_system_dialog_active(QWidget&, bool, bool) override { ++dialogs; }
  void* native_surface() const override { return nullptr; }
  void set_input_interrupted(std::function<void()> callback) override { interrupted = std::move(callback); }
  void restore_input_focus(QWidget&) override { ++recoveries; }
};
class Presenter final : public PreviewPresenterPort {
 public:
  int frames{};
  std::optional<PresentPreviewRequest> last;
  void cancel(SessionId, OperationId) override {}
  void present(const PresentPreviewRequest& request, Completion done) override {
    ++frames;
    last = request;
    done(Result<PresentReceipt, Error>::success(PresentReceipt{
        request.model.session_id, request.operation_id, request.model.frozen_desktop->frame_id,
        request.model.display_generation, request.model.selection.revision,
        request.model.annotation_document.revision, request.model.initial_highlight.revision}));
  }
};

void session_target_lock_and_stale_finish_are_shared() {
  CaptureInteractionSession session;
  HDRSHOT_CHECK(session.begin(SessionId{1}));
  HDRSHOT_CHECK(!session.begin(SessionId{2}));
  HDRSHOT_CHECK(session.ready(PresentReceipt{SessionId{1}, OperationId{1}, FrameId{1}, 7, 1, 0}));
  HDRSHOT_CHECK(session.allows(SessionId{1}, DisplayId{7}));
  HDRSHOT_CHECK(session.allows(SessionId{1}, DisplayId{8}));
  HDRSHOT_CHECK(session.initial_gesture({1}, {7}, true));
  HDRSHOT_CHECK(!session.allows({1}, {8}));
  HDRSHOT_CHECK(!session.initial_gesture({1}, {8}, true));
  HDRSHOT_CHECK(!session.initial_gesture({2}, {7}, false));
  HDRSHOT_CHECK(session.initial_gesture({1}, {7}, false));
  HDRSHOT_CHECK(session.allows({1}, {8}));
  HDRSHOT_CHECK(session.lock(SessionId{1}, DisplayId{8}));
  HDRSHOT_CHECK(!session.allows(SessionId{1}, DisplayId{7}));
  HDRSHOT_CHECK(!session.lock(SessionId{1}, DisplayId{7}));
  HDRSHOT_CHECK(session.finish(SessionId{1}));
  HDRSHOT_CHECK(session.begin(SessionId{2}));
  HDRSHOT_CHECK(!session.finish(SessionId{1}));
  HDRSHOT_CHECK(!session.ready(PresentReceipt{SessionId{1}, OperationId{1}, FrameId{1}, 7, 1, 0}));
  HDRSHOT_CHECK(session.active());
}

void shared_host_snapshot_dialog_cancel_and_acceptance() {
  hdrshot::test::TestInputPlatformAdapter input;
  auto native = std::make_unique<Window>();
  auto* window = native.get();
  QtOverlayHost host(std::move(native), DisplayId{7}, SelectionSnapshot{1, {0, 0, 500, 300}}, input);
  host.resize(500, 300);
  host.prepare_hidden_native_surface();
  HDRSHOT_CHECK(window->prepares == 1);
  auto frozen = std::make_shared<FrozenDesktop>();
  frozen->frame_id = FrameId{3};
  frozen->display_generation = 9;
  CanonicalFrameSegment segment;
  segment.display_id = DisplayId{7};
  segment.size_px = {500, 300};
  segment.point_pixel_scale = 1;
  frozen->canonical_segments.push_back(segment);
  auto presenter = std::make_shared<Presenter>();
  int finishes = 0, exports = 0;
  host.set_finished([&] { ++finishes; });
  bool accept = false;
  host.set_export_requested([&](UiCommand command, ExportSnapshot snapshot, auto done) {
    ++exports;
    HDRSHOT_CHECK(command == UiCommand::save_as);
    HDRSHOT_CHECK(snapshot.frozen_desktop == frozen);
    HDRSHOT_CHECK(snapshot.target_display_id == DisplayId{7});
    HDRSHOT_CHECK(snapshot.selection.desktop_rect == (PixelRect{0, 0, 500, 300}));
    if (!accept) {
      host.set_system_dialog_active(true);
      host.set_system_dialog_active(false, true);
      ExportReceipt receipt;
      receipt.destination = UserCancelled{};
      done(Result<ExportReceipt, Error>::success(receipt));
    }
    return accept;
  }, SettingsSnapshot{});
  host.activate_capture(CaptureReadyPayload{frozen,
      PresentReceipt{SessionId{1}, OperationId{1}, FrameId{3}, 9, 1, 0}},
      presenter, std::make_shared<std::atomic<std::uint64_t>>(2));
  QApplication::processEvents();
  auto* button = host.findChild<QToolButton*>("actionSaveAs");
  HDRSHOT_CHECK(button != nullptr);
  button->click();
  HDRSHOT_CHECK(exports == 1 && finishes == 0 && host.isVisible());
  HDRSHOT_CHECK(window->dialogs == 2 && window->fronts == 1);
  accept = true;
  button->click();
  HDRSHOT_CHECK(exports == 2 && finishes == 1 && !host.isVisible());
}

void permission_error_uses_injected_guidance() {
  const Error error{ErrorCode::permission_denied, "test", Retryability::after_user_action, {}};
  HDRSHOT_CHECK(capture_error_message(error, QStringLiteral("platform permission instructions")) ==
      QStringLiteral("platform permission instructions"));
}

void first_capture_uses_current_format_snapshot() {
  hdrshot::test::TestInputPlatformAdapter input;
  SettingsSnapshot settings;
  for (const auto format : {SaveFormat::png_display_p3_dual_range,
                           SaveFormat::ultra_hdr_jpeg,
                           SaveFormat::png_display_p3_dual_range}) {
    settings.save_format = format;
    QtOverlayHost host(std::make_unique<Window>(), DisplayId{7},
        SelectionSnapshot{1, {0, 0, 500, 300}}, input);
    host.resize(500, 300);
    auto frozen = std::make_shared<FrozenDesktop>();
    frozen->frame_id = FrameId{3};
    frozen->display_generation = 9;
    CanonicalFrameSegment segment;
    segment.display_id = DisplayId{7};
    segment.size_px = {500, 300};
    segment.point_pixel_scale = 1;
    frozen->canonical_segments.push_back(segment);
    int exports = 0;
    host.set_export_requested([&](UiCommand command, ExportSnapshot snapshot, auto) {
      ++exports;
      HDRSHOT_CHECK(command == UiCommand::copy_and_close);
      HDRSHOT_CHECK(snapshot.save_format == format);
      return true;
    }, settings);
    // Later settings changes cannot mutate an already-created capture snapshot.
    settings.save_format = format == SaveFormat::ultra_hdr_jpeg
        ? SaveFormat::png_display_p3_dual_range : SaveFormat::ultra_hdr_jpeg;
    host.activate_capture(CaptureReadyPayload{frozen,
        PresentReceipt{SessionId{1}, OperationId{1}, FrameId{3}, 9, 1, 0}},
        std::make_shared<Presenter>(), std::make_shared<std::atomic<std::uint64_t>>(2));
    auto* copy = host.findChild<QToolButton*>("actionCopy");
    HDRSHOT_CHECK(copy != nullptr);
    copy->click();
    HDRSHOT_CHECK(exports == 1);
  }
}

void locked_non_target_host_still_allows_escape() {
  hdrshot::test::TestInputPlatformAdapter input;
  QtOverlayHost host(std::make_unique<Window>(), DisplayId{7},
      SelectionSnapshot{1, {0, 0, 500, 300}}, input);
  int finishes = 0;
  host.set_finished([&] { ++finishes; });
  host.set_interaction_locked(true);
  QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
  QApplication::sendEvent(&host, &escape);
  HDRSHOT_CHECK(finishes == 1);
}

void candidate_uses_native_presenter_and_commits_target_only_on_release() {
  hdrshot::test::TestInputPlatformAdapter input;
  QtOverlayHost host(std::make_unique<Window>(), {7}, {1, {}}, input);
  host.resize(500, 300);
  auto frozen = std::make_shared<FrozenDesktop>();
  frozen->frame_id = {3}; frozen->display_generation = 9;
  CanonicalFrameSegment segment;
  segment.display_id = {7}; segment.size_px = {500, 300}; segment.point_pixel_scale = 1;
  frozen->canonical_segments.push_back(segment);
  auto windows = std::make_shared<const WindowSnapshot>(WindowSnapshot{{1}, {1}, 9,
      {{10, {7}, {50, 50, 300, 150}, 0}}});
  auto presenter = std::make_shared<Presenter>();
  int locks = 0;
  host.set_selection_established([&](DisplayId id) { HDRSHOT_CHECK(id == DisplayId{7}); ++locks; });
  host.activate_capture({frozen, {{1}, {1}, {3}, 9, 1, 0}, windows},
      presenter, std::make_shared<std::atomic<std::uint64_t>>(2));
  QApplication::processEvents();
  auto* editor = dynamic_cast<OverlayEditorWidget*>(host.findChild<QWidget*>("overlayEditor"));
  HDRSHOT_CHECK(editor != nullptr);
  HDRSHOT_CHECK(editor->cursor().shape() == Qt::CrossCursor);
  editor->refresh_window_candidate({100, 100});
  HDRSHOT_CHECK(locks == 0 && presenter->last->model.selection.desktop_rect.empty());
  HDRSHOT_CHECK(preview_selection_rect(presenter->last->model) == (PixelRect{50, 50, 300, 150}));
  HDRSHOT_CHECK(presenter->last->model.frozen_desktop == frozen);
  const auto revision = presenter->last->model.initial_highlight.revision;
  QMouseEvent down(QEvent::MouseButtonPress, {100, 100}, {100, 100}, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(editor, &down);
  HDRSHOT_CHECK(locks == 0);
  QMouseEvent up(QEvent::MouseButtonRelease, {100, 100}, {100, 100}, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(editor, &up);
  HDRSHOT_CHECK(locks == 1);
  HDRSHOT_CHECK(presenter->last->model.selection.desktop_rect == (PixelRect{50, 50, 300, 150}));
  HDRSHOT_CHECK(presenter->last->model.initial_highlight.rect.empty());
  HDRSHOT_CHECK(presenter->last->model.initial_highlight.revision > revision);
}

struct RecoveryFixture {
  hdrshot::test::TestInputPlatformAdapter input;
  Window* native{};
  std::unique_ptr<QtOverlayHost> host;
  OverlayEditorWidget* editor{};
  int finishes{}, exports{}, locks{};
  explicit RecoveryFixture(std::shared_ptr<OverlayInputRecovery> recovery = std::make_shared<OverlayInputRecovery>(),
                           bool selected = false, DisplayId display = {7}) {
    auto port = std::make_unique<Window>();
    native = port.get();
    host = std::make_unique<QtOverlayHost>(std::move(port), display,
        SelectionSnapshot{1, selected ? PixelRect{0, 0, 500, 300} : PixelRect{}}, input, recovery);
    host->resize(500, 300);
    host->set_finished([this] { ++finishes; });
    host->set_selection_established([this](DisplayId) { ++locks; });
    host->set_export_requested([this](UiCommand, ExportSnapshot, auto) { ++exports; return false; }, {});
    auto frozen = std::make_shared<FrozenDesktop>();
    frozen->frame_id = {3}; frozen->display_generation = 9;
    CanonicalFrameSegment segment;
    segment.display_id = display; segment.size_px = {500, 300}; segment.point_pixel_scale = 1;
    frozen->canonical_segments.push_back(segment);
    auto windows = std::make_shared<const WindowSnapshot>(WindowSnapshot{{1}, {1}, 9,
        {{10, display, {50, 50, 300, 150}, 0}}});
    host->activate_capture({frozen, {{1}, {1}, {3}, 9, 1, 0}, windows},
        std::make_shared<Presenter>(), std::make_shared<std::atomic<std::uint64_t>>(2));
    QApplication::processEvents();
    editor = dynamic_cast<OverlayEditorWidget*>(host->findChild<QWidget*>("overlayEditor"));
    HDRSHOT_CHECK(editor != nullptr);
  }
  void pointer(QWidget* target, QEvent::Type type, QPointF point = {100, 100}) {
    const bool move = type == QEvent::MouseMove;
    QMouseEvent event(type, point, point, target->mapToGlobal(point),
        move ? Qt::NoButton : Qt::LeftButton,
        type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(target, &event);
  }
  void click(QWidget* target, QPointF point = {100, 100}) {
    pointer(target, QEvent::MouseButtonPress, point);
    pointer(target, QEvent::MouseButtonRelease, point);
  }
};
void locked_initial_editor_does_not_offer_crosshair() {
  RecoveryFixture f;
  HDRSHOT_CHECK(f.editor->cursor().shape() == Qt::CrossCursor);
  f.host->set_interaction_locked(true);
  HDRSHOT_CHECK(!f.editor->isVisible());
  HDRSHOT_CHECK(f.editor->cursor().shape() == Qt::ArrowCursor);
  f.host->set_interaction_locked(false);
  HDRSHOT_CHECK(f.editor->isVisible());
  HDRSHOT_CHECK(f.editor->cursor().shape() == Qt::CrossCursor);
}

void recovery_press_drag_release_cannot_select_and_next_click_can() {
  RecoveryFixture f;
  f.editor->refresh_window_candidate({100, 100});
  f.native->interrupted();
  // The native OS may deliver activation BEFORE our mouse press. History wins.
  QEvent active(QEvent::WindowActivate); QApplication::sendEvent(f.host.get(), &active);
  f.pointer(f.editor, QEvent::MouseButtonPress);
  HDRSHOT_CHECK(f.native->recoveries == 1 && f.locks == 0);
  f.pointer(f.editor, QEvent::MouseMove, {250, 150});
  f.pointer(f.editor, QEvent::MouseButtonRelease, {250, 150});
  HDRSHOT_CHECK(f.editor->selection().desktop_rect.empty() && f.locks == 0 && f.exports == 0);
  HDRSHOT_CHECK(!f.editor->initial_highlight().rect.empty());
  HDRSHOT_CHECK(f.editor->cursor().shape() == Qt::CrossCursor);
  f.click(f.editor);
  HDRSHOT_CHECK(f.locks == 1 && f.exports == 0);
}
void next_system_double_click_is_ordinary_press_not_export() {
  for (const bool selected : {false, true}) {
    RecoveryFixture f(std::make_shared<OverlayInputRecovery>(), selected);
    f.native->interrupted();
    f.click(f.editor);
    f.pointer(f.editor, QEvent::MouseButtonDblClick);
    f.pointer(f.editor, QEvent::MouseButtonRelease);
    HDRSHOT_CHECK(!f.editor->selection().desktop_rect.empty() && f.exports == 0);
    f.click(f.editor, {220, 160});
    f.pointer(f.editor, QEvent::MouseButtonDblClick, {220, 160});
    HDRSHOT_CHECK(f.exports == 1);
  }
}
void recovery_is_shared_across_screens_and_escape_remains_cancel() {
  auto recovery = std::make_shared<OverlayInputRecovery>();
  RecoveryFixture a(recovery, false, {7}), b(recovery, false, {8});
  a.native->interrupted(); b.native->interrupted();
  a.click(a.editor);
  b.click(b.editor);
  HDRSHOT_CHECK(a.locks == 0 && b.locks == 1);
  HDRSHOT_CHECK(a.native->recoveries == 1 && b.native->recoveries == 0);
  QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
  QApplication::sendEvent(b.editor, &escape);
  HDRSHOT_CHECK(b.finishes == 1);
}
void first_recovery_click_on_copy_button_does_not_fire_action() {
  RecoveryFixture f(std::make_shared<OverlayInputRecovery>(), true);
  auto* copy = f.host->findChild<QToolButton*>("actionCopy");
  HDRSHOT_CHECK(copy != nullptr);
  f.native->interrupted();
  f.click(copy, copy->rect().center());
  HDRSHOT_CHECK(f.exports == 0);
  f.click(copy, copy->rect().center());
  HDRSHOT_CHECK(f.exports == 1);
}
void normal_leave_focus_transfer_and_save_dialog_do_not_arm_recovery() {
  RecoveryFixture f;
  QEvent leave(QEvent::Leave); QApplication::sendEvent(f.editor, &leave);
  QEvent inactive(QEvent::WindowDeactivate); QApplication::sendEvent(f.host.get(), &inactive);
  QEvent active(QEvent::WindowActivate); QApplication::sendEvent(f.host.get(), &active);
  f.host->set_system_dialog_active(true);
  f.native->interrupted();
  f.host->set_system_dialog_active(false, true);
  f.click(f.editor);
  HDRSHOT_CHECK(f.locks == 1 && f.native->recoveries == 0);
}
void application_deactivation_enters_same_recovery_gate() {
  RecoveryFixture f;
  QEvent inactive(QEvent::ApplicationDeactivate); QApplication::sendEvent(qApp, &inactive);
  QEvent active(QEvent::ApplicationActivate); QApplication::sendEvent(qApp, &active);
  f.click(f.editor);
  HDRSHOT_CHECK(f.locks == 0 && f.native->recoveries == 1);
  f.click(f.editor);
  HDRSHOT_CHECK(f.locks == 1);
}
void recovery_preserves_pending_text_and_cancels_uncommitted_drawing() {
  {
    RecoveryFixture f(std::make_shared<OverlayInputRecovery>(), true);
    f.host->findChild<QToolButton*>("toolText")->click();
    f.click(f.editor);
    auto* text = f.host->findChild<QLineEdit*>();
    HDRSHOT_CHECK(text != nullptr);
    text->setText(QStringLiteral("unfinished"));
    QFocusEvent focus_out(QEvent::FocusOut, Qt::OtherFocusReason);
    QApplication::sendEvent(text, &focus_out);
    HDRSHOT_CHECK(f.native->recoveries == 0);
    f.native->interrupted();
    f.click(f.editor, {400, 100});
    HDRSHOT_CHECK(f.native->recoveries == 1 && f.exports == 0);
    HDRSHOT_CHECK(f.host->findChild<QLineEdit*>() == text);
    HDRSHOT_CHECK(text->text() == QStringLiteral("unfinished"));
    HDRSHOT_CHECK(f.editor->annotations().objects.empty());
  }
  {
    RecoveryFixture f(std::make_shared<OverlayInputRecovery>(), true);
    f.host->findChild<QToolButton*>("toolRectangle")->click();
    f.pointer(f.editor, QEvent::MouseButtonPress);
    f.pointer(f.editor, QEvent::MouseMove, {200, 130});
    f.native->interrupted();
    f.pointer(f.editor, QEvent::MouseButtonRelease, {200, 130});
    HDRSHOT_CHECK(f.editor->annotations().objects.empty());
    f.click(f.editor); // restore only
    HDRSHOT_CHECK(f.editor->annotations().objects.empty());
    f.pointer(f.editor, QEvent::MouseButtonPress);
    f.pointer(f.editor, QEvent::MouseMove, {200, 130});
    f.pointer(f.editor, QEvent::MouseButtonRelease, {200, 130});
    HDRSHOT_CHECK(f.editor->annotations().objects.size() == 1);
  }
}
class DeferredPresenter final : public PreviewPresenterPort {
 public:
  std::vector<PresentPreviewRequest> requests;
  std::vector<Completion> completions;
  void present(const PresentPreviewRequest& r, Completion c) override { requests.push_back(r); completions.push_back(std::move(c)); }
  void cancel(SessionId, OperationId) override {}
};
class CountingText final : public TextRasterizerPort {
 public:
  int calls{};
  Result<TextMetrics,Error> measure(const TextMeasureRequest&) override {
    return Result<TextMetrics,Error>::success({{60,24},"fixture"});
  }
  Result<TextCoverageMask,Error> rasterize(const TextRasterRequest& r) override {
    ++calls;
    return Result<TextCoverageMask,Error>::success({r.object_id,r.mask_size_px,
        std::vector<std::uint8_t>(static_cast<std::size_t>(r.mask_size_px.width*r.mask_size_px.height),128),"fixture"});
  }
};
void pending_text_exports_final_clean_revision_without_present_completion(bool analyze = false) {
  test::TestInputPlatformAdapter input;
  QtOverlayHost host(std::make_unique<Window>(),{7},{1,{0,0,500,300}},input);
  host.resize(500,300);
  auto source=std::make_shared<FrozenDesktop>();source->frame_id={3};source->display_generation=9;
  CanonicalFrameSegment segment;segment.display_id={7};segment.size_px={500,300};
  segment.encoding={ColorPrimaries::display_p3,TransferFunction::linear,AlphaMode::opaque,0};
  segment.rgba_float.assign(500*300*4,2.0F);source->canonical_segments.push_back(std::move(segment));
  auto text=std::make_shared<CountingText>();auto presenter=std::make_shared<DeferredPresenter>();
  host.set_text_rasterizer(text,"fixture");host.set_clean_composition(std::make_shared<CpuCleanComposition>());
  std::optional<ExportSnapshot> accepted; int export_count=0;
  host.set_export_requested([&](UiCommand,ExportSnapshot snapshot,auto done) {
    accepted=std::move(snapshot);++export_count;
    ExportReceipt receipt;receipt.destination=UserCancelled{};
    done(Result<ExportReceipt,Error>::success(receipt));return false;
  },SettingsSnapshot{});
  if(analyze) host.set_analysis_requested([&](ExportSnapshot snapshot,auto done) {
    accepted=std::move(snapshot);++export_count;
    done(Result<bool,Error>::failure({ErrorCode::presenter_failed,"fixture",Retryability::same_input,{}}));
  });
  host.activate_capture({source,{{1},{1},{3},9,1,0}},presenter,std::make_shared<std::atomic<std::uint64_t>>(2));
  QApplication::processEvents();
  auto* editor=dynamic_cast<OverlayEditorWidget*>(host.findChild<QWidget*>("overlayEditor"));
  HDRSHOT_CHECK(editor!=nullptr);
  host.findChild<QToolButton*>("toolText")->click();
  QMouseEvent press(QEvent::MouseButtonPress,QPointF(80,80),QPointF(80,80),Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
  QApplication::sendEvent(editor,&press);
  QMouseEvent release(QEvent::MouseButtonRelease,QPointF(150,110),QPointF(150,110),Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
  QApplication::sendEvent(editor,&release);
  auto* field=host.findChild<QLineEdit*>("annotationTextEditor");HDRSHOT_CHECK(field!=nullptr);
  field->setText("final text");
  HDRSHOT_CHECK(text->calls==0);
  host.findChild<QToolButton*>(analyze ? "actionAnalyze" : "actionCopy")->click();
  HDRSHOT_CHECK(accepted.has_value() && text->calls==1);
  HDRSHOT_CHECK(accepted->annotations.objects.size()==1 && accepted->clean_content!=nullptr);
  HDRSHOT_CHECK(accepted->clean_document_revision==accepted->annotations.revision);
  HDRSHOT_CHECK(!accepted->clean_content->owned.empty());
  const auto content=accepted->clean_content;
  host.findChild<QToolButton*>(analyze ? "actionAnalyze" : "actionCopy")->click();
  HDRSHOT_CHECK(accepted->clean_content==content && text->calls==1 && export_count==2);
  // Every screen refresh is still pending; export nevertheless owns final content.
  HDRSHOT_CHECK(!presenter->completions.empty());
  if(analyze) {
    HDRSHOT_CHECK(host.isVisible() && editor->isEnabled());
    int finished=0;
    QtOverlayHost::AnalysisCompleted done;
    host.set_finished([&] { ++finished; });
    host.set_analysis_requested([&](ExportSnapshot,auto callback) { done=std::move(callback); });
    host.findChild<QToolButton*>("actionAnalyze")->click();
    HDRSHOT_CHECK(finished==0 && !editor->isEnabled());
    done(Result<bool,Error>::success(true));
    HDRSHOT_CHECK(finished==1 && !host.isVisible());
  }
}

}
int main(int argc, char** argv) {
  QApplication app(argc, argv);
  return hdrshot::test::run({
      {"pending text exports final clean revision before async screen completion",[]{pending_text_exports_final_clean_revision_without_present_completion();}},
      {"analysis commits text, retains failures, waits for ROI handoff",[]{pending_text_exports_final_clean_revision_without_present_completion(true);}},
      {"shared target lock and late session callbacks", session_target_lock_and_stale_finish_are_shared},
      {"locked initial editor never offers crosshair", locked_initial_editor_does_not_offer_crosshair},
      {"recovery consumes full gesture then next click selects", recovery_press_drag_release_cannot_select_and_next_click_can},
      {"recovery does not pair into completion double click", next_system_double_click_is_ordinary_press_not_export},
      {"multi-display recovery is consumed once and Esc cancels", recovery_is_shared_across_screens_and_escape_remains_cancel},
      {"recovery click cannot activate copy child control", first_recovery_click_on_copy_button_does_not_fire_action},
      {"internal focus transfers and dialogs do not consume normal click", normal_leave_focus_transfer_and_save_dialog_do_not_arm_recovery},
      {"application deactivation uses shared recovery gate", application_deactivation_enters_same_recovery_gate},
      {"recovery preserves pending text and discards interrupted drag", recovery_preserves_pending_text_and_cancels_uncommitted_drawing},
      {"candidate uses native renderer; release locks target", candidate_uses_native_presenter_and_commits_target_only_on_release},
      {"shared host owns completion lifecycle", shared_host_snapshot_dialog_cancel_and_acceptance},
      {"first capture reads current format and freezes it", first_capture_uses_current_format_snapshot},
      {"non-target display permits cancellation", locked_non_target_host_still_allows_escape},
      {"permission explanation from platform", permission_error_uses_injected_guidance}});
}
