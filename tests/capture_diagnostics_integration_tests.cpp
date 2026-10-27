#include "application/capture_session.hpp"
#include "application/capture_inflight_diagnostics.hpp"
#include "platform/qt/qt_session_diagnostics_port.hpp"
#include "test_support.hpp"
#include <QTemporaryDir>
#include <QFile>
using namespace hdrshot;
namespace {
QByteArray read(const std::string& path) { QFile file(QString::fromStdString(path)); HDRSHOT_CHECK(file.open(QIODevice::ReadOnly)); return file.readAll(); }
Error native_error(const char* api) { return {ErrorCode::capture_failed,"test_native",Retryability::same_input,{{"api",api},{"nativeCode","-55"},{"nativeDomain","SCStreamErrorDomain"}}}; }
class Catalog final : public DisplayCatalogPort {
 public: std::shared_ptr<DiagnosticsPort> log; bool fail{};
 void snapshot_displays(const SnapshotDisplaysRequest& request, Completion completion) override {
   if(fail) { auto error=native_error("getShareableContent");
     record_diagnostic_stage(log.get(),request.session_id,request.operation_id,"display","enumerate","failure",{},&error,true);
     completion(Result<DisplaySnapshotSet,Error>::failure(error));
   } else completion(Result<DisplaySnapshotSet,Error>::success({1,{{{1},{0,0,1,1},1,{1,1}}}}));
 }
};
class Capture final : public CapturePort {
 public: std::shared_ptr<DiagnosticsPort> log; bool fail{};
 void capture(const CaptureBatchRequest& request,Completion completion) override {
   if(fail) { auto error=native_error("captureImageWithFilter");
     record_diagnostic_stage(log.get(),request.session_id,request.operation_id,"capture","native_return","failure",{},&error,true);
     completion(Result<NativeFrameBatch,Error>::failure(error));
   } else completion(Result<NativeFrameBatch,Error>::success({request.session_id,request.operation_id,1,
       {{{1},{1,1},PixelFormat::rgba16_float,{ColorPrimaries::display_p3,TransferFunction::linear,AlphaMode::opaque,0},{0,0,0,0x3c00}}}}));
 }
 void cancel(SessionId,OperationId) override {}
};
class Windows final : public WindowCatalogPort {
 public: void snapshot_windows(const SnapshotWindowsRequest&,Completion completion) override {
   completion(Result<WindowSnapshot,Error>::failure(native_error("window_optional")));
 }
 void cancel(SessionId,OperationId) override {}
};
class Presenter final : public PreviewPresenterPort {
 public: void present(const PresentPreviewRequest& request,Completion completion) override {
   completion(Result<PresentReceipt,Error>::success({request.model.session_id,request.operation_id,
       request.model.frozen_desktop->frame_id,request.model.display_generation,request.model.selection.revision,
       request.model.annotation_document.revision,request.model.initial_highlight.revision}));
 }
 void cancel(SessionId,OperationId) override {}
};
void simple_failure_ownership() {
 for(int mode=0;mode<3;++mode) {
   QTemporaryDir dir; auto log=std::make_shared<QtSessionDiagnosticsPort>(dir.path().toStdString()); HDRSHOT_CHECK(log->start("test","macOS").has_value());
   auto catalog=std::make_shared<Catalog>(); catalog->log=log; catalog->fail=mode==0;
   auto capture=std::make_shared<Capture>(); capture->log=log; capture->fail=mode==1;
   CaptureSession session(catalog,capture,std::make_shared<Presenter>(),std::make_shared<Windows>(),log);
   int called{}; bool success{};
   session.begin({{1},{1},{1},{{1}},{}},[&](auto result) { ++called; success=bool(result); });
   const auto contents=read(log->exact_path()); HDRSHOT_CHECK(called==1 && success==(mode==2));
   HDRSHOT_CHECK(contents.count("failure")== (mode==2 ? 0 : 1));
   HDRSHOT_CHECK(contents.count("nativeCode=-55")== (mode==2 ? 0 : 1));
   HDRSHOT_CHECK(!contents.contains("enumerate") && !contents.contains("manual_fallback") && !contents.contains("native_return"));
 }
}
void hot_switch_snapshot_and_privacy() {
 QTemporaryDir dir; auto log=std::make_shared<QtSessionDiagnosticsPort>(dir.path().toStdString()); HDRSHOT_CHECK(log->start("test","macOS").has_value());
 CaptureInFlightDiagnostics facts({{1},{2},3},log,{{1},{2}});
 facts.stage({1},"getShareableContent","call_enter"); facts.stage({2},"captureImageWithFilter","call_enter"); facts.image_received({1});
 HDRSHOT_CHECK(!read(log->exact_path()).contains("call_enter")); log->set_detailed_logging(true); facts.snapshot();
 facts.failure({1},native_error("getShareableContent"),"native_error"); facts.failure({2},native_error("captureImageWithFilter"),"native_error");
 auto contents=read(log->exact_path()); HDRSHOT_CHECK(contents.contains("pending=1") && contents.contains("received=1"));
 HDRSHOT_CHECK(contents.contains("api=getShareableContent") && contents.contains("api=captureImageWithFilter")); HDRSHOT_CHECK(contents.count("nativeCode=-55")==2);
 Error unsafe=native_error("/private/secret_name"); unsafe.safe_context["nativeDomain"]="window secret title"; unsafe.safe_context["path"]="/secret/path";
 facts.failure({2},unsafe,"late_error"); contents=read(log->exact_path()); HDRSHOT_CHECK(!contents.contains("secret"));
 facts.stage({1},"cancel","cancel_requested"); facts.snapshot(); contents=read(log->exact_path());
 HDRSHOT_CHECK(contents.contains("reason=capture_draining")); HDRSHOT_CHECK(!facts.write_failed());
 auto unopened=std::make_shared<QtSessionDiagnosticsPort>(dir.path().toStdString());
 CaptureInFlightDiagnostics broken({{1},{1},1},unopened,{{1}}); unopened->set_detailed_logging(true);
 broken.stage({1},"captureImageWithFilter","call_enter"); HDRSHOT_CHECK(broken.write_failed());
}
}
int main() { return test::run({{"simple ownership",simple_failure_ownership},{"hot snapshot privacy write failure",hot_switch_snapshot_and_privacy}}); }
