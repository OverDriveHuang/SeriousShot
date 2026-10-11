#include "ui/qt/qt_overlay_host.hpp"
#include "ui/qt/capture_error_message.hpp"
#include "application/capture_interaction_session.hpp"
#include "qt_test_input_platform_adapter.hpp"
#include "test_support.hpp"
#include <QApplication>
#include <QLineEdit>
#include <QToolButton>
#include <QElapsedTimer>
#include <QThread>
#include <QImage>
#include <fstream>
#include <condition_variable>
#include <chrono>
#include <future>

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

class GatedPixelSource final : public LinearSource {
 public:
  PixelSize size_px() const override{return {1000,600};}
  std::size_t byte_count() const override{return 1000*600*16;}
  Result<bool,Error> wait_until_ready() const override {++waits;return Result<bool,Error>::success(true);}
  Result<LinearFloatPixels,Error> read_region(PixelRect r) const override {
    std::unique_lock lock(mutex);regions.push_back(r);entered.notify_all();
    if(block_first && regions.size()==1) release.wait(lock,[&]{return released;});
    return Result<LinearFloatPixels,Error>::success(LinearFloatPixels{float(r.x),float(r.y),4.F,1.F});
  }
  void unblock(){std::lock_guard lock(mutex);released=true;release.notify_all();}
  bool await_first(){std::unique_lock lock(mutex);return entered.wait_for(lock,std::chrono::seconds(2),[&]{return !regions.empty();});}
  mutable std::mutex mutex;mutable std::condition_variable entered,release;
  mutable std::vector<PixelRect> regions;mutable std::atomic<int> waits{};
  bool block_first{true},released{};
};
FrozenDesktopRef magnifier_source(const std::shared_ptr<GatedPixelSource>& pixel,double scale=1) {
  auto frozen=std::make_shared<FrozenDesktop>();frozen->frame_id={31};frozen->display_generation=9;
  CanonicalFrameSegment s;s.display_id={7};s.desktop_frame_points={-800,-400,1000/scale,600/scale};s.point_pixel_scale=scale;s.size_px={1000,600};s.pixel_format=PixelFormat::rgba32_float;s.encoding={ColorPrimaries::display_p3,TransferFunction::linear,AlphaMode::opaque,0};s.linear_source=pixel;frozen->canonical_segments.push_back(std::move(s));return frozen;
}
void tiny_sampler_coalesces_stale_results_and_releases_cancelled_owners() {
  auto pixels=std::make_shared<GatedPixelSource>();auto frozen=magnifier_source(pixels);
  MagnifierSampler sampler;
  std::mutex delivered_mutex;std::vector<MagnifierSampleResult> delivered;
  const auto done=[&](auto result){std::lock_guard lock(delivered_mutex);delivered.push_back(std::move(result));};
  sampler.submit({frozen,{7},{10,20},1,203},done);
  const bool entered=pixels->await_first();
  if(!entered)pixels->unblock(); HDRSHOT_CHECK(entered);
  for(std::uint64_t r=2;r<=100;++r) sampler.submit({frozen,{7},{int(r),40},r,203},done);
  const auto busy=sampler.counters();
  pixels->unblock(); // unblock before any assertion can unwind fixture references
  HDRSHOT_CHECK(busy.in_flight==1 && busy.pending==1 && busy.readbacks==1 && busy.requested==100);
  const auto drained=[&]{std::lock_guard lock(delivered_mutex);return delivered.size()==1 && sampler.counters().in_flight==0 && sampler.counters().pending==0;};
  QElapsedTimer timer;timer.start();while(!drained() && timer.elapsed()<2000) QThread::msleep(1);
  HDRSHOT_CHECK(drained());
  {std::lock_guard lock(delivered_mutex);HDRSHOT_CHECK(delivered.size()==1);HDRSHOT_CHECK(delivered[0].revision==100 && delivered[0].point==(PixelPoint{100,40}));HDRSHOT_CHECK(delivered[0].readout.source==(analysis::Rgb{100,40,4}));}
  HDRSHOT_CHECK(pixels->regions.size()==2 && pixels->waits==0);
  for(const auto r:pixels->regions)HDRSHOT_CHECK(r.width==1 && r.height==1);
  for(bool close : {false,true}) {
    auto blocked=std::make_shared<GatedPixelSource>();auto owner=magnifier_source(blocked);
    std::weak_ptr<const FrozenDesktop> weak=owner;std::atomic<int> callbacks{};
    MagnifierSampler cancelled;
    cancelled.submit({owner,{7},{1,2},1,203},[&](auto){++callbacks;});
    const bool began=blocked->await_first();if(!began)blocked->unblock();HDRSHOT_CHECK(began);
    cancelled.submit({owner,{7},{3,4},2,203},[&](auto){++callbacks;});
    if(close)cancelled.close();else cancelled.invalidate();
    owner.reset();blocked->unblock();timer.restart();
    while((cancelled.counters().in_flight || !weak.expired()) && timer.elapsed()<2000) QThread::msleep(1);
    HDRSHOT_CHECK(!cancelled.counters().in_flight && callbacks==0 && weak.expired());
    HDRSHOT_CHECK(blocked->regions.size()==1 && cancelled.counters().pending==0);
  }
}

