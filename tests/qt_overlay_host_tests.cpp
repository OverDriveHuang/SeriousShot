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
#include <QStyle>
#include <fstream>
#include <condition_variable>
#include <chrono>
#include <future>
#include <limits>

using namespace hdrshot;
namespace {
class Window final : public QtOverlayWindowPort {
 public:
  int prepares{}, fronts{}, dialogs{}, resizes{}, recoveries{}, card_moves{}, card_shows{}, card_hides{};
  QRect card_rect;
  void move_magnifier(QWidget&,const QRect& r) override {++card_moves;card_rect=r;}
  void show_magnifier() override {++card_shows;}
  void hide_magnifier() override {++card_hides;}
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
class ReadoutPixelSource final : public LinearSource {
 public:
  explicit ReadoutPixelSource(LinearFloatPixels pixels) : pixels_(std::move(pixels)) {}
  PixelSize size_px() const override { return {1, 1}; }
  std::size_t byte_count() const override { return 4 * sizeof(float); }
  Result<bool, Error> wait_until_ready() const override {
    return Result<bool, Error>::success(true);
  }
  Result<LinearFloatPixels, Error> read_region(PixelRect rect) const override {
    HDRSHOT_CHECK((rect == PixelRect{0, 0, 1, 1}));
    return Result<LinearFloatPixels, Error>::success(pixels_);
  }
 private:
  LinearFloatPixels pixels_;
};
void sampler_frozen_sdr_provenance_reaches_scalar_rows() {
  struct Case {
    analysis::Rgb source;
    bool frozen_sdr, tolerance;
    DisplayDynamicRange range;
    bool valid, sdr;
  };
  const auto first_hdr = std::nextafter(kCaptureSdrMaximumEdr,
      std::numeric_limits<float>::infinity());
  const std::vector<Case> cases{
    {{1.0001456, .9998814, 1.0002469}, true, false, DisplayDynamicRange::sdr, true, true},
    {{1.0040311, .8, .2}, true, false, DisplayDynamicRange::sdr, true, true},
    {{1.0040311, .8, .2}, false, false, DisplayDynamicRange::sdr, true, false},
    {{1.0001456, .9998814, 1.0002469}, false, true, DisplayDynamicRange::hdr, true, true},
    {{kCaptureSdrMaximumEdr, .5, .2}, false, true, DisplayDynamicRange::hdr, true, true},
    {{first_hdr, .5, .2}, false, true, DisplayDynamicRange::hdr, true, false},
    {{2, .5, .2}, false, true, DisplayDynamicRange::hdr, true, false},
    // A contradictory hand-built segment cannot override HDR pixels.
    {{2, .5, .2}, true, true, DisplayDynamicRange::hdr, true, false},
    {{-.1, .3, .7}, true, false, DisplayDynamicRange::sdr, true, true},
    {{std::numeric_limits<double>::quiet_NaN(), .3, .7}, true, false,
        DisplayDynamicRange::sdr, false, false},
  };
  MagnifierReadings fields;
  fields.selected.fill(false);
  for (int i : {1, 4, 7, 9}) fields.selected[i] = true;
  for (bool native : {false, true}) for (const auto& test : cases) {
    auto frozen = std::make_shared<FrozenDesktop>();
    CanonicalFrameSegment segment;
    segment.display_id = {7}; segment.size_px = {1, 1};
    segment.pixel_format = PixelFormat::rgba32_float;
    segment.encoding = {ColorPrimaries::display_p3, TransferFunction::linear,
                        AlphaMode::opaque, 0};
    segment.display_dynamic_range = test.range;
    segment.capture_frozen_sdr = test.frozen_sdr;
    segment.capture_sdr_tolerance = test.tolerance;
    LinearFloatPixels pixels{float(test.source[0]), float(test.source[1]),
                             float(test.source[2]), 1};
    if (native) segment.linear_source = std::make_shared<ReadoutPixelSource>(pixels);
    else segment.rgba_float = pixels;
    frozen->canonical_segments.push_back(std::move(segment));
    auto completion = std::make_shared<std::promise<MagnifierSampleResult>>();
    auto ready = completion->get_future();
    MagnifierSampler sampler;
    sampler.submit({frozen, {7}, {0, 0}, 1, 203},
        [completion](auto result) { completion->set_value(std::move(result)); });
    HDRSHOT_CHECK(ready.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    const auto result = ready.get();
    HDRSHOT_CHECK(result.readout.valid == test.valid);
    HDRSHOT_CHECK(result.readout.sdr == test.sdr);
    if (test.valid) {
      for (int c = 0; c < 3; ++c)
        HDRSHOT_CHECK(result.readout.source[c] == pixels[c]);
      const auto strict = magnifier_readout(result.readout.source, 203, false);
      HDRSHOT_CHECK(result.readout.nits == strict.nits && result.readout.pq == strict.pq);
    }
    for (const auto& row : magnifier_scalar_rows(result.readout, fields))
      if (!row.heading) HDRSHOT_CHECK((row.value != "--") == (test.valid && test.sdr));
  }
}
void magnifier_all_groups_keep_complete_headings_and_scalar_lines() {
  for (bool all : {false, true}) for (int width : {200, 208, 216}) {
    QWidget parent;
    CaptureMagnifierPanel panel(&parent);
    // Force the long new heading to wrap so the previous fixed 19px height
    // fails this regression even on platforms with a small default font.
    if (!all) panel.setFont(QFont(panel.font().family(), 16));
    MagnifierReadings fields;
    fields.selected.fill(all);
    if (!all) fields.selected[9] = true;
    // Compute the very first frame with its target width, before setGeometry.
    const int text_height = panel.text_height(fields, width);
    panel.resize(width, CaptureMagnifierPanel::header_height() + width + text_height);
    panel.show_sample({1, 2}, magnifier_readout({.2, .5, .1}, 203, false), fields, width, 8);
    parent.resize(panel.size());
    parent.show();
    QApplication::processEvents();
    HDRSHOT_CHECK(panel.text_height(fields) == text_height);
    const auto headings = panel.findChildren<QLabel*>("magnifierGroupHeading");
    HDRSHOT_CHECK(headings.size() == (all ? 11 : 1));
    if (!all) HDRSHOT_CHECK(headings.front()->height() > 19);
    QStringList actual;
    for (const auto* label : headings) {
      actual << label->text();
      HDRSHOT_CHECK(label->wordWrap());
      HDRSHOT_CHECK(label->height() >= label->heightForWidth(label->width()));
      HDRSHOT_CHECK(label->geometry().right() < width - 8);
    }
    HDRSHOT_CHECK(actual == (all ? magnifier_reading_names().mid(0, 11)
                                 : QStringList{magnifier_reading_names()[2]}));
    const auto rows = magnifier_scalar_rows(magnifier_readout({.2, .5, .1}, 203, false), fields);
    const auto values = panel.findChildren<QLabel*>("magnifierScalarValue");
    HDRSHOT_CHECK(values.size() == std::count_if(rows.begin(), rows.end(), [](const auto& r) { return !r.heading; }));
    auto* content = panel.findChild<QWidget*>("captureMagnifierReadings");
    for (const auto* label : values) {
      HDRSHOT_CHECK(label->text() != "--");
      HDRSHOT_CHECK(label->mapTo(content, QPoint(0, label->height())).y() <= content->height() - 6);
    }
  }
}
FrozenDesktopRef magnifier_source(const std::shared_ptr<LinearSource>& pixel,double scale=1) {
  auto frozen=std::make_shared<FrozenDesktop>();frozen->frame_id={31};frozen->display_generation=9;
  CanonicalFrameSegment s;s.display_id={7};s.desktop_frame_points={-800,-400,1000/scale,600/scale};s.point_pixel_scale=scale;s.size_px={1000,600};s.pixel_format=PixelFormat::rgba32_float;s.encoding={ColorPrimaries::display_p3,TransferFunction::linear,AlphaMode::opaque,0};s.linear_source=pixel;frozen->canonical_segments.push_back(std::move(s));return frozen;
}
// Every read waits for its own permit. Only tests control latency/failure;
// production sampling and its latest-pending queue remain unchanged.
class SteppedPixelSource final : public LinearSource {
 public:
  PixelSize size_px() const override { return {1000,600}; }
  std::size_t byte_count() const override { return 1000*600*16; }
  Result<bool,Error> wait_until_ready() const override { return Result<bool,Error>::success(true); }
  Result<LinearFloatPixels,Error> read_region(PixelRect rect) const override {
    std::unique_lock lock(mutex_);
    const auto index = ++entered_;
    changed_.notify_all();
    changed_.wait(lock,[&]{return open_ || permits_.size() >= index;});
    if (!open_ && !permits_[index-1])
      return Result<LinearFloatPixels,Error>::failure({ErrorCode::presenter_failed,"gated readback",Retryability::same_input,{}});
    return Result<LinearFloatPixels,Error>::success({float(rect.x)/1000.F,float(rect.y)/1000.F,.4F,1.F});
  }
  bool await_read(std::size_t count) {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock,std::chrono::seconds(2),[&]{return entered_ >= count;});
  }
  void permit(bool success=true) {
    std::lock_guard lock(mutex_);permits_.push_back(success);changed_.notify_all();
  }
  void unblock_all() {
    std::lock_guard lock(mutex_);open_=true;changed_.notify_all();
  }
 private:
  mutable std::mutex mutex_;
  mutable std::condition_variable changed_;
  mutable std::size_t entered_{};
  std::vector<bool> permits_;
  bool open_{};
};
void host_pending_readings_have_one_bounded_grace(bool use_card) {
  struct Card final : MagnifierCardPresenterPort {
    std::optional<MagnifierCardFrame> last;
    void present_card(MagnifierCardFrame frame,Completion done) override {
      const auto revision=frame.revision;last=std::move(frame);
      done(Result<std::uint64_t,Error>::success(revision));
    }
    void invalidate_card(std::uint64_t) override {}
  };
  test::TestInputPlatformAdapter input;
  const auto pixels=std::make_shared<SteppedPixelSource>();
  struct ReleaseOnExit {
    std::shared_ptr<SteppedPixelSource> pixels;
    ~ReleaseOnExit(){pixels->unblock_all();}
  } release{pixels};
  auto native=std::make_unique<Window>();auto* window=native.get();
  QtOverlayHost host(std::move(native),{7},{1,{}},input);host.resize(1000,600);
  auto card=std::make_shared<Card>();
  if(use_card)host.set_magnifier_card(card);
  auto presenter=std::make_shared<Presenter>();
  SettingsSnapshot settings;
  settings.analysis_defaults.magnifier.selected.fill(false);
  settings.analysis_defaults.magnifier.selected[0]=true;
  host.set_export_requested({},settings);
  host.activate_capture({magnifier_source(pixels),{{1},{1},{31},9,1,0}},presenter,
      std::make_shared<std::atomic<std::uint64_t>>(2));
  auto* editor=host.findChild<QWidget*>("overlayEditor");HDRSHOT_CHECK(editor);
  auto* panel=host.findChild<QWidget*>("captureMagnifierPanel");HDRSHOT_CHECK(panel);
  const auto move=[&](PixelPoint point) {
    const QPointF at(point.x,point.y);
    QMouseEvent event(QEvent::MouseMove,at,at,QPointF(editor->mapToGlobal(at.toPoint())),Qt::NoButton,Qt::NoButton,Qt::NoModifier);
    QApplication::sendEvent(editor,&event);
  };
  const auto values=[&] {
    QStringList result;
    for(const auto* label:panel->findChildren<QLabel*>("magnifierScalarValue"))result << label->text();
    return result;
  };
  const auto expected=[&](PixelPoint point) {
    const auto readout=magnifier_readout({float(point.x)/1000.F,float(point.y)/1000.F,.4F},203,false);
    QStringList result;
    for(const auto& row:magnifier_scalar_rows(readout,settings.analysis_defaults.magnifier))
      if(!row.heading)result << row.value;
    return result;
  };
  const auto blank=[&] {
    const auto text=values();return !text.empty() && std::all_of(text.begin(),text.end(),[](const auto& v){return v=="--";});
  };
  const auto spin=[&](auto predicate) {
    QElapsedTimer timer;timer.start();
    while(!predicate() && timer.elapsed()<2000){QApplication::processEvents();QThread::msleep(1);}
    return predicate();
  };
  const auto process_for=[&](int ms) {
    QElapsedTimer timer;timer.start();
    while(timer.elapsed()<ms){QApplication::processEvents();QThread::msleep(1);}
  };
  const auto at=[&](PixelPoint point) {
    HDRSHOT_CHECK(host.magnifier_preview().visible && host.magnifier_preview().center==point);
    bool coordinates=false;
    for(const auto* label:panel->findChildren<QLabel*>())
      coordinates |= label->text()==QString("%1, %2").arg(point.x).arg(point.y);
    HDRSHOT_CHECK(coordinates);
    if(use_card) {
      HDRSHOT_CHECK(card->last && card->last->image.center==point);
      HDRSHOT_CHECK(card->last->revision==host.magnifier_preview().revision);
      HDRSHOT_CHECK(window->card_rect==panel->geometry());
    } else HDRSHOT_CHECK(presenter->last->model.magnifier.center==point);
  };
  move({123,87});HDRSHOT_CHECK(pixels->await_read(1));HDRSHOT_CHECK(blank());at({123,87});
  pixels->permit();HDRSHOT_CHECK(spin([&]{return values()==expected({123,87});}));
  const auto first=values();
  move({200,100});HDRSHOT_CHECK(pixels->await_read(2));
  HDRSHOT_CHECK(values()==first);at({200,100});
  process_for(15);HDRSHOT_CHECK(values()==first);
  pixels->permit();HDRSHOT_CHECK(spin([&]{return values()==expected({200,100});}));
  process_for(110);HDRSHOT_CHECK(values()==expected({200,100})); // replacement cancels expiry

  move({300,100});HDRSHOT_CHECK(pixels->await_read(3));
  HDRSHOT_CHECK(values()==expected({200,100}));
  // Requests keep arriving faster than the grace interval. A restarted timer
  // would leave old digits visible here indefinitely.
  QElapsedTimer motion;motion.start();PixelPoint latest{300,100};
  int moves=0;
  while(motion.elapsed()<130) {
    latest={310+moves++,120};move(latest);process_for(5);
  }
  HDRSHOT_CHECK(moves>2 && blank());at(latest);
  const auto expired_position=panel->geometry();
  process_for(20);HDRSHOT_CHECK(blank() && panel->geometry()==expired_position);at(latest);
  pixels->permit();HDRSHOT_CHECK(pixels->await_read(4)); // obsolete third request is discarded
  process_for(5);HDRSHOT_CHECK(blank());at(latest);
  pixels->permit();
  HDRSHOT_CHECK(spin([&]{return host.magnifier_counters().in_flight==0;}));
  process_for(5);HDRSHOT_CHECK(blank()); // current success alone cannot unlatch moving digits
  std::size_t read=4;
  for(int i=0;i<6;++i) {
    latest={400+i*3,140};move(latest);HDRSHOT_CHECK(pixels->await_read(++read));
    const auto delivered=host.magnifier_counters().delivered;
    pixels->permit();
    HDRSHOT_CHECK(spin([&]{return host.magnifier_counters().delivered>delivered;}));
    process_for(5);HDRSHOT_CHECK(blank());at(latest);
  }
  // Only the last point's cached result may return after 50ms stationary.
  HDRSHOT_CHECK(spin([&]{return values()==expected(latest);}));at(latest);
  process_for(100);HDRSHOT_CHECK(values()==expected(latest));

  move({500,100});HDRSHOT_CHECK(pixels->await_read(++read));
  HDRSHOT_CHECK(values()==expected(latest));
  pixels->permit(false);HDRSHOT_CHECK(spin(blank));at({500,100});
  move({550,100});HDRSHOT_CHECK(pixels->await_read(++read));HDRSHOT_CHECK(blank());
  pixels->permit();HDRSHOT_CHECK(spin([&]{return values()==expected({550,100});}));

  move({570,100});HDRSHOT_CHECK(pixels->await_read(++read));
  process_for(110);HDRSHOT_CHECK(blank()); // already stationary >50ms, but no current result
  const auto delivered=host.magnifier_counters().delivered;
  pixels->permit();
  HDRSHOT_CHECK(spin([&]{return host.magnifier_counters().delivered>delivered;}));
  process_for(5);HDRSHOT_CHECK(values()==expected({570,100})); // no additional settling delay

  move({600,100});HDRSHOT_CHECK(pixels->await_read(++read));HDRSHOT_CHECK(!blank());
  host.set_system_dialog_active(true);HDRSHOT_CHECK(!host.magnifier_preview().visible);
  const auto hidden_shows=window->card_shows;
  process_for(110);HDRSHOT_CHECK(!host.magnifier_preview().visible && window->card_shows==hidden_shows);
  host.set_system_dialog_active(false);
  move({700,100});HDRSHOT_CHECK(blank());at({700,100});
  pixels->permit();HDRSHOT_CHECK(pixels->await_read(++read));
  process_for(5);HDRSHOT_CHECK(blank()); // late hidden request cannot seed the reopened panel
  pixels->permit();HDRSHOT_CHECK(spin([&]{return values()==expected({700,100});}));
  process_for(110);HDRSHOT_CHECK(values()==expected({700,100}));at({700,100});

  move({750,100});HDRSHOT_CHECK(pixels->await_read(++read));HDRSHOT_CHECK(!blank());
  // Re-activation invalidates both queued sampling and old initial callbacks.
  // Two activations before the event loop also leave an obsolete initial timer.
  for(std::uint64_t session : {2U,3U})
    host.activate_capture({magnifier_source(pixels),{{session},{1},{31},9,1,0}},presenter,
        std::make_shared<std::atomic<std::uint64_t>>(2));
  editor=host.findChildren<QWidget*>("overlayEditor").back();
  panel=host.findChildren<QWidget*>("captureMagnifierPanel").back();
  move({780,120});HDRSHOT_CHECK(blank());at({780,120});
  pixels->permit();HDRSHOT_CHECK(pixels->await_read(++read));
  process_for(5);HDRSHOT_CHECK(blank());
  pixels->permit();HDRSHOT_CHECK(spin([&]{return values()==expected({780,120});}));
  process_for(110);HDRSHOT_CHECK(values()==expected({780,120}));at({780,120});

  move({800,100});HDRSHOT_CHECK(pixels->await_read(++read));
  QKeyEvent escape(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);QApplication::sendEvent(&host,&escape);
  pixels->permit();process_for(110);HDRSHOT_CHECK(!host.magnifier_preview().visible && !host.isVisible());
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
  host.set_export_requested([&](UiCommand command, ExportSnapshot snapshot, auto done, QWidget *source) {
    HDRSHOT_CHECK(source == &host);
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
    host.set_export_requested([&](UiCommand command, ExportSnapshot snapshot, auto, QWidget *) {
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
    host->set_export_requested([this](UiCommand, ExportSnapshot, auto, QWidget *) { ++exports; return false; }, {});
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
struct CleanTasks final : ExportTaskExecutorPort {
  std::vector<std::function<void()>> builds,callbacks;
  void background(std::function<void()> f) override{builds.push_back(std::move(f));}
  void main_thread(std::function<void()> f) override{callbacks.push_back(std::move(f));}
  void drain() {
    while(!builds.empty() || !callbacks.empty()) {
      auto work=std::move(builds);builds.clear();for(auto& f:work)f();
      auto done=std::move(callbacks);callbacks.clear();
      std::reverse(done.begin(),done.end());for(auto& f:done)f();
    }
  }
};
// Deterministic scheduling: a newer input arrives before the old build
// callback.
void continuous_annotation_results_publish_within_their_gesture() {
  test::TestInputPlatformAdapter input;
  CleanTasks tasks;
  QtOverlayHost host(std::make_unique<Window>(), {7}, {1, {0, 0, 500, 300}},
                     input);
  host.resize(500, 300);
  auto source = std::make_shared<FrozenDesktop>();
  source->frame_id = {3};
  source->display_generation = 9;
  CanonicalFrameSegment segment;
  segment.display_id = {7};
  segment.size_px = {500, 300};
  segment.encoding = {ColorPrimaries::display_p3, TransferFunction::linear,
                      AlphaMode::opaque, 0};
  segment.rgba_float.assign(500 * 300 * 4, .5F);
  source->canonical_segments.push_back(std::move(segment));
  auto presenter = std::make_shared<Presenter>();
  host.set_background_clean_composition(
      tasks, std::make_shared<CpuCleanComposition>());
  host.set_annotation_preview_optimization(true);
  host.activate_capture({source, {{1}, {1}, {3}, 9, 1, 0}}, presenter,
                        std::make_shared<std::atomic<std::uint64_t>>(2));
  QApplication::processEvents();
  tasks.drain();
  auto* editor = dynamic_cast<OverlayEditorWidget*>(
      host.findChild<QWidget*>("overlayEditor"));
  HDRSHOT_CHECK(editor);
  const auto mouse = [&](QEvent::Type type, QPointF point) {
    QMouseEvent e(
        type, point, point,
        type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
        type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
        Qt::NoModifier);
    QApplication::sendEvent(editor, &e);
  };
  const auto build = [&] {
    auto work = std::move(tasks.builds);
    tasks.builds.clear();
    for (auto& f : work) f();
  };
  const auto publish = [&] {
    auto work = std::move(tasks.callbacks);
    tasks.callbacks.clear();
    for (auto& f : work) f();
  };
  host.findChild<QToolButton*>("toolArrow")->click();
  mouse(QEvent::MouseButtonPress, {60, 60});
  mouse(QEvent::MouseMove, {130, 100});
  QApplication::processEvents();
  const auto before = presenter->frames;
  for (int i = 0; i < 8; ++i) {
    build();
    mouse(QEvent::MouseMove, {double(150 + i * 20), double(110 + i * 8)});
    QApplication::processEvents();
    publish();
    HDRSHOT_CHECK(presenter->frames == before + i + 1);
    HDRSHOT_CHECK(!presenter->last->model.clean_content->owned.empty());
  }
  // Reverse callback order: older same-gesture completion cannot replace newer.
  build();
  mouse(QEvent::MouseMove, {340, 200});
  QApplication::processEvents();
  build();
  const auto previous = presenter->frames;
  std::reverse(tasks.callbacks.begin(), tasks.callbacks.end());
  publish();
  HDRSHOT_CHECK(presenter->frames == previous + 1);
  // Interrupt with unchanged document revision: old transient result is
  // invalid.
  mouse(QEvent::MouseMove, {360, 220});
  QApplication::processEvents();
  build();
  const auto epoch = editor->annotation_preview_epoch();
  editor->interrupt_pointer_gesture();
  HDRSHOT_CHECK(editor->annotation_preview_epoch() > epoch);
  const auto stopped = presenter->frames;
  publish();
  HDRSHOT_CHECK(presenter->frames == stopped);
  tasks.drain();
  HDRSHOT_CHECK(presenter->last->model.clean_content->owned.empty());
  // A new gesture in the same uncommitted document is a separate epoch.
  mouse(QEvent::MouseButtonPress, {60, 60});
  mouse(QEvent::MouseMove, {150, 120});
  QApplication::processEvents();
  build();
  mouse(QEvent::MouseButtonPress, {80, 70});
  const auto restarted = presenter->frames;
  publish();
  HDRSHOT_CHECK(presenter->frames == restarted);
  mouse(QEvent::MouseButtonRelease, {380, 230});
  tasks.drain();
  HDRSHOT_CHECK(editor->annotations().objects.size() == 1);
  HDRSHOT_CHECK(presenter->last->model.annotation_document.revision ==
                editor->annotations().revision);
  HDRSHOT_CHECK(!presenter->last->model.clean_content->owned.empty());
}

void background_publication_ignores_old_document_and_closed_host() {
  test::TestInputPlatformAdapter input;CleanTasks tasks;
  QtOverlayHost host(std::make_unique<Window>(),{7},{1,{0,0,500,300}},input);host.resize(500,300);
  auto source=std::make_shared<FrozenDesktop>();source->frame_id={3};source->display_generation=9;
  CanonicalFrameSegment segment;segment.display_id={7};segment.size_px={500,300};
  segment.encoding={ColorPrimaries::display_p3,TransferFunction::linear,AlphaMode::opaque,0};
  segment.rgba_float.assign(500*300*4,.5F);source->canonical_segments.push_back(std::move(segment));
  auto presenter=std::make_shared<Presenter>();
  host.set_background_clean_composition(tasks,std::make_shared<CpuCleanComposition>());
  host.set_annotation_preview_optimization(true);
  host.activate_capture({source,{{1},{1},{3},9,1,0}},presenter,std::make_shared<std::atomic<std::uint64_t>>(2));
  QApplication::processEvents();tasks.drain();
  auto* editor=dynamic_cast<OverlayEditorWidget*>(host.findChild<QWidget*>("overlayEditor"));HDRSHOT_CHECK(editor);
  const auto draw=[&] {
    host.findChild<QToolButton*>("toolRectangle")->click();
    QMouseEvent press(QEvent::MouseButtonPress,QPointF(80,80),QPointF(80,80),Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease,QPointF(200,180),QPointF(200,180),Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
    QApplication::sendEvent(editor,&press);QApplication::sendEvent(editor,&release);
  };
  const auto build_only=[&] {auto builds=std::move(tasks.builds);tasks.builds.clear();for(auto& task:builds)task();};
  draw();HDRSHOT_CHECK(editor->annotations().objects.size()==1);build_only();
  HDRSHOT_CHECK(!tasks.callbacks.empty()); // Old nonempty composition ready, unpublished.
  host.findChild<QToolButton*>("actionUndo")->click();
  const auto revision=editor->annotations().revision;HDRSHOT_CHECK(editor->annotations().objects.empty());
  build_only();tasks.drain(); // Publish newest first, then the old success.
  HDRSHOT_CHECK(presenter->last->model.annotation_document.revision==revision);
  HDRSHOT_CHECK(presenter->last->model.clean_content && presenter->last->model.clean_content->owned.empty());
  draw();build_only();const auto before=presenter->frames;
  QKeyEvent cancel(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);QApplication::sendEvent(&host,&cancel);
  tasks.drain();HDRSHOT_CHECK(!host.isVisible() && presenter->frames==before);
}
void card_geometry_content_and_late_callbacks_are_separate() {
  struct Card final : MagnifierCardPresenterPort {
    std::vector<MagnifierCardFrame> frames;
    std::vector<Completion> callbacks;
    void present_card(MagnifierCardFrame f,Completion c) override {frames.push_back(std::move(f));callbacks.push_back(std::move(c));}
    void invalidate_card(std::uint64_t) override {}
  };
  test::TestInputPlatformAdapter input;
  auto native=std::make_unique<Window>();auto* window=native.get();
  QtOverlayHost host(std::move(native),{7},{1,{}},input);host.resize(1000,600);
  auto card=std::make_shared<Card>();auto preview=std::make_shared<Presenter>();host.set_magnifier_card(card);
  auto pixels=std::make_shared<GatedPixelSource>();pixels->block_first=false;
  host.activate_capture({magnifier_source(pixels),{{1},{1},{31},9,1,0}},preview,std::make_shared<std::atomic<std::uint64_t>>(2));
  QApplication::processEvents();
  auto* editor=host.findChild<QWidget*>("overlayEditor");HDRSHOT_CHECK(editor);
  const auto move=[&](int x,int y) {
    const QPointF at(x,y);QMouseEvent event(QEvent::MouseMove,at,at,QPointF(editor->mapToGlobal(at.toPoint())),Qt::NoButton,Qt::NoButton,Qt::NoModifier);
    QApplication::sendEvent(editor,&event);
  };
  move(123,87);QApplication::processEvents();
  const int backgrounds=preview->frames;const int moves=window->card_moves;
  for(int i=0;i<30;++i)move(130+i,90+i);
  HDRSHOT_CHECK(window->card_moves==moves+30 && window->card_shows==0);
  HDRSHOT_CHECK(preview->frames==backgrounds && !preview->last->model.magnifier.visible);
  auto* panel=host.findChild<QWidget*>("captureMagnifierPanel");HDRSHOT_CHECK(panel && !panel->isVisible());
  const auto geometry=window->card_rect;
  const auto& frame=card->frames.back();
  HDRSHOT_CHECK(frame.image.center==(PixelPoint{159,119}));
  HDRSHOT_CHECK(frame.ui_rgba.size()==std::size_t(frame.size_px.width)*frame.size_px.height*4);
  HDRSHOT_CHECK(frame.image.viewport.x==0 && frame.image.viewport.y>0);
  const auto center=(std::size_t(frame.image.viewport.y+frame.image.viewport.height/2)*
      std::size_t(frame.size_px.width)+std::size_t(frame.image.viewport.width/2))*4;
  HDRSHOT_CHECK(frame.ui_rgba[center+3]==0); // Native HDR image must remain uncovered.
  bool rgba_border=false;
  for(std::size_t i=0;i<frame.ui_rgba.size();i+=4)
    rgba_border |= frame.ui_rgba[i]==0xbb && frame.ui_rgba[i+1]==0xc2 &&
        frame.ui_rgba[i+2]==0xcc && frame.ui_rgba[i+3]==255;
  HDRSHOT_CHECK(rgba_border); // ARGB painting must still upload straight RGBA bytes.
  auto local=frame.image;local.center={0,0};HDRSHOT_CHECK(valid_magnifier_preview(local,frame.size_px));
  // An older complete frame can show at the latest geometry, without moving it.
  card->callbacks.front()(Result<std::uint64_t,Error>::success(card->frames.front().revision));
  QApplication::processEvents();HDRSHOT_CHECK(window->card_rect==geometry && window->card_shows>0);
  host.set_system_dialog_active(true);const auto shows=window->card_shows;
  card->callbacks.back()(Result<std::uint64_t,Error>::success(frame.revision));
  QApplication::processEvents();HDRSHOT_CHECK(window->card_shows==shows && !host.magnifier_preview().visible);
}
void panel_reuses_controls_and_invalidates_layout_and_theme() {
  QWidget parent;CaptureMagnifierPanel panel(&parent);MagnifierReadings fields;
  panel.resize(216,700);panel.show_sample({1,2},{},fields,216,8,false);
  const auto values=panel.findChildren<QLabel*>("magnifierScalarValue");HDRSHOT_CHECK(!values.empty());
  QPointer<QLabel> first=values.front();
  panel.show_sample({2,3},magnifier_readout({.5,.5,.5},203,false),fields,216,8,false);
  HDRSHOT_CHECK(panel.findChildren<QLabel*>("magnifierScalarValue")==values);
  HDRSHOT_CHECK(values.front()->text()!="--");
  fields.selected.fill(true);panel.show_sample({2,3},{},fields,216,8,false);
  HDRSHOT_CHECK(!first);const auto headings=panel.findChildren<QLabel*>("magnifierGroupHeading");
  HDRSHOT_CHECK(!headings.empty());
  panel.resize(180,900);auto font=panel.font();font.setPointSize(17);panel.setFont(font);
  auto palette=panel.palette();palette.setColor(QPalette::Window,QColor("#101010"));panel.setPalette(palette);
  panel.show_sample({4,5},{},fields,180,16,false);
  HDRSHOT_CHECK(panel.findChildren<QLabel*>("magnifierGroupHeading")==headings);
  HDRSHOT_CHECK(headings.front()->height()>=19 && headings.front()->font().pointSizeF()>=16);
  QImage image(panel.size(),QImage::Format_ARGB32_Premultiplied);image.fill(Qt::transparent);panel.render(&image);
  HDRSHOT_CHECK(image.pixelColor(1,1)==QColor("#25282d"));
  palette.setColor(QPalette::Window,QColor("#eeeeee"));panel.setPalette(palette);
  panel.show_sample({4,5},{},fields,180,16,false);panel.render(&image);
  HDRSHOT_CHECK(image.pixelColor(1,1)==QColor("#fafafa"));
}
void pending_text_exports_final_clean_revision_without_present_completion(bool analyze = false, bool background = false, bool pin = false) {
  test::TestInputPlatformAdapter input;
  QtOverlayHost host(std::make_unique<Window>(),{7},{1,{0,0,500,300}},input);
  host.resize(500,300);
  auto source=std::make_shared<FrozenDesktop>();source->frame_id={3};source->display_generation=9;
  CanonicalFrameSegment segment;segment.display_id={7};segment.size_px={500,300};
  segment.encoding={ColorPrimaries::display_p3,TransferFunction::linear,AlphaMode::opaque,0};
  segment.rgba_float.assign(500*300*4,2.0F);source->canonical_segments.push_back(std::move(segment));
  auto text=std::make_shared<CountingText>();auto presenter=std::make_shared<DeferredPresenter>();
  CleanTasks tasks;
  host.set_text_rasterizer(text,"fixture");
  if (background) {
    host.set_background_clean_composition(
        tasks, std::make_shared<CpuCleanComposition>());
    host.set_annotation_preview_optimization(true);
  } else
    host.set_clean_composition(std::make_shared<CpuCleanComposition>());
  std::optional<ExportSnapshot> accepted; int export_count=0;
  host.set_export_requested([&](UiCommand,ExportSnapshot snapshot,auto done,QWidget *) {
    accepted=std::move(snapshot);++export_count;
    ExportReceipt receipt;receipt.destination=UserCancelled{};
    done(Result<ExportReceipt,Error>::success(receipt));return false;
  },SettingsSnapshot{});
  auto prepare_action = [&](ExportSnapshot snapshot,QtOverlayHost::AnalysisCompleted done) {
    accepted=std::move(snapshot);++export_count;
    done(Result<bool,Error>::failure({ErrorCode::presenter_failed,"fixture",Retryability::same_input,{}}));
  };
  if(analyze) host.set_analysis_requested(prepare_action);
  if(pin) host.set_pin_requested(prepare_action);
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
  host.findChild<QToolButton*>(pin ? "actionPinScreenshot" : analyze ? "actionAnalyze" : "actionCopy")->click();
  if(background) {HDRSHOT_CHECK(!accepted && !editor->isEnabled());tasks.drain();}
  HDRSHOT_CHECK(accepted.has_value() && text->calls==1);
  HDRSHOT_CHECK(accepted->annotations.objects.size()==1 && accepted->clean_content!=nullptr);
  HDRSHOT_CHECK(accepted->clean_document_revision==accepted->annotations.revision);
  HDRSHOT_CHECK(!accepted->clean_content->owned.empty());
  const auto content=accepted->clean_content;
  host.findChild<QToolButton*>(pin ? "actionPinScreenshot" : analyze ? "actionAnalyze" : "actionCopy")->click();
  if(background) tasks.drain();
  HDRSHOT_CHECK(accepted->clean_content==content && text->calls==1 && export_count==2);
  // Every screen refresh is still pending; export nevertheless owns final content.
  HDRSHOT_CHECK(!presenter->completions.empty());
  if(analyze || pin) {
    HDRSHOT_CHECK(host.isVisible() && editor->isEnabled());
    int finished=0;
    QtOverlayHost::AnalysisCompleted done;
    host.set_finished([&] { ++finished; });
    auto deferred = [&](ExportSnapshot,QtOverlayHost::AnalysisCompleted callback) { done=std::move(callback); };
    if(pin) host.set_pin_requested(deferred); else host.set_analysis_requested(deferred);
    host.findChild<QToolButton*>(pin ? "actionPinScreenshot" : "actionAnalyze")->click();
    HDRSHOT_CHECK(finished==0 && !editor->isEnabled());
    if(background) tasks.drain();
    done(Result<bool,Error>::success(true));
    HDRSHOT_CHECK(finished==1 && !host.isVisible());
  }
}

}
int main(int argc, char** argv) {
  QApplication app(argc, argv);
#ifdef Q_OS_MACOS
  if(app.arguments().contains("--magnifier-native-raster")) {
    if(QApplication::platformName()!="cocoa" ||
        QByteArray(app.style()->metaObject()->className())!="QMacStyle") {
      std::fprintf(stderr,"Native raster regression requires cocoa and QMacStyle\n");
      return 1;
    }
    static int context_warnings=0;
    const auto previous=qInstallMessageHandler([](QtMsgType type,const QMessageLogContext& context,const QString& message) {
      if(QByteArray(context.category)=="qt.widgets.styles.macos" &&
          message.contains("invalid (nullptr) graphics context")) ++context_warnings;
      std::fprintf(stderr,"%s: %s\n",context.category,qPrintable(message));
      if(type==QtFatalMsg) std::abort();
    });
    const auto result=hdrshot::test::run({
      {"native QMacStyle card raster and straight RGBA payload",card_geometry_content_and_late_callbacks_are_separate},
      {"native QMacStyle labels, separator and theme refresh",panel_reuses_controls_and_invalidates_layout_and_theme}});
    qInstallMessageHandler(previous);
    std::printf("platform=cocoa style=QMacStyle dpr=%.2f invalid_context_warnings=%d\n",
        app.devicePixelRatio(),context_warnings);
    return result || context_warnings ? 1 : 0;
  }
#endif
  const auto screenshot=qEnvironmentVariable("HDRSHOT_MAGNIFIER_SCREENSHOT");
  if(!screenshot.isEmpty()){
    if(qEnvironmentVariableIsSet("HDRSHOT_MAGNIFIER_DARK")){auto pal=app.palette();pal.setColor(QPalette::Window,QColor("#25282d"));pal.setColor(QPalette::WindowText,Qt::white);app.setPalette(pal);}
    else if(qEnvironmentVariableIsSet("HDRSHOT_MAGNIFIER_LIGHT")){auto pal=app.palette();pal.setColor(QPalette::Window,QColor("#efefef"));pal.setColor(QPalette::WindowText,Qt::black);app.setPalette(pal);}
    QWidget parent;CaptureMagnifierPanel panel(&parent);MagnifierReadings fields;
    if(qEnvironmentVariableIsSet("HDRSHOT_MAGNIFIER_ALL"))fields.selected.fill(true);
    if(qEnvironmentVariableIsSet("HDRSHOT_MAGNIFIER_SRGB_ONLY")){fields.selected.fill(false);fields.selected[9]=true;}
    panel.resize(216,CaptureMagnifierPanel::header_height()+216+panel.text_height(fields,216));
    analysis::Rgb rgb{.5,.3,.1};const auto rgb_path=qEnvironmentVariable("HDRSHOT_MAGNIFIER_NATIVE_RGB");if(!rgb_path.isEmpty()){std::ifstream file(rgb_path.toStdString());for(auto& c:rgb)file>>c;HDRSHOT_CHECK(file.good());}
    panel.show_sample({159,240},magnifier_readout(rgb,203,false),fields,216,8);
    parent.resize(panel.size());parent.show();QApplication::processEvents();
    std::printf("magnifier_size=%dx%d platform=%s dpr=%.2f\n",panel.width(),panel.height(),QGuiApplication::platformName().toUtf8().constData(),panel.devicePixelRatioF());
    for(const auto* h:panel.findChildren<QLabel*>("magnifierGroupHeading"))
      std::printf("heading=%s geometry=%dx%d natural_height=%d\n",h->text().toUtf8().constData(),h->width(),h->height(),h->heightForWidth(h->width()));
    auto pixels=panel.grab();const auto native_path=qEnvironmentVariable("HDRSHOT_MAGNIFIER_NATIVE_IMAGE");
    if(!native_path.isEmpty()){QImage native(native_path);HDRSHOT_CHECK(!native.isNull());QPainter paint(&pixels);paint.drawImage(QRect(0,CaptureMagnifierPanel::header_height(),216,216),native);}
    HDRSHOT_CHECK(pixels.save(screenshot));return 0;
  }
  return hdrshot::test::run(
      {{"pending readings retain briefly, expire through motion, "
        "replace/fail/hide safely (panel)",
        [] { host_pending_readings_have_one_bounded_grace(false); }},
       {"pending readings retain briefly, expire through motion, "
        "replace/fail/hide safely (card)",
        [] { host_pending_readings_have_one_bounded_grace(true); }},
       {"continuous annotation publishes monotonic snapshots with gesture "
        "barriers",
        continuous_annotation_results_publish_within_their_gesture},
       {"background old document/closed host publication is ignored",
        background_publication_ignores_old_document_and_closed_host},
       {"single card geometry, isolated background and stale completion",
        card_geometry_content_and_late_callbacks_are_separate},
       {"magnifier control reuse with layout and theme invalidation",
        panel_reuses_controls_and_invalidates_layout_and_theme},
       {"background exact text export retains requested document",
        [] {
          pending_text_exports_final_clean_revision_without_present_completion(
              false, true);
        }},
       {"background exact pin commits text, retains failure and hands off once",
        [] { pending_text_exports_final_clean_revision_without_present_completion(false,true,true); }},
       {"background exact analysis retains requested document",
        [] {
          pending_text_exports_final_clean_revision_without_present_completion(
              true, true);
        }},
       {"frozen SDR capture sampler keeps SDR scalar rows with strict "
        "ordinary/HDR inputs",
        sampler_frozen_sdr_provenance_reaches_scalar_rows},
       {"all magnifier groups retain complete wrapped headings and one scalar "
        "per line",
        magnifier_all_groups_keep_complete_headings_and_scalar_lines},
       {"host DPR mapping and paired native viewport, same-pixel dedup, off "
        "zero work",
        host_magnifier_pairs_physical_coordinates_and_off_does_no_work},
       {"magnifier image hole excludes annotation and restores on hide",
        magnifier_hole_excludes_existing_annotation_during_selection_resize},
       {"capture defaults freeze white/readings and refresh each new session",
        magnifier_capture_defaults_are_frozen_and_next_session_reads_new_defaults},
       {"delayed readback cannot restore hidden, cancelled or destroyed host",
        magnifier_late_readback_cannot_restore_hidden_cancelled_or_destroyed_host},
       {"tiny sampler bounds, latest completion and cancellation lifetime",
        tiny_sampler_coalesces_stale_results_and_releases_cancelled_owners},
       {"pending text exports final clean revision before async screen "
        "completion",
        [] {
          pending_text_exports_final_clean_revision_without_present_completion();
        }},
       {"analysis commits text, retains failures, waits for ROI handoff",
        [] {
          pending_text_exports_final_clean_revision_without_present_completion(
              true);
        }},
       {"shared target lock and late session callbacks",
        session_target_lock_and_stale_finish_are_shared},
       {"locked initial editor never offers crosshair",
        locked_initial_editor_does_not_offer_crosshair},
       {"recovery consumes full gesture then next click selects",
        recovery_press_drag_release_cannot_select_and_next_click_can},
       {"recovery does not pair into completion double click",
        next_system_double_click_is_ordinary_press_not_export},
       {"multi-display recovery is consumed once and Esc cancels",
        recovery_is_shared_across_screens_and_escape_remains_cancel},
       {"recovery click cannot activate copy child control",
        first_recovery_click_on_copy_button_does_not_fire_action},
       {"internal focus transfers and dialogs do not consume normal click",
        normal_leave_focus_transfer_and_save_dialog_do_not_arm_recovery},
       {"application deactivation uses shared recovery gate",
        application_deactivation_enters_same_recovery_gate},
       {"recovery preserves pending text and discards interrupted drag",
        recovery_preserves_pending_text_and_cancels_uncommitted_drawing},
       {"candidate uses native renderer; release locks target",
        candidate_uses_native_presenter_and_commits_target_only_on_release},
       {"shared host owns completion lifecycle",
        shared_host_snapshot_dialog_cancel_and_acceptance},
       {"first capture reads current format and freezes it",
        first_capture_uses_current_format_snapshot},
       {"non-target display permits cancellation",
        locked_non_target_host_still_allows_escape},
       {"permission explanation from platform",
        permission_error_uses_injected_guidance}});
}
