#pragma once
#include "application/capture_session.hpp"
#include "application/magnifier_sampler.hpp"
#include "application/async_clean_content_cache.hpp"
#include "ports/magnifier_card_port.hpp"
#include "ui/qt/capture_magnifier_panel.hpp"
#include "domain/color/capture_edr_boundary.hpp"
#include "application/export_workflow.hpp"
#include "application/overlay_input_recovery.hpp"
#include "ui/qt/overlay_editor_widget.hpp"
#include "ui/qt/qt_overlay_window_port.hpp"
#include <QKeyEvent>
#include <QApplication>
#include <QCursor>
#include <QMouseEvent>
#include <QPointer>
#include <QResizeEvent>
#include <QTimer>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <utility>
namespace hdrshot {
inline void print_overlay_error(const Error& error) {
  std::cerr << to_string(error.code) << '\n';
}
// Shared editor/model/preview/completion wiring. The native port never owns
// selection policy, annotation geometry, output format or export snapshots.
class QtOverlayHost final : public QWidget {
 public:
  MagnifierSampler::Counters magnifier_counters() const{return magnifier_sampler_.counters();}
  MagnifierPreview magnifier_preview() const{return magnifier_;}
  void set_diagnostics(std::shared_ptr<hdrshot::DiagnosticsPort> diagnostics) {
    diagnostics_ = std::move(diagnostics);
  }
  using ExportResult = hdrshot::Result<hdrshot::ExportReceipt, hdrshot::Error>;
  using ExportCompleted = std::function<void(ExportResult)>;
  using ExportRequested = std::function<bool(
      hdrshot::UiCommand, hdrshot::ExportSnapshot, ExportCompleted)>;
  using Finished = std::function<void()>;
  using AnalysisCompleted = std::function<void(Result<bool, Error>)>;
  using AnalysisRequested = std::function<void(ExportSnapshot, AnalysisCompleted)>;
  using InteractionAllowed = std::function<bool(hdrshot::DisplayId)>;
  using SelectionEstablished = std::function<void(hdrshot::DisplayId)>;
  using InitialGestureChanged = std::function<bool(hdrshot::DisplayId, bool)>;

  QtOverlayHost(
      std::unique_ptr<QtOverlayWindowPort> window,
      const hdrshot::DisplayId target_display_id,
      hdrshot::SelectionSnapshot initial_selection,
      hdrshot::QtInputPlatformAdapter& input_platform,
      std::shared_ptr<OverlayInputRecovery> recovery = std::make_shared<OverlayInputRecovery>())
      : window_(std::move(window)),
        target_display_id_(target_display_id),
        input_platform_(input_platform),
        selection_(initial_selection), recovery_(std::move(recovery)) {
    // Native window semantics are provided by the injected platform port.
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setProperty("seriousshotCaptureOverlay", true);
    magnifier_readout_timer_.setSingleShot(true);
    magnifier_readout_timer_.setTimerType(Qt::PreciseTimer);
    connect(&magnifier_readout_timer_, &QTimer::timeout, this, [this] {
      if (!magnifier_readout_deadline_) return;
      expire_magnifier_readout();
      if (capture_active_ && magnifier_.visible)
        show_magnifier({magnifier_.center, magnifier_revision_, {}});
    });
    magnifier_settle_timer_.setSingleShot(true);
    magnifier_settle_timer_.setTimerType(Qt::PreciseTimer);
    connect(&magnifier_settle_timer_, &QTimer::timeout, this, [this] {
      if (!magnifier_readout_expired_ || !magnifier_readout_.valid) return;
      magnifier_readout_expired_ = false;
      if (capture_active_ && magnifier_.visible)
        show_magnifier({magnifier_.center, magnifier_revision_, magnifier_readout_});
    });
    window_->set_input_interrupted([this] { input_interrupted(); });
    qApp->installEventFilter(this);
  }

  ~QtOverlayHost() override {
    capture_active_ = false;
    hide_magnifier();
    if (async_clean_cache_) async_clean_cache_->close();
    magnifier_sampler_.close();
    window_->set_input_interrupted({});
    if (qApp) qApp->removeEventFilter(this);
    // QWidget's base destructor hides children AFTER our members are gone.
    // An editor with a hover candidate can report Hide/clear-preview there.
    // Destroy it while the session/presenter members are still alive.
    delete editor_;
    editor_ = nullptr;
  }