void host_magnifier_pairs_physical_coordinates_and_off_does_no_work() {
  hdrshot::test::TestInputPlatformAdapter input;
  for(double scale : {1.,1.25,1.5,2.}) for(bool enabled : {false,true}) {
    auto native=std::make_unique<Window>();
    QtOverlayHost host(std::move(native),{7},{1,{}},input);
    host.resize(int(1000/scale),int(600/scale));
    auto pixels=std::make_shared<GatedPixelSource>();pixels->block_first=enabled;
    const auto frozen=magnifier_source(pixels,scale);
    SettingsSnapshot settings;settings.analysis_defaults.magnifier_enabled=enabled;
    host.set_export_requested({},settings);
    auto presenter=std::make_shared<Presenter>();
    host.activate_capture({frozen,{{1},{1},{31},9,1,0}},presenter,std::make_shared<std::atomic<std::uint64_t>>(2));
    auto* editor=dynamic_cast<OverlayEditorWidget*>(host.findChild<QWidget*>("overlayEditor"));HDRSHOT_CHECK(editor);
    const auto spin=[&](auto predicate){QElapsedTimer t;t.start();while(!predicate() && t.elapsed()<1500){QApplication::processEvents();QThread::msleep(1);}return predicate();};
    const auto move=[&](QPointF at){QMouseEvent event(QEvent::MouseMove,at,at,QPointF(editor->mapToGlobal(at.toPoint())),Qt::NoButton,Qt::NoButton,Qt::NoModifier);QApplication::sendEvent(editor,&event);};
    const QPointF point{123.,87.};move(point);
    const PixelPoint expected{int(std::floor(123*scale)),int(std::floor(87*scale))};
    if(enabled) {
      const bool immediate=host.magnifier_preview().visible && host.magnifier_preview().center==expected;
      const bool sampling=pixels->await_first();pixels->unblock();
      HDRSHOT_CHECK(immediate && sampling);
      HDRSHOT_CHECK(spin([&]{return host.magnifier_preview().visible && host.magnifier_preview().center==expected;}));
      const auto preview=host.magnifier_preview();HDRSHOT_CHECK(valid_magnifier_preview(preview,{1000,600}));
      auto* panel=dynamic_cast<CaptureMagnifierPanel*>(host.findChild<QWidget*>("captureMagnifierPanel"));HDRSHOT_CHECK(panel && panel->isVisible());
      HDRSHOT_CHECK_NEAR(panel->x()*scale,preview.viewport.x,.51);
      HDRSHOT_CHECK_NEAR((panel->y()+CaptureMagnifierPanel::header_height())*scale,preview.viewport.y,.51);
      HDRSHOT_CHECK(std::abs(panel->width()*scale-preview.viewport.width)<scale+0.01);
      const auto values=panel->findChildren<QLabel*>("magnifierScalarValue");HDRSHOT_CHECK(values.size()>=4);
      for(const auto* value:values) HDRSHOT_CHECK(value->width()>=value->fontMetrics().horizontalAdvance(value->text()));
      HDRSHOT_CHECK(presenter->last->model.magnifier.center==expected);
      HDRSHOT_CHECK(spin([&]{return host.magnifier_counters().delivered>0;}));
      const auto before=host.magnifier_counters().readbacks;
      for(int i=0;i<50;++i)move(point);
      QApplication::processEvents();QThread::msleep(10);QApplication::processEvents();
      HDRSHOT_CHECK(host.magnifier_counters().readbacks==before);
      host.set_system_dialog_active(true);QApplication::processEvents();
      HDRSHOT_CHECK(!host.magnifier_preview().visible);
      const auto stopped=host.magnifier_counters().readbacks;
      move({100,100});QApplication::processEvents();HDRSHOT_CHECK(host.magnifier_counters().readbacks==stopped);
    } else {
      for(int i=0;i<20;++i)move({double(i+100),80});
      QApplication::processEvents();
      HDRSHOT_CHECK(host.magnifier_counters().requested==0 && host.magnifier_counters().readbacks==0);
      HDRSHOT_CHECK(!host.magnifier_preview().visible && pixels->regions.empty());
    }
  }
}

