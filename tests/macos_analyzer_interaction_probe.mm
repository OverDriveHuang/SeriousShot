// Synthetic-only interaction acceptance probe. No capture or clipboard writes.
#include "ui/qt/analyzer_window.hpp"
#include "ui/qt/analyzer_scope_plot.hpp"
#include "ui/qt/analyzer_layout.hpp"
#include "platform/macos/macos_analysis_backend.hpp"
#include "platform/cpu/cpu_analysis_port.hpp"
#include "application/analysis_session.hpp"
#include "domain/analysis/engine.hpp"
#include <QApplication>
#include <QEventLoop>
#include <QTimer>
#include <QPointer>
#include <QWheelEvent>
#include <QNativeGestureEvent>
#include <QPointingDevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QLabel>
#include <QMouseEvent>
#include <QToolButton>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>
#include <iostream>
#include <algorithm>
#include <sys/resource.h>
#import <QuartzCore/CAMetalLayer.h>
#import <objc/runtime.h>

using namespace hdrshot;
using Clock=std::chrono::steady_clock;
static double ms(Clock::time_point a,Clock::time_point b=Clock::now()) {return std::chrono::duration<double,std::milli>(b-a).count();}
struct Measures {
  int handled{},requests{},accepted{},presented{},uploads{},drawable_calls{},errors{};
  double present_ms{},accept_ms{},drawable_ms{},last_event_ms{};
  std::vector<double> event_ms,event_age_ms,present_times,drawable_times,compute_ms;
  int heavy_stats{},detail_calls{},prepare_calls{};
  std::size_t max_retained{},max_payload_peak{};
  Clock::time_point last_handled{};
  std::mutex mutex;
};
static Measures* active=nullptr;
static std::mutex drawable_probe_mutex;
static IMP original_drawable;
static id timed_drawable(id self,SEL sel) {
  auto start=Clock::now();id result=((id(*)(id,SEL))original_drawable)(self,sel);
  std::lock_guard lock(drawable_probe_mutex);
  if(active){const double d=ms(start);++active->drawable_calls;active->drawable_ms+=d;active->drawable_times.push_back(d);}return result;
}
struct ProbeWheel:QWheelEvent {
  Clock::time_point posted;
  std::function<void()> action;
  ProbeWheel(QWidget* target,bool pixels,bool horizontal):QWheelEvent(QPointF(target->width()/2.,target->height()/2.),target->mapToGlobal(QPoint(target->width()/2,target->height()/2)),pixels?(horizontal?QPoint(2,0):QPoint(0,2)):QPoint(),pixels?QPoint():(horizontal?QPoint(2,0):QPoint(0,2)),Qt::NoButton,Qt::NoModifier,Qt::ScrollUpdate,false),posted(Clock::now()){}
};
class App:public QApplication {
public:using QApplication::QApplication;
 bool notify(QObject* o,QEvent* e)override {
  auto* input=dynamic_cast<ProbeWheel*>(e);
  if(!input||!active)return QApplication::notify(o,e);
  auto start=Clock::now();const double age=ms(input->posted,start);
  bool value=true;if(input->action)input->action();else value=QApplication::notify(o,e);
  active->event_ms.push_back(ms(start));active->event_age_ms.push_back(age);active->last_handled=Clock::now();++active->handled;return value;
 }
};
class Port:public AnalysisPort {
 std::shared_ptr<AnalysisPort> inner_;std::shared_ptr<Measures> measured_;
public:Port(std::shared_ptr<AnalysisPort> p,std::shared_ptr<Measures> m):inner_(std::move(p)),measured_(std::move(m)){}
 Result<LinearSourceRef,Error> prepare(const SelectionRoiView&r,const AnnotationPixelPlan&p)override{return inner_->prepare(r,p);}
 Result<analysis::ResultRef,Error> analyze(const analysis::Input&i,const analysis::Request&r)override{
  auto start=Clock::now();auto v=inner_->analyze(i,r);std::lock_guard lock(measured_->mutex);measured_->compute_ms.push_back(ms(start));
  if(v){measured_->heavy_stats+=v.value()->statistics_ms>0;measured_->detail_calls+=v.value()->detail_ms>0;measured_->prepare_calls+=v.value()->prepare_ms>0;
    measured_->max_retained=std::max(measured_->max_retained,v.value()->retained_bytes);measured_->max_payload_peak=std::max(measured_->max_payload_peak,v.value()->peak_bytes);}
  return v;
 }
 Result<LinearSourceRef,Error> compose_report(const analysis::Input&i,const analysis::ReportPlan&p)override{return inner_->compose_report(i,p);}
};
static QJsonObject distribution(std::vector<double> values){
 if(values.empty())return {};std::sort(values.begin(),values.end());double sum=0;for(auto x:values)sum+=x;
 return {{"n",int(values.size())},{"mean_ms",sum/values.size()},{"p95_ms",values[std::min(values.size()-1,std::size_t(values.size()*.95))]},{"max_ms",values.back()}};
}
static analysis::Input make_input(std::shared_ptr<AnalysisPort> port,int width,int height){
 analysis::FloatImage pixels(std::size_t(width)*height);
 for(int y=0;y<height;++y)for(int x=0;x<width;++x)pixels[std::size_t(y)*width+x]={float(x%257)/256*4,float(y%257)/256*2,.1f,1};
 auto cpu=make_cpu_analysis_source({width,height},std::move(pixels));if(!cpu)throw std::runtime_error("cpu source");
 SelectionRoiView roi;roi.size_px={width,height};roi.source_rect_px={0,0,width,height};roi.linear_source=cpu.value();roi.row_stride_samples=width*4;roi.encoding={ColorPrimaries::display_p3,TransferFunction::linear,AlphaMode::opaque,0};
 AnnotationPixelPlan p;p.output_size_px=roi.size_px;for(int y=0;y<height;++y)p.source_visible_spans.push_back({y,0,width});
 auto native=port->prepare(roi,p);if(!native)throw std::runtime_error("prepare");return {native.value(),1,true};
}
static void pump(int milliseconds){QEventLoop l;QTimer::singleShot(milliseconds,&l,&QEventLoop::quit);l.exec();}
static QJsonObject run(const QString& name,bool real_present,bool analysis_updates,int target_kind,int width,int height,int count,int interval,bool pinch=false,bool mask=false){
 auto b=make_macos_analysis_backend();if(!b)throw std::runtime_error("backend");
 auto input=make_input(b.value().port,width,height);AnalyzerWindow window(input);window.resize(1280,780);window.set_close_confirmation([]{return true;});window.show();pump(100);
 auto initial=b.value().port->analyze(input,window.current_request());if(!initial)throw std::runtime_error("initial analyze");window.accept_result(initial.value());pump(50);
 auto m=std::make_shared<Measures>();auto profiled=std::make_shared<Port>(b.value().port,m);AnalysisSession session(input,profiled);QPointer<AnalyzerWindow> guard(&window);
 window.set_request_handler([&](analysis::Request r){++m->requests;if(!analysis_updates)return;session.request(std::move(r),[guard,m](auto result){QMetaObject::invokeMethod(qApp,[guard,m,result]{if(guard&&result){auto t=Clock::now();guard->accept_result(result.value());++m->accepted;m->accept_ms+=ms(t);}},Qt::QueuedConnection);});});
 std::shared_ptr<const analysis::UiImage> last_marks;
 window.set_present_handler([&](analysis::SourceView v,std::uintptr_t surface){
  auto t=Clock::now();++m->presented;if(last_marks!=v.operation_overlay){++m->uploads;last_marks=v.operation_overlay;}
  if(real_present){auto r=b.value().presenter->present(input,v,surface);if(!r)++m->errors;}
  double d=ms(t);m->present_ms+=d;m->present_times.push_back(d);
 });pump(200);
 if(target_kind==1||target_kind==2){
   auto options=window.current_request().scopes;
   if(target_kind==1)options.amplitude_view={2,.2};else options.histogram_view={2,.2};
   auto* plot=target_kind==1?window.waveform_plot():window.histogram_plot();plot->set_options(options);plot->options_changed(options);pump(400);
 }
 if(mask){auto* button=window.findChild<QToolButton*>("analyzerTool2");if(!button)throw std::runtime_error("mask tool");button->click();pump(40);}
 // Counters cover the injected burst and its drain, excluding warm-up.
 m->handled=m->requests=m->accepted=m->presented=m->uploads=m->drawable_calls=m->errors=0;
 m->present_ms=m->accept_ms=m->drawable_ms=0;m->event_ms.clear();m->event_age_ms.clear();m->present_times.clear();m->drawable_times.clear();
 {std::lock_guard lock(m->mutex);m->compute_ms.clear();m->heavy_stats=m->detail_calls=m->prepare_calls=0;m->max_retained=m->max_payload_peak=0;}
 const double active_dpr=window.devicePixelRatioF();
 const double source_dpr=window.source_widget()->devicePixelRatioF();
 const auto active_view=window.source_view();
 {std::lock_guard lock(drawable_probe_mutex);active=m.get();}
 QWidget* target=target_kind==0?static_cast<QWidget*>(window.source_widget()):target_kind==1?static_cast<QWidget*>(window.waveform_plot()):target_kind==2?static_cast<QWidget*>(window.histogram_plot()):static_cast<QWidget*>(window.vectorscope_plot());
 if(target_kind==4){auto* pane=dynamic_cast<AnalyzerSplitPane*>(window.findChild<QWidget*>("analyzerTopSplit"));if(!pane)throw std::runtime_error("split");target=pane->handle();}
 const QPointF center(target->width()/2.,target->height()/2.);const QPointF global=target->mapToGlobal(center.toPoint());
 if(target_kind==4){QMouseEvent press(QEvent::MouseButtonPress,center,global,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);QCoreApplication::sendEvent(target,&press);}
 if(mask){const auto pt=window.source_widget()->local_at({width*.2,height*.2});QMouseEvent press(QEvent::MouseButtonPress,pt,target->mapToGlobal(pt.toPoint()),Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);QCoreApplication::sendEvent(target,&press);}
 // All events have ScrollUpdate and no momentum. They cannot be OS inertia.
 auto start=Clock::now();QEventLoop loop;QTimer check;check.setInterval(10);bool finished=false;
 QObject::connect(&check,&QTimer::timeout,[&]{if(!finished&&m->handled>=count){finished=true;m->last_event_ms=ms(start);QTimer::singleShot(250,&loop,&QEventLoop::quit);}if(ms(start)>12000)loop.quit();});check.start();
 // Capture target geometry on GUI thread; the producer only posts precreated events.
 std::vector<ProbeWheel*> events;for(int i=0;i<count;++i){auto* event=new ProbeWheel(target,target_kind==0,target_kind==2);
   if(target_kind==4)event->action=[target,center,global,i]{QMouseEvent move(QEvent::MouseMove,center,global+QPointF(i*.5,0),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);QCoreApplication::sendEvent(target,&move);};
   if(pinch)event->action=[target,center,global]{QNativeGestureEvent zoom(Qt::ZoomNativeGesture,QPointingDevice::primaryPointingDevice(),2,center,center,global,.01,QPointF(),1);QCoreApplication::sendEvent(target,&zoom);};
   if(mask)event->action=[&,target,i]{const auto pt=window.source_widget()->local_at({width*(.3+.5*i/count),height*(.3+.5*i/count)});QMouseEvent move(QEvent::MouseMove,pt,target->mapToGlobal(pt.toPoint()),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);QCoreApplication::sendEvent(target,&move);};
   events.push_back(event);}
 std::thread producer([&,start]{for(int i=0;i<count;++i){std::this_thread::sleep_until(start+std::chrono::microseconds(interval*i));events[std::size_t(i)]->posted=Clock::now();QCoreApplication::postEvent(target,events[std::size_t(i)]);}});
 loop.exec();producer.join();check.stop();{std::lock_guard lock(drawable_probe_mutex);active=nullptr;}
 if(target_kind==4){QMouseEvent release(QEvent::MouseButtonRelease,center,global,Qt::LeftButton,Qt::NoButton,Qt::NoModifier);QCoreApplication::sendEvent(target,&release);}
 if(mask){const auto pt=window.source_widget()->local_at({width*(.3+.5*(count-1)/count),height*(.3+.5*(count-1)/count)});QMouseEvent release(QEvent::MouseButtonRelease,pt,target->mapToGlobal(pt.toPoint()),Qt::LeftButton,Qt::NoButton,Qt::NoModifier);QCoreApplication::sendEvent(target,&release);pump(250);}
 if(m->handled<count)QCoreApplication::removePostedEvents(target,QEvent::Wheel);
 window.set_request_handler({});session.stop();bool stopped=session.wait_for_stopped(std::chrono::seconds(5));
 const auto presentation=macos_analysis_presentation_status(*b.value().presenter);
 window.set_present_handler({});window.hide();pump(20);
 const double injection_ms=(count-1)*interval/1000.;
 QJsonObject out{{"case",name},{"source_width",width},{"source_height",height},{"active_dpr",active_dpr},{"source_dpr",source_dpr},{"target_width",active_view.target_size.width},{"target_height",active_view.target_size.height},{"input_events",count},{"handled",m->handled},{"injection_ms",injection_ms},{"last_event_ms",m->last_event_ms},{"tail_ms",std::max(0.,m->last_event_ms-injection_ms)},{"requests",m->requests},{"delivered_callbacks",m->accepted},{"presents",m->presented},{"operation_image_changes",m->uploads},{"present_errors",m->errors},{"present_total_ms",m->present_ms},{"drawable_total_ms",m->drawable_ms},{"callback_total_ms",m->accept_ms},{"event_handler",distribution(m->event_ms)},{"event_queue_age",distribution(m->event_age_ms)},{"native_present",distribution(m->present_times)},{"drawable_wait",distribution(m->drawable_times)},{"stopped",stopped}};
 {std::lock_guard lock(m->mutex);out.insert("compute",distribution(m->compute_ms));out.insert("heavy_statistics_calls",m->heavy_stats);out.insert("detail_rebin_calls",m->detail_calls);out.insert("work_prepare_calls",m->prepare_calls);out.insert("max_retained_payload_bytes",double(m->max_retained));out.insert("max_estimated_payload_peak_bytes",double(m->max_payload_peak));}
 out.insert("last_input_handled_ms",ms(start,m->last_handled));
 out.insert("actual_queue_tail_ms",std::max(0.,ms(start,m->last_handled)-injection_ms));
 out.insert("presentation",QJsonObject{{"requested",double(presentation.requested)},{"submitted",double(presentation.submitted)},{"completed",double(presentation.completed)},{"completed_generation",double(presentation.completed_generation)},{"failed",double(presentation.failed)},{"dropped",double(presentation.dropped)}});
 struct rusage usage{};getrusage(RUSAGE_SELF,&usage);
 out.insert("process_peak_rss_bytes",double(usage.ru_maxrss));
 out.insert("initial_retained_bytes",double(initial.value()->retained_bytes));
 out.insert("note","Synthetic no-momentum events; tail is GUI queue drain, not photon/display latency. Peak RSS covers entire process; compare identical run parameters.");
 return out;
}
int main(int argc,char**argv){
 App app(argc,argv);app.setQuitOnLastWindowClosed(false);
 Method method=class_getInstanceMethod(CAMetalLayer.class,@selector(nextDrawable));original_drawable=method_setImplementation(method,(IMP)timed_drawable);
 try{int which=argc>1?std::atoi(argv[1]):0;int width=argc>2?std::atoi(argv[2]):2048;int height=argc>3?std::atoi(argv[3]):1280;int count=argc>4?std::atoi(argv[4]):120;int interval=argc>5?std::atoi(argv[5]):8333;
  if(which<0||which>10||width<16||height<16||width>6144||height>4096||count<1||count>240||interval<4000)return 2;
  const QString names[]{"source_ui_no_gpu_no_analysis","source_gpu_no_analysis","source_gpu_and_analysis","wave_pan_gpu_and_analysis","hist_pan_gpu_and_analysis","vector_scroll_gpu_and_analysis","layout_gpu_and_analysis","wave_pinch_refine","hist_pinch_refine","vector_pinch_refine","mask_drag"};
  const int target=which==10?0:which>=7?which-6:std::max(0,which-2);
  auto result=run(names[which],which!=0,which>=2,target,width,height,count,interval,which>=7&&which<=9,which==10);
  std::cout<<QJsonDocument(result).toJson(QJsonDocument::Compact).toStdString()<<std::endl;
 }catch(const std::exception&e){std::cerr<<e.what()<<std::endl;return 1;}
 method_setImplementation(method,original_drawable);return 0;
}