  Result<bool,Error> prepare_hidden_native_surface() {
    window_->prepare(*this);
    if(auto error=window_->preparation_error()) return Result<bool,Error>::failure(std::move(*error));
    return Result<bool,Error>::success(true);
  }

  [[nodiscard]] void* native_surface() const { return window_->native_surface(); }

  void set_export_requested(
      ExportRequested callback,
      hdrshot::SettingsSnapshot export_settings) {
    export_requested_ = std::move(callback);
    export_settings_ = std::move(export_settings);
  }

  void set_finished(Finished callback) {
    finished_ = std::move(callback);
  }

  void set_analysis_requested(AnalysisRequested callback) {
    analysis_requested_ = std::move(callback);
    if (editor_) editor_->set_analysis_available(static_cast<bool>(analysis_requested_));
  }

  void set_interaction_allowed(InteractionAllowed callback) {
    interaction_allowed_ = std::move(callback);
  }

  void set_selection_established(SelectionEstablished callback) {
    selection_established_ = std::move(callback);
  }
  void set_initial_gesture_changed(InitialGestureChanged callback) {
    initial_gesture_changed_ = std::move(callback);
  }

  void set_interaction_locked(const bool locked) {
    interaction_locked_ = locked;
    if(locked)hide_magnifier();
    if (editor_ != nullptr) {
      editor_->set_interaction_locked(locked);
      editor_->setVisible(!locked);
    }
  }

  void set_system_dialog_active(const bool active, const bool restore_focus = false) {
    system_dialog_active_ = active;
    if(active)hide_magnifier();
    window_->set_system_dialog_active(*this, active, restore_focus);
    if (!active) {
      if (restore_focus && editor_ != nullptr && !interaction_locked_) editor_->setFocus();
      present();
    }
  }

  void set_magnifier_card(std::shared_ptr<MagnifierCardPresenterPort> card) { magnifier_card_ = std::move(card); }
  void* native_magnifier_surface() const { return window_->magnifier_surface(); }
  void set_background_clean_composition(ExportTaskExecutorPort& tasks, std::shared_ptr<CleanCompositionPort> composition) {
    async_clean_cache_ = std::make_unique<AsyncCleanContentCache>(tasks, std::move(composition));
  }

  void set_clean_composition(std::shared_ptr<CleanCompositionPort> executor) {
    clean_cache_ = std::make_unique<CleanContentCache>(std::move(executor));
  }

  void set_text_rasterizer(
      std::shared_ptr<hdrshot::TextRasterizerPort> text_rasterizer,
      std::string text_editor_font_family) {
    text_rasterizer_ = std::move(text_rasterizer);
    text_editor_font_family_ = std::move(text_editor_font_family);
  }