void magnifier_hole_excludes_existing_annotation_during_selection_resize() {
  hdrshot::test::TestInputPlatformAdapter input;
  QtOverlayHost host(std::make_unique<Window>(),{7},{1,{0,0,1000,600}},input);
  host.resize(1000,600);
  auto pixels=std::make_shared<GatedPixelSource>();pixels->block_first=false;
  host.activate_capture({magnifier_source(pixels),{{1},{1},{31},9,1,0}},
      std::make_shared<Presenter>(),std::make_shared<std::atomic<std::uint64_t>>(2));
  auto* editor=dynamic_cast<OverlayEditorWidget*>(host.findChild<QWidget*>("overlayEditor"));
  HDRSHOT_CHECK(editor);
  const auto mouse=[&](QEvent::Type type,QPointF at,Qt::MouseButton button,Qt::MouseButtons buttons){
    QMouseEvent event(type,at,at,QPointF(editor->mapToGlobal(at.toPoint())),button,buttons,Qt::NoModifier);
    QApplication::sendEvent(editor,&event);
  };
  host.findChild<QToolButton*>("toolRectangle")->click();
  mouse(QEvent::MouseButtonPress,{180,180},Qt::LeftButton,Qt::LeftButton);
  mouse(QEvent::MouseMove,{300,300},Qt::NoButton,Qt::LeftButton);
  mouse(QEvent::MouseButtonRelease,{300,300},Qt::LeftButton,Qt::NoButton);
  HDRSHOT_CHECK(editor->annotations().objects.size()==1);
  host.findChild<QToolButton*>("toolSelect")->click();
  const auto painted=[&] {
    QImage image(editor->size(),QImage::Format_ARGB32_Premultiplied);image.fill(Qt::transparent);
    QPainter painter(&image);editor->render(&painter,QPoint(),QRegion(),QWidget::RenderFlags{});
    return image;
  };
  HDRSHOT_CHECK(painted().pixelColor(240,180).alpha()>0);
  mouse(QEvent::MouseButtonPress,{0,300},Qt::LeftButton,Qt::LeftButton);
  mouse(QEvent::MouseMove,{120,300},Qt::NoButton,Qt::LeftButton);
  const auto preview=host.magnifier_preview();
  HDRSHOT_CHECK(preview.visible);
  const QRect hole(preview.viewport.x,preview.viewport.y,preview.viewport.width,preview.viewport.height);
  HDRSHOT_CHECK(hole.contains(240,180));
  const auto excluded=painted();
  for(int y=hole.top()+2;y<hole.bottom()-1;++y)
    for(int x=hole.left()+2;x<hole.right()-1;++x)
      HDRSHOT_CHECK(excluded.pixelColor(x,y).alpha()==0);
  host.set_system_dialog_active(true);
  HDRSHOT_CHECK(!host.magnifier_preview().visible);
  HDRSHOT_CHECK(painted().pixelColor(240,180).alpha()>0);
}

