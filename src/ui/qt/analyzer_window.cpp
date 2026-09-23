#include "ui/qt/analyzer_window.hpp"
#include "ui/qt/analyzer_control_style.hpp"
#include "domain/analysis/engine.hpp"
#include "domain/analysis/math.hpp"
#include "ui/qt/analyzer_layout.hpp"
#include "ui/qt/analyzer_scope_plot.hpp"
#include "ui/qt/analyzer_swatch_panel.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QDialogButtonBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QListView>
#include <QStyledItemDelegate>
#include <QStyleOptionToolButton>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QScreen>
#include <QSignalBlocker>
#include <QStyle>
#include <QStyleFactory>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidgetAction>
#include <QWindow>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace hdrshot {
namespace {
class AnalyzerToolButton final : public QToolButton {
public:
  using QToolButton::QToolButton;
protected:
  void paintEvent(QPaintEvent *event) override {
    if (popupMode() != QToolButton::MenuButtonPopup || !menu()) {
      QToolButton::paintEvent(event);
      return;
    }
    QStyleOptionToolButton option;
    initStyleOption(&option);
    const QRect arrow = style()->subControlRect(QStyle::CC_ToolButton, &option,
                                                QStyle::SC_ToolButtonMenu, this);
    QPainter p(this);
    // QStyleSheetStyle's SC_ToolButton includes the menu area; its default
    // label is centred over the entire split button. Paint the label only in
    // the main action area, keeping QToolButton's native hit/event behaviour.
    QStyleOptionToolButton background = option;
    background.icon = {};
    background.text.clear();
    style()->drawComplexControl(QStyle::CC_ToolButton, &background, &p, this);
    QStyleOptionToolButton label = option;
    label.rect.setRight(arrow.left() - 1);
    style()->drawControl(QStyle::CE_ToolButtonLabel, &label, &p, this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(isEnabled() ? QColor("#d6dbe3") : QColor("#747982"),
                  1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    const QPointF c = QRectF(arrow).center();
    p.drawPolyline(QPolygonF{c + QPointF(-3, -1), c + QPointF(0, 2), c + QPointF(3, -1)});
  }
};
class AnalyzerComboBox final : public QComboBox {
public:
  using QComboBox::QComboBox;

protected:
  void paintEvent(QPaintEvent *event) override {
    QComboBox::paintEvent(event);
    // Draw the indicator ourselves: native style primitives cannot render
    // consistently to the report's offscreen image, even with a local style.
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(isEnabled() ? QColor("#d6dbe3") : QColor("#747982"),
                        1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    const QPointF center(width() - 10., height() * .5);
    painter.drawPolyline(QPolygonF{center + QPointF(-3.5, -1.5),
                                   center + QPointF(0, 2),
                                   center + QPointF(3.5, -1.5)});
  }
};
QIcon icon(int kind, bool ellipse = false) {
  QPixmap pixmap(48, 48);
  pixmap.fill(Qt::transparent);
  QPainter p(&pixmap);
  p.scale(2, 2);
  p.setRenderHint(QPainter::Antialiasing);
  p.setPen(
      QPen(QColor("#e9ebef"), 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  if (kind == 0) {
    QPolygonF shape{{4, 3}, {20, 10}, {13, 12}, {11, 20}, {4, 3}};
    p.drawPolyline(shape);
  } else if (kind == 1) {
    p.drawLine(5, 19, 17, 7);
    p.drawLine(8, 21, 20, 9);
    p.drawLine(5, 19, 4, 22);
    p.drawLine(4, 22, 8, 21);
    p.drawLine(13, 4, 22, 13);
    p.drawLine(17, 5, 21, 1);
  } else if (kind == 2 || kind == 3) {
    p.setPen(QPen(QColor("#e9ebef"), 1.7, Qt::DashLine));
    if (ellipse)
      p.drawEllipse(QRectF(4, 4, 16, 16));
    else
      p.drawRoundedRect(QRectF(4, 4, 16, 16), 2, 2);
    if (kind == 3) {
      p.setPen(QPen(QColor("#ff7580"), 2.6));
      p.drawLine(3, 21, 21, 3);
    }
  } else if (kind == 4) {
    p.drawLine(4, 6, 20, 6);
    p.drawLine(7, 12, 17, 12);
    p.drawLine(10, 18, 14, 18);
  } else if (kind == 5) {
    p.drawLine(5, 5, 19, 19);
    p.drawLine(19, 5, 5, 19);
  } else if (kind == 6) {
    p.drawRoundedRect(QRectF(5, 3, 14, 18), 2, 2);
    p.drawRect(QRectF(8, 3, 8, 6));
    p.drawRect(QRectF(8, 14, 8, 7));
  } else if (kind == 7) {
    p.drawRoundedRect(QRectF(8, 8, 13, 13), 2, 2);
    p.drawLine(4, 16, 3, 16);
    p.drawLine(3, 16, 3, 3);
    p.drawLine(3, 3, 16, 3);
  } else if (kind == 8) {
    p.drawLine(4, 6, 20, 6);
    p.drawLine(9, 3, 15, 3);
    p.drawPolyline(QPolygonF{{6, 9}, {7, 21}, {17, 21}, {18, 9}});
    p.drawLine(10, 10, 10, 17);
    p.drawLine(14, 10, 14, 17);
  }
  return QIcon(pixmap);
}
QToolButton *button(QString text, QString name, QWidget *parent,
                    int icon_kind = -1) {
  auto *b = new AnalyzerToolButton(parent);
  b->setObjectName(name);
  b->setText(text);
  b->setToolTip(text);
  b->setAccessibleName(text);
  b->setMinimumHeight(28);
  b->setFocusPolicy(Qt::StrongFocus);
  if (icon_kind >= 0) {
    b->setIcon(icon(icon_kind));
    b->setIconSize(QSize(18, 18));
    b->setToolButtonStyle(text.isEmpty() ? Qt::ToolButtonIconOnly
                                         : Qt::ToolButtonTextBesideIcon);
  }
  return b;
}
QComboBox *combo(QString name, QWidget *parent,
                 const QStringList &entries = {}) {
  auto *c = new AnalyzerComboBox(parent);
  c->setObjectName(name);
  c->addItems(entries);
  c->setMinimumContentsLength(1);
  c->setSizeAdjustPolicy(QComboBox::AdjustToContents);
  c->setMinimumHeight(28);
  // The default menu delegate follows native popup painting. A normal item
  // view keeps hover/selection visible with the window-local raster style.
  auto *view = new QListView(c);
  view->setMouseTracking(true);
  view->setUniformItemSizes(true);
  c->setView(view);
  c->setItemDelegate(new QStyledItemDelegate(view));
  return c;
}
QWidget *labeled(QString text, QWidget *control) {
  auto *w = new QWidget;
  auto *l = new QHBoxLayout(w);
  l->setContentsMargins(0, 0, 0, 0);
  l->setSpacing(5);
  l->addWidget(new QLabel(text));
  l->addWidget(control);
  return w;
}
QWidget *panel(QString name, QWidget *parent) {
  auto *w = new QFrame(parent);
  w->setObjectName(name);
  w->setProperty("analyzerPanel", true);
  auto *l = new QVBoxLayout(w);
  l->setContentsMargins(0, 0, 0, 0);
  l->setSpacing(0);
  w->setMinimumSize(0, 0);
  return w;
}
QWidget *header(QWidget *parent) {
  auto *w = new QWidget(parent);
  w->setProperty("analyzerHeader", true);
  new AnalyzerFlowLayout(w);
  return w;
}
QWidget *right_group(QWidget *parent) {
  auto *w = new QWidget(parent);
  w->setProperty("analyzerAlignRight", true);
  auto *layout = new QHBoxLayout(w);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(5);
  parent->layout()->addWidget(w);
  return w;
}
void add(QWidget *box, QWidget *child) { box->layout()->addWidget(child); }
QString rgb(const analysis::Rgb &value, int decimals) {
  return QString("%1 / %2 / %3")
      .arg(value[0], 0, 'f', decimals)
      .arg(value[1], 0, 'f', decimals)
      .arg(value[2], 0, 'f', decimals);
}
analysis::UiImage ui_image(const QImage &source) {
  const QImage image = source.convertToFormat(QImage::Format_RGBA8888);
  analysis::UiImage result;
  result.size = {image.width(), image.height()};
  const auto stride = std::size_t(image.width()) * 4;
  result.rgba.resize(stride * std::size_t(image.height()));
  for (int y = 0; y < image.height(); ++y)
    std::memcpy(result.rgba.data() + std::size_t(y) * stride,
                image.constScanLine(y), stride);
  return result;
}
double safe_number(const QJsonObject &object, const char *key, double fallback,
                   double low, double high) {
  const auto v = object.value(key);
  const double n = v.toDouble(fallback);
  return v.isDouble() && std::isfinite(n) && n >= low && n <= high ? n
                                                                   : fallback;
}
} // namespace

AnalyzerWindow::AnalyzerWindow(analysis::Input input, std::string preferences,
                               QWidget *parent)
    : QWidget(parent), input_(std::move(input)) {
  setObjectName("analyzerWindow");
  setWindowTitle("SeriousShot · 图像分析");
  setAttribute(Qt::WA_QuitOnClose, false);
  setMinimumSize(920, 640);
  resize(1280, 720);
  restore_preferences(preferences);
  resize(preferred_size_);
  request_.settings.working_space =
      input_.is_hdr ? analysis::WorkingSpace::display_p3_pq
                    : analysis::WorkingSpace::display_p3_sdr;
  if (!input_.is_hdr &&
      request_.scopes.histogram_mode == analysis::HistogramMode::rgb_adobe)
    request_.scopes.histogram_mode = analysis::HistogramMode::rgb;
  if (!input_.is_hdr &&
      request_.scopes.histogram_mode == analysis::HistogramMode::parade_adobe)
    request_.scopes.histogram_mode = analysis::HistogramMode::parade_rgb;
  request_timer_ = new QTimer(this);
  present_timer_ = new QTimer(this);
  present_timer_->setSingleShot(true);
  present_timer_->setTimerType(Qt::PreciseTimer);
  connect(present_timer_, &QTimer::timeout, this, [this] { present_now(); });
  refine_timer_ = new QTimer(this);
  refine_timer_->setSingleShot(true);
  refine_timer_->setInterval(160);
  connect(refine_timer_, &QTimer::timeout, this, [this] {
    update_scope_resolution();
    // View changes are already stored by the time this debounce fires. Compare
    // with the last dispatch, not with the current view before sizing, or a pan
    // at an unchanged fine resolution would never request its new detail tile.
    if (!dispatched_request_ ||
        !analysis::same_statistics_request(*dispatched_request_, request_) ||
        !analysis::same_detail_request(*dispatched_request_, request_))
      schedule_request();
  });
  request_timer_->setSingleShot(true);
  request_timer_->setInterval(0);
  connect(request_timer_, &QTimer::timeout, this,
          [this] { dispatch_request(); });
  build_ui();
  // Native macOS styles draw through an NSGraphicsContext, which is absent
  // during report rendering. Scope a pure Qt style to this window and all its
  // children, including future menu/reference/swatch widgets.
  analyzer_style_ = QStyleFactory::create("Fusion");
  analyzer_style_->setParent(this);
  setStyle(analyzer_style_);
  for (auto *widget : findChildren<QWidget *>()) {
    widget->setProperty("analyzerRasterStyle", true);
    widget->setStyle(analyzer_style_);
  }
  qApp->installEventFilter(this);
  sync_controls();
  apply_layout();
  source_->set_source_size(input_.source
                               ? QSize(input_.source->size_px().width,
                                       input_.source->size_px().height)
                               : QSize(1, 1));
  initialized_ = true;
  schedule_request();
}
AnalyzerWindow::~AnalyzerWindow() {
  closed_ = true;
  qApp->removeEventFilter(this);
  for (auto *timer : {request_timer_, present_timer_, refine_timer_})
    if (timer)
      timer->stop();
  // Native-child teardown may deliver hide/ungrab events while QWidget removes
  // its children. Do not call back into an already-destructing owner or timer.
  if (source_) {
    source_->view_changed = {};
    source_->overlay_changed = {};
    source_->hover_changed = {};
    source_->mask_changed = {};
    source_->pin_requested = {};
    source_->gesture_changed = {};
  }
  for (auto *plot : {wave_, hist_, vector_})
    if (plot) {
      plot->options_changed = {};
      plot->surface_resized = {};
    }
  for (auto *pane : split_)
    if (pane)
      pane->changed = {};
  if (swatches_) {
    swatches_->removeSwatch = {};
    swatches_->set_reading_formatter({});
  }
}
void AnalyzerWindow::set_request_handler(
    std::function<void(analysis::Request)> h) {
  request_handler_ = std::move(h);
  schedule_request();
}
void AnalyzerWindow::set_export_handler(
    std::function<void(analysis::ExportAction, analysis::ReportPlan)> h) {
  export_handler_ = std::move(h);
}
void AnalyzerWindow::set_present_handler(
    std::function<void(analysis::SourceView, std::uintptr_t)> h) {
  present_handler_ = std::move(h);
  present();
}
void AnalyzerWindow::set_preferences_changed(
    std::function<void(std::string)> h) {
  preferences_changed_ = std::move(h);
}
void AnalyzerWindow::set_close_confirmation(std::function<bool()> h) {
  confirm_close_ = std::move(h);
}

void AnalyzerWindow::build_ui() {
  setStyleSheet(
      "QWidget#analyzerWindow{background:#202124;color:#e9ebef;} "
      "QMessageBox#analyzerCloseConfirmation{background:#202124;color:#e9ebef;} "
      "QMessageBox#analyzerCloseConfirmation QPushButton:default,"
      "QMessageBox#analyzerCloseConfirmation QPushButton:focus{border-color:#9fb4c3;} "
      "QWidget{color:#e9ebef;font-size:12px;} "
      "QFrame[analyzerPanel=true]{background:#292b2f;border:1px solid "
      "#424750;border-radius:7px;} "
      "QWidget[analyzerHeader=true]{background:#292b2f;border-bottom:1px solid "
      "#424750;} "
      "QToolButton,QPushButton,QComboBox,QLineEdit{background:#30343b;border:"
      "1px solid #4b515c;border-radius:5px;padding:4px 6px;min-height:20px;} "
      "QToolButton:hover,QPushButton:hover{background:#424750;} "
      "QToolButton:checked{background:#405460;border-color:#9fb4c3;} "
      "QToolButton::menu-button{width:16px;border-left:1px solid #626873;} "
      "QToolButton::menu-arrow{image:none;} "
      "QComboBox{padding-right:22px;} "
      "QComboBox::down-arrow{image:none;} "
      "QComboBox::drop-down{width:18px;border:0;} QComboBox "
      "QAbstractItemView{background:#30343b;color:#e9ebef;selection-background-color:#526e85;"
      "selection-color:#ffffff;outline:0;} "
      "QComboBox QAbstractItemView::item{min-height:24px;padding:3px 8px;border:0;} "
      "QComboBox QAbstractItemView::item:selected,QComboBox QAbstractItemView::item:hover"
      "{background:#526e85;color:#ffffff;} "
      "QLabel{background:transparent;border:0;} QCheckBox{spacing:5px;} "
      "QScrollArea{border:0;background:transparent;} "
      "QScrollBar:vertical{background:#292b2f;width:9px;} "
      "QScrollBar::handle:vertical{background:#626873;border-radius:4px;min-"
      "height:22px;} QMenu{background:#30343b;border:1px solid #626873;} "
      "QMenu::item{padding:5px 20px;} QMenu::item:selected{background:#526e85;color:#ffffff;} "
      "QFrame#analyzerReferencePopup{background:#30343b;border:1px solid "
      "#626873;border-radius:7px;}" + analyzer_control_style::danger_qss());
  auto *root = new QVBoxLayout(this);
  root->setContentsMargins(12, 10, 12, 10);
  root->setSpacing(7);
  auto *top = header(this);
  auto *brand = new QLabel(
      "<b>SeriousShot</b> <span style='color:#abb1bb'>· 图像分析</span>", top);
  add(top, brand);
  space_ = combo("analyzerWorkingSpace", top,
                 {"sRGB / sRGB · SDR", "Display P3 / sRGB · SDR",
                  "Display P3 / PQ · HDR", "BT.2020 / PQ · HDR"});
  add(top, labeled("Working Space", space_));
  white_ = combo("analyzerReferenceWhite", top, {"203 nit", "100 nit"});
  add(top, labeled("分析参考白", white_));
  blur_ = combo("analyzerBlur", top,
                {"关闭", "0.5 px", "1 px", "2 px", "4 px", "8 px"});
  add(top, labeled("Blur", blur_));
  gain_ = new QLineEdit(QString::number(scope_gain_, 'g', 8), top);
  gain_->setObjectName("analyzerVectorGain");
  gain_->setAccessibleName("Scope 轨迹显示增益");
  gain_->setToolTip("Waveform / Parade / Vectorscope 轨迹显示增益 0.1–3；按回车应用；不影响 Histogram");
  gain_->setFixedWidth(54);
  add(top, labeled("Gain", gain_));
  connect(gain_, &QLineEdit::returnPressed, this, [this] {
    bool ok = false;
    const double value = gain_->text().toDouble(&ok);
    ok = ok && std::isfinite(value) && value >= .1 && value <= 3.;
    gain_->setStyleSheet(ok ? QString() : "QLineEdit{border-color:#ff7580;}");
    if (!ok) {
      gain_->setToolTip("请输入 0.1–3 的数值；当前生效值 " + QString::number(scope_gain_));
      return;
    }
    scope_gain_ = value;
    wave_->set_display_gain(value);
    vector_->set_display_gain(value);
    gain_->setToolTip("Waveform / Parade / Vectorscope 轨迹显示增益 0.1–3；按回车应用；不影响 Histogram");
    persist();
  });
  auto *exports = right_group(top);
  for (int i = 0; i < 2; ++i) {
    auto *b = button(i ? "复制分析图" : "保存分析图",
                     i ? "analyzerCopy" : "analyzerSave", exports, i ? 7 : 6);
    export_buttons_[std::size_t(i)] = b;
    b->setPopupMode(QToolButton::MenuButtonPopup);
    auto *menu = new QMenu(b);
    menu->addAction(i ? "复制分析图" : "保存分析图", this, [this, i] {
      export_original_[std::size_t(i)] = false;
      update_export_buttons();
      persist();
      export_action(i ? analysis::ExportAction::copy_analysis
                      : analysis::ExportAction::save_analysis);
    });
    menu->addAction(i ? "复制原截图" : "保存原截图", this, [this, i] {
      export_original_[std::size_t(i)] = true;
      update_export_buttons();
      persist();
      export_action(i ? analysis::ExportAction::copy_original
                      : analysis::ExportAction::save_original);
    });
    b->setMenu(menu);
    connect(b, &QToolButton::clicked, this, [this, i] {
      export_action(export_original_[std::size_t(i)]
                        ? (i ? analysis::ExportAction::copy_original
                             : analysis::ExportAction::save_original)
                        : (i ? analysis::ExportAction::copy_analysis
                             : analysis::ExportAction::save_analysis));
    });
    add(exports, b);
  }
  root->addWidget(top);
  auto *visibility = new QWidget(this);
  auto *visibility_layout = new QHBoxLayout(visibility);
  visibility_layout->setContentsMargins(0, 0, 0, 0);
  visibility_layout->setSpacing(18);
  const QStringList visible_names{"Waveform / Parade", "Histogram",
                                  "Vectorscope"};
  for (int i = 0; i < 3; ++i) {
    visibility_[std::size_t(i)] = new QCheckBox(visible_names[i], visibility);
    visibility_[std::size_t(i)]->setObjectName("analyzerVisible" +
                                               QString::number(i));
    visibility_layout->addWidget(visibility_[std::size_t(i)]);
    connect(visibility_[std::size_t(i)], &QCheckBox::toggled, this,
            [this, i](bool checked) {
              if (syncing_)
                return;
              if (i == 0)
                request_.scopes.waveform_visible = checked;
              else if (i == 1)
                request_.scopes.histogram_visible = checked;
              else
                request_.scopes.vector_visible = checked;
              apply_layout();
              persist();
              schedule_request();
            });
  }
  visibility_layout->addStretch();
  root->addWidget(visibility);
  split_[0] = new AnalyzerSplitPane(Qt::Vertical, "analyzerMainSplit", this);
  split_[1] =
      new AnalyzerSplitPane(Qt::Horizontal, "analyzerTopSplit", split_[0]);
  split_[2] =
      new AnalyzerSplitPane(Qt::Horizontal, "analyzerBottomSplit", split_[0]);
  split_[3] =
      new AnalyzerSplitPane(Qt::Vertical, "analyzerAmplitudeSplit", split_[1]);
  source_panel_ = panel("analyzerSourcePanel", split_[1]);
  wave_panel_ = panel("analyzerWavePanel", split_[3]);
  hist_panel_ = panel("analyzerHistogramPanel", split_[3]);
  vector_panel_ = panel("analyzerVectorPanel", split_[2]);
  swatch_panel_ = panel("analyzerSwatchesPanel", split_[2]);
  split_[3]->set_panes(wave_panel_, hist_panel_);
  split_[1]->set_panes(source_panel_, split_[3]);
  split_[2]->set_panes(swatch_panel_, vector_panel_);
  split_[0]->set_panes(split_[1], split_[2]);
  root->addWidget(split_[0], 1);
  split_[0]->set_minimum_spans(260, 130);
  split_[1]->set_minimum_spans(340, 400);
  split_[2]->set_minimum_spans(260, 280);
  split_[3]->set_minimum_spans(110, 110);
  for (int i = 0; i < 4; ++i) {
    split_[std::size_t(i)]->set_ratio(ratios_[std::size_t(i)]);
    split_[std::size_t(i)]->changed = [this, i](double ratio) {
      ratios_[std::size_t(i)] = ratio;
      persist();
      present();
      schedule_refinement();
    };
  }
  auto *source_head = header(source_panel_);
  add(source_head, new QLabel("<b>Source Signal</b>"));
  source_mode_ =
      combo("analyzerSourceMode", source_head, {"Original", "False Color"});
  add(right_group(source_head), source_mode_);
  add(source_panel_, source_head);
  auto *source_tools = header(source_panel_);
  const QStringList tool_names{"指针／平移", "吸管", "分析 Mask", "清除 Mask"};
  for (int i = 0; i < 4; ++i) {
    tools_[std::size_t(i)] =
        button("", "analyzerTool" + QString::number(i), source_tools, i);
    tools_[std::size_t(i)]->setToolTip(tool_names[i]);
    tools_[std::size_t(i)]->setAccessibleName(tool_names[i]);
    tools_[std::size_t(i)]->setCheckable(i < 3);
    tools_[std::size_t(i)]->setFixedWidth(i == 1 || i == 2 ? 49 : 32);
    add(source_tools, tools_[std::size_t(i)]);
    if (i < 2) {
      auto *separator = new QFrame(source_tools);
      separator->setObjectName("analyzerToolSeparator" + QString::number(i));
      separator->setFrameShape(QFrame::VLine);
      separator->setFixedSize(1, 22);
      separator->setStyleSheet("background:#59616d;");
      add(source_tools, separator);
    }
    connect(tools_[std::size_t(i)], &QToolButton::clicked, this, [this, i] {
      if (i == 3)
        set_mask({});
      else
        set_tool(i);
    });
  }
  auto *picker_menu = new QMenu(tools_[1]);
  tools_[1]->setPopupMode(QToolButton::MenuButtonPopup);
  tools_[1]->setMenu(picker_menu);
  auto *sample_widget = new QWidget(picker_menu);
  auto *sample_layout = new QHBoxLayout(sample_widget);
  sample_layout->setContentsMargins(8, 6, 8, 6);
  sample_layout->addWidget(new QLabel("采样范围"));
  auto *sample = combo("analyzerSampleSize", sample_widget,
                       {"1×1", "3×3", "5×5", "11×11", "31×31", "101×101"});
  const std::array<int, 6> sizes{1, 3, 5, 11, 31, 101};
  auto found = std::find(sizes.begin(), sizes.end(), sample_size_);
  sample->setCurrentIndex(int(found - sizes.begin()));
  sample_layout->addWidget(sample);
  auto *sample_action = new QWidgetAction(picker_menu);
  sample_action->setDefaultWidget(sample_widget);
  picker_menu->addAction(sample_action);
  picker_menu->addSeparator();
  connect(sample, qOverload<int>(&QComboBox::currentIndexChanged), this,
          [this, sizes](int index) {
            if (syncing_ || index < 0 || index >= int(sizes.size()))
              return;
            sample_size_ = sizes[std::size_t(index)];
            source_->set_sample_size(sample_size_);
            set_tool(1);
            schedule_request();
            persist();
          });
  const QStringList reading_names{"源线性 RGB / EDR · P3", "工作 R′G′B′ / code",
                                  "线性 YRGB / nit", "强度 Y′ / I",
                                  "Lab / ITP · Hue / Chroma"};
  for (int i = 0; i < 5; ++i) {
    auto *action = new QWidgetAction(picker_menu);
    auto *check = new QCheckBox(reading_names[i], picker_menu);
    check->setContentsMargins(8, 5, 8, 5);
    check->setChecked(readings_[std::size_t(i)]);
    check->setObjectName("analyzerReading" + QString::number(i));
    action->setDefaultWidget(check);
    picker_menu->addAction(action);
    connect(check, &QCheckBox::toggled, this, [this, i](bool checked) {
      readings_[std::size_t(i)] = checked;
      if (!syncing_)
        set_tool(1);
      refresh_readouts();
      refresh_swatches();
      update_minimum_height();
      persist();
    });
  }
  auto *mask_menu = new QMenu(tools_[2]);
  tools_[2]->setPopupMode(QToolButton::MenuButtonPopup);
  tools_[2]->setMenu(mask_menu);
  mask_menu->addAction("矩形", this, [this] { set_mask_shape(false); });
  mask_menu->addAction("椭圆", this, [this] { set_mask_shape(true); });
  source_zoom_ = combo("analyzerSourceZoom", source_tools,
                       {"Fit", "25%", "50%", "66.7%", "100%", "200%", "400%"});
  // Fit→numeric text must not change this flow row's wrapping threshold while
  // a pinch is in progress. Keep the same reservation for every zoom label.
  source_zoom_->setFixedWidth(116);
  auto *source_navigation = right_group(source_tools);
  add(source_navigation, source_zoom_);
  auto *source_fit = button("Fit", "analyzerSourceFit", source_navigation);
  add(source_navigation, source_fit);
  connect(source_fit, &QToolButton::clicked, this, [this] { source_->fit(); });
  connect(source_zoom_, qOverload<int>(&QComboBox::activated), this,
          [this](int index) {
            if (!index)
              source_->fit();
            else {
              const double scales[]{.25, .5, 2. / 3., 1., 2., 4.};
              source_->zoom_at(
                  QPointF(source_->width() * .5, source_->height() * .5),
                  scales[index - 1] / source_->effective_scale());
            }
          });
  add(source_panel_, source_tools);
  source_ = new AnalyzerSourceView(source_panel_);
  source_->overlay_changed = [this] { present(); };
  static_cast<QVBoxLayout *>(source_panel_->layout())->addWidget(source_, 1);
  false_legend_ = new QLabel(source_panel_);
  false_legend_->setWordWrap(true);
  false_legend_->setContentsMargins(8, 3, 8, 3);
  false_legend_->setTextFormat(Qt::RichText);
  add(source_panel_, false_legend_);
  readout_ = new QLabel(source_panel_);
  readout_->setObjectName("analyzerHoverReadout");
  readout_->setContentsMargins(9, 4, 9, 4);
  readout_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  // Rows are explicit and reserve their height even while hover is absent.
  // Do not let changing numeric strings advertise different wrapped heights
  // while a Source gesture is in progress.
  readout_->setWordWrap(false);
  readout_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  add(source_panel_, readout_);
  coordinate_ = new QLabel(source_panel_);
  coordinate_->setContentsMargins(9, 3, 9, 4);
  coordinate_->setStyleSheet(
      "color:#abb1bb;font-size:11px;border-top:1px solid #424750;");
  add(source_panel_, coordinate_);
  mask_readout_ = new QLabel(source_panel_);
  mask_readout_->setObjectName("analyzerMaskReadout");
  mask_readout_->setWordWrap(true);
  mask_readout_->setContentsMargins(9, 5, 9, 5);
  mask_readout_->setStyleSheet("border-top:1px solid #424750;");
  add(source_panel_, mask_readout_);
  source_->view_changed = [this] {
    const QSignalBlocker blocker(source_zoom_);
    source_zoom_->setItemText(
        0, QString(source_->transform().fit ? "Fit · %1%" : "%1%")
               .arg(source_->effective_scale() * 100, 0, 'f', 1));
    source_zoom_->setCurrentIndex(0);
    present();
  };
  source_->hover_changed = [this](std::optional<QPointF> point) {
    const auto pixel = [](std::optional<QPointF> p) -> std::optional<QPoint> {
      return p ? std::optional<QPoint>(QPoint(int(std::floor(p->x())), int(std::floor(p->y()))))
               : std::nullopt;
    };
    const bool changed = pixel(hover_) != pixel(point);
    hover_ = point;
    if (changed)
      schedule_request();
    refresh_readouts();
  };
  source_->mask_changed = [this](std::optional<AnalyzerSourceMask> mask) {
    set_mask(mask);
  };
  source_->pin_requested = [this](QPointF point, int size) {
    add_swatch(point, size);
  };
  source_->gesture_changed = [this](bool active) {
    gesture_active_ = active;
    refresh_readouts();
    if (!active)
      update_minimum_height();
  };
  auto *wave_head = header(wave_panel_);
  wave_kind_ = combo("analyzerWaveKind", wave_head, {"Waveform", "Parade"});
  add(wave_head, wave_kind_);
  wave_mode_ = combo("analyzerWaveMode", wave_head);
  add(wave_head, wave_mode_);
  colorize_ = new QCheckBox("Colorize", wave_head);
  colorize_->setObjectName("analyzerColorize");
  add(wave_head, colorize_);
  auto *hist_head = header(hist_panel_);
  add(hist_head, new QLabel("<b>Histogram</b>"));
  hist_mode_ = combo("analyzerHistogramMode", hist_head);
  add(hist_head, hist_mode_);
  auto *vector_head = header(vector_panel_);
  add(vector_head, new QLabel("<b>Vectorscope</b>"));
  vector_mode_ = combo("analyzerVectorMode", vector_head);
  add(vector_head, vector_mode_);
  const std::array<QWidget *, 3> heads{wave_head, hist_head, vector_head};
  for (int i = 0; i < 3; ++i) {
    auto *navigation = right_group(heads[std::size_t(i)]);
    zoom_labels_[std::size_t(i)] = new QLabel("1.0×");
    zoom_labels_[std::size_t(i)]->setFixedWidth(48);
    zoom_labels_[std::size_t(i)]->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    add(navigation, zoom_labels_[std::size_t(i)]);
    auto *fit_button =
        button("Fit", "analyzerScopeFit" + QString::number(i), navigation);
    add(navigation, fit_button);
    connect(fit_button, &QToolButton::clicked, this, [this, i] {
      (i == 0 ? wave_ : i == 1 ? hist_ : vector_)->fit();
    });
    if (i < 2) {
      auto *ref =
          button("", "analyzerReferences" + QString::number(i), navigation, 4);
      ref->setToolTip("参考线");
      ref->setAccessibleName("参考线");
      add(navigation, ref);
      connect(ref, &QToolButton::clicked, this,
              [this, i] { open_references(i == 1); });
    }
  }
  add(wave_panel_, wave_head);
  add(hist_panel_, hist_head);
  add(vector_panel_, vector_head);
  wave_ = new AnalyzerScopePlot(AnalyzerScopePlot::Kind::waveform, wave_panel_);
  hist_ =
      new AnalyzerScopePlot(AnalyzerScopePlot::Kind::histogram, hist_panel_);
  vector_ = new AnalyzerScopePlot(AnalyzerScopePlot::Kind::vectorscope,
                                  vector_panel_);
  static_cast<QVBoxLayout *>(wave_panel_->layout())->addWidget(wave_, 1);
  static_cast<QVBoxLayout *>(hist_panel_->layout())->addWidget(hist_, 1);
  static_cast<QVBoxLayout *>(vector_panel_->layout())->addWidget(vector_, 1);
  for (auto *plot : {wave_, hist_, vector_}) {
    plot->options_changed = [this](const analysis::ScopeOptions &options) {
      set_scope_options(options);
    };
    plot->surface_resized = [this] { schedule_refinement(); };
  }
  auto *swatch_head = header(swatch_panel_);
  add(swatch_head, new QLabel("<b>Swatches</b>"));
  auto *clear = button("", "analyzerClearSwatches", swatch_head, 8);
  clear->setIcon(analyzer_control_style::danger_glyph(true));
  clear->setToolTip("清空 Swatches");
  clear->setAccessibleName("清空 Swatches");
  analyzer_control_style::set_icon_button(clear, true);
  add(right_group(swatch_head), clear);
  connect(clear, &QToolButton::clicked, this, [this] { clear_swatches(); });
  add(swatch_panel_, swatch_head);
  swatches_ = new AnalyzerSwatchPanel(swatch_panel_);
  swatches_->set_copy_icon(icon(7));
  swatch_scroll_ = swatches_->findChild<QScrollArea *>("analyzerSwatchScroll");
  swatches_->removeSwatch = [this](std::uint64_t id) {
    request_.samples.erase(
        std::remove_if(request_.samples.begin(), request_.samples.end(),
                       [id](const auto &sample) { return sample.id == id; }),
        request_.samples.end());
    refresh_swatches();
    apply_layout();
    schedule_request();
  };
  swatches_->set_reading_formatter(
      [this](const analysis::Readout &readout, bool fallback) {
        return reading_text(readout, fallback);
      });
  static_cast<QVBoxLayout *>(swatch_panel_->layout())
      ->addWidget(swatches_, 1);
  status_ = new QLabel(this);
  status_->setObjectName("analyzerStatus");
  status_->setWordWrap(true);
  status_->hide();
  root->addWidget(status_);
  for (int i = 0; i < 2; ++i) {
    auto &editor = ref_editors_[std::size_t(i)];
    editor.frame = new QFrame(this);
    editor.frame->setObjectName("analyzerReferencePopup");
    editor.frame->setProperty("histogram", i == 1);
    auto *l = new QVBoxLayout(editor.frame);
    l->setContentsMargins(9, 9, 9, 9);
    l->setSpacing(5);
    auto *heading = new QHBoxLayout;
    editor.title = new QLabel;
    heading->addWidget(editor.title, 1);
    auto *clear_refs =
        button("清空", "analyzerClearRefs" + QString::number(i), editor.frame);
    heading->addWidget(clear_refs);
    l->addLayout(heading);
    editor.channel = combo("analyzerRefChannel" + QString::number(i),
                           editor.frame, {"R′G′B′", "I"});
    l->addWidget(editor.channel);
    auto *row = new QHBoxLayout;
    editor.input = new QLineEdit(editor.frame);
    editor.input->setObjectName("analyzerRefValue" + QString::number(i));
    editor.input->setText("203");
    editor.input->setMinimumWidth(40);
    row->addWidget(editor.input, 1);
    editor.unit = new QLabel("nit");
    row->addWidget(editor.unit);
    auto *add_ref = button("添加", "analyzerAddReference" + QString::number(i),
                           editor.frame);
    row->addWidget(add_ref);
    l->addLayout(row);
    editor.list = new QWidget;
    editor.list_layout = new QVBoxLayout(editor.list);
    editor.list_layout->setContentsMargins(0, 0, 0, 0);
    editor.list_layout->setSpacing(3);
    l->addWidget(editor.list);
    editor.error = new QLabel;
    editor.error->setWordWrap(true);
    editor.error->setStyleSheet("color:#ff7580;");
    l->addWidget(editor.error);
    editor.frame->hide();
    connect(editor.input, &QLineEdit::returnPressed, this,
            [this, i] { add_reference(i == 1); });
    connect(add_ref, &QToolButton::clicked, this,
            [this, i] { add_reference(i == 1); });
    connect(editor.channel, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this, i] { refresh_references(i == 1); });
    connect(clear_refs, &QToolButton::clicked, this, [this, i] {
      references_[reference_key(i == 1, reference_intensity(i == 1))].clear();
      refresh_references(i == 1);
      persist();
    });
  }
  connect(space_, qOverload<int>(&QComboBox::currentIndexChanged), this,
          [this](int index) {
            if (syncing_)
              return;
            request_.settings.working_space =
                static_cast<analysis::WorkingSpace>(index);
            request_.scopes.vector_pan = {};
            if (!analysis::is_hdr(request_.settings.working_space)) {
              if (request_.scopes.histogram_mode ==
                  analysis::HistogramMode::rgb_adobe)
                request_.scopes.histogram_mode = analysis::HistogramMode::rgb;
              if (request_.scopes.histogram_mode ==
                  analysis::HistogramMode::parade_adobe)
                request_.scopes.histogram_mode =
                    analysis::HistogramMode::parade_rgb;
            }
            sync_controls();
            schedule_request();
            present();
            persist();
          });
  connect(white_, qOverload<int>(&QComboBox::currentIndexChanged), this,
          [this](int index) {
            if (syncing_)
              return;
            request_.settings.reference_white_nits = index ? 100 : 203;
            sync_controls();
            schedule_request();
            present();
            persist();
          });
  connect(blur_, qOverload<int>(&QComboBox::currentIndexChanged), this,
          [this](int index) {
            if (syncing_)
              return;
            const double values[]{0, .5, 1, 2, 4, 8};
            request_.settings.blur_sigma_px = values[index];
            schedule_request();
            present();
            persist();
          });
  connect(source_mode_, qOverload<int>(&QComboBox::currentIndexChanged), this,
          [this](int index) {
            if (syncing_)
              return;
            false_color_ = index == 1;
            sync_controls();
            present();
            persist();
          });
  connect(wave_kind_, qOverload<int>(&QComboBox::currentIndexChanged), this,
          [this](int index) {
            if (syncing_)
              return;
            request_.scopes.wave_mode = index ? last_parade_ : last_wave_;
            sync_controls();
            update_scope_resolution();
            schedule_request();
            persist();
          });
  connect(wave_mode_, qOverload<int>(&QComboBox::currentIndexChanged), this,
          [this](int index) {
            if (syncing_)
              return;
            if (wave_kind_->currentIndex()) {
              last_parade_ = index ? analysis::WaveMode::parade_intensity_rgb
                                   : analysis::WaveMode::parade_rgb;
              request_.scopes.wave_mode = last_parade_;
            } else {
              last_wave_ = index ? analysis::WaveMode::rgb
                                 : analysis::WaveMode::intensity;
              request_.scopes.wave_mode = last_wave_;
            }
            sync_controls();
            update_scope_resolution();
            schedule_request();
            persist();
          });
  connect(hist_mode_, qOverload<int>(&QComboBox::currentIndexChanged), this,
          [this](int index) {
            if (syncing_ || index < 0)
              return;
            request_.scopes.histogram_mode =
                static_cast<analysis::HistogramMode>(
                    hist_mode_->itemData(index).toInt());
            request_.scopes.histogram_view = {};
            sync_controls();
            schedule_request();
            persist();
          });
  connect(vector_mode_, qOverload<int>(&QComboBox::currentIndexChanged), this,
          [this](int index) {
            if (syncing_)
              return;
            request_.scopes.vector_mode =
                index ? analysis::VectorMode::ycbcr
                      : analysis::VectorMode::perceptual;
            request_.scopes.vector_pan = {};
            sync_controls();
            schedule_request();
            persist();
          });
  connect(colorize_, &QCheckBox::toggled, this, [this](bool checked) {
    if (syncing_)
      return;
    request_.scopes.colorize = checked;
    set_scope_options(request_.scopes);
    persist();
  });
}

void AnalyzerWindow::sync_controls() {
  syncing_ = true;
  const bool hdr = analysis::is_hdr(request_.settings.working_space);
  space_->setCurrentIndex(int(request_.settings.working_space));
  white_->setCurrentIndex(request_.settings.reference_white_nits == 100 ? 1
                                                                        : 0);
  const std::array<double, 6> blur_values{0, .5, 1, 2, 4, 8};
  blur_->setCurrentIndex(int(std::find(blur_values.begin(), blur_values.end(),
                                       request_.settings.blur_sigma_px) -
                             blur_values.begin()));
  source_mode_->setCurrentIndex(false_color_ ? 1 : 0);
  wave_->set_display_gain(scope_gain_);
  vector_->set_display_gain(scope_gain_);
  update_export_buttons();
  visibility_[0]->setChecked(request_.scopes.waveform_visible);
  visibility_[1]->setChecked(request_.scopes.histogram_visible);
  visibility_[2]->setChecked(request_.scopes.vector_visible);
  tools_[0]->setChecked(!picker_ && !mask_armed_);
  tools_[1]->setChecked(picker_);
  tools_[2]->setChecked(mask_armed_);
  tools_[2]->setIcon(icon(2, ellipse_));
  tools_[3]->setIcon(icon(3, ellipse_));
  tools_[3]->setEnabled(request_.mask.enabled);
  source_->set_tools(picker_, mask_armed_, ellipse_);
  source_->set_sample_size(sample_size_);
  const bool parade =
      request_.scopes.wave_mode == analysis::WaveMode::parade_rgb ||
      request_.scopes.wave_mode == analysis::WaveMode::parade_intensity_rgb;
  wave_kind_->setCurrentIndex(parade ? 1 : 0);
  wave_mode_->clear();
  wave_mode_->addItems(parade
                           ? QStringList{"R′G′B′", hdr ? "IR′G′B′" : "Y′R′G′B′"}
                           : QStringList{hdr ? "I" : "Y′", "R′G′B′"});
  wave_mode_->setCurrentIndex(
      request_.scopes.wave_mode == analysis::WaveMode::rgb ||
              request_.scopes.wave_mode ==
                  analysis::WaveMode::parade_intensity_rgb
          ? 1
          : 0);
  colorize_->setVisible(request_.scopes.wave_mode ==
                        analysis::WaveMode::intensity);
  colorize_->setChecked(request_.scopes.colorize);
  hist_mode_->clear();
  const auto option = [&](QString title, analysis::HistogramMode mode) {
    hist_mode_->addItem(title, int(mode));
  };
  option(hdr ? "I" : "Y′", analysis::HistogramMode::intensity);
  option(hdr ? "R′G′B′ · PQ" : "R′G′B′", analysis::HistogramMode::rgb);
  if (hdr)
    option("R′G′B′ · Adobe-style", analysis::HistogramMode::rgb_adobe);
  option(hdr ? "R′G′B′ Parade · PQ" : "R′G′B′ Parade",
         analysis::HistogramMode::parade_rgb);
  if (hdr)
    option("R′G′B′ Parade · Adobe-style",
           analysis::HistogramMode::parade_adobe);
  option(hdr ? "Hue TP" : "Hue a*b*", analysis::HistogramMode::hue);
  hist_mode_->setCurrentIndex(
      hist_mode_->findData(int(request_.scopes.histogram_mode)));
  vector_mode_->clear();
  vector_mode_->addItems({hdr ? "ITP / T–P" : "Lab D65 / a*b*", "Y′CbCr"});
  vector_mode_->setCurrentIndex(
      request_.scopes.vector_mode == analysis::VectorMode::ycbcr ? 1 : 0);
  zoom_labels_[0]->setText(
      QString::number(request_.scopes.amplitude_view.zoom, 'f', 1) + "×");
  zoom_labels_[1]->setText(
      QString::number(request_.scopes.histogram_view.zoom, 'f', 1) + "×");
  zoom_labels_[2]->setText(
      QString::number(request_.scopes.vector_zoom, 'f', 1) + "×");
  false_legend_->setVisible(false_color_);
  QString legend =
      QString("EV · 18%灰 = %1 nit　")
          .arg(request_.settings.reference_white_nits * .18, 0, 'f', 2);
  const std::array<double, 15> stops{-7, -5, -4, -3, -2, -1, -.5, 0,
                                     .5, 1,  2,  3,  4,  5,  6};
  for (std::size_t i = 0; i < stops.size(); ++i) {
    const auto linear =
        analysis_math::false_color_rgb(float(.18 * std::exp2(stops[i])));
    const auto display = analysis::display_srgb(
        {linear.x, linear.y, linear.z}, analysis::WorkingSpace::display_p3_pq);
    const QColor color = QColor::fromRgbF(display[0], display[1], display[2]);
    const QString label =
        i == 0    ? "＜−6"
        : i == 14 ? "≥+6"
                  : (stops[i] > 0 ? "+" : "") + QString::number(stops[i]);
    legend += QString("<span style='white-space:nowrap'><span "
                      "style='color:%1'>■</span> %2</span> ")
                  .arg(color.name(), label);
  }
  false_legend_->setText(legend);
  for (auto *plot : {wave_, hist_, vector_})
    plot->set_options(request_.scopes);
  syncing_ = false;
  refresh_references(false);
  refresh_references(true);
  refresh_readouts();
  update_minimum_height();
}

void AnalyzerWindow::set_tool(int tool) {
  if ((tool == 0 && !picker_ && !mask_armed_) ||
      (tool == 1 && picker_) || (tool == 2 && mask_armed_)) {
    tools_[std::size_t(tool)]->setChecked(true);
    return;
  }
  source_->cancel_gesture();
  picker_ = tool == 1;
  mask_armed_ = tool == 2;
  sync_controls();
  persist();
}
void AnalyzerWindow::set_mask_shape(bool ellipse) {
  ellipse_ = ellipse;
  picker_ = false;
  mask_armed_ = true;
  sync_controls();
  persist();
}
void AnalyzerWindow::set_mask(std::optional<AnalyzerSourceMask> mask) {
  if (mask) {
    request_.mask = {true,
                     mask->ellipse ? analysis::MaskShape::ellipse
                                   : analysis::MaskShape::rectangle,
                     {mask->bounds.x(), mask->bounds.y(), mask->bounds.width(),
                      mask->bounds.height()}};
  } else
    request_.mask = {};
  source_->set_mask(mask);
  // A changed region must not display the previous region's mean while its
  // new analysis request is still pending.
  for (auto *plot : {wave_, hist_, vector_})
    plot->set_mask_enabled(false);
  tools_[3]->setEnabled(mask.has_value());
  if (!gesture_active_) {
    refresh_readouts();
    update_minimum_height();
  }
  schedule_request();
  present();
}
void AnalyzerWindow::set_scope_options(const analysis::ScopeOptions &options) {
  const auto previous = request_;
  request_.scopes = options;
  // A plot emits its view state, not ownership of the retained allocation.
  // Its cached options may predate a resize/refinement result.
  request_.scopes.wave_width = previous.scopes.wave_width;
  request_.scopes.wave_height = previous.scopes.wave_height;
  request_.scopes.vector_width = previous.scopes.vector_width;
  request_.scopes.vector_height = previous.scopes.vector_height;
  request_.scopes.histogram_bins = previous.scopes.histogram_bins;
  request_.scopes.wave_detail_width = previous.scopes.wave_detail_width;
  request_.scopes.wave_detail_height = previous.scopes.wave_detail_height;
  request_.scopes.vector_detail_width = previous.scopes.vector_detail_width;
  request_.scopes.vector_detail_height = previous.scopes.vector_detail_height;
  request_.scopes.vector_viewport_width = previous.scopes.vector_viewport_width;
  request_.scopes.vector_viewport_height = previous.scopes.vector_viewport_height;
  for (auto *plot : {wave_, hist_, vector_})
    plot->set_options(request_.scopes);
  zoom_labels_[0]->setText(
      QString::number(options.amplitude_view.zoom, 'f', 1) + "×");
  zoom_labels_[1]->setText(
      QString::number(options.histogram_view.zoom, 'f', 1) + "×");
  zoom_labels_[2]->setText(QString::number(options.vector_zoom, 'f', 1) + "×");
  update_reference_projection();
  if (!analysis::same_statistics_request(previous, request_))
    schedule_request();
  else
    schedule_refinement();
  persist();
}
void AnalyzerWindow::schedule_request() {
  if (!request_timer_ || closed_)
    return;
  // Do not restart a running timer: continuous hover/Mask input must not starve
  // delivery. The application worker retains only one latest pending request.
  if (!request_timer_->isActive())
    request_timer_->start();
  if (!source_ || !wave_ || !hist_ || !vector_ || !swatch_scroll_)
    return;
  const bool pending =
      !painted_request_ ||
      !analysis::same_statistics_request(*painted_request_, request_);
  for (auto *plot : {wave_, hist_, vector_})
    plot->set_pending(pending);
  const bool readings_pending =
      !result_ || result_->settings != request_.settings;
  refresh_readouts();
  if (readings_pending_ != readings_pending) {
    readings_pending_ = readings_pending;
    refresh_swatches();
  }
}
void AnalyzerWindow::dispatch_request() {
  if (!source_ || !wave_)
    return;
  request_.samples.erase(
      std::remove_if(request_.samples.begin(), request_.samples.end(),
                     [](const auto &s) { return s.id == 0; }),
      request_.samples.end());
  if (hover_ && !gesture_active_)
    request_.samples.push_back({0, int(std::floor(hover_->x())),
                                int(std::floor(hover_->y())),
                                std::uint32_t(sample_size_), true});
  ++request_.revision;
  if (!dispatched_request_ || !analysis::same_statistics_request(*dispatched_request_, request_))
    compatible_result_revision_ = request_.revision;
  dispatched_request_ = request_;
  if (request_handler_)
    request_handler_(request_);
}
void AnalyzerWindow::schedule_refinement() {
  if (refine_timer_ && !closed_ && wave_ && hist_ && vector_)
    refine_timer_->start();
}
void AnalyzerWindow::update_scope_resolution() {
  request_.scopes.wave_width = std::max(request_.scopes.wave_width, std::uint32_t(std::clamp(
      int(std::lround(wave_->plot_rect().width() * wave_->devicePixelRatioF())),
      16, 2048)));
  request_.scopes.wave_height =
      std::max(request_.scopes.wave_height, std::uint32_t(std::clamp(int(std::lround(wave_->plot_rect().height() *
                                               wave_->devicePixelRatioF())),
                               16, 2048)));
  request_.scopes.vector_width =
      std::max(request_.scopes.vector_width, std::uint32_t(std::clamp(int(std::lround(vector_->plot_rect().width() *
                                               vector_->devicePixelRatioF())),
                               16, 2048)));
  request_.scopes.vector_height =
      std::max(request_.scopes.vector_height, std::uint32_t(std::clamp(int(std::lround(vector_->plot_rect().height() *
                                               vector_->devicePixelRatioF())),
                               16, 2048)));
  const bool parade =
      request_.scopes.histogram_mode == analysis::HistogramMode::parade_rgb ||
      request_.scopes.histogram_mode == analysis::HistogramMode::parade_adobe;
  const double width = hist_->plot_rect().width() * hist_->devicePixelRatioF() /
                       (parade ? 3 : 1);
  const double bins = std::clamp(width * request_.scopes.histogram_view.zoom,
                                 16., double(analysis::max_histogram_bins));
  request_.scopes.histogram_bins =
      std::max(request_.scopes.histogram_bins, std::uint32_t(std::pow(2., std::ceil(std::log2(bins)))));
  const auto pixels = [](double extent) {
    return std::uint32_t(std::clamp(int(std::ceil(extent)), 16, 2048));
  };
  auto &scope = request_.scopes;
  if (scope.waveform_visible && scope.amplitude_view.zoom > 1.) {
    const int lanes = scope.wave_mode == analysis::WaveMode::parade_intensity_rgb ? 4
                    : scope.wave_mode == analysis::WaveMode::parade_rgb ? 3 : 1;
    scope.wave_detail_width = pixels(wave_->plot_rect().width() * wave_->devicePixelRatioF() / lanes);
    scope.wave_detail_height = pixels(wave_->plot_rect().height() * wave_->devicePixelRatioF());
  } else {
    scope.wave_detail_width = scope.wave_detail_height = 0;
  }
  scope.vector_viewport_width = std::uint32_t(std::max(1., std::ceil(vector_->plot_rect().width() * vector_->devicePixelRatioF())));
  scope.vector_viewport_height = std::uint32_t(std::max(1., std::ceil(vector_->plot_rect().height() * vector_->devicePixelRatioF())));
  const auto vector_width = pixels(scope.vector_viewport_width);
  const auto vector_height = pixels(scope.vector_viewport_height);
  const auto extents = analysis::vector_domain_extents(request_.settings, scope.vector_mode,
      scope.vector_viewport_width, scope.vector_viewport_height);
  const bool vector_underresolved = result_ &&
      (double(result_->vectorscope.width) * extents[0] / result_->vector_grid_x_extent / scope.vector_zoom < vector_width ||
       double(result_->vectorscope.height) * extents[1] / result_->vector_grid_y_extent / scope.vector_zoom < vector_height);
  if (scope.vector_visible && (scope.vector_zoom > 1. || vector_underresolved)) {
    scope.vector_detail_width = vector_width;
    scope.vector_detail_height = vector_height;
  } else {
    scope.vector_detail_width = scope.vector_detail_height = 0;
  }
  for (auto *plot : {wave_, hist_, vector_})
    plot->set_options(request_.scopes);
}
void AnalyzerWindow::accept_result(analysis::ResultRef result) {
  if (!result || !dispatched_request_ ||
      result->revision < compatible_result_revision_ ||
      result->revision > request_.revision ||
      (result_ && result->revision < result_->revision) ||
      result->settings != request_.settings ||
      !analysis::same_statistics_request(*dispatched_request_, request_))
    return;
  const bool source_work_changed = !result_ || result_->settings != result->settings;
  const bool vector_domain_changed = !result_ ||
      result_->vectorscope.width != result->vectorscope.width ||
      result_->vectorscope.height != result->vectorscope.height ||
      result_->vector_grid_x_extent != result->vector_grid_x_extent ||
      result_->vector_grid_y_extent != result->vector_grid_y_extent;
  result_ = std::move(result);
  // Each plot compares immutable payload identity, including its fine grid.
  // A Vector refinement must not force all Waveform bitmaps to regenerate.
  const bool rebuild_density = !painted_request_;
  painted_request_ = *dispatched_request_;
  readings_pending_ = false;
  for (auto *plot : {wave_, hist_, vector_}) {
    plot->set_mask_enabled(request_.mask.enabled);
    plot->set_result(result_, rebuild_density);
    plot->set_pending(false);
  }
  refresh_readouts();
  // Snapshot values can change while fixed sample identities remain the same.
  // Update existing cards on every accepted result; the panel rebuilds only
  // when sample or pair structure changes.
  if (swatches_)
    swatches_->set_samples_and_result(request_, result_);
  if (source_work_changed && false_color_)
    present();
  // The true data-domain extent is only known after the first statistics pass.
  // If it exceeds the visible calibration, refine from cached projections.
  if (source_work_changed || vector_domain_changed)
    schedule_refinement();
  if (status_->property("analysisError").toBool()) {
    status_->setProperty("analysisError", false);
    status_->setProperty("error", false);
    status_->setStyleSheet("color:#abb1bb;");
  }
  if (status_->property("error").toBool())
    return;
  if (result_->invalid_count) {
    status_->setText(
        QString("%1 个非有限样本未参与分析").arg(result_->invalid_count));
    status_->show();
  } else
    status_->hide();
}
void AnalyzerWindow::show_error(const QString &message) {
  status_->setProperty("analysisError", false);
  status_->setProperty("error", true);
  status_->setStyleSheet("color:#ff7580;");
  status_->setText(message);
  status_->show();
}
void AnalyzerWindow::accept_error(std::uint64_t revision,
                                  const QString &message) {
  if (request_timer_->isActive() || revision != request_.revision)
    return;
  show_error(message);
  status_->setProperty("analysisError", true);
}
void AnalyzerWindow::set_export_busy(bool busy) {
  for (auto *b : export_buttons_)
    b->setEnabled(!busy);
  if (!busy) {
    status_->setProperty("error", false);
    status_->hide();
  }
}
analysis::SourceView AnalyzerWindow::source_view() const {
  const double dpr = source_->devicePixelRatioF();
  const auto rect = source_->image_rect();
  analysis::SourceView view;
  view.target_size = {int(std::lround(source_->width() * dpr)),
                      int(std::lround(source_->height() * dpr))};
  view.scale = source_->effective_scale() * dpr;
  view.offset_x = rect.x() * dpr;
  view.offset_y = rect.y() * dpr;
  view.settings = request_.settings;
  view.false_color = false_color_;
  view.mask = request_.mask;
  return view;
}
void AnalyzerWindow::present() {
  if (!present_timer_ || closed_ || present_timer_->isActive())
    return;
  const double hz = screen() ? screen()->refreshRate() : 60.;
  const int interval = int(std::ceil(1000. / std::clamp(hz, 30., 120.)));
  present_timer_->start(interval);
}
void AnalyzerWindow::present_now() {
  if (present_handler_ && source_ && isVisible()) {
    const auto surface = std::uintptr_t(source_->presentation_surface()->winId());
    auto view = source_view();
    const QImage marks = source_->presentation_overlay(source_->devicePixelRatioF());
    if (!operation_overlay_ || operation_overlay_key_ != marks.cacheKey()) {
      operation_overlay_ = std::make_shared<analysis::UiImage>(ui_image(marks));
      operation_overlay_key_ = marks.cacheKey();
    }
    view.operation_overlay = operation_overlay_;
    present_handler_(std::move(view), surface);
  }
}
void AnalyzerWindow::set_fixture_image(QImage image) {
  source_->set_fixture_image(std::move(image));
}
void AnalyzerWindow::persist() {
  if (!syncing_ && preferences_changed_)
    preferences_changed_(preferences_json());
}

void AnalyzerWindow::apply_layout() {
  const bool swatches =
      std::any_of(request_.samples.begin(), request_.samples.end(),
                  [](const auto &s) { return s.id != 0; });
  wave_panel_->setVisible(request_.scopes.waveform_visible);
  hist_panel_->setVisible(request_.scopes.histogram_visible);
  vector_panel_->setVisible(request_.scopes.vector_visible);
  swatch_panel_->setVisible(swatches);
  split_[3]->setVisible(request_.scopes.waveform_visible ||
                        request_.scopes.histogram_visible);
  split_[2]->setVisible(swatches || request_.scopes.vector_visible);
  for (auto *pane : split_)
    pane->relayout();
  for (auto &editor : ref_editors_)
    if (editor.frame->isVisible())
      editor.frame->raise();
  present();
}
QString AnalyzerWindow::reading_text(const analysis::Readout &r,
                                     bool fallback) const {
  if (!result_ || result_->settings != request_.settings)
    return "—";
  if (!r.valid_count)
    return "无有效样本";
  QStringList rows;
  const bool hdr = analysis::is_hdr(request_.settings.working_space);
  if (readings_[0])
    rows << "源 P3 RGB · EDR   " + rgb(r.source_rgb_edr, 4);
  if (readings_[1])
    rows << QString("工作 R′G′B′ · %1   ").arg(hdr ? "PQ" : "sRGB") +
                rgb(r.signal_rgb, 4);
  if (readings_[2] ||
      (fallback && std::none_of(readings_.begin(), readings_.end(),
                                [](bool v) { return v; }))) {
    rows << QString("Y · nit   %1").arg(r.y_nits, 0, 'f', 2);
    rows << "R / G / B · nit   " + rgb(r.work_rgb_nits, 2);
  }
  if (readings_[3])
    rows << QString(hdr ? "I · nit（等效）   %1" : "Y′ · %   %1")
                .arg(hdr ? r.intensity_nits : r.intensity * 100, 0, 'f', 2);
  if (readings_[4]) {
    if (hdr) {
      rows << QString("I · nit（等效）   %1").arg(r.intensity_nits, 0, 'f', 2);
      rows << QString("ITP T / P   %1 / %2  |  ICtCp Ct / Cp   %3 / %4")
                  .arg(r.perceptual[1], 0, 'f', 4)
                  .arg(r.perceptual[2], 0, 'f', 4)
                  .arg(2 * r.perceptual[1], 0, 'f', 4)
                  .arg(r.perceptual[2], 0, 'f', 4);
    } else
      rows << "Lab D65 L* / a* / b*   " + rgb(r.perceptual, 2);
    rows << QString("Hue / Chroma · %1   %2 / %3")
                .arg(hdr ? "T–P" : "a*b*",
                     r.hue_degrees
                         ? QString::number(*r.hue_degrees, 'f', 1) + "°"
                         : "—",
                     QString::number(r.chroma, 'f', 4));
  }
  return rows.join('\n');
}
void AnalyzerWindow::refresh_readouts() {
  const int lines =
      (readings_[0] ? 1 : 0) + (readings_[1] ? 1 : 0) + (readings_[2] ? 2 : 0) +
      (readings_[3] ? 1 : 0) +
      (readings_[4]
           ? (analysis::is_hdr(request_.settings.working_space) ? 3 : 2)
           : 0);
  readout_->setFixedHeight(std::max(2, lines) * 16 + 8);
  const analysis::SampleResult *hover_result = nullptr;
  if (result_)
    for (const auto &sample : result_->samples)
      // A completed compatible sample remains visible until its replacement
      // arrives. Exact cursor equality blanks every frame during motion.
      if (!sample.request.id && hover_ &&
          sample.request.side == std::uint32_t(sample_size_)) {
        hover_result = &sample;
        break;
      }
  const bool hover_visible =
      hover_.has_value() && !gesture_active_ && !report_mode_;
  const bool mask_current =
      painted_request_ && painted_request_->mask == request_.mask;
  for (auto *plot : {wave_, hist_, vector_})
    if (plot)
      plot->set_transient_hidden(!hover_visible || !hover_result || !mask_current ||
                                !result_ || result_->settings != request_.settings);
  readout_->setText(hover_visible ? (hover_result && mask_current
                                         ? reading_text(hover_result->mean)
                                         : "—")
                                  : QString());
  readout_->setToolTip(readout_->text());
  const auto size = input_.source ? input_.source->size_px() : PixelSize{};
  coordinate_->setText(hover_visible && hover_result && mask_current
                           ? QString("Linear P3 · EDR　 %1, %2 · %3×%3 / %4 px")
                                 .arg(hover_result->request.x)
                                 .arg(hover_result->request.y)
                                 .arg(hover_result->request.side)
                                 .arg(hover_result->mean.valid_count)
                           : QString("Linear P3 · EDR　 %1 × %2")
                                 .arg(size.width)
                                 .arg(size.height));
  if (!gesture_active_) {
    mask_readout_->setVisible(request_.mask.enabled);
    mask_readout_->setText(request_.mask.enabled
                               ? QString("Mask 平均 · %1 px\n%2")
                                     .arg(result_ && mask_current
                                              ? result_->mask_mean.valid_count
                                              : 0)
                                     .arg(result_ && mask_current
                                              ? reading_text(result_->mask_mean)
                                              : "—")
                               : QString());
  }
}
void AnalyzerWindow::add_swatch(QPointF point, int size) {
  request_.samples.push_back({next_swatch_id_++, int(std::floor(point.x())),
                              int(std::floor(point.y())), std::uint32_t(size),
                              false});
  refresh_swatches();
  apply_layout();
  schedule_request();
}
void AnalyzerWindow::clear_swatches() {
  request_.samples.erase(
      std::remove_if(request_.samples.begin(), request_.samples.end(),
                     [](const auto &s) { return s.id != 0; }),
      request_.samples.end());
  refresh_swatches();
  apply_layout();
  schedule_request();
}
void AnalyzerWindow::refresh_swatches() {
  if (!swatches_)
    return;
  std::vector<AnalyzerSourcePin> pins;
  for (const auto &sample : request_.samples) {
    if (!sample.id)
      continue;
    pins.push_back({sample.id, QPointF(sample.x, sample.y), int(sample.side)});
  }
  source_->set_pins(std::move(pins));
  swatches_->set_samples_and_result(request_, result_);
  apply_layout();
}

bool AnalyzerWindow::reference_intensity(bool hist) const {
  if (hist)
    return request_.scopes.histogram_mode == analysis::HistogramMode::intensity;
  if (request_.scopes.wave_mode == analysis::WaveMode::intensity)
    return true;
  return request_.scopes.wave_mode ==
             analysis::WaveMode::parade_intensity_rgb &&
         ref_editors_[0].channel->currentIndex() == 1;
}
QString AnalyzerWindow::reference_key(bool hist, bool intensity) const {
  const auto mode = request_.scopes.histogram_mode;
  const QString signal = hist && mode == analysis::HistogramMode::hue ? "hue"
                         : intensity ? "intensity"
                                     : "rgb";
  const bool adobe = hist && (mode == analysis::HistogramMode::rgb_adobe ||
                              mode == analysis::HistogramMode::parade_adobe);
  return QString("%1|%2|%3|%4")
      .arg(hist ? "hist" : "wave")
      .arg(int(request_.settings.working_space))
      .arg(signal, adobe ? "adobe" : "signal");
}
void AnalyzerWindow::open_references(bool hist) {
  auto &editor = ref_editors_[hist ? 1 : 0];
  const bool show = !editor.frame->isVisible();
  dismiss_menus();
  if (show) {
    refresh_references(hist);
    QWidget *panel = hist ? hist_panel_ : wave_panel_;
    const QPoint origin =
        panel->mapTo(this, QPoint(std::max(8, panel->width() - 310), 45));
    editor.frame->setGeometry(
        std::clamp(origin.x(), 8, std::max(8, width() - 310)),
        std::clamp(origin.y(), 8, std::max(8, height() - 260)),
        std::min(302, width() - 16), editor.frame->sizeHint().height());
    editor.frame->show();
    editor.frame->raise();
    editor.input->setFocus();
    editor.input->selectAll();
  }
}
void AnalyzerWindow::refresh_references(bool hist) {
  auto &editor = ref_editors_[hist ? 1 : 0];
  if (!editor.frame)
    return;
  const bool hue =
      hist && request_.scopes.histogram_mode == analysis::HistogramMode::hue;
  const bool hdr = analysis::is_hdr(request_.settings.working_space);
  const bool intensity = reference_intensity(hist);
  const QString bank_key = reference_key(hist, intensity);
  auto &refs = references_[bank_key];
  if (editor.bank_key != bank_key) {
    editor.bank_key = bank_key;
    editor.page = 0;
    editor.input->setText(hue ? "120" : hdr ? "203" : "50");
  }
  constexpr std::size_t page_size = 5;
  const std::size_t last_page =
      refs.empty() ? 0 : (refs.size() - 1) / page_size;
  editor.page = std::min(editor.page, last_page);
  const QString unit = hue ? "°" : hdr ? "nit" : "%";
  editor.title->setText(QString("参考线 · %1 · %2 条")
                            .arg(hue         ? "Hue"
                                 : intensity ? (hdr ? "I" : "Y′")
                                             : "R′G′B′")
                            .arg(refs.size()));
  editor.unit->setText(unit);
  editor.channel->setVisible(!hist &&
                             request_.scopes.wave_mode ==
                                 analysis::WaveMode::parade_intensity_rgb);
  editor.channel->setItemText(1, hdr ? "I" : "Y′");
  while (auto *item = editor.list_layout->takeAt(0)) {
    delete item->widget();
    delete item;
  }
  for (std::size_t i = editor.page * page_size;
       i < std::min(refs.size(), (editor.page + 1) * page_size); ++i) {
    auto *row = new QWidget;
    auto *l = new QHBoxLayout(row);
    l->setContentsMargins(0, 0, 0, 0);
    auto *check =
        new QCheckBox(QString::number(refs[i].value, 'g', 8) + " " + unit);
    check->setChecked(refs[i].enabled);
    l->addWidget(check, 1);
    auto *remove = button("×", "analyzerDeleteReference", row);
    remove->setStyleSheet("QToolButton{padding:0;min-height:0;}");
    remove->setFixedSize(28, 28);
    row->setMinimumHeight(28);
    l->addWidget(remove);
    editor.list_layout->addWidget(row);
    const auto key = reference_key(hist, intensity);
    connect(check, &QCheckBox::toggled, this, [this, key, i](bool enabled) {
      references_[key][i].enabled = enabled;
      update_reference_projection();
      persist();
    });
    connect(remove, &QToolButton::clicked, this, [this, key, i, hist] {
      auto &bank = references_[key];
      if (i < bank.size())
        bank.erase(bank.begin() + std::ptrdiff_t(i));
      refresh_references(hist);
      persist();
    });
  }
  if (refs.empty())
    editor.list_layout->addWidget(new QLabel("尚无参考线"));
  if (last_page) {
    auto *row = new QWidget;
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *previous = button("‹", "analyzerRefPrevious", row);
    previous->setEnabled(editor.page > 0);
    auto *next = button("›", "analyzerRefNext", row);
    next->setEnabled(editor.page < last_page);
    layout->addWidget(previous);
    layout->addWidget(
        new QLabel(QString("%1 / %2").arg(editor.page + 1).arg(last_page + 1)),
        1, Qt::AlignCenter);
    layout->addWidget(next);
    editor.list_layout->addWidget(row);
    connect(previous, &QToolButton::clicked, this, [this, hist] {
      --ref_editors_[hist ? 1 : 0].page;
      refresh_references(hist);
    });
    connect(next, &QToolButton::clicked, this, [this, hist] {
      ++ref_editors_[hist ? 1 : 0].page;
      refresh_references(hist);
    });
  }
  editor.error->clear();
  // Newly inserted children of a visible list are shown on a later Qt event.
  // Measure them now, not while they are implicitly hidden (which produced a
  // zero/one-row height and clipped the remove buttons until the next update).
  for (int i = 0; i < editor.list_layout->count(); ++i)
    if (auto *widget = editor.list_layout->itemAt(i)->widget()) {
      widget->ensurePolished();
      widget->show();
    }
  editor.list_layout->invalidate();
  editor.list_layout->activate();
  editor.list->setFixedHeight(editor.list_layout->sizeHint().height());
  editor.frame->layout()->invalidate();
  editor.frame->layout()->activate();
  editor.frame->setFixedWidth(302);
  // QWidget's total size hint includes the styled QFrame border. A raw layout
  // hint omits those pixels and can clip the final remove button at the bottom.
  editor.frame->setFixedHeight(editor.frame->sizeHint().height());
  if (editor.frame->isVisible())
    editor.frame->move(
        std::clamp(editor.frame->x(), 8,
                   std::max(8, width() - editor.frame->width() - 8)),
        std::clamp(editor.frame->y(), 8,
                   std::max(8, height() - editor.frame->height() - 8)));
  update_reference_projection();
}
void AnalyzerWindow::add_reference(bool hist) {
  auto &editor = ref_editors_[hist ? 1 : 0];
  bool valid = false;
  const double value = editor.input->text().toDouble(&valid);
  const bool hue =
      hist && request_.scopes.histogram_mode == analysis::HistogramMode::hue;
  const double maximum = hue ? 360
                         : analysis::is_hdr(request_.settings.working_space)
                             ? 10000
                             : 100;
  if (!valid || !std::isfinite(value) || value < 0 || value > maximum) {
    editor.error->setText(
        QString("请输入 0–%1 %2").arg(maximum).arg(editor.unit->text()));
    editor.frame->adjustSize();
    return;
  }
  references_[reference_key(hist, reference_intensity(hist))].push_back(
      {value, true});
  editor.page =
      (references_[reference_key(hist, reference_intensity(hist))].size() - 1) /
      5;
  const double domain =
      analysis::axis_value_to_domain(request_.settings,
                                     hist ? request_.scopes.histogram_mode
                                          : analysis::HistogramMode::intensity,
                                     value);
  const auto view =
      hist ? request_.scopes.histogram_view : request_.scopes.amplitude_view;
  if (analysis::project_axis(domain, view) < 0 ||
      analysis::project_axis(domain, view) > 1)
    (hist ? hist_ : wave_)->fit();
  refresh_references(hist);
  editor.input->selectAll();
  persist();
}
void AnalyzerWindow::update_reference_projection() {
  for (int which = 0; which < 2; ++which) {
    const bool hist = which == 1;
    std::vector<AnalyzerReferenceLine> lines;
    for (bool intensity : {false, true}) {
      if (hist && intensity != reference_intensity(true))
        continue;
      if (!hist &&
          request_.scopes.wave_mode !=
              analysis::WaveMode::parade_intensity_rgb &&
          intensity !=
              (request_.scopes.wave_mode == analysis::WaveMode::intensity))
        continue;
      const auto found = references_.find(reference_key(hist, intensity));
      if (found == references_.end())
        continue;
      for (const auto &ref : found->second) {
        const bool hue = hist && request_.scopes.histogram_mode ==
                                     analysis::HistogramMode::hue;
        const QString unit = hue ? "°"
                             : analysis::is_hdr(request_.settings.working_space)
                                 ? "nit"
                                 : "%";
        lines.push_back({ref.value,
                         analysis::axis_value_to_domain(
                             request_.settings,
                             hist ? request_.scopes.histogram_mode
                                  : analysis::HistogramMode::intensity,
                             ref.value),
                         ref.enabled, intensity,
                         QString::number(ref.value, 'g', 8) + " " + unit});
      }
    }
    (hist ? hist_ : wave_)->set_references(std::move(lines));
  }
}

void AnalyzerWindow::dismiss_menus() {
  for (auto &e : ref_editors_)
    if (e.frame)
      e.frame->hide();
  if (auto *popup = qApp->activePopupWidget())
    popup->hide();
}
bool AnalyzerWindow::cancel_transient() {
  for (auto *pane : split_)
    if (pane && pane->cancel_drag())
      return true;
  if (source_ && source_->cancel_gesture())
    return true;
  for (auto *plot : {wave_, hist_, vector_})
    if (plot && plot->cancel_gesture())
      return true;
  if (swatches_ && swatches_->cancel_pair_gesture())
    return true;
  bool menus = false;
  for (auto &editor : ref_editors_)
    if (editor.frame && editor.frame->isVisible())
      menus = true;
  if (qApp->activePopupWidget())
    menus = true;
  if (menus) {
    dismiss_menus();
    return true;
  }
  return false;
}
bool AnalyzerWindow::eventFilter(QObject *object, QEvent *event) {
  if (event->type() == QEvent::Expose) {
    auto *native = qobject_cast<QWindow *>(object);
    auto *surface = source_ ? source_->findChild<QWidget *>("analyzerSourceSurface") : nullptr;
    if (native && (native == windowHandle() || (surface && native == surface->windowHandle())))
      present(); // replay retained view after occlusion/unlock, never reanalyze
  }
  auto *widget = qobject_cast<QWidget *>(object);
  if (!widget || (widget != this && !isAncestorOf(widget)))
    return QWidget::eventFilter(object, event);
  if (event->type() == QEvent::Polish && analyzer_style_ &&
      !widget->property("analyzerRasterStyle").toBool()) {
    widget->setProperty("analyzerRasterStyle", true);
    widget->setStyle(analyzer_style_);
  }
  if (event->type() == QEvent::DevicePixelRatioChange) {
    schedule_refinement();
    present();
  }
  if (event->type() == QEvent::WindowActivate || event->type() == QEvent::Show)
    present();
  if (event->type() == QEvent::KeyPress &&
      static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
    if (!cancel_transient())
      close();
    return true;
  }
  if (event->type() == QEvent::WindowDeactivate) {
    for (auto *pane : split_)
      if (pane)
        pane->cancel_drag();
    if (source_)
      source_->cancel_gesture();
    for (auto *plot : {wave_, hist_, vector_})
      if (plot)
        plot->cancel_gesture();
    if (swatches_)
      swatches_->cancel_pair_gesture();
  }
  if (event->type() == QEvent::MouseButtonPress) {
    for (int i = 0; i < 2; ++i) {
      auto &e = ref_editors_[std::size_t(i)];
      QWidget *panel = i ? hist_panel_ : wave_panel_;
      if (e.frame && e.frame->isVisible() && widget != e.frame &&
          !e.frame->isAncestorOf(widget) && widget != panel &&
          !panel->isAncestorOf(widget))
        e.frame->hide();
    }
  }
  return QWidget::eventFilter(object, event);
}
void AnalyzerWindow::closeEvent(QCloseEvent *e) {
  if (cancel_transient()) {
    e->ignore();
    return;
  }
  bool confirmed = false;
  if (confirm_close_)
    confirmed = confirm_close_();
  else {
    QMessageBox box(QMessageBox::Question, "关闭分析窗口",
                    "确定结束这个分析窗口吗？", QMessageBox::NoButton, this);
    // The analyzer is always dark, including its confirmation dialog. A native
    // dialog can otherwise mix the system light background with inherited QSS.
    box.setObjectName("analyzerCloseConfirmation");
    box.setOption(QMessageBox::Option::DontUseNativeDialog);
    auto *keep = box.addButton("继续分析 · Esc", QMessageBox::RejectRole);
    auto *leave = new QPushButton("结束分析 · Enter");
    leave->setObjectName("analyzerEndAnalysis");
    analyzer_control_style::set_danger(leave);
    box.addButton(leave, QMessageBox::DestructiveRole);
    // Keep the approved equal-width layout; shortcut hints are part of the
    // button labels rather than a separate row below the dialog.
    const int button_width = std::max({184, keep->sizeHint().width(),
                                      leave->sizeHint().width()});
    keep->setFixedWidth(button_width);
    leave->setFixedWidth(button_width);
    if (auto *buttons = box.findChild<QDialogButtonBox *>())
      buttons->layout()->setSpacing(16);
    keep->setAutoDefault(false);
    box.setDefaultButton(leave);
    box.setEscapeButton(keep);
    box.exec();
    confirmed = box.clickedButton() == leave;
  }
  if (!confirmed) {
    e->ignore();
    return;
  }
  closed_ = true;
  request_timer_->stop();
  present_timer_->stop();
  refine_timer_->stop();
  e->accept();
}
void AnalyzerWindow::resizeEvent(QResizeEvent *) {
  if (!initialized_)
    return;
  apply_layout();
  schedule_refinement();
  if (!first_show_ && !adapting_size_ && !isMaximized() && !isFullScreen()) {
    preferred_size_ = size();
    persist();
  }
}
void AnalyzerWindow::showEvent(QShowEvent *) {
  if (first_show_) {
    adapting_size_ = true;
    const auto available = screen() ? screen()->availableGeometry() : QRect(0, 0, 1280, 720);
    // Persist the logical preferred size, not an old monitor or desktop point.
    // Temporary fit-to-screen must not overwrite a larger remembered window.
    const int frame_width = std::max(0, frameGeometry().width() - width());
    const int available_width = std::max(1, available.width() - frame_width);
    setMinimumWidth(std::min(920, available_width));
    setMinimumHeight(std::min(minimumHeight(), std::max(1, available.height() - 32)));
    resize(preferred_size_.boundedTo(QSize(available_width, std::max(1, available.height() - 32))));
    const auto frame = frameGeometry();
    move(std::clamp(frame.x(), available.left(), std::max(available.left(), available.right() - frame.width() + 1)),
         std::clamp(frame.y(), available.top(), std::max(available.top(), available.bottom() - frame.height() + 1)));
    adapting_size_ = false;
    first_show_ = false;
  }
  apply_layout();
  present();
  if (!result_) {
    update_scope_resolution();
    schedule_request();
  } else
    schedule_refinement();
}
void AnalyzerWindow::update_minimum_height() {
  if (!source_)
    return;
  const int rows = (readings_[0] ? 1 : 0) + (readings_[1] ? 1 : 0) +
                   (readings_[2] ? 2 : 0) + (readings_[3] ? 1 : 0) +
                   (readings_[4] ? 3 : 0);
  const int source_required = 110 + std::max(2, rows) * 16 +
                              (request_.mask.enabled ? (rows + 1) * 16 : 0) +
                              (false_color_ ? 40 : 0) + 90;
  const int available = screen() ? screen()->availableGeometry().height() - 32 : 10000;
  setMinimumHeight(std::min(std::max(640, source_required + 220), std::max(1, available)));
  split_[0]->set_minimum_spans(source_required, 130);
}

analysis::ReportPlan AnalyzerWindow::report_plan() {
  const double dpr = devicePixelRatioF();
  analysis::ReportPlan plan;
  plan.revision = result_ ? result_->revision : request_.revision;
  plan.source_view = source_view();
  const QPoint origin = source_->mapTo(this, QPoint());
  plan.source_rect = {
      int(std::lround(origin.x() * dpr)), int(std::lround(origin.y() * dpr)),
      plan.source_view.target_size.width, plan.source_view.target_size.height};
  report_mode_ = true;
  if (swatches_)
    swatches_->set_transient_hidden(true);
  const QString gain_draft = gain_->text();
  const QString gain_style = gain_->styleSheet();
  gain_->setText(QString::number(scope_gain_, 'g', 8));
  gain_->setStyleSheet({});
  refresh_readouts();
  source_->set_report_mode(true);
  for (auto *plot : {wave_, hist_, vector_})
    plot->set_transient_hidden(true);
  const QSize pixels(int(std::lround(width() * dpr)),
                     int(std::lround(height() * dpr)));
  QImage underlay(pixels, QImage::Format_RGBA8888);
  underlay.setDevicePixelRatio(dpr);
  underlay.fill(Qt::transparent);
  {
    QPainter p(&underlay);
    render(&p);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.fillRect(QRectF(origin, QSizeF(source_->size())), Qt::transparent);
  }
  QImage overlay(pixels, QImage::Format_RGBA8888);
  overlay.setDevicePixelRatio(dpr);
  overlay.fill(Qt::transparent);
  {
    QPainter p(&overlay);
    p.drawImage(origin, source_->retained_overlay(dpr));
  }
  plan.underlay = ui_image(underlay);
  plan.overlay = ui_image(overlay);
  gain_->setText(gain_draft);
  gain_->setStyleSheet(gain_style);
  source_->set_report_mode(false);
  report_mode_ = false;
  if (swatches_)
    swatches_->set_transient_hidden(false);
  refresh_readouts();
  return plan;
}
void AnalyzerWindow::export_action(analysis::ExportAction action) {
  if (!export_handler_)
    return;
  for (auto *button : export_buttons_)
    if (button->menu())
      button->menu()->hide();
  const bool original = action == analysis::ExportAction::save_original ||
                        action == analysis::ExportAction::copy_original;
  if (!original) {
    refine_timer_->stop();
    update_scope_resolution();
    if (!dispatched_request_ ||
        !analysis::same_statistics_request(*dispatched_request_, request_) ||
        !analysis::same_detail_request(*dispatched_request_, request_))
      schedule_request();
  }
  const bool fixed_current = result_ && std::all_of(request_.samples.begin(), request_.samples.end(), [&](const auto &s) {
    return !s.id || std::any_of(result_->samples.begin(), result_->samples.end(), [&](const auto &r) {
      return r.request.id == s.id && r.request.x == s.x && r.request.y == s.y && r.request.side == s.side;
    });
  });
  const auto &scopes = request_.scopes;
  const auto detail_extents = analysis::vector_detail_extents(request_);
  const bool detail_current = result_ &&
      (!scopes.wave_detail_width || (result_->waveform_detail_view == scopes.amplitude_view &&
          std::any_of(result_->waveform_detail.begin(), result_->waveform_detail.end(), [&](const auto &grid) {
            const auto source_width = input_.source ? std::uint32_t(input_.source->size_px().width) : scopes.wave_detail_width;
            return grid.width >= std::min(scopes.wave_detail_width, source_width) && grid.height >= scopes.wave_detail_height;
          }))) &&
      (!scopes.vector_detail_width || (result_->vectorscope_detail.width >= scopes.vector_detail_width &&
          result_->vectorscope_detail.height >= scopes.vector_detail_height &&
          result_->vector_detail_center == scopes.vector_pan &&
          std::abs(result_->vector_detail_x_extent - detail_extents[0]) < 1e-9 &&
          std::abs(result_->vector_detail_y_extent - detail_extents[1]) < 1e-9));
  if (!original && (!result_ || !painted_request_ || !fixed_current || !detail_current ||
                    !analysis::same_statistics_request(*painted_request_, request_))) {
    show_error("分析正在更新，请稍后重试。");
    status_->setProperty("analysisError", true);
    return;
  }
  status_->setProperty("error", false);
  status_->hide();
  export_handler_(action, original ? analysis::ReportPlan{} : report_plan());
}
void AnalyzerWindow::update_export_buttons() {
  for (std::size_t i = 0; i < export_buttons_.size(); ++i)
    if (export_buttons_[i])
      export_buttons_[i]->setText(export_original_[i]
          ? (i ? "复制原截图" : "保存原截图")
          : (i ? "复制分析图" : "保存分析图"));
}

std::string AnalyzerWindow::preferences_json() const {
  QJsonObject object;
  object["version"] = 1;
  object["windowWidth"] = preferred_size_.width();
  object["windowHeight"] = preferred_size_.height();
  // Preserve the existing preference key so previously committed Gain survives.
  object["vectorGain"] = scope_gain_;
  object["saveOriginal"] = export_original_[0];
  object["copyOriginal"] = export_original_[1];
  // Per-image views are deliberately not preferences. Keep the current
  // window/report view, but every new image starts Source and all scopes Fit.
  object["white"] = request_.settings.reference_white_nits;
  object["blur"] = request_.settings.blur_sigma_px;
  object["wave"] = int(request_.scopes.wave_mode);
  object["lastWave"] = int(last_wave_);
  object["lastParade"] = int(last_parade_);
  object["histogram"] = int(request_.scopes.histogram_mode);
  object["vector"] = int(request_.scopes.vector_mode);
  object["colorize"] = request_.scopes.colorize;
  object["waveVisible"] = request_.scopes.waveform_visible;
  object["histogramVisible"] = request_.scopes.histogram_visible;
  object["vectorVisible"] = request_.scopes.vector_visible;
  object["falseColor"] = false_color_;
  object["picker"] = picker_;
  object["maskArmed"] = mask_armed_;
  object["ellipse"] = ellipse_;
  object["sampleSize"] = sample_size_;
  QJsonArray readings, ratios;
  for (bool value : readings_)
    readings.append(value);
  for (double value : ratios_)
    ratios.append(value);
  object["readings"] = readings;
  object["ratios"] = ratios;
  QJsonObject refs;
  for (const auto &[key, bank] : references_) {
    if (bank.empty())
      continue;
    QJsonArray entries;
    for (const auto &ref : bank)
      entries.append(
          QJsonObject{{"value", ref.value}, {"enabled", ref.enabled}});
    refs[key] = entries;
  }
  object["references"] = refs;
  return QJsonDocument(object).toJson(QJsonDocument::Compact).toStdString();
}
void AnalyzerWindow::restore_preferences(const std::string &text) {
  QJsonParseError error;
  const auto doc =
      QJsonDocument::fromJson(QByteArray::fromStdString(text), &error);
  if (error.error != QJsonParseError::NoError || !doc.isObject())
    return;
  const auto o = doc.object();
  if (o.value("version").toInt() != 1)
    return;
  request_.settings.reference_white_nits =
      safe_number(o, "white", 203, 100, 203);
  if (request_.settings.reference_white_nits != 100 &&
      request_.settings.reference_white_nits != 203)
    request_.settings.reference_white_nits = 203;
  request_.settings.blur_sigma_px = safe_number(o, "blur", 0, 0, 8);
  const std::array<double, 6> sigmas{0, .5, 1, 2, 4, 8};
  if (std::find(sigmas.begin(), sigmas.end(),
                request_.settings.blur_sigma_px) == sigmas.end())
    request_.settings.blur_sigma_px = 0;
  request_.scopes.wave_mode =
      static_cast<analysis::WaveMode>(int(safe_number(o, "wave", 0, 0, 3)));
  last_wave_ =
      static_cast<analysis::WaveMode>(int(safe_number(o, "lastWave", 0, 0, 1)));
  last_parade_ = static_cast<analysis::WaveMode>(
      int(safe_number(o, "lastParade", 2, 2, 3)));
  request_.scopes.histogram_mode = static_cast<analysis::HistogramMode>(
      int(safe_number(o, "histogram", 0, 0, 5)));
  request_.scopes.vector_mode =
      static_cast<analysis::VectorMode>(int(safe_number(o, "vector", 1, 0, 1)));
  request_.scopes.colorize = o.value("colorize").toBool(true);
  request_.scopes.waveform_visible = o.value("waveVisible").toBool(true);
  request_.scopes.histogram_visible = o.value("histogramVisible").toBool(true);
  request_.scopes.vector_visible = o.value("vectorVisible").toBool(true);
  false_color_ = o.value("falseColor").toBool(false);
  picker_ = o.value("picker").toBool(false);
  mask_armed_ = o.value("maskArmed").toBool(false);
  if (mask_armed_)
    picker_ = false; // repair older dual-on preferences deterministically
  scope_gain_ = safe_number(o, "vectorGain", 1., .1, 3.);
  export_original_ = {o.value("saveOriginal").toBool(false), o.value("copyOriginal").toBool(false)};
  preferred_size_ = QSize(int(safe_number(o, "windowWidth", 1280, 320, 16384)),
                         int(safe_number(o, "windowHeight", 720, 240, 16384)));
  // Ignore legacy v1 view keys without discarding ordinary options/layout.
  request_.scopes.amplitude_view = {};
  request_.scopes.histogram_view = {};
  request_.scopes.vector_zoom = 1.;
  request_.scopes.vector_pan = {};
  ellipse_ = o.value("ellipse").toBool(false);
  sample_size_ = o.value("sampleSize").toInt(1);
  const std::array<int, 6> sizes{1, 3, 5, 11, 31, 101};
  if (std::find(sizes.begin(), sizes.end(), sample_size_) == sizes.end())
    sample_size_ = 1;
  const auto readings = o.value("readings").toArray();
  if (readings.size() == 5)
    for (int i = 0; i < 5; ++i)
      readings_[std::size_t(i)] = readings[i].toBool(i == 2);
  const auto ratios = o.value("ratios").toArray();
  if (ratios.size() == 4)
    for (int i = 0; i < 4; ++i) {
      const double value = ratios[i].toDouble(ratios_[std::size_t(i)]);
      if (std::isfinite(value) && value >= .01 && value <= .99)
        ratios_[std::size_t(i)] = value;
    }
  const auto refs = o.value("references").toObject();
  for (auto it = refs.begin(); it != refs.end(); ++it) {
    if (it.key().size() > 60 || !it.value().isArray())
      continue;
    for (const auto entry : it.value().toArray()) {
      const auto value = entry.toObject();
      const double n = value.value("value").toDouble(-1);
      if (std::isfinite(n) && n >= 0 && n <= 10000)
        references_[it.key()].push_back(
            {n, value.value("enabled").toBool(true)});
      if (references_[it.key()].size() >= 100)
        break;
    }
  }
}
} // namespace hdrshot