  void activate_capture(
      const hdrshot::CaptureReadyPayload& payload,
      std::shared_ptr<hdrshot::PreviewPresenterPort> presenter,
      std::shared_ptr<std::atomic<std::uint64_t>> operation_sequence) {
    hide_magnifier();
    frozen_desktop_ = payload.frozen_desktop;
    presenter_ = std::move(presenter);
    operation_sequence_ = std::move(operation_sequence);
    session_id_ = payload.present_receipt.session_id;
    const auto* segment = target_segment();
    if (segment == nullptr) {
      std::cerr << "Overlay target segment missing displayId="
                << target_display_id_.value << '\n';
      finish_capture();
      return;
    }
    editor_ = new hdrshot::OverlayEditorWidget(
        segment->size_px,
        segment->point_pixel_scale,
        text_rasterizer_.get(),
        text_editor_font_family_,
        input_platform_,
        selection_,
        this);
    editor_->set_clean_content_presented(clean_cache_ != nullptr || async_clean_cache_ != nullptr);
    editor_->set_analysis_available(static_cast<bool>(analysis_requested_));
    editor_->setGeometry(rect());
    editor_->setObjectName(QStringLiteral("overlayEditor"));
    editor_->set_window_candidates(payload.window_snapshot, target_display_id_);
    editor_->set_initial_gesture_changed([this](bool begin) {
      return !initial_gesture_changed_ || initial_gesture_changed_(target_display_id_, begin);
    });
    editor_->set_completion_bindings(hdrshot::CompletionBindings{
        export_settings_.enter_completion_action,
        export_settings_.double_click_completion_action,
    });
    annotation_render_plan_ = editor_->annotation_render_plan();
    editor_->set_interaction_started([this] {
      return !interaction_allowed_ || interaction_allowed_(target_display_id_);
    });
    editor_->set_preview_changed(
        [this](
            const hdrshot::SelectionSnapshot& selection,
            const hdrshot::AnnotationDocumentSnapshot& annotations,
            std::shared_ptr<const hdrshot::AnnotationRenderPlan> render_plan) {
      const bool establishes_selection = selection_.desktop_rect.empty() &&
          !selection.desktop_rect.empty();
      selection_ = selection;
      annotation_document_ = annotations;
      annotation_render_plan_ = std::move(render_plan);
      initial_highlight_ = editor_->initial_highlight();
      std::cout << "previewModelChanged selectionRevision=" << selection_.revision
                << " documentRevision=" << annotation_document_.revision
                << " annotationCount=" << annotation_document_.objects.size()
                << " coverageLayers="
                << (annotation_render_plan_ == nullptr
                        ? 0U
                        : annotation_render_plan_->ordered_layers.size())
                << '\n';
      if (establishes_selection && selection_established_) {
        selection_established_(target_display_id_);
      }
      present();
    });
    editor_->set_action_requested([this](const hdrshot::UiCommand command) {
      std::cout << "uiCommand=" << static_cast<int>(command)
                << " selectionRevision=" << selection_.revision
                << " documentRevision=" << annotation_document_.revision << '\n';
      if (command == hdrshot::UiCommand::cancel_capture) {
        if (exporting_) {
          return;
        }
        finish_capture();
        return;
      }
      if (command != hdrshot::UiCommand::copy_and_close &&
          command != hdrshot::UiCommand::save_default &&
          command != hdrshot::UiCommand::save_as && command != hdrshot::UiCommand::analyze) {
        return;
      }
      if (exporting_) {
        return;
      }
      if (command == hdrshot::UiCommand::analyze ? !analysis_requested_ : !export_requested_) {
        editor_->show_status(QStringLiteral("导出服务尚未就绪"), true);
        return;
      }

      selection_ = editor_->selection();
      annotation_document_ = editor_->annotations();
      annotation_render_plan_ = editor_->annotation_render_plan();
      // Pending text is committed before this action. Freeze this version.
      ExportSnapshot snapshot{session_id_, next_operation_id(), frozen_desktop_, selection_,
          annotation_document_, annotation_render_plan_, export_settings_.default_save_folder,
          target_display_id_, export_settings_.save_format, export_settings_.pq_diffuse_white,
          export_settings_.hdr_pq_precision, export_settings_.ultra_hdr_jpeg_quality,
          clean_content_, annotation_document_.revision};
      if (async_clean_cache_ && annotation_render_plan_) {
        exporting_ = true; hide_magnifier(); editor_->setEnabled(false);
        editor_->show_status(QStringLiteral("正在准备图像…"), false);
        const QPointer<QtOverlayHost> guard(this);
        async_clean_cache_->exact(frozen_desktop_, target_display_id_, annotation_render_plan_,
            [guard, command, snapshot=std::move(snapshot)](auto result) mutable {
          if (!guard || !guard->capture_active_ || guard->session_id_ != snapshot.session_id || guard->frozen_desktop_ != snapshot.frozen_desktop) return;
          if (!result) {
            guard->exporting_ = false; guard->editor_->setEnabled(true);
            guard->editor_->show_status(QStringLiteral("标注合成失败，编辑内容已保留"), true);
            record_diagnostic_failure(guard->diagnostics_.get(), "overlay.clean_content", result.error());
            return;
          }
          snapshot.clean_content = std::move(result.value());
          guard->dispatch_completion(command, std::move(snapshot));
        });
        return;
      }
      if (!refresh_clean_content()) return;
      snapshot.clean_content = clean_content_;
      dispatch_completion(command, std::move(snapshot));
    });

    magnifier_panel_=new CaptureMagnifierPanel(editor_);
    capture_active_ = true;
    show();
    window_->configure(*this, true);
    raise();
    activateWindow();
    setFocus();
    editor_->show();
    editor_->raise();
    editor_->setFocus();
    const auto initial_magnifier_revision = magnifier_revision_;
    const QPointer<OverlayEditorWidget> initial_editor(editor_);
    QTimer::singleShot(0, this, [this, initial_magnifier_revision, initial_editor] {
      if (!capture_active_ || editor_ != initial_editor) return;
      if (!interaction_locked_ && isVisible())
        editor_->refresh_window_candidate(editor_->mapFromGlobal(QCursor::pos()));
      if(magnifier_revision_==initial_magnifier_revision)request_magnifier(editor_->mapFromGlobal(QCursor::pos()));
      present();
    });
    std::cout << "qtHostReady=1 displayId=" << target_display_id_.value
              << " interaction=drag_selection_or_escape"
                 " nativeFullscreenExpected=0\n";
  }

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override {
    if (capture_active_ && watched == this &&
        (event->type() == QEvent::Hide ||
         (event->type() == QEvent::Leave && !rect().contains(mapFromGlobal(QCursor::pos())))))
      hide_magnifier();
    if (!capture_active_ || !isVisible() || system_dialog_active_)
      return QWidget::eventFilter(watched, event);
    if (watched == qApp && event->type() == QEvent::ApplicationDeactivate) {
      input_interrupted();
      return false;
    }
    auto* target = qobject_cast<QWidget*>(watched);
    if (!target || (target != this && !isAncestorOf(target))) return false;
    RecoveryPointerEvent kind;
    switch (event->type()) {
      case QEvent::MouseButtonPress: kind = RecoveryPointerEvent::press; break;
      case QEvent::MouseButtonDblClick: kind = RecoveryPointerEvent::double_click; break;
      case QEvent::MouseMove: kind = RecoveryPointerEvent::move; break;
      case QEvent::MouseButtonRelease: kind = RecoveryPointerEvent::release; break;
      default: return false;
    }
    auto* mouse = static_cast<QMouseEvent*>(event);
    if (kind != RecoveryPointerEvent::move && mouse->button() != Qt::LeftButton) return false;
    const auto action = recovery_->pointer(kind);
    if (action == RecoveryPointerAction::forward) {
      if(kind==RecoveryPointerEvent::move) request_magnifier(editor_->mapFromGlobal(mouse->globalPosition()));
      if(kind!=RecoveryPointerEvent::move) QTimer::singleShot(0,this,[this]{if(editor_)request_magnifier(editor_->mapFromGlobal(QCursor::pos()));});
      return false;
    }
    hide_magnifier();
    if (action == RecoveryPointerAction::ordinary_press) {
      // Qt may label the next press a double click because it counted the
      // recovery click. Deliver an ordinary press, including to child controls.
      QMouseEvent press(QEvent::MouseButtonPress, mouse->position(), mouse->scenePosition(),
          mouse->globalPosition(), mouse->button(), mouse->buttons(), mouse->modifiers(),
          mouse->pointingDevice());
      QApplication::sendEvent(watched, &press);
    } else if (action == RecoveryPointerAction::restore_focus) {
      window_->restore_input_focus(*this);
      if (editor_ && !interaction_locked_)
        editor_->restore_interaction_focus(editor_->mapFromGlobal(mouse->globalPosition()));
    } else if (kind == RecoveryPointerEvent::release && !recovery_->pending() &&
               editor_ && !interaction_locked_) {
      editor_->refresh_window_candidate(editor_->mapFromGlobal(mouse->globalPosition()));
    }
    event->accept();
    return true;
  }