void magnifier_capture_defaults_are_frozen_and_next_session_reads_new_defaults() {
  hdrshot::test::TestInputPlatformAdapter input;
  SettingsSnapshot settings;
  settings.analysis_defaults.magnifier_zoom=4;
  settings.analysis_defaults.reference_white_nits=100;
  for(int session=0;session<3;++session) {
    const auto expected=settings.analysis_defaults;
    QtOverlayHost host(std::make_unique<Window>(),{7},{1,{}},input);
    host.resize(1000,600);
    host.set_export_requested({},settings);
    auto pixels=std::make_shared<GatedPixelSource>();pixels->block_first=false;
    host.activate_capture({magnifier_source(pixels),{{1},{1},{31},9,1,0}},
        std::make_shared<Presenter>(),std::make_shared<std::atomic<std::uint64_t>>(2));
    auto* editor=dynamic_cast<OverlayEditorWidget*>(host.findChild<QWidget*>("overlayEditor"));
    HDRSHOT_CHECK(editor);
    // A default change after creation belongs to the next capture, including
    // readings and reference white; ordinary pointer motion cannot reload it.
    settings.analysis_defaults.magnifier_zoom=16;
    settings.analysis_defaults.reference_white_nits=203;
    settings.analysis_defaults.magnifier.selected.fill(false);
    settings.analysis_defaults.magnifier.selected[3]=true;
    settings.analysis_defaults.magnifier_enabled=session==0;
    for(const QPointF at:{QPointF(10,20),QPointF(11,21)}) {
      QMouseEvent event(QEvent::MouseMove,at,at,QPointF(editor->mapToGlobal(at.toPoint())),Qt::NoButton,Qt::NoButton,Qt::NoModifier);
      QApplication::sendEvent(editor,&event);
      if(!expected.magnifier_enabled) {
        HDRSHOT_CHECK(!host.magnifier_preview().visible && host.magnifier_counters().requested==0);
        continue;
      }
      HDRSHOT_CHECK(host.magnifier_preview().zoom==expected.magnifier_zoom);
      std::vector<QString> values;
      for(const auto& row:magnifier_scalar_rows(magnifier_readout({at.x(),at.y(),4},expected.reference_white_nits,false),expected.magnifier))
        if(!row.heading)values.push_back(row.value);
      const auto correct=[&] {
        const auto actual=host.findChildren<QLabel*>("magnifierScalarValue");
        if(std::size_t(actual.size())!=values.size())return false;
        for(std::size_t i=0;i<values.size();++i)if(actual[qsizetype(i)]->text()!=values[i])return false;
        return true;
      };
      QElapsedTimer timer;timer.start();
      while(!correct() && timer.elapsed()<2000){QApplication::processEvents();QThread::msleep(1);}
      HDRSHOT_CHECK(correct());
    }
  }
}

