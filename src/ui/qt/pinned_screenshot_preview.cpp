#include "ui/qt/pinned_screenshot_preview.hpp"
#include "ui/qt/analyzer_source_view.hpp"
#include "ui/qt/source_presentation_controller.hpp"
#include <QApplication>
#include <QPainter>
#include <QMouseEvent>
#include <QCursor>
#include <QPainterPath>
#include <QScreen>
#include <QToolButton>
#include <QVariantAnimation>
#include <algorithm>
namespace hdrshot {
namespace {
// This is an explicitly passive owned Tool window, not a text tooltip.
class PreviewCard final : public QWidget {
public:
  explicit PreviewCard(QWidget &owner) : QWidget(&owner, Qt::Tool | Qt::FramelessWindowHint |
      Qt::WindowDoesNotAcceptFocus | Qt::WindowTransparentForInput) {
    setObjectName("pinHoverPreview");
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);
  }
protected:
  void paintEvent(QPaintEvent *) override {
    const bool dark=palette().color(QPalette::Window).lightness()<128;
    QPainter p(this);p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QColor(dark?"#5A626C":"#B8BEC6"));
    p.setBrush(QColor(dark?"#26292E":"#F5F6F8"));
    p.drawRoundedRect(QRectF(rect()).adjusted(.5,.5,-.5,-.5),9,9);
  }
};
class ExpandBadge final : public QWidget {
public:
  explicit ExpandBadge(QWidget *parent):QWidget(parent) {
    setObjectName("pinExpandBadge");
    setAttribute(Qt::WA_TransparentForMouseEvents);
    // An opaque native sibling stays above the native Source surface.
    setAttribute(Qt::WA_NativeWindow);
    setFocusPolicy(Qt::NoFocus);
    setGeometry(24,24,14,14);
  }
protected:
  void paintEvent(QPaintEvent *) override {
    const bool dark=parentWidget()->property("darkTheme").toBool();
    QPainter p(this);p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(),QColor(dark?"#26292E":"#F5F6F8"));
    p.setPen(QPen(QColor(dark?"#44484F":"#D5D8DD"),1));
    p.drawRoundedRect(QRectF(.5,.5,13,13),3,3);
    p.setPen(QPen(QColor(dark?"#E7E9EC":"#343940"),1.7,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
    QPainterPath mark;mark.moveTo(4,5.5);mark.lineTo(7,8.5);mark.lineTo(10,5.5);p.drawPath(mark);
  }
};
}
PinnedScreenshotPreview::PinnedScreenshotPreview(QWidget &owner,QToolButton &button,
    const analysis::Input &input):QObject(&owner),owner_(owner),button_(button),
      image_(int(input.source->size_px().width),int(input.source->size_px().height)) {
  card_=new PreviewCard(owner);
  sources_[0]=new AnalyzerSourceView(&button);sources_[0]->setObjectName("pinThumbnailSource");
  auto thumb=image_.scaled(QSize(32,30),Qt::KeepAspectRatio).expandedTo(QSize(1,1));
  sources_[0]->setGeometry((38-thumb.width())/2,(38-thumb.height())/2,thumb.width(),thumb.height());
  sources_[1]=new AnalyzerSourceView(card_);sources_[1]->setObjectName("pinPreviewSource");
  for(int i=0;i<2;++i) {
    auto *view=sources_[std::size_t(i)];
    view->set_source_size(image_);view->set_tools(false,false,false);
    view->setFocusPolicy(Qt::NoFocus);view->setAttribute(Qt::WA_TransparentForMouseEvents);
    view->setAccessibleName(i==0?QStringLiteral("截图缩略图"):QStringLiteral("截图临时预览"));
    view->setCursor(Qt::ArrowCursor);
    // The opaque native child must also be passive on both OS backends.
    view->presentation_widget()->setAttribute(Qt::WA_TransparentForMouseEvents);
    view->presentation_widget()->setFocusPolicy(Qt::NoFocus);
    controllers_[std::size_t(i)]=new SourcePresentationController(host(i),*view,[hdr=input.is_hdr] {
      SourcePresentationOptions options;
      options.settings.working_space=hdr?analysis::WorkingSpace::display_p3_pq:analysis::WorkingSpace::display_p3_sdr;
      return options;
    },[this,i](analysis::SourceView frame,std::uintptr_t surface) {
      const bool sent=present && present(i,std::move(frame),surface);
      if(i==0 && badge_->isVisible()) badge_->raise();
      return sent;
    },[this]{return bool(present);});
    view->view_changed=[this,i]{refresh(i);};
  }
  badge_=new ExpandBadge(&button); badge_->hide();sources_[0]->hide();card_->hide();
  hover_.setSingleShot(true);hover_.setTimerType(Qt::PreciseTimer);hover_.setInterval(300);
  connect(&hover_,&QTimer::timeout,this,[this]{show_preview();});
  appearance_=new QVariantAnimation(this);appearance_->setDuration(120);
  appearance_->setStartValue(0.);appearance_->setEndValue(1.);
  connect(appearance_,&QVariantAnimation::valueChanged,this,[this](const QVariant &v){
    card_->setWindowOpacity(v.toDouble());
    card_->move(target_position_+QPoint(0,int(-4*(1.-v.toDouble()))));
  });
  qApp->installEventFilter(this);
}
PinnedScreenshotPreview::~PinnedScreenshotPreview() {
  dismiss();qApp->removeEventFilter(this);
  for(auto *controller:controllers_) {controller->stop();delete controller;}
  // Delete callbacks before QObject children of owner are destroyed.
  for(auto *view:sources_) view->view_changed={};
  delete card_;delete badge_;delete sources_[0];
}
QWidget &PinnedScreenshotPreview::host(int index) const {return index==0?owner_:*card_;}
AnalyzerSourceView &PinnedScreenshotPreview::source(int index) const {return *sources_[std::size_t(index)];}
void PinnedScreenshotPreview::refresh(int index) {controllers_[std::size_t(index)]->request_present();}
void PinnedScreenshotPreview::set_collapsed(bool value) {
  dismiss();collapsed_=value;
  sources_[0]->setVisible(value);badge_->setVisible(value);
  if(value) {badge_->raise();refresh(0);}
}
void PinnedScreenshotPreview::dismiss() {
  hovering_=false;hover_.stop();appearance_->stop();card_->hide();
}
QRect PinnedScreenshotPreview::card_geometry(QRect anchor,QSize image,QRect available) {
  const QSize bounds=QSize(208,208).boundedTo((available.size()-QSize(12,12)).expandedTo(QSize(1,1)));
  image.scale(bounds,Qt::KeepAspectRatio);
  const QSize card=image.expandedTo(QSize(1,1))+QSize(12,12);
  int x=anchor.right()+1-card.width();
  int y=anchor.bottom()+1+12;
  if(y+card.height()>available.bottom()+1) y=anchor.top()-12-card.height();
  x=std::clamp(x,available.left(),std::max(available.left(),available.right()+1-card.width()));
  y=std::clamp(y,available.top(),std::max(available.top(),available.bottom()+1-card.height()));
  return {QPoint(x,y),card};
}
void PinnedScreenshotPreview::show_preview() {
  if(!collapsed_ || !hovering_ || !owner_.isVisible() || !button_.isVisible() || button_.isDown())return;
  const QRect anchor(button_.mapToGlobal(QPoint()),button_.size());
  auto *screen=QGuiApplication::screenAt(anchor.center());if(!screen)screen=owner_.screen();
  const auto geometry=card_geometry(anchor,image_,screen?screen->availableGeometry():QRect(0,0,1024,768));
  target_position_=geometry.topLeft();
  card_->setGeometry(geometry);sources_[1]->setGeometry(card_->rect().adjusted(6,6,-6,-6));
  card_->setPalette(owner_.palette());
  if(configure_window) configure_window(*card_);
  card_->setWindowOpacity(0.);card_->show();
  // Showing the owned native window establishes its destination screen/DPR.
  // A mask installed before that transition can retain the old physical HWND
  // region even when Qt's logical mask and the Source viewport are unchanged.
  // Clear the cached mask so equal logical geometry is reapplied at the new DPR.
  QPainterPath rounded;rounded.addRoundedRect(QRectF(sources_[1]->rect()),4,4);
  sources_[1]->clearMask();
  sources_[1]->setMask(QRegion(rounded.toFillPolygon().toPolygon()));
  appearance_->start();refresh(1);
}
bool PinnedScreenshotPreview::eventFilter(QObject *object,QEvent *event) {
  const auto type=event->type();
  auto *child=qobject_cast<QWidget *>(object);
  if(collapsed_ && child && child!=&button_ && button_.isAncestorOf(child)) {
    // Native NSView/HWND delivery can bypass WA_TransparentForMouseEvents.
    // Route before AnalyzerSourceView's surface filter consumes it as pan.
    switch(type) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick:
    case QEvent::MouseMove: {
      const auto *mouse=static_cast<QMouseEvent *>(event);
      const auto global=mouse->globalPosition();
      QMouseEvent routed(type,button_.mapFromGlobal(global),
          button_.window()->mapFromGlobal(global),global,
          mouse->button(),mouse->buttons(),mouse->modifiers(),mouse->source(),mouse->pointingDevice());
      routed.setTimestamp(mouse->timestamp());
      QApplication::sendEvent(&button_,&routed);
      event->setAccepted(routed.isAccepted());
      return true;
    }
    case QEvent::Enter:
      if(!hovering_) {hovering_=true;hover_.start();}
      return true;
    case QEvent::Leave:
      if(!button_.rect().contains(button_.mapFromGlobal(QCursor::pos()))) dismiss();
      return true;
    case QEvent::Wheel:
    case QEvent::NativeGesture:
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd:
    case QEvent::TouchCancel:
      return true; // A thumbnail is a button, never a navigable image viewport.
    default:break;
    }
  }
  if(object==&button_) {
    if(type==QEvent::ToolTip && collapsed_)return true;
    if(type==QEvent::Enter && collapsed_) {hovering_=true;hover_.start();}
    if(type==QEvent::Leave || type==QEvent::MouseButtonPress || type==QEvent::Hide) dismiss();
  }
  // Some Qt platform plugins emit owner deactivation while arranging an owned
  // passive window. Only an unrelated activation cancels this preview.
  if(object==&owner_ && (type==QEvent::Hide || type==QEvent::Close ||
      type==QEvent::Move || (type==QEvent::WindowDeactivate && QApplication::activeWindow()!=card_)))dismiss();
  if(type==QEvent::MouseButtonPress) {
    auto *widget=qobject_cast<QWidget *>(object);
    if(widget && widget->window()==&owner_) dismiss();
  }
  if(object==qApp && type==QEvent::ApplicationDeactivate)dismiss();
  return false;
}
}