  void mousePressEvent(QMouseEvent* event) override {
    // The full-size editor is the only mouse/selection policy. In particular,
    // ignored annotation clicks must never bubble into a second host selector.
    event->accept();
  }

  void mouseMoveEvent(QMouseEvent* event) override {
    event->accept();
  }

  void mouseReleaseEvent(QMouseEvent* event) override {
    event->accept();
  }

  void keyPressEvent(QKeyEvent* event) override {
    if (exporting_) {
      event->accept();
      return;
    }
    if (event->key() == Qt::Key_Escape) {
      finish_capture();
      event->accept();
      return;
    }
    if (interaction_locked_) {
      event->accept();
      return;
    }
    QWidget::keyPressEvent(event);
  }

  void resizeEvent(QResizeEvent* event) override {
    QWidget::resizeEvent(event);
    if (editor_ != nullptr) {
      editor_->setGeometry(rect());
      editor_->raise();
    }
    window_->resize(*this);
    if (frozen_desktop_ != nullptr) {
      present();
    }
  }

 private:

  void dispatch_completion(UiCommand command, ExportSnapshot snapshot) {
      exporting_ = true;
      const QPointer<QtOverlayHost> guard(this);
      if (command == hdrshot::UiCommand::analyze) {
        editor_->setEnabled(false);
        editor_->show_status(QStringLiteral("正在准备分析…"), false);
        analysis_requested_(std::move(snapshot),
            [guard](Result<bool, Error> result) {
              if (!guard) return;
              guard->exporting_ = false;
              if (result && result.value()) { guard->finish_capture(); return; }
              guard->editor_->setEnabled(true);
              guard->editor_->show_status(QStringLiteral("分析准备失败，选区和标注已保留"), true);
              if (!result) record_diagnostic_failure(guard->diagnostics_.get(), "analysis.prepare", result.error());
            });
        return;
      }
      const bool dispatched = export_requested_(
          command,
          std::move(snapshot),
          [guard](ExportResult result) mutable {
        if (guard != nullptr) {
          guard->handle_export_completed(std::move(result));
        }
      });
      if (!dispatched) {
        exporting_ = false; editor_->setEnabled(true);
      }
      if (dispatched) {
        // The immutable snapshot and destination ports are owned by the
        // background job. Return the user to the desktop immediately; the shell
        // reports any later failure through the resident tray application.
        finish_capture();
      }
  }

