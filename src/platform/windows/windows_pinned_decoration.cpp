#include "platform/windows/windows_pinned_decoration.hpp"
#include "platform/windows/windows_d3d.hpp"
#include "ui/qt/pinned_window_style.hpp"
#include <QAbstractNativeEventFilter>
#include <QApplication>
#include <QEvent>
#include <QPointer>
#include <QTimer>
#include <QVariant>
#include <QWidget>
#include <dcomp.h>
#include <d2d1.h>
#include <dwmapi.h>
#include <array>

namespace hdrshot {
namespace {
void checked(HRESULT result,const char *api,
    std::source_location origin=std::source_location::current()) {
  if(FAILED(result)) throw WindowsSyntheticFailure{windows_d3d_error(
      "WindowsPinnedDecoration",api,result,origin)};
}
class Decoration final : public QObject,public QAbstractNativeEventFilter {
public:
  Decoration(QWidget &widget,std::shared_ptr<DiagnosticsPort> diagnostics,bool preview)
      :QObject(&widget),widget_(&widget),diagnostics_(std::move(diagnostics)),preview_(preview) {
    setObjectName("windowsPinnedDecoration");
    widget.installEventFilter(this);qApp->installNativeEventFilter(this);schedule();
  }
  ~Decoration() override {qApp->removeNativeEventFilter(this);}
  bool nativeEventFilter(const QByteArray &,void *message,qintptr *) override {
    const auto *msg=static_cast<MSG *>(message);
    if(msg->hwnd==hwnd_ && (msg->message==WM_DWMCOMPOSITIONCHANGED ||
        msg->message==WM_THEMECHANGED || msg->message==WM_DPICHANGED))schedule();
    return false;
  }
protected:
  bool eventFilter(QObject *object,QEvent *event) override {
    if(object==widget_ && (event->type()==QEvent::Resize || event->type()==QEvent::Show ||
        event->type()==QEvent::WinIdChange || event->type()==QEvent::DevicePixelRatioChange ||
        event->type()==QEvent::ScreenChangeInternal || event->type()==QEvent::WindowStateChange))schedule();
    return false;
  }
private:
  void schedule() {
    if(pending_)return;
    pending_=true;QTimer::singleShot(0,this,[this]{pending_=false;synchronize();});
  }
  void shadow() {
    const DWMNCRENDERINGPOLICY policy=DWMNCRP_ENABLED;
    checked(DwmSetWindowAttribute(hwnd_,DWMWA_NCRENDERING_POLICY,&policy,sizeof(policy)),"DwmSetWindowAttribute(NCRENDERING_POLICY)");
    // Only a one-physical-pixel frame: never extend glass through the HDR image.
    const MARGINS margins{1,1,1,1};
    checked(DwmExtendFrameIntoClientArea(hwnd_,&margins),"DwmExtendFrameIntoClientArea");
    if(!preview_) {
      // Win11's system-colored edge is composed above client visuals. Suppress
      // that single-color edge, not the shadow or our two alpha rings. Win10
      // does not provide this optional attribute and keeps its native policy.
      const COLORREF none=DWMWA_COLOR_NONE;
      const auto hr=DwmSetWindowAttribute(hwnd_,DWMWA_BORDER_COLOR,&none,sizeof(none));
      if(FAILED(hr) && hr!=E_INVALIDARG)
        record_diagnostic_failure(diagnostics_.get(),"pin.system_border",windows_d3d_error(
            "WindowsPinnedDecoration","DwmSetWindowAttribute(BORDER_COLOR)",hr));
    }
    if(preview_) {
      const BOOL disabled=TRUE;
      checked(DwmSetWindowAttribute(hwnd_,DWMWA_TRANSITIONS_FORCEDISABLED,&disabled,sizeof(disabled)),"DwmSetWindowAttribute(TRANSITIONS_FORCEDISABLED)");
    }
  }
  void create_border() {
    // A topmost DComp tree is above all child HWND content, including Source.
    // Visuals have no HWND/input target and cannot consume resize or drag input.
    if(!gpu_) {
      checked(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,
          D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,
          gpu_.put(),nullptr,nullptr),"D3D11CreateDevice");
    }
    winrt::com_ptr<IDXGIDevice> dxgi;
    checked(gpu_->QueryInterface(__uuidof(IDXGIDevice),dxgi.put_void()),"QueryInterface(IDXGIDevice)");
    checked(DCompositionCreateDevice(dxgi.get(),__uuidof(IDCompositionDevice),composition_.put_void()),"DCompositionCreateDevice");
    checked(composition_->CreateTargetForHwnd(hwnd_,TRUE,target_.put()),"CreateTargetForHwnd");
    checked(composition_->CreateVisual(root_.put()),"CreateVisual(root)");
    winrt::com_ptr<ID2D1Factory> factory;
    checked(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,factory.put()),"D2D1CreateFactory");
    for(int ring=0;ring<2;++ring) {
      winrt::com_ptr<IDCompositionSurface> surface;
      checked(composition_->CreateSurface(1,1,DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_ALPHA_MODE_PREMULTIPLIED,surface.put()),"CreateSurface");
      winrt::com_ptr<IDXGISurface> pixels;POINT offset{};
      checked(surface->BeginDraw(nullptr,__uuidof(IDXGISurface),pixels.put_void(),&offset),"BeginDraw");
      try {
        winrt::com_ptr<ID2D1RenderTarget> paint;
        const auto properties=D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96);
        checked(factory->CreateDxgiSurfaceRenderTarget(pixels.get(),&properties,paint.put()),"CreateDxgiSurfaceRenderTarget");
        const float alpha=float(ring?pinned_window_style::inner_white_alpha:pinned_window_style::outer_black_alpha);
        const float color=ring?1.F:0.F;
        paint->BeginDraw();
        paint->PushAxisAlignedClip(D2D1::RectF(float(offset.x),float(offset.y),float(offset.x+1),float(offset.y+1)),D2D1_ANTIALIAS_MODE_ALIASED);
        paint->Clear(D2D1::ColorF(color,color,color,alpha));
        paint->PopAxisAlignedClip();checked(paint->EndDraw(),"ID2D1RenderTarget::EndDraw");
      } catch(...) {surface->EndDraw();throw;}
      checked(surface->EndDraw(),"IDCompositionSurface::EndDraw");
      for(int side=0;side<4;++side) {
        auto &visual=edges_[std::size_t(ring*4+side)];
        checked(composition_->CreateVisual(visual.put()),"CreateVisual(edge)");
        checked(visual->SetContent(surface.get()),"SetContent");
        checked(root_->AddVisual(visual.get(),TRUE,nullptr),"AddVisual");
      }
    }
    checked(target_->SetRoot(root_.get()),"SetRoot");
  }
  void synchronize() {
    if(!widget_ || !widget_->internalWinId())return;
    const auto next=reinterpret_cast<HWND>(widget_->internalWinId());
    if(next!=hwnd_) {
      for(auto &edge:edges_)edge=nullptr;
      root_=nullptr;target_=nullptr;composition_=nullptr;hwnd_=next;
    }
    try {shadow();widget_->setProperty("windowsShadowConfigured",true);}
    catch(const WindowsSyntheticFailure &failure) {
      widget_->setProperty("windowsShadowConfigured",false);
      record_diagnostic_failure(diagnostics_.get(),"pin.window_shadow",failure.error);
    }
    if(preview_)return;
    try {
      if(!target_)create_border();
      RECT rect{};
      if(!GetClientRect(hwnd_,&rect))return;
      const float width=float(rect.right),height=float(rect.bottom);
      const float line=float(pinned_window_style::border_line_physical_pixels(widget_->devicePixelRatioF()));
      for(int ring=0;ring<2;++ring) {
        const float inset=float(ring)*line,w=std::max(0.F,width-2*inset),h=std::max(0.F,height-2*inset);
        // Disjoint strips avoid double opacity at corners.
        const std::array<D2D1_RECT_F,4> strips{{{inset,inset,inset+w,inset+line},
          {inset,inset+h-line,inset+w,inset+h},
          {inset,inset+line,inset+line,inset+h-line},
          {inset+w-line,inset+line,inset+w,inset+h-line}}};
        for(int side=0;side<4;++side) {
          const auto r=strips[std::size_t(side)];auto &edge=edges_[std::size_t(ring*4+side)];
          const D2D_MATRIX_3X2_F transform{r.right-r.left,0,0,std::max(0.F,r.bottom-r.top),r.left,r.top};
          checked(edge->SetTransform(transform),"SetTransform");
        }
      }
      checked(composition_->Commit(),"Commit");
      widget_->setProperty("windowsBorderReady",true);
    } catch(const WindowsSyntheticFailure &failure) {
      widget_->setProperty("windowsBorderReady",false);
      record_diagnostic_failure(diagnostics_.get(),"pin.window_border",failure.error);
      target_=nullptr;root_=nullptr;for(auto &edge:edges_)edge=nullptr;composition_=nullptr;
      gpu_=nullptr; // A later native event can recreate a removed device.
    }
  }
  QPointer<QWidget> widget_;
  std::shared_ptr<DiagnosticsPort> diagnostics_;
  bool preview_{},pending_{};
  HWND hwnd_{};
  winrt::com_ptr<ID3D11Device> gpu_;
  winrt::com_ptr<IDCompositionDevice> composition_;
  winrt::com_ptr<IDCompositionTarget> target_;
  winrt::com_ptr<IDCompositionVisual> root_;
  std::array<winrt::com_ptr<IDCompositionVisual>,8> edges_;
};
}
void configure_windows_pinned_decoration(QWidget &widget,std::shared_ptr<DiagnosticsPort> diagnostics,bool preview) {
  if(!widget.findChild<QObject *>("windowsPinnedDecoration",Qt::FindDirectChildrenOnly))
    new Decoration(widget,std::move(diagnostics),preview);
}
}
