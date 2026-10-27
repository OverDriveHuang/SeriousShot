#include "platform/qt/qt_pinned_screenshot_controller.hpp"
#include <QFontDatabase>
#include "ui/qt/analyzer_source_view.hpp"
#include "ui/qt/pinned_screenshot_preview.hpp"
#include "platform/cpu/cpu_analysis_port.hpp"
#include "test_support.hpp"
#include <QApplication>
#include <QCursor>
#include <QFrame>
#include <QHBoxLayout>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QMouseEvent>
#include <QMenu>
#include <QNativeGestureEvent>
#include <QPointingDevice>
#include <QWheelEvent>
#include <QPointer>
#include <QScreen>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QLabel>
#include <QPainter>
#include <iostream>
#include <algorithm>
using namespace hdrshot;
namespace {
void wait(int ms) { QEventLoop loop; QTimer::singleShot(ms,&loop,&QEventLoop::quit);loop.exec(); }
bool spin(std::function<bool()> predicate) {
  QElapsedTimer time;time.start();
  while(!predicate() && time.elapsed()<4000) { QApplication::processEvents();QThread::msleep(2); }
  return predicate();
}
analysis::Input input() {
  auto source=make_cpu_analysis_source({1200,800},analysis::FloatImage(1200*800,{.2f,.3f,.4f,1.f}));
  HDRSHOT_CHECK(source.has_value());
  return {source.value(),1,false};
}
QToolButton *button(QWidget &window,const char *name) {
  auto *result=window.findChild<QToolButton *>(name);HDRSHOT_CHECK(result!=nullptr);return result;
}
void mouse(QWidget *target,QEvent::Type type,QPoint local,Qt::MouseButton b,Qt::MouseButtons bs) {
  QMouseEvent event(type,local,target->mapToGlobal(local),b,bs,Qt::NoModifier);
  QApplication::sendEvent(target,&event);
}
// Read effective widget cursors after the same event routes used by the native
// presentation child; direct Source-only moves miss inherited edge cursors.
void cursor_regression(bool native_surface, bool from_window, bool bottom) {
  PinnedScreenshotWindow window(input(),{});
  window.resize(400,400);window.show();wait(20);
  auto *source=window.source_view();
  QWidget *target=native_surface ? source->presentation_surface() : source;
  HDRSHOT_CHECK(window.minimumWidth()==450 && window.width()==450);
  const QPoint center=source->rect().center();
  const int right=window.width()-1,bottom_edge=window.height()-1;
  source->zoom_at(center,3.);
  for(const auto &[point,shape]:std::vector<std::pair<QPoint,Qt::CursorShape>>{
      {{0,center.y()},Qt::SizeHorCursor},{{center.x(),0},Qt::SizeVerCursor},
      {{0,0},Qt::SizeFDiagCursor},{{right,bottom_edge},Qt::SizeFDiagCursor},
      {{right,0},Qt::SizeBDiagCursor},{{0,bottom_edge},Qt::SizeBDiagCursor}}) {
    mouse(target,QEvent::MouseMove,point,Qt::NoButton,Qt::NoButton);
    HDRSHOT_CHECK(target->cursor().shape()==shape);
  }
  const QPoint edge=bottom ? QPoint(center.x(),bottom_edge) : QPoint(right,center.y());
  auto *origin=from_window ? static_cast<QWidget *>(&window) : target;
  mouse(origin,QEvent::MouseMove,edge,Qt::NoButton,Qt::NoButton);
  HDRSHOT_CHECK(origin->cursor().shape()==(bottom ? Qt::SizeVerCursor : Qt::SizeHorCursor));
  mouse(target,QEvent::MouseMove,center,Qt::NoButton,Qt::NoButton);
  HDRSHOT_CHECK(source->cursor().shape()==Qt::OpenHandCursor);
  HDRSHOT_CHECK(target->cursor().shape()==Qt::OpenHandCursor);
  mouse(target,QEvent::MouseButtonPress,center,Qt::LeftButton,Qt::LeftButton);
  HDRSHOT_CHECK(source->dragging());
  HDRSHOT_CHECK(target->cursor().shape()==Qt::ClosedHandCursor);
  mouse(target,QEvent::MouseMove,center+QPoint(15,10),Qt::NoButton,Qt::LeftButton);
  HDRSHOT_CHECK(target->cursor().shape()==Qt::ClosedHandCursor);
  mouse(target,QEvent::MouseButtonRelease,center+QPoint(15,10),Qt::LeftButton,Qt::NoButton);
  HDRSHOT_CHECK(!source->dragging() && target->cursor().shape()==Qt::OpenHandCursor);
  auto *toolbar=window.findChild<QWidget *>("pinnedToolbar");
  auto *grip=window.findChild<QWidget *>("pinWindowGrip");
  mouse(toolbar,QEvent::MouseMove,{100,20},Qt::NoButton,Qt::NoButton);
  HDRSHOT_CHECK(toolbar->cursor().shape()==Qt::ArrowCursor);
  mouse(grip,QEvent::MouseMove,grip->rect().center(),Qt::NoButton,Qt::NoButton);
  HDRSHOT_CHECK(grip->cursor().shape()==Qt::SizeAllCursor);
}
// These tests use public input routes and inspect geometry/transform, so the
// old Fit-pan implementation is a meaningful negative control.
void same_transform(AnalyzerSourceTransform actual,AnalyzerSourceTransform expected) {
  HDRSHOT_CHECK(actual.fit==expected.fit);
  HDRSHOT_CHECK_NEAR(actual.scale,expected.scale,1e-9);
  HDRSHOT_CHECK(actual.pan==expected.pan);
}
void global_mouse(QWidget *target,QEvent::Type type,QPoint global,
                  Qt::MouseButton button,Qt::MouseButtons buttons) {
  QMouseEvent event(type,target->mapFromGlobal(global),global,button,buttons,Qt::NoModifier);
  QApplication::sendEvent(target,&event);
}
void conditional_image_drag(bool native_surface,int scenario) {
  PinnedScreenshotWindow window(input(),{});
  window.setGeometry(100,100,500,500);window.show();wait(20);
  auto *source=window.source_view();
  QWidget *target=native_surface ? source->presentation_surface() : source;
  QPoint point(250,250);
  bool window_drag=true;
  if(scenario==1) { // Small image at 1:1, independent of the Fit flag.
    source->set_source_size({160,100});button(window,"pinOneToOne")->click();
  } else if(scenario==2) { // Fully visible small image with a retained pan.
    source->set_source_size({160,100});source->set_transform({false,1.,{25.,-20.}});
  } else if(scenario==3) { // Blank letterbox belongs to the same image region.
    point={250,440};HDRSHOT_CHECK(!source->image_rect().contains(point));
  } else if(scenario==4) {
    source->zoom_at(point,3.);window_drag=false;
  } else if(scenario==5) { // A small 1:1 image whose pan crosses one edge.
    source->set_source_size({160,100});source->set_transform({false,1.,{220.,0.}});
    window_drag=false;
  }
  const auto original=window.geometry();const auto transform=source->transform();
  int front_requests=0;window.front_requested=[&]{++front_requests;};
  const QPoint start=target->mapToGlobal(point),delta(27,19);
  mouse(target,QEvent::MouseMove,point,Qt::NoButton,Qt::NoButton);
  HDRSHOT_CHECK(target->cursor().shape()==(window_drag ? Qt::SizeAllCursor : Qt::OpenHandCursor));
  global_mouse(target,QEvent::MouseButtonPress,start,Qt::LeftButton,Qt::LeftButton);
  HDRSHOT_CHECK(source->dragging()==!window_drag);
  HDRSHOT_CHECK(front_requests==1);
  global_mouse(target,QEvent::MouseMove,start+delta,Qt::NoButton,Qt::LeftButton);
  HDRSHOT_CHECK(target->cursor().shape()==(window_drag ? Qt::SizeAllCursor : Qt::ClosedHandCursor));
  global_mouse(target,QEvent::MouseButtonRelease,start+delta,Qt::LeftButton,Qt::NoButton);
  HDRSHOT_CHECK(!source->dragging());
  if(window_drag) {
    HDRSHOT_CHECK(window.geometry()==original.translated(delta));
    same_transform(source->transform(),transform);
  } else {
    HDRSHOT_CHECK(window.geometry()==original);
    HDRSHOT_CHECK(source->transform().pan==transform.pan+delta);
  }
}
void locked_image_drag(bool begin_window_drag,bool native_surface) {
  PinnedScreenshotWindow window(input(),{});
  window.setGeometry(100,100,500,500);window.show();wait(20);
  auto *source=window.source_view();
  QWidget *target=native_surface ? source->presentation_surface() : source;
  if(!begin_window_drag) source->zoom_at({250,250},3.);
  const QPoint start=target->mapToGlobal(QPoint(250,250));
  global_mouse(target,QEvent::MouseButtonPress,start,Qt::LeftButton,Qt::LeftButton);
  // A view change during a press must not switch its purpose. The Source pan
  // case deliberately becomes fully contained before the next mouse move.
  if(begin_window_drag) { source->zoom_at({250,250},3.);window.resize(540,520); }
  else source->fit();
  const auto geometry=window.geometry();const auto transform=source->transform();
  if(begin_window_drag)
    global_mouse(target,QEvent::MouseButtonRelease,start,Qt::RightButton,Qt::LeftButton);
  const QPoint delta(35,22);
  global_mouse(target,QEvent::MouseMove,start+delta,Qt::NoButton,Qt::LeftButton);
  global_mouse(target,QEvent::MouseButtonRelease,start+delta,Qt::LeftButton,Qt::NoButton);
  if(begin_window_drag) {
    HDRSHOT_CHECK(window.pos()==geometry.topLeft()+delta);
    HDRSHOT_CHECK(window.size()==geometry.size());
    same_transform(source->transform(),transform);
  } else {
    HDRSHOT_CHECK(window.geometry()==geometry);
    HDRSHOT_CHECK(source->transform().pan==transform.pan+delta);
  }
}
void drag_interruption(QEvent::Type interruption,bool native_surface) {
  PinnedScreenshotWindow window(input(),{});
  window.setGeometry(100,100,500,500);window.show();wait(20);
  auto *source=window.source_view();
  QWidget *target=native_surface ? source->presentation_surface() : source;
  const auto transform=source->transform();
  const QPoint start=target->mapToGlobal(QPoint(250,250));
  global_mouse(target,QEvent::MouseButtonPress,start,Qt::LeftButton,Qt::LeftButton);
  if(interruption==QEvent::MouseMove)
    global_mouse(&window,QEvent::MouseMove,start,Qt::NoButton,Qt::NoButton);
  else if(interruption==QEvent::Hide) window.hide();
  else if(interruption==QEvent::Close) window.close();
  else { QEvent cancel(interruption);QApplication::sendEvent(&window,&cancel); }
  const auto stopped=window.geometry();
  global_mouse(target,QEvent::MouseMove,start+QPoint(40,20),Qt::NoButton,Qt::LeftButton);
  HDRSHOT_CHECK(window.geometry()==stopped);
  HDRSHOT_CHECK(!source->dragging());same_transform(source->transform(),transform);
  if(interruption==QEvent::Close) return;
  window.show();
  const QPoint fresh=target->mapToGlobal(QPoint(250,250));
  global_mouse(target,QEvent::MouseButtonPress,fresh,Qt::LeftButton,Qt::LeftButton);
  global_mouse(&window,QEvent::MouseMove,fresh+QPoint(16,12),Qt::NoButton,Qt::LeftButton);
  HDRSHOT_CHECK(window.pos()==stopped.topLeft()+QPoint(16,12));
  global_mouse(&window,QEvent::MouseButtonRelease,fresh+QPoint(16,12),Qt::LeftButton,Qt::NoButton);
}
void input_routes_remain_shared(bool native_surface) {
  PinnedScreenshotWindow window(input(),{});
  window.setGeometry(100,100,500,500);window.show();wait(20);
  auto *source=window.source_view();
  QWidget *target=native_surface ? source->presentation_surface() : source;
  const auto geometry=window.geometry();const QPointF center(250,250);
  auto native=[&](Qt::NativeGestureType kind,qreal value=0.,QPointF delta={}) {
    QNativeGestureEvent event(kind,QPointingDevice::primaryPointingDevice(),2,
        center,center,target->mapToGlobal(center.toPoint()),value,delta);
    QApplication::sendEvent(target,&event);
  };
  const auto fit_scale=source->effective_scale();
  native(Qt::BeginNativeGesture);native(Qt::PanNativeGesture,0.,{12.,9.});
  HDRSHOT_CHECK(source->transform().pan==QPointF(12,9));
  // Qt's touch-synthesized press must never start a window drag.
  QMouseEvent synthesized(QEvent::MouseButtonPress,center,center,
      target->mapToGlobal(center.toPoint()),Qt::LeftButton,Qt::LeftButton,
      Qt::NoModifier,Qt::MouseEventSynthesizedByQt);
  QApplication::sendEvent(target,&synthesized);
  HDRSHOT_CHECK(QWidget::mouseGrabber()!=&window);
  native(Qt::ZoomNativeGesture,.1);native(Qt::EndNativeGesture);
  HDRSHOT_CHECK_NEAR(source->effective_scale(),fit_scale*1.1,1e-9);
  HDRSHOT_CHECK(window.geometry()==geometry);
  source->fit();
  QApplication::sendEvent(target,&synthesized);
  HDRSHOT_CHECK(QWidget::mouseGrabber()!=&window);
  const auto synthesized_start=target->mapToGlobal(center.toPoint());
  global_mouse(target,QEvent::MouseMove,synthesized_start+QPoint(9,6),Qt::NoButton,Qt::LeftButton);
  global_mouse(target,QEvent::MouseButtonRelease,synthesized_start+QPoint(9,6),Qt::LeftButton,Qt::NoButton);
  HDRSHOT_CHECK(window.geometry()==geometry);
  source->fit();
  auto wheel=[&](QPoint pixel,QPoint angle,Qt::ScrollPhase phase) {
    QWheelEvent event(center,target->mapToGlobal(center.toPoint()),pixel,angle,
        Qt::NoButton,Qt::NoModifier,phase,false);
    QApplication::sendEvent(target,&event);
  };
  wheel({}, {},Qt::ScrollBegin);wheel({13,7},{},Qt::ScrollUpdate);wheel({}, {},Qt::ScrollEnd);
  HDRSHOT_CHECK(source->transform().pan==QPointF(13,7));
  HDRSHOT_CHECK_NEAR(source->effective_scale(),fit_scale,1e-9);
  wheel({}, {0,120},Qt::NoScrollPhase);
  HDRSHOT_CHECK(source->effective_scale()>fit_scale);
  HDRSHOT_CHECK(window.geometry()==geometry);
  // Owner deactivation must cancel an already running Source pan too.
  source->zoom_at(center,3.);const auto before_pan=source->transform();
  mouse(target,QEvent::MouseButtonPress,center.toPoint(),Qt::LeftButton,Qt::LeftButton);
  mouse(target,QEvent::MouseMove,center.toPoint()+QPoint(18,11),Qt::NoButton,Qt::LeftButton);
  HDRSHOT_CHECK(source->dragging());
  QEvent deactivate(QEvent::WindowDeactivate);QApplication::sendEvent(&window,&deactivate);
  HDRSHOT_CHECK(!source->dragging());same_transform(source->transform(),before_pan);
  // The analysis host does not own conditional window dragging.
  AnalyzerSourceView analyzer;analyzer.resize(500,500);analyzer.set_source_size({1200,800});
  analyzer.set_tools(false,false,false);analyzer.show();
  mouse(&analyzer,QEvent::MouseButtonPress,{250,250},Qt::LeftButton,Qt::LeftButton);
  mouse(&analyzer,QEvent::MouseMove,{270,260},Qt::NoButton,Qt::LeftButton);
  mouse(&analyzer,QEvent::MouseButtonRelease,{270,260},Qt::LeftButton,Qt::NoButton);
  HDRSHOT_CHECK(analyzer.transform().pan==QPointF(20,10));
}
void resize_and_buttons_take_priority(bool native_surface) {
  PinnedScreenshotWindow window(input(),{});
  window.setGeometry(100,100,500,500);window.show();wait(20);
  auto *source=window.source_view();
  QWidget *target=native_surface ? source->presentation_surface() : source;
  const auto geometry=window.geometry();const auto transform=source->transform();
  const auto start=target->mapToGlobal(target->rect().bottomRight());
  global_mouse(target,QEvent::MouseButtonPress,start,Qt::LeftButton,Qt::LeftButton);
  global_mouse(&window,QEvent::MouseMove,start+QPoint(20,30),Qt::NoButton,Qt::LeftButton);
  global_mouse(&window,QEvent::MouseButtonRelease,start+QPoint(20,30),Qt::LeftButton,Qt::NoButton);
  HDRSHOT_CHECK(window.pos()==geometry.topLeft() && window.size()==geometry.size()+QSize(20,30));
  same_transform(source->transform(),transform);
  const auto resized=window.geometry();auto *fit=button(window,"pinFit");
  mouse(fit,QEvent::MouseButtonPress,fit->rect().center(),Qt::LeftButton,Qt::LeftButton);
  mouse(fit,QEvent::MouseMove,fit->rect().center()+QPoint(10,10),Qt::NoButton,Qt::LeftButton);
  mouse(fit,QEvent::MouseButtonRelease,fit->rect().center(),Qt::LeftButton,Qt::NoButton);
  HDRSHOT_CHECK(window.geometry()==resized);
}
void grip_arrowheads_remain_separate(bool dark) {
  PinnedScreenshotWindow window(input(),{});
  auto palette=window.palette();
  palette.setColor(QPalette::Window,dark ? QColor("#26292E") : QColor("#F5F6F8"));
  window.setPalette(palette);
  window.resize(450,400);window.show();wait(20);
  auto *grip=window.findChild<QWidget *>("pinWindowGrip");
  const auto image=grip->grab().toImage();
  const qreal ratio=image.devicePixelRatio();
  const auto sample=[&](double dx,double dy) {
    return image.pixelColor(int((grip->width()/2.+dx)*ratio),
                            int((grip->height()/2.+dy)*ratio));
  };
  const auto background=sample(-13,-12);
  // The approved move icon has four open gaps between the arrowheads.
  // Long chevrons join into a diamond despite passing the old cardinal-arm test.
  for(double x:{-4.5,4.5}) for(double y:{-4.5,4.5})
    HDRSHOT_CHECK(sample(x,y)==background);
  for(const auto p:{QPointF(0,-8),QPointF(0,8),QPointF(-8,0),QPointF(8,0),
                    QPointF(-2,-7),QPointF(2,-7),QPointF(-2,7),QPointF(2,7),
                    QPointF(-7,-2),QPointF(-7,2),QPointF(7,-2),QPointF(7,2)})
    HDRSHOT_CHECK(sample(p.x(),p.y())!=background);
}
void separated_grip_layout() {
  PinnedScreenshotWindow window(input(),{});window.resize(500,500);window.show();wait(20);
  auto *toolbar=window.findChild<QWidget *>("pinnedToolbar");
  auto *row=qobject_cast<QHBoxLayout *>(toolbar->layout());
  for(int width:{400,450,500}) {
    window.resize(width,500);QApplication::processEvents();
    std::cout<<"toolbar geometry width="<<width<<" actual="<<toolbar->width()<<'\n';
    for(int i=0;i<row->count();++i) {
      const auto r=row->itemAt(i)->geometry();auto *w=row->itemAt(i)->widget();
      std::cout<<i<<' '<<(w ? w->objectName().toStdString() : "spacer")
               <<" x="<<r.x()<<" w="<<r.width()<<" h="<<r.height()<<'\n';
    }
    HDRSHOT_CHECK(window.width()==std::max(450,width));
    HDRSHOT_CHECK(window.minimumWidth()==450);
    const auto separators=toolbar->findChildren<QFrame *>("pinSeparator");
    HDRSHOT_CHECK(separators.size()==5);
    for(auto *line:separators) HDRSHOT_CHECK(line->size()==QSize(1,26));
    auto *grip=window.findChild<QWidget *>("pinWindowGrip");
    const int grip_index=row->indexOf(grip);
    HDRSHOT_CHECK(grip_index>0 && row->itemAt(grip_index-1)->widget()->objectName()=="pinSeparator");
    HDRSHOT_CHECK(row->itemAt(grip_index+1)->widget()->objectName()=="pinSeparator");
    HDRSHOT_CHECK(grip->width()>=22); // Full icon survives the minimum and default widths.
    const auto icon=grip->grab().toImage();
    const qreal ratio=icon.devicePixelRatio();
    const auto color=[&](int x,int y) { return icon.pixelColor(int(x*ratio),int(y*ratio)); };
    const int cx=grip->width()/2,cy=grip->height()/2;
    const auto background=color(cx-13,cy-12);
    for(auto p:{QPoint(cx,cy-7),QPoint(cx,cy+7),QPoint(cx-7,cy),QPoint(cx+7,cy)})
      HDRSHOT_CHECK(color(p.x(),p.y())!=background);
    for(auto name:{"pinFit","pinOneToOne","pinAnalyze","pinSave","pinCopy","pinTopmost","pinClose"})
      HDRSHOT_CHECK(button(window,name)->size()==QSize(38,38));
    HDRSHOT_CHECK(button(window,"pinClose")->x()-button(window,"pinTopmost")->geometry().right()>=38);
  }
}
class EnabledChanges final : public QObject {
public:
  int count{};
  bool eventFilter(QObject *,QEvent *event) override {
    if(event->type()==QEvent::EnabledChange) ++count;
    return false;
  }
};
QColor rendered_background(QToolButton *b,bool hover,bool pressed) {
  b->setAttribute(Qt::WA_UnderMouse,hover);
  b->setDown(pressed);
  auto *parent=b->parentWidget();
  const auto point=b->mapTo(parent,QPoint(3,b->height()/2));
  const auto image=parent->grab().toImage();
  return image.pixelColor(int(point.x()*image.devicePixelRatio()),
                          int(point.y()*image.devicePixelRatio()));
}
void rendered_feedback(bool dark,const char *name,bool checked=false) {
  PinnedScreenshotWindow window(input(),{});
  auto palette=window.palette();
  palette.setColor(QPalette::Window,dark ? QColor("#26292E") : QColor("#F5F6F8"));
  window.setPalette(palette);
  window.resize(400,400);window.show();wait(20);
  auto *b=button(window,name);
  b->setChecked(checked);
  const auto normal=rendered_background(b,false,false);
  const auto hover=rendered_background(b,true,false);
  const auto pressed=rendered_background(b,true,true);
  std::cout<<name<<(checked ? " checked" : " unchecked")<<(dark ? " dark" : " light")
           <<" background "<<normal.name().toStdString()<<" / "<<hover.name().toStdString()
           <<" / "<<pressed.name().toStdString()<<'\n';
  HDRSHOT_CHECK(normal!=hover);
  HDRSHOT_CHECK(normal!=pressed);
  HDRSHOT_CHECK(hover!=pressed);
  HDRSHOT_CHECK(rendered_background(b,false,false)==normal);
}
void window_move_cursor_regression() {
  PinnedScreenshotWindow window(input(),{});
  window.resize(400,400);window.show();wait(20);
  auto *grip=window.findChild<QWidget *>("pinWindowGrip");
  auto *source=window.source_view();
  source->zoom_at({200,200},3.);
  const auto original=window.geometry();
  const auto pan=source->transform().pan;
  mouse(grip,QEvent::MouseMove,grip->rect().center(),Qt::NoButton,Qt::NoButton);
  HDRSHOT_CHECK(grip->cursor().shape()==Qt::SizeAllCursor);
  mouse(grip,QEvent::MouseButtonPress,grip->rect().center(),Qt::LeftButton,Qt::LeftButton);
  HDRSHOT_CHECK(grip->cursor().shape()==Qt::SizeAllCursor);
  HDRSHOT_CHECK(window.cursor().shape()==Qt::SizeAllCursor);
  // After grabMouse, moves can arrive on the window outside the grip.
  const auto outside=grip->mapTo(&window,grip->rect().center())+QPoint(30,80);
  mouse(&window,QEvent::MouseMove,outside,Qt::NoButton,Qt::LeftButton);
  HDRSHOT_CHECK(window.cursor().shape()==Qt::SizeAllCursor);
  HDRSHOT_CHECK(window.pos()==original.topLeft()+QPoint(30,80));
  HDRSHOT_CHECK(source->transform().pan==pan);
  mouse(&window,QEvent::MouseButtonRelease,{200,200},Qt::LeftButton,Qt::NoButton);
  HDRSHOT_CHECK(grip->cursor().shape()==Qt::SizeAllCursor);
  mouse(source,QEvent::MouseMove,{200,200},Qt::NoButton,Qt::NoButton);
  HDRSHOT_CHECK(source->cursor().shape()==Qt::OpenHandCursor);
  mouse(grip,QEvent::MouseMove,grip->rect().center(),Qt::NoButton,Qt::NoButton);
  HDRSHOT_CHECK(grip->cursor().shape()==Qt::SizeAllCursor);
}
void busy_regression() {
  PinnedScreenshotWindow window(input(),{});
  window.resize(400,400);window.show();wait(20);
  std::vector<UiCommand> actions;
  window.action_requested=[&](UiCommand command) { actions.push_back(command); };
  const auto buttons=window.findChildren<QToolButton *>();
  EnabledChanges enabled_changes;
  for(auto *b:buttons) b->installEventFilter(&enabled_changes);
  std::vector<QImage> before;
  for(auto *b:buttons) { HDRSHOT_CHECK(b->isEnabled());before.push_back(b->grab().toImage()); }
  window.set_busy(true);
  HDRSHOT_CHECK(enabled_changes.count==0);
  for(std::size_t i=0;i<before.size();++i) {
    auto *b=buttons.at(static_cast<qsizetype>(i));
    HDRSHOT_CHECK(b->isEnabled());
    HDRSHOT_CHECK(b->grab().toImage()==before[i]);
  }
  for(auto name:{"pinSave","pinCopy","pinAnalyze","pinSaveMenu"}) button(window,name)->click();
  HDRSHOT_CHECK(actions.empty());
  window.set_busy(false);
  HDRSHOT_CHECK(enabled_changes.count==0);
  for(auto name:{"pinSave","pinCopy","pinAnalyze"}) button(window,name)->click();
  HDRSHOT_CHECK((actions==std::vector<UiCommand>{UiCommand::save_default,UiCommand::copy_and_close,UiCommand::analyze}));
}
void layout_regression(int requested_width) {
  PinnedScreenshotWindow window(input(),{});
  window.resize(requested_width,400);window.show();wait(20);
  auto *toolbar=window.findChild<QWidget *>("pinnedToolbar");
  auto *row=qobject_cast<QHBoxLayout *>(toolbar->layout());
  HDRSHOT_CHECK(row && row->spacing()==5 && window.width()==std::max(450,requested_width));
  HDRSHOT_CHECK(window.minimumWidth()==450);
  auto *grip=window.findChild<QWidget *>("pinWindowGrip");
  HDRSHOT_CHECK(grip && grip->width()>=22);
  const auto separators=toolbar->findChildren<QFrame *>("pinSeparator");
  HDRSHOT_CHECK(separators.size()==5);
  for(auto *separator:separators) HDRSHOT_CHECK(separator->size()==QSize(1,26));
  for(auto name:{"pinFit","pinOneToOne","pinAnalyze","pinSave","pinCopy","pinTopmost","pinClose"})
    HDRSHOT_CHECK(button(window,name)->size()==QSize(38,38));
  auto *save=button(window,"pinSave");auto *arrow=button(window,"pinSaveMenu");
  HDRSHOT_CHECK(save->parentWidget()==arrow->parentWidget() && save->parentWidget()!=toolbar);
  HDRSHOT_CHECK(save->parentWidget()->layout()->spacing()==0);
  HDRSHOT_CHECK(save->mapTo(toolbar,QPoint(save->width(),0)).x()==arrow->mapTo(toolbar,QPoint(0,0)).x());
  HDRSHOT_CHECK(button(window,"pinClose")->x()-button(window,"pinTopmost")->geometry().right()>=38);
  // Layout spacing also has to survive at the supported minimum width.
  for(int i=1;i<row->count();++i) {
    if(row->itemAt(i-1)->spacerItem() || row->itemAt(i)->spacerItem()) continue;
    const auto previous=row->itemAt(i-1)->geometry();
    const auto current=row->itemAt(i)->geometry();
    HDRSHOT_CHECK(current.left()-previous.right()-1==5);
  }
  for(auto *b:window.findChildren<QToolButton *>())
    HDRSHOT_CHECK(toolbar->rect().contains(QRect(b->mapTo(toolbar,QPoint(0,0)),b->size())));
}
// Collapse is exercised through the shipped button, without adding a public
// state/testing API. These cases must compile and fail on the previous shell.
void collapse_layout(bool dark,int width) {
  PinnedScreenshotWindow window(input(),{});
  auto palette=window.palette();
  palette.setColor(QPalette::Window,dark ? QColor("#26292E") : QColor("#F5F6F8"));
  window.setPalette(palette);window.resize(width,400);window.show();wait(20);
  auto *collapse=button(window,"pinCollapse");
  auto *close=button(window,"pinClose");
  auto *toolbar=window.findChild<QWidget *>("pinnedToolbar");
  auto *row=qobject_cast<QHBoxLayout *>(toolbar->layout());
  HDRSHOT_CHECK(collapse->size()==QSize(38,38) && collapse->text().isEmpty());
  HDRSHOT_CHECK(row->indexOf(collapse)+1==row->indexOf(close));
  HDRSHOT_CHECK(close->x()-collapse->geometry().right()-1==5);
  HDRSHOT_CHECK(collapse->toolTip()==QStringLiteral("收起贴图"));
  HDRSHOT_CHECK(collapse->accessibleName()==collapse->toolTip());
  HDRSHOT_CHECK(collapse->property("toolbarSymbol").toString()=="panel-top-close");
  const auto expanded=collapse->grab().toImage();
  const auto normal=rendered_background(collapse,false,false);
  const auto hover=rendered_background(collapse,true,false);
  const auto down=rendered_background(collapse,true,true);
  HDRSHOT_CHECK(normal!=hover && hover!=down && normal!=down);
  collapse->setDown(false);collapse->setAttribute(Qt::WA_UnderMouse,false);
  collapse->click();
  HDRSHOT_CHECK(collapse->toolTip().isEmpty());
  HDRSHOT_CHECK(collapse->accessibleName()==QStringLiteral("展开贴图"));
  HDRSHOT_CHECK(collapse->property("toolbarSymbol").toString()=="panel-top-open");
  HDRSHOT_CHECK(collapse->grab().toImage()!=expanded);
  HDRSHOT_CHECK(window.width()==width);
  HDRSHOT_CHECK(row->spacing()==5);
  for(auto *line:toolbar->findChildren<QFrame *>("pinSeparator"))
    HDRSHOT_CHECK(line->size()==QSize(1,26));
  HDRSHOT_CHECK(window.findChild<QWidget *>("pinWindowGrip")->width()>=22);
  for(auto *b:window.findChildren<QToolButton *>())
    HDRSHOT_CHECK(toolbar->rect().contains(QRect(b->mapTo(toolbar,QPoint(0,0)),b->size())));
}
void collapse_geometry_and_view(bool manual) {
  PinnedScreenshotWindow window(input(),{});
  window.setGeometry(60,60,500,360);window.show();wait(20);
  auto *source=window.source_view();
  auto *surface=source->presentation_surface();
  if(manual) source->set_transform({false,1.37,{37.,-29.}});
  const auto original=window.geometry(),viewport=source->geometry();
  const auto minimum=window.minimumSize(),maximum=window.maximumSize();
  const auto transform=source->transform();const auto image=source->image_rect();
  const auto scale=source->effective_scale();
  auto *toolbar=window.findChild<QWidget *>("pinnedToolbar");
  auto *grip=window.findChild<QWidget *>("pinWindowGrip");
  auto *collapse=button(window,"pinCollapse");
  int topmost_changes=0;window.topmost_changed=[&](bool){++topmost_changes;};
  collapse->click();
  HDRSHOT_CHECK(window.pos()==original.topLeft() && window.width()==original.width());
  HDRSHOT_CHECK(window.height()>=toolbar->geometry().bottom()+1);
  HDRSHOT_CHECK(window.height()<=toolbar->height()+2*toolbar->y());
  HDRSHOT_CHECK(source->geometry()==viewport && source->isHidden() && !surface->isVisible());
  HDRSHOT_CHECK(window.source_view()==source);
  same_transform(source->transform(),transform);
  HDRSHOT_CHECK(source->image_rect()==image);HDRSHOT_CHECK_NEAR(source->effective_scale(),scale,1e-9);
  HDRSHOT_CHECK(!button(window,"pinFit")->isEnabled() && !button(window,"pinOneToOne")->isEnabled());
  button(window,"pinFit")->click();button(window,"pinOneToOne")->click();
  same_transform(source->transform(),transform);
  QEvent leave(QEvent::Leave);QApplication::sendEvent(&window,&leave);
  wait(710);HDRSHOT_CHECK(toolbar->isVisible() && collapse->isVisible());
  const auto bar=window.geometry();
  for(const auto edge:{QPoint(0,0),QPoint(window.width()-1,window.height()-1)}) {
    const auto start=window.mapToGlobal(edge);
    global_mouse(&window,QEvent::MouseButtonPress,start,Qt::LeftButton,Qt::LeftButton);
    global_mouse(&window,QEvent::MouseMove,start+QPoint(21,15),Qt::NoButton,Qt::LeftButton);
    global_mouse(&window,QEvent::MouseButtonRelease,start+QPoint(21,15),Qt::LeftButton,Qt::NoButton);
    HDRSHOT_CHECK(window.geometry()==bar);
  }
  const QPoint delta(18,12),start=grip->mapToGlobal(grip->rect().center());
  global_mouse(grip,QEvent::MouseButtonPress,start,Qt::LeftButton,Qt::LeftButton);
  global_mouse(&window,QEvent::MouseMove,start+delta,Qt::NoButton,Qt::LeftButton);
  global_mouse(&window,QEvent::MouseButtonRelease,start+delta,Qt::LeftButton,Qt::NoButton);
  HDRSHOT_CHECK(window.pos()==bar.topLeft()+delta);
  const auto available=window.screen()->availableGeometry();
  auto expected=original.translated(delta);
  // Keep the moved position when toolbar entries fit. In the DPR2 offscreen
  // fixture the whole logical screen may be narrower than the 450 minimum;
  // retain the width and anchor that unavoidable overflow at the left edge.
  if(expected.width()>available.width()) expected.moveLeft(available.left());
  else if(expected.left()<available.left()) expected.moveLeft(available.left());
  else if(expected.right()>available.right()) expected.moveRight(available.right());
  const auto desired_entries=toolbar->geometry().translated(expected.topLeft());
  if(desired_entries.top()<available.top())
    expected.translate(0,available.top()-desired_entries.top());
  else if(desired_entries.bottom()>available.bottom())
    expected.translate(0,available.bottom()-desired_entries.bottom());
  collapse->click();
  HDRSHOT_CHECK(window.geometry()==expected && window.minimumWidth()==450);
  const auto entries=toolbar->geometry().translated(window.pos());
  HDRSHOT_CHECK(entries.top()>=available.top() && entries.bottom()<=available.bottom());
  if(entries.width()<=available.width()) HDRSHOT_CHECK(available.contains(entries));
  else HDRSHOT_CHECK(entries.left()==available.left());
  HDRSHOT_CHECK(button(window,"pinCollapse")->isVisible() && button(window,"pinClose")->isVisible());
  HDRSHOT_CHECK(source->isVisible() && surface->isVisible() && source->geometry()==viewport);
  HDRSHOT_CHECK(window.minimumSize()==minimum && window.maximumSize()==maximum);
  same_transform(source->transform(),transform);HDRSHOT_CHECK(source->image_rect()==image);
  HDRSHOT_CHECK(button(window,"pinFit")->isEnabled() && button(window,"pinOneToOne")->isEnabled());
  HDRSHOT_CHECK(window.always_on_top() && topmost_changes==0);
  for(int i=0;i<4;++i) { collapse->click();collapse->click(); }
  HDRSHOT_CHECK(window.geometry()==expected);
  same_transform(source->transform(),transform);
  // Flush geometry/show events before an actual pointer leave. An artificial
  // Leave while the global cursor remains over the bar is not a real exit.
  QApplication::processEvents();
  QCursor::setPos(window.mapToGlobal(QPoint(-50,-50)));QApplication::processEvents();
  QApplication::sendEvent(&window,&leave);wait(710);HDRSHOT_CHECK(!toolbar->isVisible());
  const auto resize_start=window.mapToGlobal(window.rect().bottomRight());
  global_mouse(&window,QEvent::MouseButtonPress,resize_start,Qt::LeftButton,Qt::LeftButton);
  global_mouse(&window,QEvent::MouseMove,resize_start+QPoint(20,16),Qt::NoButton,Qt::LeftButton);
  global_mouse(&window,QEvent::MouseButtonRelease,resize_start+QPoint(20,16),Qt::LeftButton,Qt::NoButton);
  HDRSHOT_CHECK(window.size()==original.size()+QSize(20,16));
}
void collapse_hover_and_small_error() {
  PinnedScreenshotWindow window(input(),{});
  window.setGeometry(100,100,500,100);window.show();wait(20);
  auto *toolbar=window.findChild<QWidget *>("pinnedToolbar");
  auto *collapse=button(window,"pinCollapse");
  const auto old_cursor=QCursor::pos();
  collapse->click();
  QCursor::setPos(collapse->mapToGlobal(collapse->rect().center()));
  QApplication::processEvents();collapse->click();
  wait(710);HDRSHOT_CHECK(toolbar->isVisible());
  collapse->click();
  window.show_error(QStringLiteral("保存失败：测试错误提示必须完整显示，并允许重试。"));
  auto *status=window.findChild<QLabel *>("pinStatus");
  HDRSHOT_CHECK(window.source_view()->isVisible() && status->isVisible());
  HDRSHOT_CHECK(status->geometry().top()>toolbar->geometry().bottom());
  HDRSHOT_CHECK(window.rect().contains(status->geometry()) && status->height()>=status->heightForWidth(status->width()));
  HDRSHOT_CHECK(collapse->isVisible());
  QCursor::setPos(window.mapToGlobal(QPoint(-50,-50)));QApplication::processEvents();
  QEvent leave(QEvent::Leave);QApplication::sendEvent(&window,&leave);
  wait(710);HDRSHOT_CHECK(!toolbar->isVisible());
  QCursor::setPos(old_cursor);
}
void collapse_presentation(bool manual) {
  PinnedScreenshotWindow window(input(),{});window.resize(500,360);
  int submitted=0;analysis::SourceView last;
  window.present_requested=[&](auto view,std::uintptr_t){++submitted;last=view;};
  window.show();HDRSHOT_CHECK(spin([&]{return submitted>0;}));
  if(manual) window.source_view()->set_transform({false,1.3,{43.,-31.}});
  window.refresh_presentation();wait(45);
  const auto expected=window.presentation_view();
  auto *surface=window.source_view()->presentation_widget();
  // A scheduled frame must be cancelled by hiding Source, even before dispatch.
  window.refresh_presentation();button(window,"pinCollapse")->click();
  const int hidden_submitted=submitted;
  for(int i=0;i<4;++i) {
    QEvent display(QEvent::ScreenChangeInternal);QApplication::sendEvent(&window,&display);
    window.refresh_presentation();wait(25);
  }
  HDRSHOT_CHECK(submitted==hidden_submitted && !surface->isVisible());
  button(window,"pinCollapse")->click();
  HDRSHOT_CHECK(spin([&]{return submitted>hidden_submitted;}));
  HDRSHOT_CHECK(window.source_view()->presentation_widget()==surface);
  HDRSHOT_CHECK(last.target_size.width==expected.target_size.width && last.target_size.height==expected.target_size.height);
  HDRSHOT_CHECK_NEAR(last.scale,expected.scale,1e-9);
  HDRSHOT_CHECK_NEAR(last.offset_x,expected.offset_x,1e-9);HDRSHOT_CHECK_NEAR(last.offset_y,expected.offset_y,1e-9);
  window.close();const int closed_submitted=submitted;window.refresh_presentation();wait(45);
  HDRSHOT_CHECK(submitted==closed_submitted);
}
void ui() {
  PinnedScreenshotWindow window(input(),{});
  const int capture_width=qEnvironmentVariableIntValue("HDRSHOT_PIN_LAYOUT_WIDTH");
  window.resize(capture_width==450 || capture_width==500 ? capture_width : 400,400);window.show();wait(20);
  auto *toolbar=window.findChild<QWidget *>("pinnedToolbar");
  auto *grip=window.findChild<QWidget *>("pinWindowGrip");
  HDRSHOT_CHECK(toolbar && grip);
  HDRSHOT_CHECK(button(window,"pinSaveMenu")->arrowType()==Qt::DownArrow);
  if(qEnvironmentVariableIsSet("HDRSHOT_PIN_DARK")) {
    auto palette=window.palette();palette.setColor(QPalette::Window,QColor("#26292E"));window.setPalette(palette);
  }
  const auto image_path=qEnvironmentVariable("HDRSHOT_PIN_LAYOUT_CAPTURE");
  if(!image_path.isEmpty()) {
    QImage fixture(1200,800,QImage::Format_RGB32);fixture.fill(QColor("#596B83"));
    QPainter painter(&fixture);painter.setPen(Qt::white);
    auto font=window.font();font.setPointSize(40);painter.setFont(font);
    painter.drawText(fixture.rect(),Qt::AlignCenter,"Source Signal · 1200 × 800");painter.end();
    window.source_view()->set_fixture_image(fixture);
    HDRSHOT_CHECK(window.grab().save(image_path));
    auto *collapse=button(window,"pinCollapse");
    const int suffix=image_path.lastIndexOf('.');
    const auto stem=suffix<0 ? image_path : image_path.left(suffix);
    const auto extension=suffix<0 ? QStringLiteral(".png") : image_path.mid(suffix);
    collapse->click();
    HDRSHOT_CHECK(window.grab().save(stem+"_collapsed"+extension));
    collapse->click();
    HDRSHOT_CHECK(window.grab().save(stem+"_restored"+extension));
  }
  int previous=-1;
  for(auto name:{"pinFit","pinOneToOne","pinAnalyze","pinSave","pinSaveMenu","pinCopy","pinTopmost","pinClose"}) {
    auto *b=button(window,name);
    HDRSHOT_CHECK(b->mapTo(toolbar,QPoint(0,0)).y()==button(window,"pinFit")->mapTo(toolbar,QPoint(0,0)).y() && b->mapTo(toolbar,QPoint(0,0)).x()>previous && b->mapTo(toolbar,QPoint(b->width()-1,0)).x()<toolbar->width());
    previous=b->mapTo(toolbar,QPoint(b->width()-1,0)).x();
    HDRSHOT_CHECK(b->focusPolicy()==Qt::NoFocus);
  }
  HDRSHOT_CHECK(button(window,"pinClose")->x()-button(window,"pinTopmost")->geometry().right()>=38);
  HDRSHOT_CHECK(window.source_view()->geometry()==window.rect());
  HDRSHOT_CHECK(window.always_on_top());
  button(window,"pinTopmost")->click();HDRSHOT_CHECK(!window.always_on_top());
  button(window,"pinOneToOne")->click();HDRSHOT_CHECK(std::abs(window.source_view()->effective_scale()-1.)<1e-8);
  button(window,"pinFit")->click();HDRSHOT_CHECK(window.source_view()->transform().fit);
  window.source_view()->zoom_at({200,200},3.);
  const auto geometry=window.geometry();
  mouse(window.source_view(),QEvent::MouseButtonPress,{100,150},Qt::LeftButton,Qt::LeftButton);
  mouse(window.source_view(),QEvent::MouseMove,{130,170},Qt::NoButton,Qt::LeftButton);
  mouse(window.source_view(),QEvent::MouseButtonRelease,{130,170},Qt::LeftButton,Qt::NoButton);
  HDRSHOT_CHECK(window.geometry()==geometry);
  HDRSHOT_CHECK(window.source_view()->transform().pan!=QPointF());
  auto transform=window.source_view()->transform();
  mouse(grip,QEvent::MouseButtonPress,grip->rect().center(),Qt::LeftButton,Qt::LeftButton);
  mouse(grip,QEvent::MouseMove,grip->rect().center()+QPoint(30,20),Qt::NoButton,Qt::LeftButton);
  mouse(grip,QEvent::MouseButtonRelease,grip->rect().center(),Qt::LeftButton,Qt::NoButton);
  HDRSHOT_CHECK(window.pos()==geometry.topLeft()+QPoint(30,20));
  HDRSHOT_CHECK(window.source_view()->transform().pan==transform.pan);
  const int old_width=window.width();
  mouse(&window,QEvent::MouseButtonPress,{window.width()-1,200},Qt::LeftButton,Qt::LeftButton);
  mouse(&window,QEvent::MouseMove,{old_width+39,200},Qt::NoButton,Qt::LeftButton);
  mouse(&window,QEvent::MouseButtonRelease,{window.width()-1,200},Qt::LeftButton,Qt::NoButton);
  HDRSHOT_CHECK(window.width()==old_width+40);
  HDRSHOT_CHECK(window.source_view()->geometry()==window.rect());
  // Native hide/show (including capture's collapse/restore) and resize enqueue
  // Enter events. Finish those transitions, then establish a real pointer exit
  // before measuring the one-shot Leave timeout.
  QApplication::processEvents();
  QCursor::setPos(window.mapToGlobal(QPoint(-50,-50)));QApplication::processEvents();
  HDRSHOT_CHECK(!window.rect().contains(window.mapFromGlobal(QCursor::pos())));
  QEvent leave(QEvent::Leave);QApplication::sendEvent(&window,&leave);
  wait(200);HDRSHOT_CHECK(toolbar->isVisible());
  wait(500);HDRSHOT_CHECK(!toolbar->isVisible());
  QEvent enter(QEvent::Enter);QApplication::sendEvent(window.source_view(),&enter);
  HDRSHOT_CHECK(toolbar->isVisible());
  window.set_busy(true);
  HDRSHOT_CHECK(button(window,"pinSave")->isEnabled() && button(window,"pinClose")->isEnabled());
  window.set_busy(false);
}
void minimum_geometry_regression() {
  PinnedScreenshotWindow window(input(),{});
  HDRSHOT_CHECK(window.minimumWidth()==450);
  window.resize(400,400);HDRSHOT_CHECK(window.size()==QSize(450,400));
  const QRect desktop(0,0,1920,1080);
  // The smallest selectable short edge is realizable in both orientations.
  HDRSHOT_CHECK(PinnedScreenshotWindow::initial_geometry({1200,800},450,desktop).size()==QSize(675,450));
  HDRSHOT_CHECK(PinnedScreenshotWindow::initial_geometry({800,1200},450,desktop).size()==QSize(450,675));
  // Default short edge stays 500, independently of the 450 minimum width.
  HDRSHOT_CHECK(PinnedScreenshotWindow::initial_geometry({1200,800},500,desktop).size()==QSize(750,500));
  HDRSHOT_CHECK(PinnedScreenshotWindow::initial_geometry({800,1200},500,desktop).size()==QSize(500,750));
  for(const auto available:{QRect(0,0,1440,900),QRect(-700,120,550,360)}) {
    for(const auto image:{QSize(4000,100),QSize(100,4000),QSize(800,1200)}) {
      const auto constrained=PinnedScreenshotWindow::initial_geometry(image,400,available);
      HDRSHOT_CHECK(available.contains(constrained));
      HDRSHOT_CHECK(constrained.width()>=450);
      window.setGeometry(constrained);
      HDRSHOT_CHECK(window.geometry().width()>=450);
      HDRSHOT_CHECK(available.contains(window.geometry()));
    }
  }
}

class Native final:public QtPinnedWindowPort {
public:int topmost{},front{};
  void set_always_on_top(QWidget&,bool) override {++topmost;}
  void bring_forward(QWidget&) override {++front;}
};
class Presenter final:public AnalysisPresenterPort {
public:Result<bool,Error> present(const analysis::Input &,const analysis::SourceView &,std::uintptr_t) override {
  return Result<bool,Error>::success(true);
}};
class ControlledPresenter final:public AnalysisPresenterPort {
public:
  bool fail{};
  std::string reason{"fixture_failure"};
  Result<bool,Error> present(const analysis::Input &,const analysis::SourceView &,std::uintptr_t) override {
    if(fail) return Result<bool,Error>::failure(
        {ErrorCode::presenter_failed,"display_fixture",Retryability::after_recreate,{{"reason",reason}}});
    return Result<bool,Error>::success(true);
  }
};
PinnedScreenshotWindow *latest() {
  for(auto *widget:QApplication::topLevelWidgets())
    if(auto *pin=dynamic_cast<PinnedScreenshotWindow *>(widget);pin&&!pin->is_closed()) return pin;
  return nullptr;
}
void controller_display_error_ownership(QApplication &app,bool asynchronous) {
  auto presenter=std::make_shared<ControlledPresenter>();
  std::function<void(Error)> native_error;
  std::function<void()> recovered;
  QtPinnedScreenshotController c(app,[&](const ExportSnapshot &s) {
    PreparedPinnedScreenshot prepared{{input(),s},presenter,{}, {}};
    prepared.observe_errors=[&](auto callback){native_error=std::move(callback);};
    prepared.observe_recovered=[&](auto callback){recovered=std::move(callback);};
    return Result<PreparedPinnedScreenshot,Error>::success(std::move(prepared));
  },std::make_shared<Native>(),[](UiCommand,ExportSnapshot,auto done,QWidget *) {
    done(ExportCompletionCoordinator::Outcome::failure(
        {ErrorCode::invalid_input,"export_fixture",Retryability::after_user_action,{}}));return true;
  },[](ExportSnapshot,auto done) {
    done(Result<bool,Error>::failure(
        {ErrorCode::invalid_input,"analysis_fixture",Retryability::after_user_action,{}}));
  },[]{return SettingsSnapshot{};},nullptr);
  bool opened=false;c.open({},[&](auto result){HDRSHOT_CHECK(result.has_value());opened=true;});
  HDRSHOT_CHECK(spin([&]{return opened;}));
  QPointer<PinnedScreenshotWindow> pin=latest();HDRSHOT_CHECK(pin && native_error && recovered);
  auto *status=pin->findChild<QLabel *>("pinStatus");HDRSHOT_CHECK(status);
  presenter->fail=true;presenter->reason="analysis_work_pending";
  pin->present_requested(pin->presentation_view(),0);
  native_error({ErrorCode::presenter_failed,"display_fixture",Retryability::after_recreate,
      {{"reason","drawable_retry_exhausted"}}});
  HDRSHOT_CHECK(status->isHidden());
  presenter->fail=false;presenter->reason="fixture_failure";
  // Drive the installed production callbacks directly: no repaint probe is
  // needed to observe the status ownership, and no global native fault hook.
  if(asynchronous) native_error({ErrorCode::presenter_failed,"display_fixture",Retryability::after_recreate,{}});
  else {
    presenter->fail=true;
    pin->present_requested(pin->presentation_view(),0);
  }
  HDRSHOT_CHECK(!status->isHidden() && status->property("displayError").toBool());
  presenter->fail=false;
  pin->present_requested(pin->presentation_view(),0);
  HDRSHOT_CHECK(!status->isHidden() && status->property("displayError").toBool());
  recovered();
  HDRSHOT_CHECK(status->isHidden() && !status->property("displayError").toBool());
  native_error({ErrorCode::presenter_failed,"display_fixture",Retryability::after_recreate,{}});
  button(*pin,"pinCopy")->click();
  HDRSHOT_CHECK(!status->isHidden() && !status->property("displayError").toBool());
  HDRSHOT_CHECK(status->text().contains("export_fixture"));
  recovered();
  HDRSHOT_CHECK(!status->isHidden() && status->text().contains("export_fixture"));
  native_error({ErrorCode::presenter_failed,"display_fixture",Retryability::after_recreate,{}});
  button(*pin,"pinAnalyze")->click();
  HDRSHOT_CHECK(!status->isHidden() && !status->property("displayError").toBool());
  HDRSHOT_CHECK(status->text().contains("analysis_fixture"));
  recovered();
  HDRSHOT_CHECK(!status->isHidden() && status->text().contains("analysis_fixture"));
  pin->close();QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
  HDRSHOT_CHECK(!pin);
  native_error({ErrorCode::presenter_failed,"late_fixture",Retryability::after_recreate,{}});
  recovered();
  c.shutdown();
}
void collapse_actions_and_lifetime(QApplication &app) {
  const auto image=input();
  std::vector<ExportCompletionCoordinator::Completed> pending_exports;
  std::vector<QtPinnedScreenshotController::Completed> pending_analysis;
  std::vector<UiCommand> commands;
  std::vector<std::function<void(Error)>> errors;
  std::vector<std::function<void()>> recoveries;
  auto native=std::make_shared<Native>();
  QPointer<PinnedScreenshotWindow> expected_source;
  QtPinnedScreenshotController c(app,[&](const ExportSnapshot &s) {
    auto snapshot=s;snapshot.selection={1,{0,0,1200,800}};
    PreparedPinnedScreenshot p{{image,snapshot},std::make_shared<Presenter>(),{}, {}};
    p.observe_errors=[&](auto cb){errors.push_back(std::move(cb));};
    p.observe_recovered=[&](auto cb){recoveries.push_back(std::move(cb));};
    return Result<PreparedPinnedScreenshot,Error>::success(std::move(p));
  },native,[&](UiCommand command,ExportSnapshot snapshot,auto completed,QWidget *source) {
    HDRSHOT_CHECK(source && source == expected_source);
    HDRSHOT_CHECK(snapshot.selection.desktop_rect.width==1200 && snapshot.selection.desktop_rect.height==800);
    commands.push_back(command);pending_exports.push_back(std::move(completed));return true;
  },[&](ExportSnapshot snapshot,auto completed) {
    HDRSHOT_CHECK(snapshot.selection.desktop_rect.width==1200 && snapshot.selection.desktop_rect.height==800);
    commands.push_back(UiCommand::analyze);pending_analysis.push_back(std::move(completed));
  },[]{return SettingsSnapshot{};},nullptr);
  auto open=[&]() {
    bool done=false;c.open({},[&](auto result){HDRSHOT_CHECK(result.has_value());done=true;});
    HDRSHOT_CHECK(spin([&]{return done;}));
    return QPointer<PinnedScreenshotWindow>(latest());
  };
  auto first=open();HDRSHOT_CHECK(first);auto second=open();HDRSHOT_CHECK(second);
  // latest() has no ordering guarantee: select the other live top-level widget.
  if(second==first) for(auto *w:QApplication::topLevelWidgets())
    if(auto *p=dynamic_cast<PinnedScreenshotWindow *>(w);p && p!=first && !p->is_closed()) second=p;
  HDRSHOT_CHECK(second && first!=second);
  expected_source=first;
  first->source_view()->set_transform({false,1.5,{41.,-23.}});
  const auto original=first->size();button(*first,"pinCollapse")->click();
  HDRSHOT_CHECK(first->source_view()->isHidden() && second->source_view()->isVisible());
  EnabledChanges changes;
  for(auto name:{"pinSave","pinCopy","pinAnalyze"}) button(*first,name)->installEventFilter(&changes);
  auto duplicates=[&] {
    const auto count=commands.size();
    for(auto name:{"pinSave","pinCopy","pinAnalyze","pinSaveMenu"}) button(*first,name)->click();
    HDRSHOT_CHECK(commands.size()==count && changes.count==0);
  };
  button(*first,"pinSave")->click();HDRSHOT_CHECK(commands.back()==UiCommand::save_default);duplicates();
  pending_exports.back()(ExportCompletionCoordinator::Outcome::success({}));
  button(*first,"pinCopy")->click();HDRSHOT_CHECK(commands.back()==UiCommand::copy_and_close);duplicates();
  pending_exports.back()(ExportCompletionCoordinator::Outcome::success({}));
  // Choose Save As through the real dropdown's nested event loop.
  bool menu_chosen=false;
  QTimer menu_timeout;menu_timeout.setSingleShot(true);
  QObject::connect(&menu_timeout,&QTimer::timeout,[&] {
    if(auto *menu=qobject_cast<QMenu *>(QApplication::activePopupWidget())) menu->close();
  });
  menu_timeout.start(1500);
  QTimer::singleShot(0,[&] {
    auto *menu=qobject_cast<QMenu *>(QApplication::activePopupWidget());
    if(!menu) return;
    auto *action=menu->actions().back();const auto point=menu->actionGeometry(action).center();
    mouse(menu,QEvent::MouseButtonPress,point,Qt::LeftButton,Qt::LeftButton);
    mouse(menu,QEvent::MouseButtonRelease,point,Qt::LeftButton,Qt::NoButton);menu_chosen=true;
  });
  button(*first,"pinSaveMenu")->click();menu_timeout.stop();
  HDRSHOT_CHECK(menu_chosen && commands.back()==UiCommand::save_as);
  pending_exports.back()(ExportCompletionCoordinator::Outcome::success({}));
  button(*first,"pinAnalyze")->click();HDRSHOT_CHECK(commands.back()==UiCommand::analyze);duplicates();
  pending_analysis.back()(Result<bool,Error>::failure({ErrorCode::invalid_input,"collapse_analysis",Retryability::after_user_action,{}}));
  auto *status=first->findChild<QLabel *>("pinStatus");
  HDRSHOT_CHECK(first->source_view()->isVisible() && first->size()==original && status->isVisible());
  HDRSHOT_CHECK(status->text().contains("collapse_analysis"));
  HDRSHOT_CHECK(!status->geometry().intersects(first->findChild<QWidget *>("pinnedToolbar")->geometry()));
  button(*first,"pinCollapse")->click();button(*first,"pinCopy")->click();
  pending_exports.back()(ExportCompletionCoordinator::Outcome::failure({ErrorCode::invalid_input,"collapse_export",Retryability::after_user_action,{}}));
  HDRSHOT_CHECK(first->source_view()->isVisible() && status->text().contains("collapse_export"));
  button(*first,"pinCollapse")->click();errors.front()({ErrorCode::presenter_failed,"collapse_display",Retryability::after_recreate,{}});
  HDRSHOT_CHECK(first->source_view()->isVisible() && status->isVisible());
  recoveries.front()();HDRSHOT_CHECK(status->isHidden());
  button(*first,"pinCollapse")->click();button(*first,"pinSave")->click();
  first->close();QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);HDRSHOT_CHECK(!first);
  pending_exports.back()(ExportCompletionCoordinator::Outcome::failure({ErrorCode::invalid_input,"late_export",Retryability::after_user_action,{}}));
  errors.front()({ErrorCode::presenter_failed,"late_display",Retryability::after_recreate,{}});recoveries.front()();
  HDRSHOT_CHECK(second->isVisible() && second->source_view()->isVisible());
  button(*second,"pinCollapse")->click();button(*second,"pinAnalyze")->click();
  c.shutdown();HDRSHOT_CHECK(!second && !c.has_windows());
  pending_analysis.back()(Result<bool,Error>::success(true));
}
void controller(QApplication &app) {
  auto native=std::make_shared<Native>();
  auto image=input();
  SettingsSnapshot settings;
  bool fail=false,fail_attach=false;int exports=0,analyzes=0;
  ExportCompletionCoordinator::Completed export_completed;
  QtPinnedScreenshotController c(app,[&](const ExportSnapshot &s) {
    if(fail) return Result<PreparedPinnedScreenshot,Error>::failure(
        {ErrorCode::invalid_input,"fixture",Retryability::after_user_action,{}});
    auto original=s; original.selection={1,{0,0,1200,800}};
    PreparedPinnedScreenshot prepared{{image,original},std::make_shared<Presenter>(),{}, {}};
    prepared.attach_window=[&](PinnedScreenshotWindow &) {
      if(fail_attach) return Result<bool,Error>::failure(
          {ErrorCode::presenter_failed,"native_fixture",Retryability::after_recreate,{}});
      return Result<bool,Error>::success(true);
    };
    return Result<PreparedPinnedScreenshot,Error>::success(std::move(prepared));
  },native,[&](UiCommand command,ExportSnapshot snapshot,auto completed,QWidget *) {
    HDRSHOT_CHECK(command==UiCommand::copy_and_close);
    HDRSHOT_CHECK(snapshot.selection.desktop_rect.width==1200);
    HDRSHOT_CHECK(snapshot.default_folder=="new-folder");
    ++exports;export_completed=std::move(completed);return true;
  },[&](ExportSnapshot snapshot,auto completed) {
    HDRSHOT_CHECK(snapshot.selection.desktop_rect.width==1200);++analyzes;
    completed(Result<bool,Error>::success(true));
  },[&]{return settings;},nullptr);
  bool done=false;c.open({},[&](auto result){HDRSHOT_CHECK(result.has_value());done=true;});
  HDRSHOT_CHECK(spin([&]{return done;}));
  QPointer<PinnedScreenshotWindow> pin=latest();HDRSHOT_CHECK(pin && c.has_windows());
  settings.pinned_screenshot_defaults={false,800};settings.default_save_folder="new-folder";
  HDRSHOT_CHECK(pin->always_on_top());
  pin->source_view()->zoom_at({100,100},4);
  button(*pin,"pinAnalyze")->click();HDRSHOT_CHECK(analyzes==1 && pin->isVisible());
  button(*pin,"pinCopy")->click();HDRSHOT_CHECK(exports==1);
  pin->close();QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
  HDRSHOT_CHECK(!pin && !c.has_windows());
  export_completed(ExportCompletionCoordinator::Outcome::failure(
      {ErrorCode::invalid_input,"late",Retryability::after_user_action,{}}));
  fail=true;done=false;
  c.open({},[&](auto result){HDRSHOT_CHECK(!result);done=true;});
  HDRSHOT_CHECK(spin([&]{return done;}));HDRSHOT_CHECK(!c.has_windows());
  fail=false;fail_attach=true;done=false;
  c.open({},[&](auto result){HDRSHOT_CHECK(!result && result.error().module=="native_fixture");done=true;});
  HDRSHOT_CHECK(spin([&]{return done;}));HDRSHOT_CHECK(!c.has_windows() && !latest());
  fail_attach=false;
  fail=false;done=false;
  c.open({},[&](auto result){HDRSHOT_CHECK(!result);done=true;});
  c.shutdown();HDRSHOT_CHECK(done && !c.has_windows());
}
void thumbnail_child_input() {
  PinnedScreenshotWindow window(input(),{});window.resize(500,340);window.show();wait(20);
  auto *toggle=button(window,"pinCollapse");
  auto &thumbnail=window.preview()->source(0);
  auto *surface=thumbnail.presentation_surface();
  int clicks=0,fronts=0;
  QObject::connect(toggle,&QToolButton::clicked,[&]{++clicks;});
  window.front_requested=[&]{++fronts;};
  for(auto *target:std::vector<QWidget *>{surface,&thumbnail,window.findChild<QWidget *>("pinExpandBadge"),toggle}) {
    toggle->click();wait(10);const int before=clicks;const int front_before=fronts;
    const QPoint point=target->rect().center();
    QEvent enter(QEvent::Enter);QApplication::sendEvent(toggle,&enter);wait(330);
    HDRSHOT_CHECK(window.preview()->host(1).isVisible());
    mouse(target,QEvent::MouseButtonPress,point,Qt::LeftButton,Qt::LeftButton);
    HDRSHOT_CHECK(toggle->isDown() && !thumbnail.dragging());
    HDRSHOT_CHECK(!window.preview()->host(1).isVisible());
    mouse(target,QEvent::MouseButtonRelease,point,Qt::LeftButton,Qt::NoButton);
    HDRSHOT_CHECK(window.height()==340 && clicks==before+1 && fronts==front_before+1);
    wait(330);HDRSHOT_CHECK(!window.preview()->host(1).isVisible());
  }
  // A quick click must also revoke a hover timeout that has not fired yet.
  toggle->click();QEvent enter(QEvent::Enter);QApplication::sendEvent(toggle,&enter);wait(40);
  mouse(surface,QEvent::MouseButtonPress,surface->rect().center(),Qt::LeftButton,Qt::LeftButton);
  mouse(surface,QEvent::MouseButtonRelease,surface->rect().center(),Qt::LeftButton,Qt::NoButton);
  wait(350);HDRSHOT_CHECK(window.height()==340 && !window.preview()->host(1).isVisible());
  toggle->click();const int before=clicks;
  const auto transform=thumbnail.transform();
  const QPointF center=surface->rect().center();
  QWheelEvent wheel(center,surface->mapToGlobal(center),QPoint(),QPoint(0,120),
      Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);
  QApplication::sendEvent(surface,&wheel);
  HDRSHOT_CHECK(thumbnail.transform().fit==transform.fit && thumbnail.transform().pan==transform.pan);
  mouse(surface,QEvent::MouseButtonPress,surface->rect().center(),Qt::LeftButton,Qt::LeftButton);
  const QPoint outside=surface->mapFromGlobal(toggle->mapToGlobal(QPoint(-8,-8)));
  mouse(surface,QEvent::MouseMove,outside,Qt::NoButton,Qt::LeftButton);
  mouse(surface,QEvent::MouseButtonRelease,outside,Qt::LeftButton,Qt::NoButton);
  HDRSHOT_CHECK(window.height()==60 && clicks==before && !toggle->isDown() && !thumbnail.dragging());
}