  void stop_magnifier_readout_grace() {
    magnifier_readout_timer_.stop();
    magnifier_readout_deadline_.reset();
  }
  void reset_magnifier_readout() {
    stop_magnifier_readout_grace();
    magnifier_settle_timer_.stop();
    magnifier_readout_expired_ = false;
    magnifier_readout_ = {};
  }
  void expire_magnifier_readout() {
    stop_magnifier_readout_grace();
    magnifier_readout_ = {};
    magnifier_readout_expired_ = true;
    // After an actual 80ms timeout, require 50ms at the same cursor point
    // before restoring digits. Sporadic successes during motion stay hidden.
    const auto remaining = std::chrono::milliseconds(50) -
        (std::chrono::steady_clock::now() - magnifier_last_move_);
    if (remaining > std::chrono::steady_clock::duration::zero())
      magnifier_settle_timer_.start(int(std::chrono::ceil<std::chrono::milliseconds>(remaining).count()));
  }
  void hide_magnifier() {
    reset_magnifier_readout();
    ++magnifier_revision_;magnifier_sampler_.invalidate();
    const bool shown=magnifier_.visible;magnifier_.visible=false;
    if(magnifier_panel_)magnifier_panel_->hide();
    ++card_generation_;
    window_->hide_magnifier();
    if(magnifier_card_) magnifier_card_->invalidate_card(card_generation_);
    if(editor_)editor_->set_magnifier_exclusion({});
    if(shown && capture_active_ && !magnifier_card_)present();
  }
  void request_magnifier(QPointF position) {
    if(!editor_ || !capture_active_ || interaction_locked_ || system_dialog_active_ || exporting_ || recovery_->pending() || recovery_->swallowing() ||
       !export_settings_.analysis_defaults.magnifier_enabled || !editor_->magnifier_applicable() || !editor_->rect().contains(position.toPoint())) {hide_magnifier();return;}
    const auto* segment=target_segment();if(!segment)return;
    const auto point=magnifier_source_point(position.x(),position.y(),segment->point_pixel_scale,segment->size_px);
    if(magnifier_.visible && magnifier_.center==point)return;
    const auto revision=++magnifier_revision_;
    const auto now = std::chrono::steady_clock::now();
    magnifier_last_move_ = now;
    // Image/coordinates follow immediately. A completed reading may bridge one
    // short readback gap, bounded from the first move, never extended by motion.
    if (magnifier_readout_expired_) {
      magnifier_readout_ = {};
      magnifier_settle_timer_.start(50);
    } else if (magnifier_readout_.valid) {
      if (!magnifier_readout_deadline_) {
        magnifier_readout_deadline_ = now + std::chrono::milliseconds(80);
        magnifier_readout_timer_.start(80);
      } else if (now >= *magnifier_readout_deadline_) {
        expire_magnifier_readout();
      }
    }
    show_magnifier({point,revision,magnifier_readout_expired_ ? MagnifierReadout{} : magnifier_readout_});
    const QPointer<QtOverlayHost> guard(this);
    magnifier_sampler_.submit({frozen_desktop_,target_display_id_,point,revision,export_settings_.analysis_defaults.reference_white_nits},
      [guard](MagnifierSampleResult result){QMetaObject::invokeMethod(qApp,[guard,result=std::move(result)]{
        if(!guard || !guard->capture_active_ || result.revision!=guard->magnifier_revision_)return;
        guard->stop_magnifier_readout_grace();
        guard->magnifier_readout_ = result.readout;
        if (guard->magnifier_readout_expired_ && result.readout.valid &&
            std::chrono::steady_clock::now() - guard->magnifier_last_move_ >= std::chrono::milliseconds(50)) {
          guard->magnifier_readout_expired_ = false;
          guard->magnifier_settle_timer_.stop();
        }
        guard->show_magnifier({result.point,result.revision,
            guard->magnifier_readout_expired_ ? MagnifierReadout{} : result.readout});
      },Qt::QueuedConnection);});
  }
  void show_magnifier(const MagnifierSampleResult& sample) {
    const auto* segment=target_segment();if(!segment || !magnifier_panel_)return;
    const auto& d=export_settings_.analysis_defaults;
    const double scale=segment->point_pixel_scale;const int zoom=d.magnifier_zoom;
    const int available=std::min({int(std::floor(220*scale)),segment->size_px.width,segment->size_px.height-int(std::ceil(CaptureMagnifierPanel::header_height()*scale))});
    if(available<zoom)return;
    const int image_px=magnifier_odd_pixels(available,zoom)*zoom;
    const int panel_width=int(std::ceil(image_px/scale));const int image_height=panel_width;
    const int max_height=std::max(1,editor_->height()-8);
    const int panel_height=std::min(max_height,image_height+CaptureMagnifierPanel::header_height()+magnifier_panel_->text_height(d.magnifier,panel_width));
    int x=int(sample.point.x/scale)+24,y=int(sample.point.y/scale)+24;
    if(x+panel_width>editor_->width()-8)x=int(sample.point.x/scale)-panel_width-24;
    if(y+panel_height>editor_->height()-8)y=int(sample.point.y/scale)-panel_height-24;
    x=std::clamp(x,0,std::max(0,editor_->width()-panel_width));y=std::clamp(y,0,std::max(0,editor_->height()-panel_height));
    const int native_x=std::min(int(std::round(x*scale)),segment->size_px.width-image_px);
    const int native_y=std::min(int(std::round((y+CaptureMagnifierPanel::header_height())*scale)),segment->size_px.height-image_px);
    const MagnifierPreview next{true,sample.point,{native_x,native_y,image_px,image_px},zoom,sample.revision};
    const bool image_changed=next!=magnifier_;magnifier_=next;
    magnifier_panel_->setGeometry(x,y,panel_width,panel_height);
    if (magnifier_card_) {
      // Desktop movement never enters the render worker or its resource mutex.
      window_->move_magnifier(*this, QRect(x,y,panel_width,panel_height));
      editor_->set_magnifier_exclusion({});
      magnifier_panel_->show_sample(sample.point,sample.readout,d.magnifier,image_height,zoom,false);
      const auto backing = devicePixelRatioF();
      const PixelSize size{qRound(panel_width*backing),qRound(panel_height*backing)};
      // Logical ceil() can add a physical UI pixel at fractional DPI. Keep
      // the HDR image on the original odd, integral source-cell grid.
      const int output_image_px=std::min(image_px,
          magnifier_odd_pixels(qRound(image_height*backing),zoom)*zoom);
      if (size != card_size_) {
        card_size_ = size; ++card_generation_;
        window_->hide_magnifier(); magnifier_card_->invalidate_card(card_generation_);
      }
      // QMacStyle needs a Core Graphics-compatible premultiplied bitmap,
      // even for QLabel's NoFrame draw. Convert to the card's straight RGBA
      // upload contract only after native style painting has finished.
#ifdef Q_OS_MACOS
      QImage ui(size.width,size.height,QImage::Format_ARGB32_Premultiplied);
#else
      QImage ui(size.width,size.height,QImage::Format_RGBA8888);
#endif
      ui.setDevicePixelRatio(backing); ui.fill(Qt::transparent);
      magnifier_panel_->render(&ui);
      ui=ui.convertToFormat(QImage::Format_RGBA8888);
      MagnifierCardFrame frame{session_id_,frozen_desktop_,target_display_id_,
          frozen_desktop_->display_generation,card_generation_,sample.revision,size,
          {true,sample.point,{0,qRound(CaptureMagnifierPanel::header_height()*backing),
           output_image_px,output_image_px},zoom,sample.revision},{}};
      frame.ui_rgba.resize(std::size_t(size.width)*size.height*4);
      for(int row=0;row<size.height;++row)
        std::copy_n(ui.constScanLine(row),std::size_t(size.width)*4,
            frame.ui_rgba.data()+std::size_t(row)*size.width*4);
      const auto generation=card_generation_;
      const QPointer<QtOverlayHost> guard(this);
      magnifier_card_->present_card(std::move(frame),[guard,generation](auto result) {
        QMetaObject::invokeMethod(qApp,[guard,generation,result=std::move(result)] {
          if(!guard || !guard->capture_active_ || !guard->magnifier_.visible ||
              guard->card_generation_!=generation) return;
          if(result) guard->window_->show_magnifier();
          else if(result.error().code!=ErrorCode::operation_cancelled)
            record_diagnostic_failure(guard->diagnostics_.get(),"overlay.card",result.error());
        },Qt::QueuedConnection);
      });
    } else {
      editor_->set_magnifier_exclusion(magnifier_panel_->geometry());
      magnifier_panel_->show_sample(sample.point,sample.readout,d.magnifier,image_height,zoom);
      if(image_changed)present();
    }
  }