void magnifier_late_readback_cannot_restore_hidden_cancelled_or_destroyed_host() {
  hdrshot::test::TestInputPlatformAdapter input;
  for(int action=0;action<4;++action) {
    auto host=std::make_unique<QtOverlayHost>(std::make_unique<Window>(),DisplayId{7},SelectionSnapshot{1,{}},input);
    host->resize(1000,600);
    auto pixels=std::make_shared<GatedPixelSource>();
    auto presenter=std::make_shared<Presenter>();
    auto frozen=magnifier_source(pixels);
    std::weak_ptr<const FrozenDesktop> weak_source=frozen;
    host->activate_capture({frozen,{{1},{1},{31},9,1,0}},presenter,
        std::make_shared<std::atomic<std::uint64_t>>(2));
    frozen.reset();
    auto* editor=dynamic_cast<OverlayEditorWidget*>(host->findChild<QWidget*>("overlayEditor"));
    const QPointF at(20,40);
    QMouseEvent event(QEvent::MouseMove,at,at,QPointF(editor->mapToGlobal(at.toPoint())),Qt::NoButton,Qt::NoButton,Qt::NoModifier);
    QApplication::sendEvent(editor,&event);
    const bool began=pixels->await_first();
    if(!began)pixels->unblock();HDRSHOT_CHECK(began);
    QApplication::processEvents();
    if(action==0)host->hide();
    if(action==1)host->set_interaction_locked(true);
    if(action==2){QKeyEvent escape(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);QApplication::sendEvent(host.get(),&escape);}
    if(action==3){host.reset();presenter->last.reset();}
    const bool hidden=!host || !host->magnifier_preview().visible;
    const auto frames=presenter->frames;
    pixels->unblock();
    QElapsedTimer timer;timer.start();
    const auto finished=[&]{return host ? host->magnifier_counters().in_flight==0 : weak_source.expired();};
    while(!finished() && timer.elapsed()<2000){QApplication::processEvents();QThread::msleep(1);}
    QApplication::processEvents();
    HDRSHOT_CHECK(finished());
    HDRSHOT_CHECK(hidden);
    HDRSHOT_CHECK(!host || (!host->magnifier_preview().visible && host->magnifier_counters().delivered==0));
    HDRSHOT_CHECK(presenter->frames==frames);
    std::lock_guard lock(pixels->mutex);HDRSHOT_CHECK(pixels->regions.size()==1);
  }
}

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
  const auto screenshot=qEnvironmentVariable("HDRSHOT_MAGNIFIER_SCREENSHOT");
  if(!screenshot.isEmpty()){
    if(qEnvironmentVariableIsSet("HDRSHOT_MAGNIFIER_DARK")){auto pal=app.palette();pal.setColor(QPalette::Window,QColor("#25282d"));pal.setColor(QPalette::WindowText,Qt::white);app.setPalette(pal);}
    QWidget parent;CaptureMagnifierPanel panel(&parent);MagnifierReadings fields;
    if(qEnvironmentVariableIsSet("HDRSHOT_MAGNIFIER_ALL"))fields.selected.fill(true);
    panel.resize(216,CaptureMagnifierPanel::header_height()+216+panel.text_height(fields));
    analysis::Rgb rgb{.5,.3,.1};const auto rgb_path=qEnvironmentVariable("HDRSHOT_MAGNIFIER_NATIVE_RGB");if(!rgb_path.isEmpty()){std::ifstream file(rgb_path.toStdString());for(auto& c:rgb)file>>c;HDRSHOT_CHECK(file.good());}
    panel.show_sample({159,240},magnifier_readout(rgb,203,false),fields,216,8);
    parent.resize(panel.size());parent.show();QApplication::processEvents();
    auto pixels=panel.grab();const auto native_path=qEnvironmentVariable("HDRSHOT_MAGNIFIER_NATIVE_IMAGE");
    if(!native_path.isEmpty()){QImage native(native_path);HDRSHOT_CHECK(!native.isNull());QPainter paint(&pixels);paint.drawImage(QRect(0,CaptureMagnifierPanel::header_height(),216,216),native);}
    HDRSHOT_CHECK(pixels.save(screenshot));return 0;
  }
  return hdrshot::test::run({
      {"host DPR mapping and paired native viewport, same-pixel dedup, off zero work", host_magnifier_pairs_physical_coordinates_and_off_does_no_work},
      {"magnifier image hole excludes annotation and restores on hide", magnifier_hole_excludes_existing_annotation_during_selection_resize},
      {"capture defaults freeze white/readings and refresh each new session", magnifier_capture_defaults_are_frozen_and_next_session_reads_new_defaults},
      {"delayed readback cannot restore hidden, cancelled or destroyed host", magnifier_late_readback_cannot_restore_hidden_cancelled_or_destroyed_host},
      {"tiny sampler bounds, latest completion and cancellation lifetime", tiny_sampler_coalesces_stale_results_and_releases_cancelled_owners},
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