void preview_geometry() {
  const QRect available(-800,40,800,560);
  for(const QSize image:{QSize(1200,800),QSize(800,1200),QSize(10,1500),QSize(1500,10)}) {
    for(const QRect anchor:{QRect(-100,80,38,38),QRect(-790,550,38,38),QRect(-100,550,38,38)}) {
      const auto card=PinnedScreenshotPreview::card_geometry(anchor,image,available);
      HDRSHOT_CHECK(available.contains(card));
      HDRSHOT_CHECK(card.width()<=220 && card.height()<=220);
      HDRSHOT_CHECK(card.width()>10 && card.height()>10);
      if(anchor.y()==80)HDRSHOT_CHECK(card.top()==anchor.bottom()+13 && card.right()==anchor.right());
      else HDRSHOT_CHECK(card.bottom()==anchor.top()-13);
    }
  }
}

void thumbnail_preview_contract() {
  PinnedScreenshotWindow window(input(),{});
  window.resize(500,340); window.show(); wait(20);
  auto *expand=button(window,"pinCollapse"); expand->click(); wait(20);
  HDRSHOT_CHECK(expand->toolTip().isEmpty());
  auto *thumb=window.findChild<QWidget *>("pinThumbnailSource");
  auto *card=window.findChild<QWidget *>("pinHoverPreview");
  HDRSHOT_CHECK(thumb && card && thumb->isVisible() && !card->isVisible());
  HDRSHOT_CHECK(QRect(3,4,32,30).contains(thumb->geometry()));
  HDRSHOT_CHECK(thumb->width()==32 && thumb->height()>0);
  const auto collapsed=window.geometry();
  QEvent enter(QEvent::Enter); QApplication::sendEvent(expand,&enter);
  wait(180); HDRSHOT_CHECK(!card->isVisible());
  wait(160); HDRSHOT_CHECK(card->isVisible() && window.geometry()==collapsed);
  HDRSHOT_CHECK(card->windowFlags().testFlag(Qt::WindowDoesNotAcceptFocus));
  HDRSHOT_CHECK(card->windowFlags().testFlag(Qt::WindowTransparentForInput));
  mouse(expand,QEvent::MouseButtonPress,expand->rect().center(),Qt::LeftButton,Qt::LeftButton);
  HDRSHOT_CHECK(!card->isVisible());
  mouse(expand,QEvent::MouseButtonRelease,expand->rect().center(),Qt::LeftButton,Qt::NoButton);
  wait(350); HDRSHOT_CHECK(!card->isVisible() && window.height()==340);
  expand->click(); QApplication::sendEvent(expand,&enter); wait(100);
  QEvent leave(QEvent::Leave); QApplication::sendEvent(expand,&leave);
  wait(250); HDRSHOT_CHECK(!card->isVisible());
  QApplication::sendEvent(expand,&enter); window.hide(); wait(350);
  HDRSHOT_CHECK(!card->isVisible());
}

}
int main(int argc,char **argv) {
  QApplication app(argc,argv);app.setQuitOnLastWindowClosed(false);
  if(qEnvironmentVariableIsSet("HDRSHOT_PIN_LAYOUT_CAPTURE")) {
    const int id=QFontDatabase::addApplicationFont(
        QCoreApplication::applicationDirPath()+"/assets/fonts/NotoSansSC-VF.ttf");
    const auto families=QFontDatabase::applicationFontFamilies(id);
    if(!families.isEmpty())app.setFont(QFont(families.front(),10));
  }
  int failures=0;
  auto run=[&](const char *name,auto test) {
    try { test();std::cout<<name<<" passed\n"; }
    catch(const std::exception &e) { ++failures;std::cerr<<name<<": "<<e.what()<<'\n'; }
  };
  run("thumbnail child input",thumbnail_child_input);
  if(app.arguments().contains("--thumbnail-click-only"))return failures?1:0;
  run("preview screen geometry",preview_geometry);
  run("thumbnail preview contract",thumbnail_preview_contract);
  if(app.arguments().contains("--preview-only"))return failures?1:0;
  for(bool dark:{false,true}) for(int width:{450,500})
    run("collapse layout and symbol feedback",[&]{collapse_layout(dark,width);});
  for(bool manual:{false,true}) {
    run("collapse persistent bar, geometry and view",[&]{collapse_geometry_and_view(manual);});
    run("collapse hidden presentation and resume",[&]{collapse_presentation(manual);});
  }
  run("collapse hover restoration and small-window error",collapse_hover_and_small_error);
  run("collapse full actions, errors and lifetime",[&]{collapse_actions_and_lifetime(app);});
  run("separated grip default layout and minimum geometry",separated_grip_layout);
  for(bool dark:{false,true})
    run("move icon has separate arrowheads",[&]{grip_arrowheads_remain_separate(dark);});
  for(bool native_surface:{false,true}) {
    for(int scenario=0;scenario<6;++scenario)
      run((std::string(native_surface ? "native conditional drag " : "source conditional drag ")+std::to_string(scenario)).c_str(),
          [&]{conditional_image_drag(native_surface,scenario);});
    for(bool begin_window_drag:{false,true})
      run(begin_window_drag ? "locked window gesture" : "locked pan gesture",[&]{locked_image_drag(begin_window_drag,native_surface);});
    run("shared wheel/native input and analyzer",[&]{input_routes_remain_shared(native_surface);});
    run("resize and buttons before image drag",[&]{resize_and_buttons_take_priority(native_surface);});
    for(auto interruption:{QEvent::UngrabMouse,QEvent::WindowDeactivate,QEvent::Hide,QEvent::Close,QEvent::MouseMove})
      run("image drag cancellation and restart",[&]{drag_interruption(interruption,native_surface);});
  }
  run("controller synchronous display error ownership",[&]{controller_display_error_ownership(app,false);});
  run("controller asynchronous display error ownership",[&]{controller_display_error_ownership(app,true);});
  for(bool native_surface:{false,true}) for(bool bottom:{false,true})
    run(native_surface ? "native edge cursor" : "source edge cursor",[&]{cursor_regression(native_surface,false,bottom);});
  for(bool bottom:{false,true})
    run("window to native cursor",[&]{cursor_regression(true,true,bottom);});
  for(bool dark:{false,true}) {
    run("copy rendered feedback",[&]{rendered_feedback(dark,"pinCopy");});
    run("topmost off rendered feedback",[&]{rendered_feedback(dark,"pinTopmost",false);});
    run("topmost on rendered feedback",[&]{rendered_feedback(dark,"pinTopmost",true);});
    run("close rendered feedback",[&]{rendered_feedback(dark,"pinClose");});
  }
  run("window move uses distinct cursor",window_move_cursor_regression);
  run("busy appearance and dispatch",busy_regression);
  run("minimum and initial geometry",minimum_geometry_regression);
  for(int requested_width:{400,450,500})
    run("minimum and default toolbar layout",[&]{layout_regression(requested_width);});
  run("shared window",ui);
  run("controller",[&]{controller(app);});
  if(!failures) std::cout<<"pinned screenshot shared tests passed\n";
  return failures ? 1 : 0;
}