  void input_interrupted() {
    if (!capture_active_ || !isVisible() || system_dialog_active_) return;
    hide_magnifier();
    recovery_->interrupt();
    if (editor_) editor_->interrupt_pointer_gesture();
  }
  void handle_export_completed(ExportResult exported) {
    exporting_ = false;
    if (!exported) {
      std::cerr << "Export failed ";
      print_overlay_error(exported.error());
      if (editor_ != nullptr) {
        editor_->show_status(
            QStringLiteral("导出失败：%1；当前编辑内容已保留")
                .arg(QString::fromStdString(hdrshot::to_string(exported.error().code))),
            true);
      }
      return;
    }
    if (std::holds_alternative<hdrshot::UserCancelled>(exported.value().destination)) {
      if (editor_ != nullptr) {
        editor_->show_status(QStringLiteral("已取消另存为，当前编辑内容已保留"), false);
      }
      return;
    }
    if (const auto* file = std::get_if<hdrshot::FileReceipt>(&exported.value().destination)) {
      std::cout << "exportCompleted destination=file path=" << file->exact_path
                << " bytes=" << file->bytes_written << '\n';
    } else if (const auto* clipboard =
                   std::get_if<hdrshot::ClipboardReceipt>(&exported.value().destination)) {
      std::cout << "exportCompleted destination=clipboard bytes="
                << clipboard->bytes_written << '\n';
    }
    finish_capture();
  }

  void finish_capture() {
    capture_active_ = false;
    hide_magnifier();
    if(async_clean_cache_) async_clean_cache_->close();
    magnifier_sampler_.close();
    hide();
    if (finished_) {
      finished_();
    }
  }

  [[nodiscard]] const hdrshot::CanonicalFrameSegment* target_segment() const {
    if (frozen_desktop_ == nullptr) {
      return nullptr;
    }
    const auto found = std::find_if(
        frozen_desktop_->canonical_segments.begin(),
        frozen_desktop_->canonical_segments.end(),
        [this](const hdrshot::CanonicalFrameSegment& candidate) {
          return candidate.display_id == target_display_id_;
        });
    return found == frozen_desktop_->canonical_segments.end() ? nullptr : &*found;
  }

  bool refresh_clean_content() {
    if (!clean_cache_ || !annotation_render_plan_) return true;
    auto result = clean_cache_->get(frozen_desktop_, target_display_id_, annotation_render_plan_);
    if (!result) {
      if (editor_) editor_->show_status(QStringLiteral("标注合成失败，编辑内容已保留"), true);
      return false;
    }
    clean_content_ = std::move(result.value());
    const auto counts = clean_cache_->counters();
    record_diagnostic_stage(diagnostics_.get(), session_id_, OperationId{}, "overlay", "clean_content", "success", {
        {"contentBuilds", std::to_string(counts.content_builds)},
        {"contentCacheHits", std::to_string(counts.cache_hits)},
        {"composedPixels", std::to_string(counts.composed_pixels)},
        {"cleanBytes", std::to_string(counts.retained_sample_bytes)}});
    return true;
  }

  void present() {
    if (!capture_active_ || presenter_ == nullptr || frozen_desktop_ == nullptr ||
        operation_sequence_ == nullptr) {
      return;
    }
    if (async_clean_cache_ && annotation_render_plan_) {
      const auto plan = annotation_render_plan_;
      if (clean_ready_plan_ != plan) {
        if (clean_pending_plan_ == plan) return;
        clean_pending_plan_ = plan;
        const QPointer<QtOverlayHost> guard(this);
        const auto source=frozen_desktop_;
        async_clean_cache_->preview(source, target_display_id_, plan,
            [guard,plan,source](auto result) {
          if (!guard || !guard->capture_active_ || guard->frozen_desktop_ != source || guard->annotation_render_plan_ != plan) return;
          guard->clean_pending_plan_.reset();
          if (!result) {
            if(result.error().code!=ErrorCode::operation_cancelled) {
              guard->editor_->show_status(QStringLiteral("标注合成失败，编辑内容已保留"), true);
              record_diagnostic_failure(guard->diagnostics_.get(),"overlay.clean_content",result.error());
            }
            return;
          }
          guard->clean_content_ = std::move(result.value());
          guard->clean_ready_plan_ = plan;
          guard->present();
        });
        return;
      }
    } else if (!refresh_clean_content()) return;
    const auto operation_id = next_operation_id();
    presenter_->present(
        hdrshot::PresentPreviewRequest{
            operation_id,
            hdrshot::PreviewModel{
                session_id_,
                frozen_desktop_,
                selection_,
                annotation_document_,
                hdrshot::OverlayStyle{},
                frozen_desktop_->display_generation,
                initial_highlight_,
                clean_content_,
                magnifier_card_ ? MagnifierPreview{} : magnifier_,
            },
            target_display_id_,
        },
        [operation_id, session = session_id_, log = diagnostics_](
            hdrshot::Result<hdrshot::PresentReceipt, hdrshot::Error> result) {
      if (!result) {
        if (result.error().code == hdrshot::ErrorCode::operation_cancelled) {
          return;
        }
        std::cerr << "Qt host presentation failed operationId=" << operation_id.value << ' ';
        hdrshot::record_diagnostic_stage(log.get(), session, operation_id,
            "overlay", "present", "failure", {}, &result.error());
        print_overlay_error(result.error());
      }
    });
  }

  [[nodiscard]] hdrshot::OperationId next_operation_id() const {
    return hdrshot::OperationId{
        operation_sequence_->fetch_add(1U, std::memory_order_relaxed)};
  }

  std::unique_ptr<QtOverlayWindowPort> window_;
  hdrshot::DisplayId target_display_id_{};
  std::shared_ptr<hdrshot::PreviewPresenterPort> presenter_;
  std::shared_ptr<std::atomic<std::uint64_t>> operation_sequence_;
  std::shared_ptr<hdrshot::TextRasterizerPort> text_rasterizer_;
  std::string text_editor_font_family_;
  hdrshot::QtInputPlatformAdapter& input_platform_;
  hdrshot::FrozenDesktopRef frozen_desktop_;
  hdrshot::SessionId session_id_{};
  hdrshot::SelectionSnapshot selection_{};
  std::shared_ptr<OverlayInputRecovery> recovery_;
  hdrshot::AnnotationDocumentSnapshot annotation_document_{};
  std::shared_ptr<const hdrshot::AnnotationRenderPlan> annotation_render_plan_;
  std::unique_ptr<CleanContentCache> clean_cache_;
  CleanContentRef clean_content_;
  std::unique_ptr<AsyncCleanContentCache> async_clean_cache_;
  std::shared_ptr<const AnnotationRenderPlan> clean_ready_plan_, clean_pending_plan_;
  hdrshot::PreviewHighlight initial_highlight_{};
  bool interaction_locked_{false};
  bool exporting_{false};
  bool capture_active_{false};
  bool system_dialog_active_{false};
  hdrshot::OverlayEditorWidget* editor_{};
  ExportRequested export_requested_;
  AnalysisRequested analysis_requested_;
  Finished finished_;
  std::shared_ptr<hdrshot::DiagnosticsPort> diagnostics_;
  InteractionAllowed interaction_allowed_;
  SelectionEstablished selection_established_;
  InitialGestureChanged initial_gesture_changed_;
  hdrshot::SettingsSnapshot export_settings_{};
  MagnifierSampler magnifier_sampler_;
  CaptureMagnifierPanel* magnifier_panel_{};
  std::shared_ptr<MagnifierCardPresenterPort> magnifier_card_;
  std::uint64_t card_generation_{};
  PixelSize card_size_{};
  MagnifierPreview magnifier_{};
  std::uint64_t magnifier_revision_{};
  MagnifierReadout magnifier_readout_{};
  QTimer magnifier_readout_timer_;
  QTimer magnifier_settle_timer_;
  bool magnifier_readout_expired_{};
  std::chrono::steady_clock::time_point magnifier_last_move_{};
  std::optional<std::chrono::steady_clock::time_point> magnifier_readout_deadline_;
};


} // namespace hdrshot
