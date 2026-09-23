#include "domain/analysis/engine.hpp"
#include "domain/color/extended_p3_mapper.hpp"
#include "test_support.hpp"
#include "ui/qt/analyzer_layout.hpp"
#include "ui/qt/analyzer_control_style.hpp"
#include "ui/qt/analyzer_scope_plot.hpp"
#include "ui/qt/analyzer_swatch_panel.hpp"
#include "ui/qt/analyzer_window.hpp"

#include <QApplication>
#include <QAbstractItemView>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCursor>
#include <QGuiApplication>
#include <QDateTime>
#include <QDir>
#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <QJsonDocument>
#include <QJsonObject>
#include <QEventLoop>
#include <QTimer>
#include <QTemporaryFile>
#include <QScreen>
#include <QWindow>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QNativeGestureEvent>
#include <QTouchEvent>
#include <QPainter>
#include <QPointer>
#include <QPointingDevice>
#include <QScrollArea>
#include <QScrollBar>
#include <QToolButton>
#include <QStyleOptionToolButton>
#include <QMenu>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <exception>
#include <iostream>

namespace {
using namespace hdrshot;
class FixtureSource final : public LinearSource {
public:
  PixelSize size_px() const override { return {180, 112}; }
  std::size_t byte_count() const override { return 180 * 112 * 16; }
  Result<bool, Error> wait_until_ready() const override {
    return Result<bool, Error>::success(true);
  }
  Result<LinearFloatPixels, Error> read_region(PixelRect) const override {
    return Result<LinearFloatPixels, Error>::failure(
        analysis::analysis_error("UI fixture has no readback"));
  }
};
void events() {
  for (int i = 0; i < 5; ++i)
    QApplication::processEvents();
}
void wait_events(int milliseconds) {
  QEventLoop loop;
  QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
  loop.exec();
  events();
}
void mouse(QWidget *target, QEvent::Type type, QPointF local,
           Qt::MouseButton button, Qt::MouseButtons buttons) {
  QMouseEvent e(type, local, QPointF(target->mapToGlobal(local.toPoint())),
                button, buttons, Qt::NoModifier);
  QApplication::sendEvent(target, &e);
  events();
}
void click(QWidget *target, QPointF local) {
  const QPoint global = target->mapToGlobal(local.toPoint());
  QPointer<QWidget> alive(target);
  mouse(target, QEvent::MouseButtonPress, local, Qt::LeftButton,
        Qt::LeftButton);
  // A press handler can replace the card (pair creation does this on the next
  // event turn). Never send a release through a stale test pointer.
  target = alive ? alive.data() : QApplication::widgetAt(global);
  if (target)
    mouse(target, QEvent::MouseButtonRelease,
          target->mapFromGlobal(global), Qt::LeftButton, Qt::NoButton);
}
void drag(QWidget *target, QPointF from, QPointF to) {
  mouse(target, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
  mouse(target, QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton);
  mouse(target, QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
}
void key(QWidget *target, int key,
         Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
  QKeyEvent event(QEvent::KeyPress, key, modifiers);
  QApplication::sendEvent(target, &event);
  events();
}
void wheel(QWidget *target, QPointF local, int delta, QPoint pixels = {}) {
  QWheelEvent event(local, QPointF(target->mapToGlobal(local.toPoint())),
                    pixels, QPoint(0, delta), Qt::NoButton, Qt::NoModifier,
                    Qt::NoScrollPhase, false);
  QApplication::sendEvent(target, &event);
  events();
}
template <class T> T *child(AnalyzerWindow &window, const char *name) {
  auto *value = dynamic_cast<T *>(window.findChild<QWidget *>(name));
  HDRSHOT_CHECK(value);
  return value;
}

struct Fixture {
  analysis::FloatImage pixels;
  analysis::Settings cached_settings;
  analysis::FloatImage work;
  int requests{}, work_preparations{};
  analysis::ResultRef last_result;
  AnalyzerWindow window;
  explicit Fixture(bool hdr = true, std::string preferences = {})
      : window({std::make_shared<FixtureSource>(), 1, hdr},
               std::move(preferences)) {
    pixels.resize(180 * 112);
    for (int y = 0; y < 112; ++y)
      for (int x = 0; x < 180; ++x) {
        const float u = float(x) / 179, v = float(y) / 111;
        const int band = std::min(5, x / 30);
        const std::array<std::array<float, 3>, 6> colors{{{1, .1f, .07f},
                                                          {1, .8f, .07f},
                                                          {.1f, 1, .12f},
                                                          {.04f, .7f, 1},
                                                          {.15f, .1f, 1},
                                                          {1, .06f, .7f}}};
        const float level = hdr ? (.005f + 6.f * std::pow(1.f - v, 2.f))
                                : (.005f + .995f * (1 - v));
        auto rgb = colors[std::size_t(band)];
        if (y > 84) {
          rgb = {1, 1, 1};
        }
        for (auto &c : rgb)
          c *= y > 84 ? (hdr ? 6.f : 1.f) * u * u : level;
        pixels[std::size_t(y * 180 + x)] = {rgb[0], rgb[1], rgb[2], 1};
      }
    window.set_request_handler([this](analysis::Request request) {
      ++requests;
      if (work.empty() || cached_settings != request.settings) {
        auto next =
            analysis::prepare_work({180, 112}, pixels, request.settings);
        HDRSHOT_CHECK(next.has_value());
        work = std::move(next.value());
        cached_settings = request.settings;
        ++work_preparations;
      }
      auto result = analysis::analyze_cpu({180, 112}, pixels, work, request);
      HDRSHOT_CHECK(result.has_value());
      last_result =
          std::make_shared<analysis::ResultData>(std::move(result.value()));
      window.accept_result(last_result);
    });
    window.set_close_confirmation([] { return true; });
    window.show();
    events();
    display_fixture();
  }
  ~Fixture() {
    // Failure unwinding must not leave a presenter/request callback referring
    // to a test's already-destroyed local counters while native children hide.
    window.set_present_handler({});
    window.set_request_handler({});
    window.set_preferences_changed({});
    window.hide();
  }
  void display_fixture(bool false_color = false) {
    QImage image(180, 112, QImage::Format_RGB32);
    const bool sdr =
        !analysis::is_hdr(window.current_request().settings.working_space);
    for (int y = 0; y < 112; ++y)
      for (int x = 0; x < 180; ++x) {
        analysis::Rgb rgb{pixels[std::size_t(y * 180 + x)][0],
                          pixels[std::size_t(y * 180 + x)][1],
                          pixels[std::size_t(y * 180 + x)][2]};
        if (sdr)
          for (auto &v : rgb)
            v = std::clamp(v, 0., 1.);
        std::array<float, 3> display;
        if (false_color) {
          const auto readout =
              analysis::make_readout(rgb,
                                     {work[std::size_t(y * 180 + x)][0],
                                      work[std::size_t(y * 180 + x)][1],
                                      work[std::size_t(y * 180 + x)][2]},
                                     1, 0, window.current_request().settings);
          const auto p3 = analysis_math::false_color_rgb(
              float(readout.y_nits /
                    window.current_request().settings.reference_white_nits));
          display = analysis::display_srgb(
              {p3.x, p3.y, p3.z}, analysis::WorkingSpace::display_p3_pq);
        } else
          display = analysis::display_srgb(
              rgb, analysis::WorkingSpace::display_p3_pq);
        image.setPixelColor(
            x, y, QColor::fromRgbF(display[0], display[1], display[2]));
      }
    window.set_fixture_image(image);
    events();
  }
  void add_samples() {
    window.add_swatch({20, 20}, 1);
    window.add_swatch({85, 42}, 31);
    window.add_swatch({150, 85}, 101);
    events();
  }
};

void new_inputs_and_preferences() {
  Fixture first;
  HDRSHOT_CHECK(first.window.current_request().settings.working_space ==
                analysis::WorkingSpace::display_p3_pq);
  child<QComboBox>(first.window, "analyzerReferenceWhite")->setCurrentIndex(1);
  child<QComboBox>(first.window, "analyzerBlur")->setCurrentIndex(1);
  child<QComboBox>(first.window, "analyzerHistogramMode")->setCurrentIndex(2);
  child<QCheckBox>(first.window, "analyzerVisible2")->setChecked(false);
  events();
  auto stored = first.window.preferences_json();
  HDRSHOT_CHECK(stored.find("samples") == std::string::npos &&
                stored.find("pixels") == std::string::npos);
  Fixture next(false, stored);
  HDRSHOT_CHECK(next.window.current_request().settings.working_space ==
                analysis::WorkingSpace::display_p3_sdr);
  HDRSHOT_CHECK(next.window.current_request().settings.reference_white_nits ==
                100);
  HDRSHOT_CHECK(next.window.current_request().settings.blur_sigma_px == .5);
  HDRSHOT_CHECK(!next.window.current_request().scopes.vector_visible);
  HDRSHOT_CHECK(next.window.current_request().samples.empty());
  Fixture malformed(true, "{broken");
  HDRSHOT_CHECK(
      malformed.window.current_request().settings.reference_white_nits == 203);
}

void bounded_layout_and_visibility() {
  Fixture f;
  f.add_samples();
  for (const auto size :
       {QSize(920, 640), QSize(1280, 720), QSize(1440, 900)}) {
    f.window.resize(size);
    events();
    for (int bits = 0; bits < 8; ++bits) {
      for (int i = 0; i < 3; ++i)
        child<QCheckBox>(
            f.window, QString("analyzerVisible%1").arg(i).toUtf8().constData())
            ->setChecked(bits & (1 << i));
      events();
      HDRSHOT_CHECK(f.window.size() == size);
      auto *source = child<QWidget>(f.window, "analyzerSourcePanel");
      auto *amplitude = child<QWidget>(f.window, "analyzerAmplitudeSplit");
      if (bits & 3)
        HDRSHOT_CHECK(source->height() == amplitude->height());
      if ((bits & 3) == 3)
        HDRSHOT_CHECK(
            child<QWidget>(f.window, "analyzerWavePanel")->width() ==
            child<QWidget>(f.window, "analyzerHistogramPanel")->width());
      for (const auto name : {"analyzerSourcePanel", "analyzerWavePanel",
                              "analyzerHistogramPanel", "analyzerVectorPanel",
                              "analyzerSwatchesPanel"}) {
        auto *p = child<QWidget>(f.window, name);
        if (!p->isVisible())
          continue;
        const QRect r(p->mapTo(&f.window, QPoint()), p->size());
        HDRSHOT_CHECK(f.window.rect().contains(r));
        HDRSHOT_CHECK(r.width() > 0 && r.height() > 0);
      }
      HDRSHOT_CHECK(f.window.findChildren<QScrollArea *>().size() == 1);
    }
  }
  f.window.clear_swatches();
  for (int i = 0; i < 3; ++i)
    child<QCheckBox>(f.window,
                     QString("analyzerVisible%1").arg(i).toUtf8().constData())
        ->setChecked(false);
  events();
  for (const auto name : {"analyzerMainSplit", "analyzerTopSplit",
                          "analyzerBottomSplit", "analyzerAmplitudeSplit"})
    HDRSHOT_CHECK(
        !child<AnalyzerSplitPane>(f.window, name)->handle()->isVisible());
}

void independent_splitters_cancel_and_keyboard() {
  Fixture f;
  f.add_samples();
  auto *top = child<AnalyzerSplitPane>(f.window, "analyzerTopSplit");
  auto *bottom = child<AnalyzerSplitPane>(f.window, "analyzerBottomSplit");
  const double before = bottom->ratio();
  key(top->handle(), Qt::Key_Right, Qt::ShiftModifier);
  HDRSHOT_CHECK_NEAR(bottom->ratio(), before, 1e-9);
  const auto start = top->ratio();
  mouse(top->handle(), QEvent::MouseButtonPress, top->handle()->rect().center(),
        Qt::LeftButton, Qt::LeftButton);
  mouse(top->handle(), QEvent::MouseMove, QPointF(65, 20), Qt::NoButton,
        Qt::LeftButton);
  key(top->handle(), Qt::Key_Escape);
  HDRSHOT_CHECK_NEAR(top->ratio(), start, 1e-9);
  key(bottom->handle(), Qt::Key_Home);
  HDRSHOT_CHECK(child<QWidget>(f.window, "analyzerSwatchesPanel")->width() >=
                260);
  key(bottom->handle(), Qt::Key_End);
  HDRSHOT_CHECK(child<QWidget>(f.window, "analyzerVectorPanel")->width() >=
                280);
  HDRSHOT_CHECK(f.window.isVisible());
}

void source_gestures_and_tool_composition() {
  Fixture f;
  auto *source = f.window.source_widget();
  const auto p = source->local_at({70, 50});
  mouse(source, QEvent::MouseMove, p, Qt::NoButton, Qt::NoButton);
  HDRSHOT_CHECK(source->transient_visible());
  click(source, p);
  HDRSHOT_CHECK(f.window.current_request().samples.size() == 1); // hover only
  child<QToolButton>(f.window, "analyzerTool1")->click();
  events();
  click(source, p);
  const auto pinned_request = f.window.current_request();
  HDRSHOT_CHECK(std::count_if(pinned_request.samples.begin(),
                              pinned_request.samples.end(),
                              [](const auto &s) { return s.id != 0; }) == 1);
  child<QToolButton>(f.window, "analyzerTool2")->click();
  events();
  const auto count = f.window.current_request().samples.size();
  drag(source, source->local_at({35, 25}), source->local_at({100, 75}));
  HDRSHOT_CHECK(f.window.current_request().mask.enabled);
  HDRSHOT_CHECK(f.window.current_request().samples.size() == count);
  HDRSHOT_CHECK(!child<QToolButton>(f.window, "analyzerTool1")->isChecked());
  child<QToolButton>(f.window, "analyzerTool0")->click();
  events();
  HDRSHOT_CHECK(!child<QToolButton>(f.window, "analyzerTool1")->isChecked());
  HDRSHOT_CHECK(!child<QToolButton>(f.window, "analyzerTool2")->isChecked());
  const auto mask = f.window.current_request().mask;
  drag(source, p, p + QPointF(20, 10));
  HDRSHOT_CHECK(f.window.current_request().mask == mask);
  HDRSHOT_CHECK(!source->transform().fit);
  const auto anchor = source->source_at(p);
  wheel(source, p, 120);
  HDRSHOT_CHECK_NEAR(source->source_at(p).x(), anchor.x(), 1e-6);
  HDRSHOT_CHECK_NEAR(source->source_at(p).y(), anchor.y(), 1e-6);
  const auto pan = source->transform().pan;
  wheel(source, p, 0, QPoint(12, -8));
  HDRSHOT_CHECK(source->transform().pan == pan + QPointF(12, -8));
}

void sample_sizes_and_fixed_mask_independence() {
  Fixture f;
  child<QToolButton>(f.window, "analyzerTool1")->click();
  auto *samples = child<QComboBox>(f.window, "analyzerSampleSize");
  const std::array<int, 6> sizes{1, 3, 5, 11, 31, 101};
  for (int i = 0; i < 6; ++i) {
    samples->setCurrentIndex(i);
    events();
    click(f.window.source_widget(),
          f.window.source_widget()->local_at({90, 55}));
  }
  events();
  const auto r = f.window.current_request();
  int n = 0;
  for (const auto &s : r.samples)
    if (s.id) {
      HDRSHOT_CHECK(s.side == std::uint32_t(sizes[std::size_t(n++)]));
      HDRSHOT_CHECK(!s.respect_mask);
    }
  HDRSHOT_CHECK(n == 6);
  const auto bounds = f.window.source_widget()->sample_bounds({0, 0}, 101);
  HDRSHOT_CHECK(bounds == QRectF(0, 0, 51, 51));
}

void all_modes_and_colorize() {
  Fixture f;
  auto *kind = child<QComboBox>(f.window, "analyzerWaveKind");
  kind->setCurrentIndex(1);
  events();
  HDRSHOT_CHECK(child<QCheckBox>(f.window, "analyzerColorize")->isHidden());
  child<QComboBox>(f.window, "analyzerWaveMode")->setCurrentIndex(1);
  events();
  HDRSHOT_CHECK(f.window.current_request().scopes.wave_mode ==
                analysis::WaveMode::parade_intensity_rgb);
  kind->setCurrentIndex(0);
  events();
  HDRSHOT_CHECK(child<QCheckBox>(f.window, "analyzerColorize")->isVisible());
  auto *hist = child<QComboBox>(f.window, "analyzerHistogramMode");
  HDRSHOT_CHECK(hist->count() == 6);
  for (int i = 0; i < 6; ++i) {
    hist->setCurrentIndex(i);
    events();
    HDRSHOT_CHECK(
        f.last_result->scopes.histogram_mode ==
        static_cast<analysis::HistogramMode>(hist->itemData(i).toInt()));
  }
  child<QComboBox>(f.window, "analyzerWorkingSpace")->setCurrentIndex(0);
  events();
  HDRSHOT_CHECK(hist->count() == 4);
  auto *vector = child<QComboBox>(f.window, "analyzerVectorMode");
  HDRSHOT_CHECK(vector->itemText(0).contains("Lab"));
  vector->setCurrentIndex(1);
  events();
  HDRSHOT_CHECK(f.last_result->vector_calibration.x_label == "Cb");
}

void references_validate_enter_and_stay_open() {
  Fixture f;
  child<QToolButton>(f.window, "analyzerReferences1")->click();
  events();
  auto *input = child<QLineEdit>(f.window, "analyzerRefValue1");
  auto *popup = qobject_cast<QWidget *>(input->parent());
  input->setText("10001");
  key(input, Qt::Key_Return);
  HDRSHOT_CHECK(f.window.preferences_json().find("10001") == std::string::npos);
  input->setText("203.5");
  key(input, Qt::Key_Return);
  HDRSHOT_CHECK(f.window.preferences_json().find("203.5") != std::string::npos);
  input->setText("1000");
  child<QToolButton>(f.window, "analyzerAddReference1")->click();
  events();
  wheel(f.window.histogram_plot(), QPointF(100, 50), 120);
  HDRSHOT_CHECK(popup->isVisible());
  child<QToolButton>(f.window, "analyzerScopeFit1")->click();
  events();
  HDRSHOT_CHECK(popup->isVisible());
  auto removes = popup->findChildren<QToolButton *>("analyzerDeleteReference");
  HDRSHOT_CHECK(removes.size() == 2);
  for (auto *remove : removes) {
    HDRSHOT_CHECK(remove->height() >= 28);
    HDRSHOT_CHECK(remove->parentWidget()->rect().contains(remove->geometry()));
    const QRect bounds(remove->mapTo(popup, QPoint()), remove->size());
    if (!popup->rect().contains(bounds))
      std::cerr << "REFERENCE_GEOMETRY popup=" << popup->width() << 'x' << popup->height()
                << " button=" << bounds.x() << ',' << bounds.y() << ',' << bounds.width() << ',' << bounds.height() << '\n';
    HDRSHOT_CHECK(popup->rect().contains(bounds));
  }
  const auto screenshot_dir = qEnvironmentVariable("HDRSHOT_ANALYZER_SCREENSHOT_DIR");
  if (!screenshot_dir.isEmpty()) {
    QDir().mkpath(screenshot_dir);
    const auto path = screenshot_dir + "/" +
        QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss-zzz") +
        "_reference_two_rows.png";
    HDRSHOT_CHECK(popup->grab().save(path));
    std::cout << "[SCREENSHOT] " << path.toStdString() << '\n';
  }
  removes.front()->click();
  events();
  HDRSHOT_CHECK(popup->isVisible());
  key(input, Qt::Key_Escape);
  HDRSHOT_CHECK(!popup->isVisible());
  HDRSHOT_CHECK(f.window.isVisible());
}

void report_keeps_visible_geometry_and_source_hole() {
  Fixture f;
  for (int i = 0; i < 12; ++i)
    f.window.add_swatch({double(10 + i * 10), 40}, i % 2 ? 31 : 1);
  events();
  auto *source = f.window.source_widget();
  source->zoom_at(source->rect().center(), 2);
  const auto before = source->transform();
  auto *scroll = child<QScrollArea>(f.window, "analyzerSwatchScroll");
  scroll->verticalScrollBar()->setValue(85);
  events();
  const int offset = scroll->verticalScrollBar()->value();
  const QSize size = source->size();
  const auto plan = f.window.report_plan();
  HDRSHOT_CHECK(plan.source_view.scale ==
                source->effective_scale() * source->devicePixelRatioF());
  HDRSHOT_CHECK(source->size() == size);
  HDRSHOT_CHECK(source->transform().pan == before.pan);
  HDRSHOT_CHECK(source->transform().scale == before.scale);
  HDRSHOT_CHECK(scroll->verticalScrollBar()->value() == offset);
  const auto hole =
      std::size_t(plan.source_rect.y + plan.source_rect.height / 2) *
          std::size_t(plan.underlay.size.width) +
      std::size_t(plan.source_rect.x + plan.source_rect.width / 2);
  HDRSHOT_CHECK(plan.underlay.rgba[hole * 4 + 3] == 0);
  HDRSHOT_CHECK(plan.overlay.rgba.size() == plan.underlay.rgba.size());
  HDRSHOT_CHECK(std::any_of(plan.overlay.rgba.begin(), plan.overlay.rgba.end(),
                            [](std::uint8_t c) { return c != 0; }));
}

void stale_results_and_close_confirmation() {
  Fixture f;
  const auto original = f.last_result;
  child<QComboBox>(f.window, "analyzerReferenceWhite")->setCurrentIndex(1);
  events();
  const auto latest = f.last_result;
  f.window.accept_result(original);
  HDRSHOT_CHECK(f.window.current_request().revision == latest->revision);
  f.window.accept_error(original->revision, "迟到错误");
  HDRSHOT_CHECK(!child<QLabel>(f.window, "analyzerStatus")->isVisible());
  f.window.accept_error(latest->revision, "当前计算错误");
  HDRSHOT_CHECK(child<QLabel>(f.window, "analyzerStatus")->isVisible());
  child<QComboBox>(f.window, "analyzerBlur")->setCurrentIndex(1);
  events();
  HDRSHOT_CHECK(!child<QLabel>(f.window, "analyzerStatus")->isVisible());
  f.window.set_close_confirmation([] { return false; });
  HDRSHOT_CHECK(!f.window.close());
  HDRSHOT_CHECK(f.window.isVisible());
  Fixture another;
  f.window.set_close_confirmation([] { return true; });
  HDRSHOT_CHECK(f.window.close());
  HDRSHOT_CHECK(another.window.isVisible());
}

void close_dialog_stays_dark_with_light_and_dark_application_palettes() {
  struct RestorePalette {
    QPalette saved = QApplication::palette();
    ~RestorePalette() { QApplication::setPalette(saved); }
  } restore;
  for (bool dark : {false, true}) {
    auto palette = restore.saved;
    palette.setColor(QPalette::Window, dark ? QColor("#181818") : QColor("#fafafa"));
    palette.setColor(QPalette::WindowText, dark ? Qt::white : Qt::black);
    palette.setColor(QPalette::Button, dark ? QColor("#303030") : QColor("#eeeeee"));
    palette.setColor(QPalette::ButtonText, dark ? Qt::white : Qt::black);
    QApplication::setPalette(palette);
    // Escape/Continue retain analysis; Enter and explicit End close it.
    for (int action = 0; action < 4; ++action) {
      Fixture f;
      f.window.set_close_confirmation({});
      std::exception_ptr failure;
      bool visited = false;
      QTimer::singleShot(0, &f.window, [&] {
        auto *box = f.window.findChild<QMessageBox *>("analyzerCloseConfirmation");
        try {
          HDRSHOT_CHECK(box);
          visited = true;
          HDRSHOT_CHECK(box->testOption(QMessageBox::Option::DontUseNativeDialog));
          HDRSHOT_CHECK(box->defaultButton()->text() == QStringLiteral("结束分析 · Enter"));
          HDRSHOT_CHECK(box->escapeButton()->text() == QStringLiteral("继续分析 · Esc"));
          HDRSHOT_CHECK(box->escapeButton() != box->defaultButton());
          auto check_dark = [&] {
            const auto image = box->grab().toImage();
            HDRSHOT_CHECK(image.pixelColor(2, 2) == QColor("#202124"));
            auto *label = box->findChild<QLabel *>("qt_msgbox_label");
            HDRSHOT_CHECK(label);
            HDRSHOT_CHECK(label->palette().color(QPalette::WindowText) == QColor("#e9ebef"));
            QPushButton *end = nullptr;
            for (auto *button : box->buttons()) {
              const bool destructive = box->buttonRole(button) == QMessageBox::DestructiveRole;
              HDRSHOT_CHECK(button->palette().color(QPalette::ButtonText) == QColor(destructive ? analyzer_control_style::danger_foreground : "#e9ebef"));
              HDRSHOT_CHECK(button->palette().color(QPalette::Button) == QColor(destructive ? analyzer_control_style::danger_normal : "#30343b"));
              if (destructive) end = qobject_cast<QPushButton *>(button);
            }
            HDRSHOT_CHECK(end && end == box->defaultButton());
            auto *keep = box->escapeButton();
            HDRSHOT_CHECK(end != keep);
            const auto end_rect = QRect(end->mapTo(box, QPoint()), end->size());
            const auto keep_rect = QRect(keep->mapTo(box, QPoint()), keep->size());
            const int gap = std::max(end_rect.left() - keep_rect.right() - 1,
                                     keep_rect.left() - end_rect.right() - 1);
            HDRSHOT_CHECK(gap >= 16);
            HDRSHOT_CHECK(end_rect.size() == keep_rect.size());
            HDRSHOT_CHECK(end_rect.width() >= 184);
            HDRSHOT_CHECK(end_rect.left() < keep_rect.left());
          };
          check_dark();
          if (action == 0) {
            const auto folder = qEnvironmentVariable("HDRSHOT_ANALYZER_SCREENSHOT_DIR");
            if (!folder.isEmpty()) {
              HDRSHOT_CHECK(QDir().mkpath(folder));
              HDRSHOT_CHECK(box->grab().save(folder + (dark ? "/close_dialog_dark.png" : "/close_dialog_light.png")));
            }
            // A system/application palette change must not undo the fixed theme.
            auto changed = palette;
            changed.setColor(QPalette::Window, dark ? Qt::white : Qt::black);
            changed.setColor(QPalette::WindowText, dark ? Qt::black : Qt::white);
            QApplication::setPalette(changed);
            events();
            check_dark();
            QWidget ordinary_window;
            HDRSHOT_CHECK(ordinary_window.palette().color(QPalette::Window) == changed.color(QPalette::Window));
            key(box, Qt::Key_Escape);
          } else if (action == 1) {
            box->escapeButton()->click();
          } else if (action == 2) {
            // Focusing Continue must not turn it into an implicit Enter default.
            box->escapeButton()->setFocus();
            key(box, Qt::Key_Return);
          } else {
            for (auto *button : box->buttons())
              if (box->buttonRole(button) == QMessageBox::DestructiveRole)
                button->click();
          }
        } catch (...) {
          failure = std::current_exception();
          if (auto *modal = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
            modal->reject();
        }
      });
      const bool closed = f.window.close();
      if (failure) std::rethrow_exception(failure);
      HDRSHOT_CHECK(visited);
      HDRSHOT_CHECK(closed == (action >= 2));
      HDRSHOT_CHECK(f.window.isVisible() == (action < 2));
    }
  }
}

void gamut_report_matches_visible_scope() {
  Fixture f;
  for (int space = 0; space < 4; ++space) {
    child<QComboBox>(f.window, "analyzerWorkingSpace")->setCurrentIndex(space);
    events();
    auto *plot = f.window.vectorscope_plot();
    plot->fit();
    wheel(plot, plot->rect().center(), 120);
    const auto view = f.window.current_request().scopes.vector_zoom;
    const QPoint origin = plot->mapTo(&f.window, QPoint());
    const double dpr = plot->devicePixelRatioF();
    // Use the same RGBA raster target and parent-window origin as export.
    // QPixmap grab uses different font AA; comparing it bit-for-bit would
    // test that raster backend difference rather than gamut/view preservation.
    const QRect pixels(int(std::lround(origin.x() * dpr)), int(std::lround(origin.y() * dpr)),
                       int(std::lround(plot->width() * dpr)), int(std::lround(plot->height() * dpr)));
    QImage surface(int(std::lround(f.window.width() * dpr)),
                   int(std::lround(f.window.height() * dpr)), QImage::Format_RGBA8888);
    surface.setDevicePixelRatio(dpr);
    surface.fill(Qt::transparent);
    { QPainter painter(&surface); f.window.render(&painter); }
    const auto visible = surface.copy(pixels);
    const auto plan = f.window.report_plan();
    QImage report(plan.underlay.rgba.data(), plan.underlay.size.width,
                  plan.underlay.size.height, QImage::Format_RGBA8888);
    auto crop = report.copy(pixels);
    crop.setDevicePixelRatio(visible.devicePixelRatio());
    if (crop != visible) {
      QRect changed;
      int pixels = 0;
      for (int y = 0; y < crop.height(); ++y)
        for (int x = 0; x < crop.width(); ++x)
          if (crop.pixel(x, y) != visible.pixel(x, y)) {
            changed = changed.united(QRect(x, y, 1, 1));
            ++pixels;
          }
      std::cout << "report diff space=" << space << " pixels=" << pixels << " bounds="
                << changed.x() << ',' << changed.y() << ',' << changed.width() << ',' << changed.height() << '\n';
      const auto dir = qEnvironmentVariable("HDRSHOT_ANALYZER_SCREENSHOT_DIR");
      if (!dir.isEmpty()) {
        crop.save(dir + "/report_difference.png");
        visible.save(dir + "/window_difference.png");
      }
    }
    HDRSHOT_CHECK(crop == visible);
    HDRSHOT_CHECK(f.window.current_request().scopes.vector_zoom == view);
  }
}

void pending_controls_cannot_export_mixed_report() {
  Fixture f;
  int exported = 0;
  f.window.set_export_handler(
      [&](analysis::ExportAction, analysis::ReportPlan) { ++exported; });
  child<QComboBox>(f.window, "analyzerBlur")->setCurrentIndex(1);
  // Deliberately do not drain the event queue between edit and export.
  child<QToolButton>(f.window, "analyzerSave")->click();
  HDRSHOT_CHECK(exported == 0);
  events();
  QEventLoop refinement;
  QTimer::singleShot(220, &refinement, &QEventLoop::quit);
  refinement.exec();
  child<QToolButton>(f.window, "analyzerSave")->click();
  HDRSHOT_CHECK(exported == 1);
}

void marker_states_are_not_implicit_full_image_means() {
  Fixture f;
  f.add_samples();
  auto *plot = f.window.vectorscope_plot();
  const auto baseline = plot->grab().toImage();
  HDRSHOT_CHECK(f.last_result->mask_mean.valid_count > 0);
  auto no_mean = std::make_shared<analysis::ResultData>(*f.last_result);
  no_mean->mask_mean.valid_count = 0;
  plot->set_result(no_mean, false);
  HDRSHOT_CHECK(plot->grab().toImage() == baseline);
  plot->set_result(f.last_result, false);
  plot->set_mask_enabled(true);
  HDRSHOT_CHECK(plot->grab().toImage() != baseline);
  plot->set_mask_enabled(false);
  auto hover = std::make_shared<analysis::ResultData>(*f.last_result);
  HDRSHOT_CHECK(!hover->samples.empty());
  hover->samples.front().request.id = 0;
  plot->set_transient_hidden(false);
  plot->set_result(hover, false);
  HDRSHOT_CHECK(plot->grab().toImage() != baseline);
  plot->set_transient_hidden(true);
  HDRSHOT_CHECK(plot->grab().toImage() == baseline);
}

void pending_settings_do_not_relabel_old_results() {
  Fixture f;
  f.add_samples();
  analysis::Request pending;
  f.window.set_request_handler(
      [&](analysis::Request value) { pending = value; });
  events();
  HDRSHOT_CHECK(!f.window.vectorscope_plot()->is_pending());
  child<QToolButton>(f.window, "analyzerTool1")->click();
  mouse(f.window.source_widget(), QEvent::MouseMove,
        f.window.source_widget()->local_at({60, 40}), Qt::NoButton,
        Qt::NoButton);
  // Hover only does not invalidate already matched statistics.
  HDRSHOT_CHECK(!f.window.vectorscope_plot()->is_pending());
  child<QComboBox>(f.window, "analyzerWorkingSpace")->setCurrentIndex(1);
  HDRSHOT_CHECK(f.window.vectorscope_plot()->is_pending());
  HDRSHOT_CHECK(f.window.waveform_plot()->is_pending());
  auto *swatch = child<QWidget>(f.window, "analyzerSwatch1");
  bool placeholder = false;
  for (auto *label : swatch->findChildren<QLabel *>()) {
    placeholder = placeholder || label->text().contains("—");
    HDRSHOT_CHECK(!label->text().contains("Y · nit"));
  }
  HDRSHOT_CHECK(placeholder);
  events();
  f.window.accept_result(f.last_result);
  HDRSHOT_CHECK(f.window.vectorscope_plot()->is_pending());
  auto work = analysis::prepare_work({180, 112}, f.pixels, pending.settings);
  HDRSHOT_CHECK(work.has_value());
  auto next =
      analysis::analyze_cpu({180, 112}, f.pixels, work.value(), pending);
  HDRSHOT_CHECK(next.has_value());
  f.window.accept_result(
      std::make_shared<analysis::ResultData>(std::move(next.value())));
  events();
  HDRSHOT_CHECK(!f.window.vectorscope_plot()->is_pending());
  child<QComboBox>(f.window, "analyzerBlur")->setCurrentIndex(1);
  HDRSHOT_CHECK(f.window.vectorscope_plot()->is_pending());
}

void tools_are_exclusive_and_repeat_is_noop() {
  Fixture f(true, R"({"version":1,"picker":true,"maskArmed":true})");
  for (int tool : {2, 1, 0, 1, 2}) {
    auto *button = child<QToolButton>(f.window, ("analyzerTool" + QString::number(tool)).toUtf8().constData());
    button->click();
    events();
    const int requests = f.requests;
    for (int repeat = 0; repeat < 3; ++repeat)
      button->click();
    events();
    HDRSHOT_CHECK(f.requests == requests);
    for (int other = 0; other < 3; ++other)
      HDRSHOT_CHECK(child<QToolButton>(f.window, ("analyzerTool" + QString::number(other)).toUtf8().constData())->isChecked() == (other == tool));
  }
  HDRSHOT_CHECK(child<QWidget>(f.window, "analyzerToolSeparator0")->isVisible());
  HDRSHOT_CHECK(child<QWidget>(f.window, "analyzerToolSeparator1")->isVisible());
}

void picker_options_activate_tool_and_hue_names() {
  Fixture f;
  auto *picker = child<QToolButton>(f.window, "analyzerTool1");
  auto *mask = child<QToolButton>(f.window, "analyzerTool2");
  auto *size = child<QComboBox>(f.window, "analyzerSampleSize");
  mask->click();
  size->setCurrentIndex((size->currentIndex()+1)%size->count());
  HDRSHOT_CHECK(picker->isChecked() && !mask->isChecked());
  mask->click();
  child<QCheckBox>(f.window, "analyzerReading0")->click();
  HDRSHOT_CHECK(picker->isChecked() && !mask->isChecked());
  auto *hist = child<QComboBox>(f.window, "analyzerHistogramMode");
  HDRSHOT_CHECK(hist->itemText(hist->findData(int(analysis::HistogramMode::hue))) == "Hue TP");
  const double before = f.window.histogram_plot()->plot_rect().height();
  hist->setCurrentIndex(hist->findData(int(analysis::HistogramMode::hue)));
  HDRSHOT_CHECK(f.window.histogram_plot()->plot_rect().height() >= before + 17.);
  child<QComboBox>(f.window, "analyzerWorkingSpace")->setCurrentIndex(1);
  HDRSHOT_CHECK(hist->currentText() == "Hue a*b*");
}
void lagged_hover_keeps_completed_markers_until_replaced() {
  Fixture f;
  child<QComboBox>(f.window, "analyzerSampleSize")->setCurrentIndex(5);
  events(); // Exercise the largest region, not only its 1x1 mean marker.
  analysis::Request pending;
  f.window.set_request_handler([&](auto r) { pending = r; });
  auto *source = f.window.source_widget();
  auto *readout = child<QLabel>(f.window, "analyzerHoverReadout");
  const auto complete = [&](const analysis::Request &r) {
    auto next = analysis::analyze_cpu({180, 112}, f.pixels, f.work, r);
    HDRSHOT_CHECK(next.has_value());
    f.window.accept_result(std::make_shared<analysis::ResultData>(std::move(next.value())));
    events();
  };
  const auto images = [&] {
    return std::array<QImage, 3>{f.window.waveform_plot()->grab().toImage(),
        f.window.histogram_plot()->grab().toImage(), f.window.vectorscope_plot()->grab().toImage()};
  };
  source->hover_changed(QPointF(48, 38)); events(); complete(pending);
  auto held = images();
  auto held_text = readout->text();
  HDRSHOT_CHECK(!held_text.isEmpty() && held_text != "—");
  // Deliberately one completed result behind a moving cursor. No frame may
  // clear markers/values just because it is not at the newest requested point.
  for (int i = 0; i < 12; ++i) {
    source->hover_changed(QPointF(52+i*2, 40+i)); events();
    HDRSHOT_CHECK(images() == held);
    HDRSHOT_CHECK(readout->text() == held_text);
    const auto older = pending;
    source->hover_changed(QPointF(53+i*2, 41+i)); events();
    HDRSHOT_CHECK(images() == held);
    complete(older);
    HDRSHOT_CHECK(!f.window.waveform_plot()->is_pending());
    HDRSHOT_CHECK(readout->text() != "—");
    const auto next = images();
    HDRSHOT_CHECK(next != held);
    held = next; held_text = readout->text();
  }
  // Leaving still clears transient content; delayed completion cannot restore it.
  const auto late = pending;
  source->hover_changed(std::nullopt); events();
  const auto cleared = images();
  HDRSHOT_CHECK(cleared != held && readout->text().isEmpty());
  complete(late);
  HDRSHOT_CHECK(images() == cleared && readout->text().isEmpty());
}
void vector_pan_defers_detail_and_report_checks_center() {
  Fixture f;
  auto *plot = f.window.vectorscope_plot();
  auto options = f.window.current_request().scopes;
  options.vector_zoom = 10.;
  plot->set_options(options); plot->options_changed(options);
  QEventLoop settle; QTimer::singleShot(210, &settle, &QEventLoop::quit); settle.exec();
  events();
  const auto prior = f.window.current_request();
  analysis::Request pending;
  f.window.set_request_handler([&](auto r) { pending = r; });
  const QPointF center = plot->plot_rect().center();
  wheel(plot, center, 0, {50, 20});
  HDRSHOT_CHECK(f.window.current_request().scopes.vector_pan != prior.scopes.vector_pan);
  HDRSHOT_CHECK(analysis::same_statistics_request(prior, f.window.current_request()));
  HDRSHOT_CHECK(!plot->is_pending());
  QTimer::singleShot(210, &settle, &QEventLoop::quit); settle.exec(); events();
  HDRSHOT_CHECK(!analysis::same_vector_detail_request(prior, pending));
  HDRSHOT_CHECK(pending.scopes.vector_detail_width == prior.scopes.vector_detail_width);
  auto next = analysis::analyze_cpu({180,112}, f.pixels, f.work, pending);
  HDRSHOT_CHECK(next.has_value());
  auto stale = std::make_shared<analysis::ResultData>(next.value());
  stale->vector_detail_center = {};
  f.window.accept_result(stale);
  int exports = 0;
  f.window.set_export_handler([&](auto, auto) { ++exports; });
  child<QToolButton>(f.window, "analyzerSave")->click(); events();
  HDRSHOT_CHECK(exports == 0);
  f.window.accept_result(std::make_shared<analysis::ResultData>(std::move(next.value())));
  events();
  child<QToolButton>(f.window, "analyzerSave")->click(); events();
  HDRSHOT_CHECK(exports == 1);
  plot->fit();
  HDRSHOT_CHECK((f.window.current_request().scopes.vector_pan == std::array<double,2>{}));
}
void native_surface_hover_and_interrupted_gesture_recover() {
  Fixture f;
  auto *source = f.window.source_widget();
  auto *surface = source->presentation_surface();
  HDRSHOT_CHECK(surface->hasMouseTracking());
  const auto p = source->local_at({70, 50});
  const auto phased_wheel = [&](Qt::ScrollPhase phase, QPoint angle = {}) {
    QWheelEvent event(p, surface->mapToGlobal(p.toPoint()), {}, angle,
                      Qt::NoButton, Qt::NoModifier, phase, false);
    QApplication::sendEvent(surface, &event);
  };
  phased_wheel(Qt::ScrollBegin);
  phased_wheel(Qt::ScrollEnd);
  HDRSHOT_CHECK(source->transform().fit);
  const double original_scale = source->effective_scale();
  phased_wheel(Qt::ScrollBegin);
  phased_wheel(Qt::ScrollUpdate, {0, 80});
  phased_wheel(Qt::ScrollEnd);
  HDRSHOT_CHECK_NEAR(source->effective_scale(), original_scale, 1e-9);
  HDRSHOT_CHECK(source->transform().pan == QPointF(0, 10));
  source->fit();
  mouse(surface, QEvent::MouseMove, p, Qt::NoButton, Qt::NoButton);
  HDRSHOT_CHECK(child<QLabel>(f.window, "analyzerHoverReadout")->text().contains("Y · nit"));
  child<QToolButton>(f.window, "analyzerTool1")->click();
  QNativeGestureEvent begin(Qt::BeginNativeGesture, QPointingDevice::primaryPointingDevice(), 2,
                            p, p, source->mapToGlobal(p.toPoint()), 0, {});
  QApplication::sendEvent(surface, &begin);
  // A synthesized move during the active gesture must keep the picker hidden.
  const QPointF next = source->local_at({105, 65});
  QMouseEvent synthesized(QEvent::MouseMove, next, next,
                          QPointF(surface->mapToGlobal(next.toPoint())),
                          Qt::NoButton, Qt::NoButton, Qt::NoModifier,
                          Qt::MouseEventSynthesizedByQt);
  QApplication::sendEvent(surface, &synthesized);
  HDRSHOT_CHECK(!source->transient_visible());
  // A missing native End must recover on the next genuine no-button move,
  // without requiring the click that used to restore hover.
  const auto fixed_count = [&] {
    const auto &samples = f.window.current_request().samples;
    return std::count_if(samples.begin(), samples.end(),
                         [](const auto &sample) { return sample.id != 0; });
  };
  const auto before_recovery = fixed_count();
  mouse(surface, QEvent::MouseMove, next, Qt::NoButton, Qt::NoButton);
  HDRSHOT_CHECK(source->transient_visible());
  HDRSHOT_CHECK(fixed_count() == before_recovery);
  QEventLoop recovery;
  QTimer::singleShot(30, &recovery, &QEventLoop::quit);
  recovery.exec();
  const auto &recovered_samples = f.window.current_request().samples;
  const QPointF recovered_source = source->source_at(next);
  HDRSHOT_CHECK(std::any_of(recovered_samples.begin(), recovered_samples.end(),
                            [&](const auto &sample) {
                              return sample.id == 0 &&
                                     sample.x == int(std::floor(recovered_source.x())) &&
                                     sample.y == int(std::floor(recovered_source.y()));
                            }));
  HDRSHOT_CHECK(child<QLabel>(f.window, "analyzerHoverReadout")->text().contains("Y · nit"));
  // Wheel streams can lose ScrollEnd by the same route.
  phased_wheel(Qt::ScrollBegin);
  HDRSHOT_CHECK(!source->transient_visible());
  mouse(surface, QEvent::MouseMove, p, Qt::NoButton, Qt::NoButton);
  HDRSHOT_CHECK(source->transient_visible());
  // A real two-finger touch stream remains active through mouse moves until
  // Qt delivers TouchEnd, even if its preceding native Begin lost End.
  QApplication::sendEvent(surface, &begin);
  const QList<QEventPoint> contacts{
      {1, QEventPoint::State::Pressed, p, surface->mapToGlobal(p.toPoint())},
      {2, QEventPoint::State::Pressed, p + QPointF(20, 0),
       surface->mapToGlobal((p + QPointF(20, 0)).toPoint())}};
  QTouchEvent touch_begin(QEvent::TouchBegin,
                          QPointingDevice::primaryPointingDevice(),
                          Qt::NoModifier, contacts);
  QApplication::sendEvent(source, &touch_begin);
  mouse(surface, QEvent::MouseMove, next, Qt::NoButton, Qt::NoButton);
  HDRSHOT_CHECK(!source->transient_visible());
  const QList<QEventPoint> released{
      {1, QEventPoint::State::Released, p, surface->mapToGlobal(p.toPoint())},
      {2, QEventPoint::State::Released, p + QPointF(20, 0),
       surface->mapToGlobal((p + QPointF(20, 0)).toPoint())}};
  QTouchEvent touch_end(QEvent::TouchEnd,
                        QPointingDevice::primaryPointingDevice(),
                        Qt::NoModifier, released);
  QApplication::sendEvent(source, &touch_end);
  // Directly injected QEventPoint positions default to (0,0) in the offscreen
  // plugin, so the End itself may leave hover empty; the next ordinary move
  // proves the touch gate has been released.
  mouse(surface, QEvent::MouseMove, next, Qt::NoButton, Qt::NoButton);
  HDRSHOT_CHECK(source->transient_visible());
  // A click after recovery still adds exactly one pin.
  click(surface, p);
  const auto request = f.window.current_request();
  HDRSHOT_CHECK(std::count_if(request.samples.begin(), request.samples.end(), [](const auto &s) { return s.id != 0; }) == 1);
  child<QToolButton>(f.window, "analyzerTool2")->click();
  const QSize size = source->size();
  const QRectF image = source->image_rect();
  const auto from = source->local_at({35, 25});
  mouse(surface, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
  for (int i = 0; i < 6; ++i) {
    mouse(source, QEvent::MouseMove, from + QPointF(10 + i * 5, 10 + i * 3), Qt::NoButton, Qt::LeftButton);
    HDRSHOT_CHECK(source->size() == size);
    HDRSHOT_CHECK(source->image_rect() == image);
  }
  mouse(source, QEvent::MouseButtonRelease, from + QPointF(35, 25), Qt::LeftButton, Qt::NoButton);
  HDRSHOT_CHECK(f.window.current_request().mask.enabled);
}

void scope_gain_commits_only_enter_and_never_analyzes() {
  Fixture f;
  auto *gain = child<QLineEdit>(f.window, "analyzerVectorGain");
  const int requests = f.requests;
  gain->setText("2.25");
  events();
  HDRSHOT_CHECK_NEAR(f.window.vectorscope_plot()->display_gain(), 1., 1e-9);
  HDRSHOT_CHECK_NEAR(f.window.waveform_plot()->display_gain(), 1., 1e-9);
  child<QComboBox>(f.window, "analyzerBlur")->setFocus();
  events();
  HDRSHOT_CHECK_NEAR(f.window.vectorscope_plot()->display_gain(), 1., 1e-9);
  key(gain, Qt::Key_Return);
  HDRSHOT_CHECK_NEAR(f.window.vectorscope_plot()->display_gain(), 2.25, 1e-9);
  HDRSHOT_CHECK_NEAR(f.window.waveform_plot()->display_gain(), 2.25, 1e-9);
  HDRSHOT_CHECK_NEAR(f.window.histogram_plot()->display_gain(), 1., 1e-9);
  HDRSHOT_CHECK(f.requests == requests);
  for (const auto text : {"0", "3.001", "not a number", "nan"}) {
    gain->setText(text);
    key(gain, Qt::Key_Return);
    HDRSHOT_CHECK_NEAR(f.window.vectorscope_plot()->display_gain(), 2.25, 1e-9);
    HDRSHOT_CHECK_NEAR(f.window.waveform_plot()->display_gain(), 2.25, 1e-9);
  }
  gain->setText("0.1");
  key(gain, Qt::Key_Return);
  HDRSHOT_CHECK_NEAR(f.window.vectorscope_plot()->display_gain(), .1, 1e-9);
  gain->setText("3");
  key(gain, Qt::Key_Return);
  gain->setText("1.4"); // draft never becomes the exported or persisted value
  const auto plan = f.window.report_plan();
  HDRSHOT_CHECK(!plan.underlay.rgba.empty());
  HDRSHOT_CHECK(gain->text() == "1.4");
  Fixture restored(true, f.window.preferences_json());
  HDRSHOT_CHECK_NEAR(restored.window.vectorscope_plot()->display_gain(), 3., 1e-9);
  HDRSHOT_CHECK_NEAR(restored.window.waveform_plot()->display_gain(), 3., 1e-9);
  HDRSHOT_CHECK_NEAR(restored.window.histogram_plot()->display_gain(), 1., 1e-9);
  HDRSHOT_CHECK(f.requests == requests);
}

void preferences_reset_views_without_pixels_or_stale_close_write() {
  Fixture f;
  f.window.resize(1150, 740);
  wheel(f.window.waveform_plot(), {100, 80}, 120);
  wheel(f.window.histogram_plot(), {100, 80}, 120);
  wheel(f.window.vectorscope_plot(), {100, 80}, 120);
  events();
  const auto before = f.window.current_request();
  auto stored = f.window.preferences_json();
  auto json = QJsonDocument::fromJson(QByteArray::fromStdString(stored)).object();
  HDRSHOT_CHECK(json.value("windowWidth").toInt() == 1150);
  HDRSHOT_CHECK(json.value("windowHeight").toInt() == 740);
  for (const auto *key : {"waveZoom", "wavePan", "histogramZoom", "histogramPan", "vectorZoom"})
    HDRSHOT_CHECK(!json.contains(key));
  // Simulate old persisted settings. New windows ignore only image-view keys.
  json["waveZoom"] = 8.; json["wavePan"] = .4;
  json["histogramZoom"] = 16.; json["histogramPan"] = .3;
  json["vectorZoom"] = 4.;
  json["blur"] = .5;
  f.window.add_swatch({40, 20}, 31);
  f.window.source_widget()->zoom_at({100, 80}, 2);
  Fixture restored(false, QJsonDocument(json).toJson().toStdString());
  HDRSHOT_CHECK(restored.window.source_widget()->transform().fit);
  HDRSHOT_CHECK(restored.window.current_request().samples.empty());
  HDRSHOT_CHECK(!restored.window.current_request().mask.enabled);
  HDRSHOT_CHECK(restored.window.current_request().scopes.amplitude_view == analysis::AxisView{});
  HDRSHOT_CHECK(restored.window.current_request().scopes.histogram_view == analysis::AxisView{});
  HDRSHOT_CHECK_NEAR(restored.window.current_request().scopes.vector_zoom, 1., 1e-9);
  HDRSHOT_CHECK_NEAR(restored.window.current_request().settings.blur_sigma_px, .5, 1e-9);
  HDRSHOT_CHECK(f.window.current_request().scopes.amplitude_view == before.scopes.amplitude_view);
  HDRSHOT_CHECK(f.window.current_request().scopes.histogram_view == before.scopes.histogram_view);
  HDRSHOT_CHECK_NEAR(f.window.current_request().scopes.vector_zoom, before.scopes.vector_zoom, 1e-9);
  Fixture next(true, restored.window.preferences_json());
  HDRSHOT_CHECK(next.window.current_request().scopes.amplitude_view == analysis::AxisView{});
  HDRSHOT_CHECK(next.window.current_request().scopes.histogram_view == analysis::AxisView{});
  HDRSHOT_CHECK(next.window.current_request().scopes.vector_zoom == 1.);
  int writes = 0;
  f.window.set_preferences_changed([&](std::string) { ++writes; });
  // Closing an older window must not replace another window's newer options.
  f.window.set_close_confirmation([] { return true; });
  f.window.close();
  HDRSHOT_CHECK(writes == 0);
}

void view_changes_coalesce_present_and_defer_refinement() {
  Fixture f;
  int presents = 0;
  f.window.set_present_handler([&](auto, auto) { ++presents; });
  const int requests = f.requests;
  for (int i = 0; i < 80; ++i)
    f.window.source_widget()->zoom_at({100, 80}, 1.001);
  HDRSHOT_CHECK(presents == 0);
  HDRSHOT_CHECK(f.requests == requests);
  QEventLoop loop;
  QTimer::singleShot(40, &loop, &QEventLoop::quit);
  loop.exec();
  HDRSHOT_CHECK(presents >= 1 && presents <= 2);
  int dispatched = 0;
  f.window.set_request_handler([&](auto) { ++dispatched; });
  events();
  const int baseline = dispatched;
  wheel(f.window.waveform_plot(), {100, 80}, 120);
  wheel(f.window.histogram_plot(), {100, 80}, 120);
  wheel(f.window.vectorscope_plot(), {100, 80}, 120);
  HDRSHOT_CHECK(dispatched == baseline);
  HDRSHOT_CHECK(!f.window.waveform_plot()->is_pending());
  HDRSHOT_CHECK(!f.window.vectorscope_plot()->is_pending());
  const auto current = f.window.current_request();
  QTimer::singleShot(220, &loop, &QEventLoop::quit);
  loop.exec();
  HDRSHOT_CHECK(dispatched <= baseline + 1);
  HDRSHOT_CHECK(f.window.current_request().scopes.wave_height >= current.scopes.wave_height);
  f.window.set_present_handler({});
}

void native_pan_pinch_and_cancel_preserve_source_coordinates() {
  Fixture f;
  f.add_samples();
  auto *source = f.window.source_widget();
  const QPointF local = source->local_at({70, 50});
  const auto gesture = [&](Qt::NativeGestureType type, double value = 0.,
                           QPointF delta = {}) {
    QNativeGestureEvent event(
        type, QPointingDevice::primaryPointingDevice(), 2, local, local,
        source->mapToGlobal(local.toPoint()), value, delta);
    QApplication::sendEvent(source, &event);
    events();
  };
  const auto start = source->transform();
  const auto anchor = source->source_at(local);
  const double scale = source->effective_scale();
  const QSize gesture_size = source->size();
  const auto geometry_dump = [&] {
    QStringList rows;
    for (auto *widget : source->parentWidget()->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly))
      rows << QString("%1:%2,%3,%4,%5 hidden%6").arg(widget->objectName())
                  .arg(widget->x()).arg(widget->y()).arg(widget->width()).arg(widget->height()).arg(widget->isHidden());
    return QString("window%1x%2 panel%3x%4 ")
               .arg(f.window.width()).arg(f.window.height())
               .arg(source->parentWidget()->width()).arg(source->parentWidget()->height()) + rows.join(";");
  };
  const auto geometry_before = geometry_dump();
  const auto pinned = f.window.current_request().samples.size();
  gesture(Qt::BeginNativeGesture);
  HDRSHOT_CHECK(!source->transient_visible());
  gesture(Qt::ZoomNativeGesture, .25);
  if (std::abs(source->source_at(local).y() - anchor.y()) > 1e-6)
    std::cerr << "GESTURE_GEOMETRY before=" << gesture_size.width() << 'x' << gesture_size.height()
              << " after=" << source->width() << 'x' << source->height()
              << " pan=" << start.pan.x() << ',' << start.pan.y() << "->"
              << source->transform().pan.x() << ',' << source->transform().pan.y()
              << " local=" << local.x() << ',' << local.y() << '\n'
              << "BEFORE " << geometry_before.toStdString() << '\n'
              << "AFTER " << geometry_dump().toStdString() << '\n';
  HDRSHOT_CHECK_NEAR(source->effective_scale(), scale * 1.25, 1e-6);
  HDRSHOT_CHECK_NEAR(source->source_at(local).x(), anchor.x(), 1e-6);
  HDRSHOT_CHECK_NEAR(source->source_at(local).y(), anchor.y(), 1e-6);
  const auto pan = source->transform().pan;
  gesture(Qt::PanNativeGesture, 0., {12, -9});
  HDRSHOT_CHECK(source->transform().pan == pan + QPointF(12, -9));
  key(source, Qt::Key_Escape);
  HDRSHOT_CHECK(source->transform().fit == start.fit);
  HDRSHOT_CHECK(source->transform().pan == start.pan);
  HDRSHOT_CHECK(f.window.isVisible());
  // End commits the second gesture, while escape restored the first.
  gesture(Qt::BeginNativeGesture);
  gesture(Qt::ZoomNativeGesture, .5);
  gesture(Qt::EndNativeGesture);
  HDRSHOT_CHECK_NEAR(source->effective_scale(), scale * 1.5, 1e-6);
  HDRSHOT_CHECK(f.window.current_request().samples.size() <= pinned + 1);
  HDRSHOT_CHECK(!f.window.current_request().mask.enabled);
}

void same_resolution_pan_refines_and_report_rejects_old_detail() {
  Fixture f;
  QEventLoop loop;
  const auto settle = [&] {
    QTimer::singleShot(240, &loop, &QEventLoop::quit);
    loop.exec();
  };
  settle();
  auto options = f.window.current_request().scopes;
  options.amplitude_view = {8., .2};
  f.window.waveform_plot()->options_changed(options);
  settle();
  const auto prior = f.window.current_request();
  HDRSHOT_CHECK(prior.scopes.wave_detail_width > 0);
  HDRSHOT_CHECK(f.last_result->waveform_detail_view == prior.scopes.amplitude_view);
  const int preparations = f.work_preparations;
  int dispatched = 0;
  analysis::Request pending;
  f.window.set_request_handler([&](analysis::Request request) {
    ++dispatched;
    pending = std::move(request);
  });
  events();
  const int baseline = dispatched;
  options = prior.scopes;
  options.amplitude_view.pan = .3;
  f.window.waveform_plot()->options_changed(options);
  events();
  HDRSHOT_CHECK(dispatched == baseline);
  HDRSHOT_CHECK(!f.window.waveform_plot()->is_pending());
  settle();
  HDRSHOT_CHECK(dispatched == baseline + 1);
  HDRSHOT_CHECK(analysis::same_statistics_request(prior, pending));
  HDRSHOT_CHECK(!analysis::same_detail_request(prior, pending));
  HDRSHOT_CHECK(pending.scopes.wave_detail_width == prior.scopes.wave_detail_width);
  HDRSHOT_CHECK(pending.scopes.wave_detail_height == prior.scopes.wave_detail_height);
  HDRSHOT_CHECK(f.work_preparations == preparations);
  int exported = 0;
  f.window.set_export_handler([&](auto, auto) { ++exported; });
  child<QToolButton>(f.window, "analyzerSave")->click();
  HDRSHOT_CHECK(exported == 0);
  auto result = analysis::analyze_cpu({180, 112}, f.pixels, f.work, pending);
  HDRSHOT_CHECK(result.has_value());
  f.window.accept_result(std::make_shared<analysis::ResultData>(std::move(result.value())));
  events();
  child<QToolButton>(f.window, "analyzerSave")->click();
  HDRSHOT_CHECK(exported == 1);
}

void capped_vector_viewport_refines_and_report_rejects_uncovered_result() {
  Fixture f;
  const auto settle = [&] {
    QEventLoop loop;
    QTimer::singleShot(240, &loop, &QEventLoop::quit);
    loop.exec(); events();
  };
  // A standalone production plot is free of the offscreen screen-size clamp.
  auto *plot = f.window.vectorscope_plot();
  struct RestoreParent {
    QWidget *plot, *parent;
    ~RestoreParent() { plot->setParent(parent); }
  } restore{plot, plot->parentWidget()};
  plot->setParent(nullptr);
  plot->resize(2200, 240);
  plot->show();
  auto options = f.window.current_request().scopes;
  options.vector_zoom = 20.;
  plot->options_changed(options);
  settle();
  const auto prior = f.window.current_request();
  HDRSHOT_CHECK(prior.scopes.vector_viewport_width > 2048);
  HDRSHOT_CHECK(prior.scopes.vector_detail_width == 2048);
  const auto extents = analysis::vector_domain_extents(prior.settings, prior.scopes.vector_mode,
      unsigned(std::ceil(plot->plot_rect().width() * plot->devicePixelRatioF())),
      unsigned(std::ceil(plot->plot_rect().height() * plot->devicePixelRatioF())));
  HDRSHOT_CHECK_NEAR(f.last_result->vector_detail_x_extent * 20., extents[0], 1e-12);
  HDRSHOT_CHECK_NEAR(f.last_result->vector_detail_y_extent * 20., extents[1], 1e-12);
  analysis::Request pending;
  f.window.set_request_handler([&](auto request) { pending = request; });
  plot->resize(2500, 240);
  settle();
  HDRSHOT_CHECK(pending.scopes.vector_detail_width == prior.scopes.vector_detail_width);
  HDRSHOT_CHECK(pending.scopes.vector_detail_height == prior.scopes.vector_detail_height);
  HDRSHOT_CHECK(analysis::same_statistics_request(prior, pending));
  HDRSHOT_CHECK(!analysis::same_vector_detail_request(prior, pending));
  int exported = 0;
  f.window.set_export_handler([&](auto, auto) { ++exported; });
  child<QToolButton>(f.window, "analyzerSave")->click();
  HDRSHOT_CHECK(exported == 0);
  // Also reject an otherwise current result produced with the old capped aspect.
  auto result = analysis::analyze_cpu({180, 112}, f.pixels, f.work, pending);
  HDRSHOT_CHECK(result.has_value());
  auto stale = std::make_shared<analysis::ResultData>(result.value());
  const auto old = analysis::vector_domain_extents(pending.settings, pending.scopes.vector_mode,
      pending.scopes.vector_detail_width, pending.scopes.vector_detail_height);
  stale->vector_detail_x_extent = old[0] / pending.scopes.vector_zoom;
  stale->vector_detail_y_extent = old[1] / pending.scopes.vector_zoom;
  f.window.accept_result(stale); events();
  child<QToolButton>(f.window, "analyzerSave")->click();
  HDRSHOT_CHECK(exported == 0);
  f.window.accept_result(std::make_shared<analysis::ResultData>(std::move(result.value()))); events();
  child<QToolButton>(f.window, "analyzerSave")->click();
  HDRSHOT_CHECK(exported == 1);
}

void wave_mode_switch_replans_detail_resolution() {
  Fixture f;
  auto options = f.window.current_request().scopes;
  options.amplitude_view = {20., .2};
  f.window.waveform_plot()->options_changed(options);
  for (int kind : {1, 0}) {
    child<QComboBox>(f.window, "analyzerWaveKind")->setCurrentIndex(kind);
    events();
    const auto &o = f.window.current_request().scopes;
    const int lanes = o.wave_mode == analysis::WaveMode::parade_intensity_rgb ? 4
                    : o.wave_mode == analysis::WaveMode::parade_rgb ? 3 : 1;
    const auto expected = std::clamp(int(std::ceil(f.window.waveform_plot()->plot_rect().width() *
        f.window.waveform_plot()->devicePixelRatioF() / lanes)), 16, 2048);
    HDRSHOT_CHECK(o.wave_detail_width == unsigned(expected));
  }
}
void small_screen_restore_clamps_without_replacing_large_preference() {
  const auto screens = QGuiApplication::screens();
  HDRSHOT_CHECK(screens.size() >= 2);
  QScreen *small = screens[1];
  HDRSHOT_CHECK(small->availableGeometry().width() < screens[0]->availableGeometry().width());
  AnalyzerWindow window({std::make_shared<FixtureSource>(), 1, true},
                        R"({"version":1,"windowWidth":6000,"windowHeight":4000})");
  window.set_close_confirmation([] { return true; });
  (void)window.winId();
  window.windowHandle()->setScreen(small);
  window.show();
  events();
  const auto available = small->availableGeometry();
  HDRSHOT_CHECK(window.screen() == small);
  HDRSHOT_CHECK(window.width() <= available.width());
  HDRSHOT_CHECK(window.height() <= available.height() - 32);
  HDRSHOT_CHECK(available.contains(window.geometry()));
  const auto saved = QJsonDocument::fromJson(
      QByteArray::fromStdString(window.preferences_json())).object();
  HDRSHOT_CHECK(saved.value("windowWidth").toInt() == 6000);
  HDRSHOT_CHECK(saved.value("windowHeight").toInt() == 4000);
  HDRSHOT_CHECK(window.source_widget()->transform().fit);
  HDRSHOT_CHECK(window.current_request().samples.empty());
  window.close();
}

void newly_visible_vector_checks_its_first_full_domain() {
  Fixture f(true, R"({"version":1,"vectorVisible":false})");
  QEventLoop loop;
  const auto settle = [&] {
    QTimer::singleShot(240, &loop, &QEventLoop::quit);
    loop.exec();
  };
  settle();
  // Do not let an incidental splitter resize conceal the missing result-domain
  // invalidation: this test specifically covers the newly available grid.
  for (auto *plot : {f.window.waveform_plot(), f.window.histogram_plot(),
                     f.window.vectorscope_plot()})
    plot->surface_resized = {};
  int requests = 0;
  analysis::Request latest;
  f.window.set_request_handler([&](analysis::Request request) {
    ++requests;
    latest = request;
    auto result = analysis::analyze_cpu({180, 112}, f.pixels, f.work, request);
    HDRSHOT_CHECK(result.has_value());
    if (request.scopes.vector_visible) {
      // Synthetic wide-domain metadata makes the refinement need deterministic
      // without changing source pixels or lowering the production sample count.
      result.value().vector_grid_x_extent = 16.;
      result.value().vector_grid_y_extent = 16.;
    }
    f.window.accept_result(std::make_shared<analysis::ResultData>(std::move(result.value())));
  });
  events();
  const int baseline = requests;
  child<QCheckBox>(f.window, "analyzerVisible2")->setChecked(true);
  events();
  HDRSHOT_CHECK(requests == baseline + 1);
  HDRSHOT_CHECK(latest.scopes.vector_zoom == 1.);
  HDRSHOT_CHECK(latest.scopes.vector_detail_width == 0);
  settle();
  HDRSHOT_CHECK(requests >= baseline + 2);
  HDRSHOT_CHECK(latest.scopes.vector_detail_width > 0);
  HDRSHOT_CHECK(latest.scopes.vector_detail_height > 0);
  HDRSHOT_CHECK(latest.scopes.vector_zoom == 1.);
}

void combo_indicators_render_and_style_is_local() {
  auto *application_style = qApp->style();
  Fixture f;
  HDRSHOT_CHECK(qApp->style() == application_style);
  for (auto *combo : f.window.findChildren<QComboBox *>()) {
    if (!combo->isVisible())
      continue;
    const auto image = combo->grab().toImage();
    const double ratio = combo->devicePixelRatioF();
    int light = 0;
    for (int y = int((combo->height() * .5 - 5) * ratio);
         y < int((combo->height() * .5 + 5) * ratio); ++y)
      for (int x = int((combo->width() - 16) * ratio);
           x < int((combo->width() - 4) * ratio); ++x) {
        const auto color = image.pixelColor(x, y);
        if (color.red() > 150 && color.green() > 150 && color.blue() > 150)
          ++light;
      }
    HDRSHOT_CHECK(light >= 6);
  }
}

void headers_center_controls_and_split_icons() {
  Fixture f;
  for (const QSize size : {QSize(1280, 720), QSize(920, 640), QSize(1440, 900)}) {
    f.window.resize(size);
    events();
    int pairs = 0;
    for (auto *box : f.window.findChildren<QWidget *>()) {
      auto *layout = dynamic_cast<AnalyzerFlowLayout *>(box->layout());
      if (!layout || !box->isVisible()) continue;
      for (int a = 0; a < layout->count(); ++a) {
        const auto *first = layout->itemAt(a);
        if (first->isEmpty()) continue;
        for (int b = a + 1; b < layout->count(); ++b) {
          const auto *second = layout->itemAt(b);
          if (second->isEmpty()) continue;
          const QRect x = first->geometry(), y = second->geometry();
          if (x.top() <= y.bottom() && y.top() <= x.bottom()) {
            HDRSHOT_CHECK(std::abs(x.center().y() - y.center().y()) <= 1);
            ++pairs;
          }
        }
      }
    }
    HDRSHOT_CHECK(pairs >= 12);
  }
  for (const char *name : {"analyzerTool1", "analyzerTool2"}) {
    auto *b = child<QToolButton>(f.window, name);
    QStyleOptionToolButton option;
    option.initFrom(b);
    option.rect = b->rect();
    option.features = QStyleOptionToolButton::MenuButtonPopup | QStyleOptionToolButton::HasMenu;
    const auto menu = b->style()->subControlRect(QStyle::CC_ToolButton, &option, QStyle::SC_ToolButtonMenu, b);
    const QRect main(0, 0, menu.left(), b->height());
    HDRSHOT_CHECK(menu.width() >= 12);
    const auto image = b->grab().toImage();
    const double dpr = b->devicePixelRatioF();
    QRect ink;
    for (int y = int(4 * dpr); y < int((b->height() - 4) * dpr); ++y)
      for (int x = int(3 * dpr); x < int((menu.left() - 2) * dpr); ++x) {
        auto c = image.pixelColor(x, y);
        if (c.red() > 160 && c.green() > 160 && c.blue() > 160)
          ink = ink.united(QRect(x, y, 1, 1));
      }
    HDRSHOT_CHECK(!ink.isEmpty());
    std::cout << "ICON " << name << " center=" << ink.center().x()/dpr << ',' << ink.center().y()/dpr << " main=" << main.center().x() << ',' << main.center().y() << '\n';
    HDRSHOT_CHECK(std::abs(ink.center().x() / dpr - main.center().x()) <= 2.5);
    HDRSHOT_CHECK(std::abs(ink.center().y() / dpr - main.center().y()) <= 2.5);
  }
}

void popup_hover_is_visible_for_all_combos() {
  Fixture f;
  int checked = 0;
  for (auto *c : f.window.findChildren<QComboBox *>()) {
    if (!c->isVisible() || !c->isEnabled() || c->count() < 2) continue;
    c->showPopup();
    events();
    auto *view = c->view();
    const int first = c->currentIndex() == 0 ? 1 : 0;
    const QModelIndex item = c->model()->index(first, c->modelColumn(), c->rootModelIndex());
    view->scrollTo(item);
    events();
    const QRect rect = view->visualRect(item);
    const QPoint location(rect.right() - 10, rect.center().y());
    const auto sample = [&] {
      auto image = view->viewport()->grab().toImage();
      return image.pixelColor(int(location.x()*image.devicePixelRatio()), int(location.y()*image.devicePixelRatio()));
    };
    const QColor before = sample();
    mouse(view->viewport(), QEvent::MouseMove, location, Qt::NoButton, Qt::NoButton);
    const QColor after = sample();
    std::cout << "HOVER " << c->objectName().toStdString() << ' ' << before.name().toStdString() << " -> " << after.name().toStdString() << '\n';
    HDRSHOT_CHECK(after != before);
    HDRSHOT_CHECK(after.blue() > before.blue() + 25);
    const auto directory = qEnvironmentVariable("HDRSHOT_ANALYZER_SCREENSHOT_DIR");
    if (!directory.isEmpty() && c->objectName() == "analyzerWorkingSpace") {
      QDir().mkpath(directory);
      const auto path = directory + "/" + QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss-zzz") + "_popup_hover.png";
      HDRSHOT_CHECK(view->window()->grab().save(path));
      std::cout << "[SCREENSHOT] " << path.toStdString() << '\n';
    }
    c->hidePopup();
    ++checked;
  }
  HDRSHOT_CHECK(checked >= 7);
  for (const char *name : {"analyzerSave", "analyzerTool2"}) {
    auto *b = child<QToolButton>(f.window, name);
    auto *menu = b->menu();
    HDRSHOT_CHECK(menu != nullptr && !menu->actions().empty());
    menu->popup(b->mapToGlobal(QPoint(0,b->height())));
    events();
    menu->setActiveAction(nullptr);
    const auto item = menu->actionGeometry(menu->actions().first());
    const QPoint at(item.right()-8,item.center().y());
    const auto sample = [&] { const auto image = menu->grab().toImage(); return image.pixelColor(int(at.x()*image.devicePixelRatio()),int(at.y()*image.devicePixelRatio())); };
    const auto before = sample();
    mouse(menu,QEvent::MouseMove,at,Qt::NoButton,Qt::NoButton);
    HDRSHOT_CHECK(sample().blue() > before.blue()+25);
    menu->hide();
  }
}

void operation_overlay_is_transparent_and_tracks_tools() {
  Fixture f;
  auto *source = f.window.source_widget();
  const double dpr = source->devicePixelRatioF();
  // No fixture pixels may leak into the operation-only bitmap.
  source->set_fixture_image({});
  QEvent leave(QEvent::Leave);
  QApplication::sendEvent(source, &leave);
  auto empty = source->presentation_overlay(dpr);
  for (int y=0; y<empty.height(); ++y)
    for (int x=0; x<empty.width(); ++x)
      HDRSHOT_CHECK(empty.pixelColor(x,y).alpha()==0);
  HDRSHOT_CHECK(empty.cacheKey() == source->presentation_overlay(dpr).cacheKey());
  child<QToolButton>(f.window,"analyzerTool1")->click();
  mouse(source,QEvent::MouseMove,source->local_at({60,40}),Qt::NoButton,Qt::NoButton);
  auto hover = source->presentation_overlay(dpr);
  HDRSHOT_CHECK(hover != empty);
  HDRSHOT_CHECK(source->retained_overlay(dpr) == empty);
  QApplication::sendEvent(source,&leave);
  HDRSHOT_CHECK(source->presentation_overlay(dpr) == empty);
  f.window.add_swatch({60,40},1);
  events();
  HDRSHOT_CHECK(source->presentation_overlay(dpr) != empty);
  HDRSHOT_CHECK(source->presentation_overlay(dpr) == source->retained_overlay(dpr));
}

void false_color_display_never_changes_statistics() {
  Fixture f;
  f.add_samples();
  child<QToolButton>(f.window, "analyzerTool2")->click();
  drag(f.window.source_widget(), f.window.source_widget()->local_at({35, 25}),
       f.window.source_widget()->local_at({120, 82}));
  QEvent leave(QEvent::Leave);
  QApplication::sendEvent(f.window.source_widget(), &leave);
  events();
  const auto before = f.last_result;
  const auto assert_same_statistics = [&] {
    const auto after = f.last_result;
    HDRSHOT_CHECK(before->valid_count == after->valid_count);
    HDRSHOT_CHECK(before->mask_mean.valid_count ==
                  after->mask_mean.valid_count);
    HDRSHOT_CHECK(before->mask_mean.work_rgb_edr ==
                  after->mask_mean.work_rgb_edr);
    HDRSHOT_CHECK(before->mask_mean.intensity == after->mask_mean.intensity);
    HDRSHOT_CHECK(before->vectorscope.counts == after->vectorscope.counts);
    for (std::size_t channel = 0; channel < 4; ++channel) {
      HDRSHOT_CHECK(before->waveform[channel].counts ==
                    after->waveform[channel].counts);
      HDRSHOT_CHECK(before->histograms[channel].counts ==
                    after->histograms[channel].counts);
    }
    for (const auto &fixed : before->samples) {
      if (!fixed.request.id)
        continue;
      const auto found = std::find_if(
          after->samples.begin(), after->samples.end(), [&](const auto &value) {
            return value.request.id == fixed.request.id;
          });
      HDRSHOT_CHECK(found != after->samples.end());
      HDRSHOT_CHECK(fixed.mean.valid_count == found->mean.valid_count);
      HDRSHOT_CHECK(fixed.mean.work_rgb_edr == found->mean.work_rgb_edr);
      HDRSHOT_CHECK(fixed.mean.y_nits == found->mean.y_nits);
    }
  };
  child<QComboBox>(f.window, "analyzerSourceMode")->setCurrentIndex(1);
  events();
  assert_same_statistics();
  child<QComboBox>(f.window, "analyzerSourceMode")->setCurrentIndex(0);
  events();
  assert_same_statistics();
}

void rgb_histogram_overlap_is_order_independent_color() {
  AnalyzerScopePlot plot(AnalyzerScopePlot::Kind::histogram);
  plot.resize(600, 260);
  auto result = std::make_shared<analysis::ResultData>();
  result->valid_count = 100;
  result->settings.working_space = analysis::WorkingSpace::srgb_sdr;
  result->scopes.histogram_mode = analysis::HistogramMode::rgb;
  result->histograms[1].counts = {100, 100, 100, 0};
  result->histograms[2].counts = {100, 100, 40, 0};
  result->histograms[3].counts = {100, 0, 10, 0};
  for (auto &histogram : result->histograms)
    histogram.maximum = 100;
  plot.set_options(result->scopes);
  plot.set_result(result);
  plot.set_pending(false);
  const auto pixels = plot.grab().toImage();
  const auto rect = plot.plot_rect();
  const double ratio = plot.devicePixelRatioF();
  const auto color_at = [&](double x, double height) {
    return pixels.pixelColor(
        int((rect.left() + x * rect.width()) * ratio),
        int((rect.bottom() - height * rect.height()) * ratio));
  };
  // Equal RGB is neutral, RG-only is yellow. Unequal heights retain the red
  // upper, yellow middle and neutral lower sections; blue never paints over
  // the entire taller red/green bars.
  HDRSHOT_CHECK(color_at(.125, .5) == QColor(145, 145, 145));
  HDRSHOT_CHECK(color_at(.375, .5) == QColor(145, 145, 0));
  HDRSHOT_CHECK(color_at(.625, .8) == QColor(145, 0, 0));
  HDRSHOT_CHECK(color_at(.625, .5) == QColor(145, 145, 0));
  HDRSHOT_CHECK(color_at(.625, .2) == QColor(145, 145, 145));
}

void vector_target_labels_stay_inside_visible_edges() {
  QFont font = qApp->font();
  font.setPixelSize(11);
  const QFontMetrics metrics(font);
  for (const QSize size : {QSize(480, 120), QSize(280, 240), QSize(900, 220)}) {
    const QRectF rect(QPointF(9, 9), QSizeF(size));
    for (auto space : {analysis::WorkingSpace::srgb_sdr,
                       analysis::WorkingSpace::display_p3_pq}) {
      for (double zoom : {1., 4.}) {
        const auto calibration = analysis::vector_calibration(
            {space, 203, 0}, analysis::VectorMode::ycbcr,
            std::uint32_t(size.width()), std::uint32_t(size.height()), zoom);
        HDRSHOT_CHECK(calibration.full_targets.size() == 6);
        for (const auto &target : calibration.full_targets) {
          const auto projected =
              analysis::project_vector(target.position, calibration, zoom);
          const QPointF point(rect.left() + projected[0] * rect.width(),
                              rect.bottom() - projected[1] * rect.height());
          const QSizeF text(
              metrics.horizontalAdvance(QString::fromStdString(target.label)) +
                  2,
              metrics.height());
          const auto bounds =
              analyzer_vector_target_label_bounds(rect, point, text);
          if (rect.contains(point)) {
            HDRSHOT_CHECK(!bounds.isEmpty());
            HDRSHOT_CHECK(rect.contains(bounds));
          } else
            HDRSHOT_CHECK(bounds.isEmpty());
        }
      }
    }
    for (auto point : {rect.topLeft(), rect.topRight(), rect.bottomLeft(),
                       rect.bottomRight()})
      HDRSHOT_CHECK(rect.contains(
          analyzer_vector_target_label_bounds(rect, point, {12, 14})));
    HDRSHOT_CHECK(analyzer_vector_target_label_bounds(
                      rect, rect.bottomRight() + QPointF(1, 1), {12, 14})
                      .isEmpty());
  }
}

void swatch_pairs_use_real_clicks_and_local_updates() {
  Fixture f;
  const bool dpr2 = QApplication::primaryScreen()->devicePixelRatio() > 1.5;
  f.window.resize(1280, dpr2 ? 1000 : 1400);
  events();
  f.add_samples();
  const int requests_before = f.requests;
  auto *handle1 = child<QToolButton>(f.window, "analyzerSwatchPairHandle1");
  click(handle1, handle1->rect().center());
  auto *card2 = child<QWidget>(f.window, "analyzerSwatch2");
  mouse(card2, QEvent::MouseButtonPress, card2->rect().center(),
        Qt::RightButton, Qt::RightButton);
  mouse(card2, QEvent::MouseButtonRelease, card2->rect().center(),
        Qt::RightButton, Qt::NoButton);
  HDRSHOT_CHECK(f.window.findChildren<QWidget *>("analyzerSwatchPairRow1_2").empty());
  click(card2, card2->rect().center());
  events();
  HDRSHOT_CHECK(child<QWidget>(f.window, "analyzerSwatchPairRow1_2"));

  // Real pointer drag from #2's handle onto #3's card. The target card must
  // receive the transient highlight while the drag is in progress.
  if (dpr2) {
    auto *panel = child<AnalyzerSwatchPanel>(f.window, "analyzerSwatchPanel");
    auto *scroll = panel->findChild<QScrollArea *>("analyzerSwatchScroll");
    HDRSHOT_CHECK(scroll);
    scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
    events();
  }
  auto *handle2 = child<QToolButton>(f.window, "analyzerSwatchPairHandle2");
  auto *card3 = child<QWidget>(f.window, "analyzerSwatch3");
  const QPoint drop_global = card3->mapToGlobal(card3->rect().center());
  const QPoint drop_local = handle2->mapFromGlobal(drop_global);
  mouse(handle2, QEvent::MouseButtonPress, handle2->rect().center(),
        Qt::LeftButton, Qt::LeftButton);
  QCursor::setPos(drop_global);
  mouse(handle2, QEvent::MouseMove, drop_local, Qt::NoButton, Qt::LeftButton);
  HDRSHOT_CHECK(card3->property("swatchPairTarget").toBool());
  mouse(handle2, QEvent::MouseButtonRelease, drop_local, Qt::LeftButton,
        Qt::NoButton);
  // The completed pair schedules a card rebuild. Reacquire its replacement
  // after mouse() has processed queued events rather than reading the deleted
  // pre-drop card pointer.
  card3 = child<QWidget>(f.window, "analyzerSwatch3");
  HDRSHOT_CHECK(!card3->property("swatchPairTarget").toBool());
  HDRSHOT_CHECK(f.window.findChildren<QWidget *>("analyzerSwatchPairRow2_3").size() == 1);
  events();

  // Pair #1→#3 independently; all three explicit pairs coexist.
  auto *handle1_again = child<QToolButton>(f.window, "analyzerSwatchPairHandle1");
  click(handle1_again, handle1_again->rect().center());
  card3 = child<QWidget>(f.window, "analyzerSwatch3");
  click(card3, card3->rect().center());
  events();
  HDRSHOT_CHECK(child<QWidget>(f.window, "analyzerSwatchPairRow2_3"));
  HDRSHOT_CHECK(child<QWidget>(f.window, "analyzerSwatchPairRow1_3"));
  HDRSHOT_CHECK(f.requests == requests_before);

  auto *primary = child<QLabel>(f.window, "analyzerSwatchReadout1");
  HDRSHOT_CHECK(primary->text().contains("Y · nit"));
  HDRSHOT_CHECK(primary->text().contains("R / G / B · nit"));
  child<QCheckBox>(f.window, "analyzerReading4")->setChecked(true);
  primary = child<QLabel>(f.window, "analyzerSwatchReadout1");
  HDRSHOT_CHECK(primary->text().contains("Hue / Chroma"));
  QPointer<QWidget> stable_handle =
      child<QToolButton>(f.window, "analyzerSwatchPairHandle1");
  const auto value_before = child<QLabel>(f.window, "analyzerSwatchPairValue1_2")->text();
  auto refreshed = std::make_shared<analysis::ResultData>(*f.last_result);
  refreshed->samples.front().mean.perceptual[0] += .1;
  f.window.accept_result(refreshed);
  events();
  HDRSHOT_CHECK(child<QLabel>(f.window, "analyzerSwatchPairValue1_2")->text() != value_before);
  HDRSHOT_CHECK(!stable_handle.isNull());
  HDRSHOT_CHECK(child<QWidget>(f.window, "analyzerSwatchPairHandle1") == stable_handle.data());

  // Save images while all three pairs are present, with immutable evidence
  // names that describe the captured state.
  const QString dir = qEnvironmentVariable("HDRSHOT_SWATCH_PAIR_SCREENSHOT_DIR");
  if (!dir.isEmpty()) {
    QDir().mkpath(dir);
    const auto stamp = QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss-zzz");
    HDRSHOT_CHECK(f.window.grab().save(dir + "/" + stamp + "_hdr_3pairs_before_unlink.png"));
    auto *swatch_panel = child<AnalyzerSwatchPanel>(f.window, "analyzerSwatchPanel");
    auto *swatch_scroll = swatch_panel->findChild<QScrollArea *>("analyzerSwatchScroll");
    HDRSHOT_CHECK(swatch_scroll);
    swatch_scroll->verticalScrollBar()->setValue(
        swatch_scroll->verticalScrollBar()->maximum());
    events();
    HDRSHOT_CHECK(f.window.grab().save(dir + "/" + stamp + "_window_swatches_3pairs_bottom.png"));
    swatch_scroll->verticalScrollBar()->setValue(0);
    const auto plan = f.window.report_plan();
    QImage report(plan.underlay.rgba.data(), plan.underlay.size.width,
                  plan.underlay.size.height, QImage::Format_RGBA8888);
    HDRSHOT_CHECK(report.save(dir + "/" + stamp + "_report_3pairs.png"));
    child<QComboBox>(f.window, "analyzerWorkingSpace")->setCurrentIndex(0);
    events();
    HDRSHOT_CHECK(f.window.grab().save(dir + "/" + stamp + "_sdr_3pairs.png"));
  }

  // Unordered duplicate leaves the original source direction and row count.
  auto *handle3 = child<QToolButton>(f.window, "analyzerSwatchPairHandle3");
  click(handle3, handle3->rect().center());
  card2 = child<QWidget>(f.window, "analyzerSwatch2");
  click(card2, card2->rect().center());
  events();
  HDRSHOT_CHECK(f.window.findChildren<QFrame *>("analyzerSwatchPairRow1_2").size() == 1);
  auto *unlink = child<QToolButton>(f.window, "analyzerUnlinkSwatchPair1_2");
  HDRSHOT_CHECK(unlink->text() == "解除" && !unlink->icon().isNull());
  unlink->click();
  events();
  HDRSHOT_CHECK(f.window.findChildren<QWidget *>("analyzerSwatchPairRow1_2").empty());
  HDRSHOT_CHECK(child<QWidget>(f.window, "analyzerSwatchPairRow1_3"));

  auto *panel = child<AnalyzerSwatchPanel>(f.window, "analyzerSwatchPanel");
  auto *pending_handle = child<QToolButton>(f.window, "analyzerSwatchPairHandle1");
  click(pending_handle, pending_handle->rect().center());
  const auto hidden_report = f.window.report_plan();
  HDRSHOT_CHECK(panel->cancel_pair_gesture()); // The report hid only transients.
  (void)hidden_report;
  click(child<QToolButton>(f.window, "analyzerSwatchPairHandle1"),
        child<QToolButton>(f.window, "analyzerSwatchPairHandle1")->rect().center());
  key(child<QToolButton>(f.window, "analyzerSwatchPairHandle1"), Qt::Key_Escape);
  click(child<QWidget>(f.window, "analyzerSwatch2"),
        child<QWidget>(f.window, "analyzerSwatch2")->rect().center());
  HDRSHOT_CHECK(f.window.findChildren<QWidget *>("analyzerSwatchPairRow1_2").empty());

  click(child<QToolButton>(f.window, "analyzerSwatchPairHandle1"),
        child<QToolButton>(f.window, "analyzerSwatchPairHandle1")->rect().center());
  f.window.hide(); // Hiding cancels the live click-to-pair gesture.
  HDRSHOT_CHECK(!panel->cancel_pair_gesture());
  f.window.show();
  events();
  click(child<QWidget>(f.window, "analyzerSwatch2"),
        child<QWidget>(f.window, "analyzerSwatch2")->rect().center());
  HDRSHOT_CHECK(f.window.findChildren<QWidget *>("analyzerSwatchPairRow1_2").empty());
  HDRSHOT_CHECK(f.window.findChildren<QWidget *>("analyzerSwatchPairRow1_3").size() == 1);
}

void swatch_handles_click_drag_and_curve() {
  Fixture f;
  const bool dpr2 = QApplication::primaryScreen()->devicePixelRatio() > 1.5;
  f.window.resize(1280, dpr2 ? 1000 : 1400);
  events();
  f.add_samples();
  const int requests_before = f.requests;
  auto *panel = child<AnalyzerSwatchPanel>(f.window, "analyzerSwatchPanel");
  auto *scroll = panel->findChild<QScrollArea *>("analyzerSwatchScroll");
  const auto scroll_top = [&] {
    scroll->verticalScrollBar()->setValue(0);
    events();
  };
  const auto scroll_bottom = [&] {
    scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
    events();
  };
  auto *handle1 = child<QToolButton>(f.window, "analyzerSwatchPairHandle1");
  HDRSHOT_CHECK(handle1->toolTip().contains("Δ"));
  HDRSHOT_CHECK(!handle1->icon().isNull());
  click(handle1, handle1->rect().center());
  auto *handle2 = child<QToolButton>(f.window, "analyzerSwatchPairHandle2");
  click(handle2, handle2->rect().center());
  events();
  HDRSHOT_CHECK(child<QWidget>(f.window, "analyzerSwatchPairRow1_2"));

  scroll_bottom();
  handle2 = child<QToolButton>(f.window, "analyzerSwatchPairHandle2");
  auto *handle3 = child<QToolButton>(f.window, "analyzerSwatchPairHandle3");
  const QPoint destination = handle3->mapToGlobal(handle3->rect().center());
  const QPoint local = handle2->mapFromGlobal(destination);
  mouse(handle2, QEvent::MouseButtonPress, handle2->rect().center(),
        Qt::LeftButton, Qt::LeftButton);
  mouse(handle2, QEvent::MouseMove, local, Qt::NoButton, Qt::LeftButton);
  mouse(handle2, QEvent::MouseButtonRelease, local, Qt::LeftButton, Qt::NoButton);
  events();
  HDRSHOT_CHECK(child<QWidget>(f.window, "analyzerSwatchPairRow2_3"));

  scroll_top();
  handle1 = child<QToolButton>(f.window, "analyzerSwatchPairHandle1");
  click(handle1, handle1->rect().center());
  scroll_bottom();
  handle3 = child<QToolButton>(f.window, "analyzerSwatchPairHandle3");
  click(handle3, handle3->rect().center());
  events();
  HDRSHOT_CHECK(child<QWidget>(f.window, "analyzerSwatchPairRow1_3"));
  HDRSHOT_CHECK(panel->pair_model().pairs().size() == 3);
  HDRSHOT_CHECK(f.requests == requests_before);

  // Reversed clicks must keep the existing pair count and direction.
  handle3 = child<QToolButton>(f.window, "analyzerSwatchPairHandle3");
  click(handle3, handle3->rect().center());
  scroll_top();
  handle1 = child<QToolButton>(f.window, "analyzerSwatchPairHandle1");
  click(handle1, handle1->rect().center());
  HDRSHOT_CHECK(panel->pair_model().pairs().size() == 3);

  // Hovering a result row paints a viewport clipped curved connection.
  scroll_bottom();
  auto *row = child<QWidget>(f.window, "analyzerSwatchPairRow2_3");
  QEvent enter(QEvent::Enter);
  QApplication::sendEvent(row, &enter);
  events();
  auto *overlay = child<QWidget>(f.window, "analyzerSwatchTransientOverlay");
  const QImage curve = overlay->grab().toImage();
  const QPoint source_port = overlay->mapFromGlobal(
      child<QToolButton>(f.window, "analyzerSwatchPairHandle2")
          ->mapToGlobal(QPoint(27, 14)));
  bool bends_into_gutter = false;
  for (int y = 0; y < curve.height(); ++y)
    for (int x = int(std::lround((source_port.x() + 5) * curve.devicePixelRatio()));
         x < curve.width(); ++x) {
      const auto color = curve.pixelColor(x, y);
      if (color.alpha() > 100 && color.blue() > 150 &&
          color.green() > 130 && color.red() < 190)
        bends_into_gutter = true;
    }
  HDRSHOT_CHECK(bends_into_gutter);
  const auto report = f.window.report_plan();
  (void)report;
  HDRSHOT_CHECK(panel->pair_model().pairs().size() == 3);
}

void deleted_swatches_leave_no_source_or_report_marks() {
  Fixture f;
  f.window.resize(1280, 1400);
  events();
  f.add_samples();
  auto *source = f.window.source_widget();
  QEvent leave(QEvent::Leave);
  QApplication::sendEvent(source, &leave);
  const double dpr = source->devicePixelRatioF();
  const auto old_result = f.last_result;
  analysis::SourceView presented;
  int present_count = 0;
  f.window.set_present_handler([&](analysis::SourceView view, std::uintptr_t) {
    presented = std::move(view);
    ++present_count;
  });
  const auto maximum_alpha = [&](const QImage &image, QPointF source_point) {
    const QPointF local = source->local_at(source_point) * dpr;
    int maximum = 0;
    for (int dy = -2; dy <= 2; ++dy)
      for (int dx = -2; dx <= 2; ++dx) {
        const int x = int(std::lround(local.x())) + dx;
        const int y = int(std::lround(local.y())) + dy;
        if (x >= 0 && y >= 0 && x < image.width() && y < image.height())
          maximum = std::max(maximum, image.pixelColor(x, y).alpha());
      }
    return maximum;
  };
  const std::array<std::pair<int, QPointF>, 3> marks{{
      {2, {70, 42}}, {1, {20.5, 13.5}}, {3, {100, 90}}}};
  for (const auto &[id, point] : marks) {
    HDRSHOT_CHECK(maximum_alpha(source->retained_overlay(dpr), point) > 0);
    auto *remove = child<QToolButton>(
        f.window, QString("analyzerRemoveSwatch%1").arg(id).toUtf8().constData());
    const QPointF center = remove->rect().center();
    QMouseEvent press(QEvent::MouseButtonPress, center,
                      QPointF(remove->mapToGlobal(center.toPoint())),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(remove, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, center,
                        QPointF(remove->mapToGlobal(center.toPoint())),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(remove, &release);
    events();
    f.window.accept_result(old_result); // an older completed request cannot revive a pin
    events();
    const auto current = f.window.current_request();
    HDRSHOT_CHECK(std::none_of(current.samples.begin(), current.samples.end(),
                               [id](const auto &sample) { return sample.id == std::uint64_t(id); }));
    HDRSHOT_CHECK(maximum_alpha(source->retained_overlay(dpr), point) == 0);
    HDRSHOT_CHECK(maximum_alpha(source->presentation_overlay(dpr), point) == 0);
    const auto report = f.window.report_plan();
    QImage report_image(report.overlay.rgba.data(), report.overlay.size.width,
                        report.overlay.size.height, QImage::Format_RGBA8888);
    const QPointF report_point = source->local_at(point) * dpr +
                                 QPointF(report.source_rect.x, report.source_rect.y);
    HDRSHOT_CHECK(report_image.pixelColor(int(std::lround(report_point.x())),
                                          int(std::lround(report_point.y()))).alpha() == 0);
    QEventLoop delivery;
    QTimer::singleShot(30, &delivery, &QEventLoop::quit);
    delivery.exec();
    HDRSHOT_CHECK(present_count > 0 && presented.operation_overlay);
    QImage native_marks(presented.operation_overlay->rgba.data(),
                        presented.operation_overlay->size.width,
                        presented.operation_overlay->size.height,
                        QImage::Format_RGBA8888);
    HDRSHOT_CHECK(maximum_alpha(native_marks, point) == 0);
  }
}

void swatch_rebuild_preserves_scroll_after_layout() {
  Fixture themed_window;
  analysis::Request request;
  auto result = std::make_shared<analysis::ResultData>();
  result->settings = request.settings;
  for (std::uint64_t id = 1; id <= 12; ++id) {
    analysis::SampleRequest sample{id, int(id * 3), 20, 3, false};
    request.samples.push_back(sample);
    analysis::SampleResult value;
    value.request = sample;
    value.mean.valid_count = 9;
    value.mean.perceptual = {.2 * double(id), .02 * double(id), -.01 * double(id)};
    value.mean.display_rgb = {.2f, .5f, .4f};
    result->samples.push_back(value);
  }
  const auto full_request = request;
  const auto full_result = *result;
  for (const int width : {320, 480}) {
  request = full_request;
  *result = full_result;
  AnalyzerSwatchPanel panel(&themed_window.window);
  panel.setGeometry(0, 0, width, 360);
  panel.set_samples_and_result(request, result);
  panel.show();
  panel.raise();
  events();
  auto *bar = panel.findChild<QScrollArea *>("analyzerSwatchScroll")->verticalScrollBar();
  HDRSHOT_CHECK(bar->maximum() > 300);
  const auto settle_at = [&](int value) {
    bar->setValue(value);
    events();
    return bar->value();
  };
  const auto pair = [&](int source, int target) {
    auto *first = panel.findChild<QToolButton *>(
        "analyzerSwatchPairHandle" + QString::number(source));
    click(first, first->rect().center());
    auto *second = panel.findChild<QToolButton *>(
        "analyzerSwatchPairHandle" + QString::number(target));
    click(second, second->rect().center());
    events();
  };
  const int middle = settle_at(bar->maximum() / 2);
  pair(5, 6);
  events();
  HDRSHOT_CHECK(bar->value() == middle);
  auto *unlink = panel.findChild<QToolButton *>("analyzerUnlinkSwatchPair5_6");
  HDRSHOT_CHECK(unlink);
  unlink->click();
  events();
  HDRSHOT_CHECK(bar->value() == middle);
  const int bottom = settle_at(bar->maximum());
  pair(10, 11);
  events();
  HDRSHOT_CHECK(bar->value() == bottom);
  unlink = panel.findChild<QToolButton *>("analyzerUnlinkSwatchPair10_11");
  HDRSHOT_CHECK(unlink);
  unlink->click();
  events();
  HDRSHOT_CHECK(bar->value() == std::min(bottom, bar->maximum()));

  // Replacing the card set can shorten the range; retain the nearest valid
  // position after all posted layout work has run.
  request.samples.resize(3);
  result->samples.resize(3);
  panel.set_samples_and_result(request, result);
  events();
  HDRSHOT_CHECK(bar->value() == std::min(bottom, bar->maximum()));

  // A new user movement in the same turn wins over the queued restore.
  request = full_request;
  *result = full_result;
  bool direct_scrolled = false;
  const auto direct_connection = QObject::connect(
      bar, &QScrollBar::rangeChanged, &panel, [&](int, int maximum) {
        if (!direct_scrolled && maximum >= 37) {
          direct_scrolled = true;
          bar->setValue(37);
        }
      });
  panel.set_samples_and_result(request, result);
  events();
  QObject::disconnect(direct_connection);
  HDRSHOT_CHECK(direct_scrolled);
  HDRSHOT_CHECK(bar->value() == 37);
  request.samples.front().side = 5;
  result->samples.front().request.side = 5;
  int user_position = -1;
  const auto wheel_connection = QObject::connect(
      bar, &QScrollBar::rangeChanged, &panel, [&](int, int maximum) {
        if (user_position >= 0 || maximum <= 37)
          return;
        auto *viewport = panel.findChild<QScrollArea *>("analyzerSwatchScroll")->viewport();
        const QPoint local(10, 10);
        QWheelEvent user_wheel(QPointF(local), QPointF(viewport->mapToGlobal(local)),
                               {}, QPoint(0, -120), Qt::NoButton, Qt::NoModifier,
                               Qt::NoScrollPhase, false);
        QApplication::sendEvent(viewport, &user_wheel);
        user_position = bar->value();
      });
  panel.set_samples_and_result(request, result);
  events();
  QObject::disconnect(wheel_connection);
  HDRSHOT_CHECK(user_position > 0 && user_position != 37);
  HDRSHOT_CHECK(bar->value() == user_position);
  }
}

void swatch_pair_success_feedback_lifecycle() {
  Fixture f;
  f.window.resize(1280, 1400);
  events();
  f.add_samples();
  const int requests_before = f.requests;
  auto *panel = child<AnalyzerSwatchPanel>(f.window, "analyzerSwatchPanel");
  const QString directory = qEnvironmentVariable("HDRSHOT_PAIR_SUCCESS_SCREENSHOT_DIR");
  const auto capture = [&](const QString &phase) {
    if (directory.isEmpty())
      return;
    QDir().mkpath(directory);
    const auto stamp = QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss-zzz");
    HDRSHOT_CHECK(f.window.grab().save(directory + "/" + stamp + "_" + phase + ".png"));
  };
  const auto opacity = [&](const QString &suffix) {
    auto *row = f.window.findChild<QFrame *>("analyzerSwatchPairRow" + suffix);
    HDRSHOT_CHECK(row);
    auto *effect = qobject_cast<QGraphicsOpacityEffect *>(row->graphicsEffect());
    return effect ? effect->opacity() : 1.0;
  };
  auto *handle1 = child<QToolButton>(f.window, "analyzerSwatchPairHandle1");
  click(handle1, handle1->rect().center());
  auto *handle2 = child<QToolButton>(f.window, "analyzerSwatchPairHandle2");
  click(handle2, handle2->rect().center());
  HDRSHOT_CHECK(panel->pair_model().pairs().size() == 1);
  HDRSHOT_CHECK(opacity("1_2") < .2);
  HDRSHOT_CHECK(f.requests == requests_before);
  capture("click_hold");

  wait_events(240);
  HDRSHOT_CHECK(opacity("1_2") > .05 && opacity("1_2") < .9);
  capture("click_mid");
  auto *row = child<QFrame>(f.window, "analyzerSwatchPairRow1_2");
  const QPoint row_point = row->mapTo(&f.window, QPoint(2, 2));
  const auto report = f.window.report_plan();
  QImage report_image(report.underlay.rgba.data(), report.underlay.size.width,
                      report.underlay.size.height, QImage::Format_RGBA8888);
  const int dpr = int(std::lround(f.window.devicePixelRatioF()));
  HDRSHOT_CHECK(report_image.pixelColor(row_point.x() * dpr, row_point.y() * dpr) ==
                QColor("#202328"));
  wait_events(260);
  HDRSHOT_CHECK_NEAR(opacity("1_2"), 1.0, .001);
  capture("click_final");

  // A second pair starts its own local feedback. Removing its endpoint while
  // it runs must clear the row and its animation state.
  if (f.window.devicePixelRatioF() > 1.5) {
    auto *scroll = panel->findChild<QScrollArea *>("analyzerSwatchScroll");
    scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
    events();
  }
  const int requests_before_second_pair = f.requests;
  handle2 = child<QToolButton>(f.window, "analyzerSwatchPairHandle2");
  auto *handle3 = child<QToolButton>(f.window, "analyzerSwatchPairHandle3");
  click(handle2, handle2->rect().center());
  click(handle3, handle3->rect().center());
  HDRSHOT_CHECK(panel->pair_model().pairs().size() == 2);
  HDRSHOT_CHECK(opacity("2_3") < .2);
  HDRSHOT_CHECK(f.requests == requests_before_second_pair);
  capture("drag_hold");
  child<QToolButton>(f.window, "analyzerRemoveSwatch3")->click();
  events();
  HDRSHOT_CHECK(f.window.findChildren<QFrame *>("analyzerSwatchPairRow2_3").empty());
  HDRSHOT_CHECK(panel->pair_model().pairs().size() == 1);
  const int requests_after_deletion = f.requests;

  // Self selection and reversed duplicate are unsuccessful: no new fade.
  handle1 = child<QToolButton>(f.window, "analyzerSwatchPairHandle1");
  click(handle1, handle1->rect().center());
  handle1 = child<QToolButton>(f.window, "analyzerSwatchPairHandle1");
  click(handle1, handle1->rect().center());
  handle2 = child<QToolButton>(f.window, "analyzerSwatchPairHandle2");
  click(handle2, handle2->rect().center());
  handle1 = child<QToolButton>(f.window, "analyzerSwatchPairHandle1");
  click(handle1, handle1->rect().center());
  HDRSHOT_CHECK(panel->pair_model().pairs().size() == 1);
  HDRSHOT_CHECK_NEAR(opacity("1_2"), 1.0, .001);
  HDRSHOT_CHECK(f.requests == requests_after_deletion);
}

void swatch_pair_rows_fit_narrow_viewports() {
  const QString dir = qEnvironmentVariable("HDRSHOT_SWATCH_PAIR_SCREENSHOT_DIR");
  Fixture themed_window;
  themed_window.window.resize(1280, 1000);
  themed_window.add_samples();
  events();
  analysis::Request request;
  request.settings.working_space = analysis::WorkingSpace::display_p3_pq;
  auto result = std::make_shared<analysis::ResultData>();
  result->settings = request.settings;
  for (const std::uint64_t id : {1ULL, 2ULL, 3ULL}) {
    analysis::SampleRequest sample{id, int(id * 10), int(id * 5), 3, false};
    request.samples.push_back(sample);
    analysis::SampleResult value;
    value.request = sample;
    value.mean.valid_count = 9;
    value.mean.y_nits = 18.0 + double(id);
    value.mean.perceptual = {.25 * double(id), .01 * double(id), -.02 * double(id)};
    value.mean.display_rgb = {float(id) / 3.f, .25f, .5f};
    result->samples.push_back(value);
  }
  // Keep this panel under the production window: the broad QWidget and
  // QToolButton rules must cascade exactly as they do in the application.
  AnalyzerSwatchPanel panel(&themed_window.window);
  panel.set_copy_icon(child<QToolButton>(themed_window.window,
                                        "analyzerCopySwatch1")->icon());
  panel.raise();
  panel.set_reading_formatter([](const analysis::Readout &readout, bool) {
    return QString("Y · nit   %1\nITP T / P   0.1234 / 0.5678  |  ICtCp Ct / Cp   0.2468 / 0.5678")
        .arg(readout.y_nits, 0, 'f', 2);
  });
  panel.set_samples_and_result(request, result);
  for (const auto size : {QSize(320, 820), QSize(480, 820), QSize(640, 820)}) {
    panel.resize(size);
    panel.show();
    events();
    if (qEnvironmentVariableIsSet("HDRSHOT_EXPECT_DPR2")) {
      const double dpr = panel.devicePixelRatioF();
      HDRSHOT_CHECK_NEAR(dpr, 2.0, .01);
      const auto captured = panel.grab();
      HDRSHOT_CHECK(captured.size() ==
                    QSize(qRound(size.width() * dpr),
                          qRound(size.height() * dpr)));
    }
    auto *handle1 = panel.findChild<QToolButton *>("analyzerSwatchPairHandle1");
    auto *card2 = panel.findChild<QWidget *>("analyzerSwatch2");
    click(handle1, handle1->rect().center());
    click(card2, card2->rect().center());
    auto *handle1_again = panel.findChild<QToolButton *>("analyzerSwatchPairHandle1");
    auto *card3 = panel.findChild<QWidget *>("analyzerSwatch3");
    click(handle1_again, handle1_again->rect().center());
    click(card3, card3->rect().center());
    auto *handle2 = panel.findChild<QToolButton *>("analyzerSwatchPairHandle2");
    card3 = panel.findChild<QWidget *>("analyzerSwatch3");
    click(handle2, handle2->rect().center());
    click(card3, card3->rect().center());
    events();
    wait_events(500); // Stable-state pixel assertions below exclude success feedback.

    auto *scroll = panel.findChild<QScrollArea *>("analyzerSwatchScroll");
    HDRSHOT_CHECK(scroll);
    HDRSHOT_CHECK(scroll->horizontalScrollBar()->maximum() == 0);
    for (const std::uint64_t id : {1ULL, 2ULL, 3ULL}) {
      auto *card = panel.findChild<QFrame *>("analyzerSwatch" + QString::number(id));
      auto *readout = panel.findChild<QLabel *>("analyzerSwatchReadout" + QString::number(id));
      auto *copy = panel.findChild<QToolButton *>("analyzerCopySwatch" + QString::number(id));
      auto *handle = panel.findChild<QToolButton *>("analyzerSwatchPairHandle" + QString::number(id));
      auto *remove = panel.findChild<QToolButton *>("analyzerRemoveSwatch" + QString::number(id));
      HDRSHOT_CHECK(card && readout && copy && handle && remove);
      HDRSHOT_CHECK(card->width() <= scroll->viewport()->width());
      HDRSHOT_CHECK(readout->height() <= readout->fontMetrics().lineSpacing() * 2 + 3);
      HDRSHOT_CHECK(readout->toolTip() == readout->text());
      HDRSHOT_CHECK(copy->geometry().right() < handle->geometry().left());
      const QSize button_size(analyzer_control_style::icon_button_side,
                              analyzer_control_style::icon_button_side);
      HDRSHOT_CHECK(copy->size() == button_size);
      HDRSHOT_CHECK(handle->size() == button_size);
      HDRSHOT_CHECK(remove->size() == button_size);
      HDRSHOT_CHECK(copy->geometry().center().y() == handle->geometry().center().y());
      HDRSHOT_CHECK(handle->geometry().center().y() == remove->geometry().center().y());
      HDRSHOT_CHECK(!copy->accessibleName().isEmpty());
    }
    for (const auto *name : {"1_2", "1_3", "2_3"}) {
      auto *pair_row = panel.findChild<QFrame *>("analyzerSwatchPairRow" + QString(name));
      HDRSHOT_CHECK(pair_row && pair_row->isVisible());
      HDRSHOT_CHECK(pair_row->width() <= scroll->viewport()->width());
      HDRSHOT_CHECK(panel.findChild<QLabel *>("analyzerPairColorA_" + QString(name))->size() == QSize(20, 20));
      HDRSHOT_CHECK(panel.findChild<QLabel *>("analyzerPairColorB_" + QString(name))->size() == QSize(20, 20));
      HDRSHOT_CHECK(panel.findChild<QToolButton *>("analyzerUnlinkSwatchPair" + QString(name))->isVisible());
      auto *line = panel.findChild<QWidget *>("analyzerSwatchPairResultLine" + QString(name));
      auto *endpoints = panel.findChild<QWidget *>("analyzerSwatchPairEndpoints" + QString(name));
      auto *metric = panel.findChild<QLabel *>("analyzerSwatchPairMetric" + QString(name));
      auto *separator = panel.findChild<QLabel *>("analyzerSwatchPairSeparator" + QString(name));
      auto *unlink = panel.findChild<QToolButton *>("analyzerUnlinkSwatchPair" + QString(name));
      const auto top = unlink->mapTo(line, QPoint());
      HDRSHOT_CHECK(unlink->height() == analyzer_control_style::icon_button_side);
      const QImage unlink_image = unlink->grab().toImage();
      int content_left = unlink_image.width();
      int content_right = -1;
      for (int y = 2; y < unlink_image.height() - 2; ++y)
        for (int x = 2; x < unlink_image.width() - 2; ++x) {
          const QColor pixel = unlink_image.pixelColor(x, y);
          if (pixel.red() > 140 && pixel.red() > pixel.green() + 65 &&
              pixel.red() > pixel.blue() + 65) {
            content_left = std::min(content_left, x);
            content_right = std::max(content_right, x);
          }
        }
      HDRSHOT_CHECK(content_right >= content_left);
      const int left_space = content_left;
      const int right_space = unlink_image.width() - content_right - 1;
      HDRSHOT_CHECK(std::abs(left_space - right_space) <=
                    qRound(3 * unlink->devicePixelRatioF()));
      HDRSHOT_CHECK(separator && separator->text() == QStringLiteral("│"));
      HDRSHOT_CHECK(top.y() >= 0 && top.y() + unlink->height() <= line->height());
      HDRSHOT_CHECK(std::abs(top.y() * 2 + unlink->height() - line->height()) <= 2);
      HDRSHOT_CHECK(unlink->mapTo(pair_row, QPoint()).x() + unlink->width() <= pair_row->width());
      if (size.width() >= 480) {
        const int gap = separator->mapTo(pair_row, QPoint()).x() -
                        (endpoints->geometry().right() + 1);
        HDRSHOT_CHECK(gap >= 0 && gap <= 32);
        HDRSHOT_CHECK(metric->mapTo(pair_row, QPoint()).x() <=
                      separator->mapTo(pair_row, QPoint()).x() + separator->width() + 8);
        HDRSHOT_CHECK(metric->mapTo(pair_row, QPoint()).y() <
                      endpoints->geometry().bottom());
      } else {
        HDRSHOT_CHECK(metric->mapTo(pair_row, QPoint()).x() <= 32);
        HDRSHOT_CHECK(separator->mapTo(pair_row, QPoint()).y() ==
                      metric->mapTo(pair_row, QPoint()).y());
        HDRSHOT_CHECK(metric->mapTo(pair_row, QPoint()).y() >=
                      endpoints->geometry().bottom());
      }
      const QImage screenshot = panel.grab().toImage();
      const auto pixel_at = [&](const QPoint &point) {
        return screenshot.pixelColor(qRound(point.x() * panel.devicePixelRatioF()),
                                     qRound(point.y() * panel.devicePixelRatioF()));
      };
      const QColor row_background = pixel_at(pair_row->mapTo(&panel, QPoint(2, 2)));
      HDRSHOT_CHECK(row_background == QColor("#202328"));
      for (auto *label : pair_row->findChildren<QLabel *>()) {
        if (label->objectName().startsWith("analyzerPairColor"))
          continue;
        const QPoint corner = label->mapTo(&panel,
                                          QPoint(label->width() - 1, label->height() - 1));
        HDRSHOT_CHECK(pixel_at(corner) == row_background);
      }
    }
    if (!dir.isEmpty()) {
      QDir().mkpath(dir);
      const auto stamp = QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss-zzz");
      HDRSHOT_CHECK(panel.grab().save(dir + "/" + stamp + "_panel_" + QString::number(size.width()) + "px.png"));
      HDRSHOT_CHECK(themed_window.window.grab().save(
          dir + "/" + stamp + "_themed_window_" + QString::number(size.width()) + "px.png"));
    }
    panel.set_transient_hidden(true);
    HDRSHOT_CHECK(panel.findChildren<QWidget *>("analyzerSwatchPairRow1_2").size() == 1);
    panel.set_transient_hidden(false);
    for (const auto *suffix : {"1_2", "1_3", "2_3"}) {
      auto *unlink = panel.findChild<QToolButton *>(
          "analyzerUnlinkSwatchPair" + QString(suffix));
      HDRSHOT_CHECK(unlink);
      unlink->click();
      events();
    }
  }
  panel.hide();
}

void swatch_itp_copy_and_danger_buttons() {
  Fixture f;
  f.add_samples();
  auto *field = child<QCheckBox>(f.window, "analyzerReading4");
  field->setChecked(true);
  events();
  auto *readout = child<QLabel>(f.window, "analyzerSwatchReadout1");
  const auto &sample = f.last_result->samples.front().mean;
  const QString expected = QString("ITP T / P   %1 / %2  |  ICtCp Ct / Cp   %3 / %4")
      .arg(sample.perceptual[1], 0, 'f', 4)
      .arg(sample.perceptual[2], 0, 'f', 4)
      .arg(2 * sample.perceptual[1], 0, 'f', 4)
      .arg(sample.perceptual[2], 0, 'f', 4);
  HDRSHOT_CHECK(readout->text().contains(expected));
  HDRSHOT_CHECK(readout->text().split('\n').filter("ITP T / P").size() == 1);
  HDRSHOT_CHECK(readout->toolTip() == readout->text());
  f.window.source_widget()->hover_changed(QPointF(20, 20));
  events();
  auto *hover = child<QLabel>(f.window, "analyzerHoverReadout");
  HDRSHOT_CHECK(hover->text().contains("ITP T / P"));
  HDRSHOT_CHECK(hover->text().contains("  |  ICtCp Ct / Cp   "));
  HDRSHOT_CHECK(hover->toolTip() == hover->text());
  auto *copy = child<QToolButton>(f.window, "analyzerCopySwatch1");
  copy->click();
  const QString copied = QGuiApplication::clipboard()->text();
  HDRSHOT_CHECK(copied == child<QLabel>(f.window, "analyzerSwatchLabel1")->text() +
                              "\n" + readout->text());
  HDRSHOT_CHECK(copied.contains("#1 · 20, 20 · 1×1"));
  HDRSHOT_CHECK(copied.contains(expected));
  HDRSHOT_CHECK(QGuiApplication::clipboard()->mimeData()->hasText());
  HDRSHOT_CHECK(!QGuiApplication::clipboard()->mimeData()->hasImage());
  auto *panel = child<AnalyzerSwatchPanel>(f.window, "analyzerSwatchPanel");
  auto *handle1 = child<QToolButton>(f.window, "analyzerSwatchPairHandle1");
  click(handle1, handle1->rect().center());
  auto *copy2 = child<QToolButton>(f.window, "analyzerCopySwatch2");
  click(copy2, copy2->rect().center());
  HDRSHOT_CHECK(panel->pair_model().pairs().empty());
  HDRSHOT_CHECK(QGuiApplication::clipboard()->text().startsWith("#2 ·"));
  auto *handle2 = child<QToolButton>(f.window, "analyzerSwatchPairHandle2");
  click(handle2, handle2->rect().center());
  HDRSHOT_CHECK(panel->pair_model().pairs().size() == 1);
  HDRSHOT_CHECK(panel->pair_model().pairs().front().source_id == 1);
  HDRSHOT_CHECK(panel->pair_model().pairs().front().target_id == 2);
  auto *clear = child<QToolButton>(f.window, "analyzerClearSwatches");
  auto *remove = child<QToolButton>(f.window, "analyzerRemoveSwatch1");
  auto *current_copy = child<QToolButton>(f.window, "analyzerCopySwatch1");
  auto *current_handle = child<QToolButton>(f.window, "analyzerSwatchPairHandle2");
  const QSize expected_button(analyzer_control_style::icon_button_side,
                       analyzer_control_style::icon_button_side);
  HDRSHOT_CHECK(current_copy->size() == expected_button);
  HDRSHOT_CHECK(current_handle->size() == expected_button);
  HDRSHOT_CHECK(remove->size() == expected_button);
  HDRSHOT_CHECK(current_copy->geometry().center().y() == current_handle->geometry().center().y());
  HDRSHOT_CHECK(current_handle->geometry().center().y() == remove->geometry().center().y());
  HDRSHOT_CHECK(clear->size() == expected_button);
  for (auto *danger : {clear, remove,
                       child<QToolButton>(f.window, "analyzerUnlinkSwatchPair1_2")}) {
    HDRSHOT_CHECK(danger->property("analyzerDanger").toBool());
    for (const auto *color : {analyzer_control_style::danger_normal,
                              analyzer_control_style::danger_hover,
                              analyzer_control_style::danger_pressed,
                              analyzer_control_style::danger_border,
                              analyzer_control_style::danger_foreground})
      HDRSHOT_CHECK(danger->styleSheet().contains(color));
  }
  field->setChecked(false);
  child<QComboBox>(f.window, "analyzerWorkingSpace")->setCurrentIndex(0);
  field->setChecked(true);
  events();
  readout = child<QLabel>(f.window, "analyzerSwatchReadout1");
  HDRSHOT_CHECK(readout->text().contains("Lab D65 L* / a* / b*"));
  HDRSHOT_CHECK(!readout->text().contains("ICtCp Ct / Cp"));
  child<QToolButton>(f.window, "analyzerCopySwatch1")->click();
  HDRSHOT_CHECK(QGuiApplication::clipboard()->text().contains("Lab D65"));
}

void danger_palette_matches_linear_display_p3_reference() {
  const QColor base(analyzer_control_style::danger_foreground);
  const auto linear_p3 = ExtendedP3Mapper::annotation_linear_display_p3(
      static_cast<std::uint32_t>(base.rgb() & 0x00FFFFFFU));
  const std::array<float, 3> reference_p3{0.752F, 0.109F, 0.077F};
  for (int i = 0; i < 3; ++i)
    HDRSHOT_CHECK_NEAR(linear_p3[std::size_t(i)],
                       reference_p3[std::size_t(i)],
                       0.003F);
  HDRSHOT_CHECK(QColor(analyzer_control_style::danger_normal) == QColor("#30343b"));
  HDRSHOT_CHECK(QColor(analyzer_control_style::danger_hover) == QColor("#424750"));
  HDRSHOT_CHECK(QColor(analyzer_control_style::danger_border) == QColor("#4b515c"));
  HDRSHOT_CHECK(QColor(analyzer_control_style::danger_pressed).lightness() <
                QColor(analyzer_control_style::danger_normal).lightness());
}

void visual_artifacts() {
  const QString directory =
      qEnvironmentVariable("HDRSHOT_ANALYZER_SCREENSHOT_DIR");
  if (directory.isEmpty())
    return;
  HDRSHOT_CHECK(QDir().mkpath(directory));
  const QString stamp =
      QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss-zzz");
  const auto save = [&](Fixture &f, QString name) {
    events();
    const QString path = directory + "/" + stamp + "_" + name + "_" +
                         QString::number(f.window.devicePixelRatioF(), 'f', 0) +
                         "x.png";
    HDRSHOT_CHECK(f.window.grab().save(path));
    std::cout << "[SCREENSHOT] " << path.toStdString() << '\n';
  };
  Fixture f;
  f.add_samples();
  save(f, "hdr_default_1280x720");
  child<QCheckBox>(f.window, "analyzerReading4")->setChecked(true);
  save(f, "hdr_itp_ct_cp_swatches_1280x720");
  const auto hdr_plan = f.window.report_plan();
  QImage hdr_report(hdr_plan.underlay.rgba.data(), hdr_plan.underlay.size.width,
                    hdr_plan.underlay.size.height, QImage::Format_RGBA8888);
  HDRSHOT_CHECK(hdr_report.save(directory + "/" + stamp + "_hdr_itp_report_underlay.png"));
  child<QToolButton>(f.window, "analyzerTool1")->click();
  mouse(f.window.source_widget(), QEvent::MouseMove,
        f.window.source_widget()->local_at({60, 40}), Qt::NoButton,
        Qt::NoButton);
  save(f, "hdr_actual_hover_1280x720");
  child<QComboBox>(f.window, "analyzerSampleSize")->setCurrentIndex(5);
  auto *hist_mode = child<QComboBox>(f.window, "analyzerHistogramMode");
  hist_mode->setCurrentIndex(hist_mode->findData(int(analysis::HistogramMode::hue)));
  auto *gain = child<QLineEdit>(f.window, "analyzerVectorGain");
  gain->setText("2"); key(gain, Qt::Key_Return); events();
  save(f, "hdr_roi_101_gain2_highlight4_hue_tp");
  auto view = f.window.current_request().scopes;
  view.vector_zoom = 10.; view.vector_pan = {-.04, .03};
  f.window.vectorscope_plot()->options_changed(view);
  QEventLoop refine; QTimer::singleShot(220, &refine, &QEventLoop::quit); refine.exec(); events();
  save(f, "hdr_vector_10x_panned_refined");
  f.window.vectorscope_plot()->fit();
  QEvent leave(QEvent::Leave);
  QApplication::sendEvent(f.window.source_widget(), &leave);
  child<QToolButton>(f.window, "analyzerTool0")->click();
  f.window.resize(920, 640);
  events();
  save(f, "hdr_minimum_920x640");
  f.window.resize(1440, 900);
  events();
  child<QComboBox>(f.window, "analyzerHistogramMode")->setCurrentIndex(2);
  child<QComboBox>(f.window, "analyzerWaveKind")->setCurrentIndex(1);
  child<QComboBox>(f.window, "analyzerWaveMode")->setCurrentIndex(1);
  child<QComboBox>(f.window, "analyzerVectorMode")->setCurrentIndex(1);
  events();
  save(f, "hdr_parade_adobe_ycbcr_1440x900");
  child<QToolButton>(f.window, "analyzerTool1")->click();
  child<QToolButton>(f.window, "analyzerTool2")->click();
  events();
  drag(f.window.source_widget(), f.window.source_widget()->local_at({35, 25}),
       f.window.source_widget()->local_at({120, 82}));
  QApplication::sendEvent(f.window.source_widget(), &leave);
  save(f, "hdr_mask_no_hover");
  child<QComboBox>(f.window, "analyzerSourceMode")->setCurrentIndex(1);
  events();
  f.display_fixture(true);
  save(f, "false_color_mask");
  child<QToolButton>(f.window, "analyzerReferences1")->click();
  events();
  save(f, "reference_editor");
  Fixture sdr(false);
  sdr.add_samples();
  save(sdr, "sdr_p3_lab");
  child<QComboBox>(sdr.window, "analyzerHistogramMode")->setCurrentIndex(3);
  events();
  save(sdr, "sdr_hue");
  const auto plan = f.window.report_plan();
  QImage underlay(plan.underlay.rgba.data(), plan.underlay.size.width,
                  plan.underlay.size.height, QImage::Format_RGBA8888);
  HDRSHOT_CHECK(
      underlay.save(directory + "/" + stamp + "_report_underlay.png"));
  QImage overlay(plan.overlay.rgba.data(), plan.overlay.size.width,
                 plan.overlay.size.height, QImage::Format_RGBA8888);
  HDRSHOT_CHECK(overlay.save(directory + "/" + stamp + "_report_overlay.png"));
}
} // namespace
int main(int argc, char **argv) {
  // Qt's offscreen default is only 800×800 physical pixels. At scale 2 that
  // becomes a 400×400 logical screen and silently changes these UI fixtures.
  // Configure real room for a 1280×720 logical window at both tested DPRs,
  // plus a second smaller screen for an independent restoration/clamp test.
  // Qt source: src/plugins/platforms/offscreen/qoffscreenintegration.cpp.
  QTemporaryFile platform_config;
  if (!platform_config.open())
    qFatal("Cannot create offscreen test-screen configuration");
  platform_config.write(R"({"screens":[
    {"name":"analyzer-main","x":0,"y":0,"width":3840,"height":2160,"dpr":1},
    {"name":"analyzer-small","x":3840,"y":0,"width":1920,"height":1440,"dpr":1}
  ]})");
  platform_config.flush();
  qputenv("QT_QPA_PLATFORM", "offscreen:configfile=" + platform_config.fileName().toUtf8());
  QApplication app(argc, argv);
  app.setFont(QFont("Arial", 10));
  std::cout << "TEST_SCREEN logical=" << app.primaryScreen()->size().width()
            << 'x' << app.primaryScreen()->size().height()
            << " dpr=" << app.primaryScreen()->devicePixelRatio() << '\n';
  if (qEnvironmentVariableIsSet("HDRSHOT_SWATCH_PAIR_ONLY"))
    return hdrshot::test::run(
        {{"swatch pairing uses real click events and local updates",
          swatch_pairs_use_real_clicks_and_local_updates},
         {"swatch handles click, drag and curved links",
          swatch_handles_click_drag_and_curve},
         {"deleted swatches clear source, native overlay and report marks",
          deleted_swatches_leave_no_source_or_report_marks},
         {"swatch rebuild keeps scroll and respects a new wheel event",
          swatch_rebuild_preserves_scroll_after_layout},
         {"swatch pair success feedback click drag report and deletion",
          swatch_pair_success_feedback_lifecycle},
         {"swatch pair rows fit 320/480/640 logical-pixel panels",
          swatch_pair_rows_fit_narrow_viewports},
         {"swatch HDR ITP copy and danger actions",
          swatch_itp_copy_and_danger_buttons},
         {"danger palette matches linear Display P3 reference",
          danger_palette_matches_linear_display_p3_reference}});
  return hdrshot::test::run(
      {{"new source defaults and safe preference restore",
        new_inputs_and_preferences},
       {"bounded three-size eight-visibility layout",
        bounded_layout_and_visibility},
       {"four independent splitters and cancel",
        independent_splitters_cancel_and_keyboard},
       {"source gestures and mask picker composition",
        source_gestures_and_tool_composition},
       {"six source sample sizes and fixed independence",
        sample_sizes_and_fixed_mask_independence},
       {"all scope modes and local controls", all_modes_and_colorize},
       {"reference input validation and persistent editor",
        references_validate_enter_and_stay_open},
       {"visible report geometry and transparent source hole",
        report_keeps_visible_geometry_and_source_hole},
       {"perceptual gamut report matches current visible scope", gamut_report_matches_visible_scope},
       {"stale result and independent close confirmation",
        stale_results_and_close_confirmation},
       {"close dialog stays dark without leaking into system windows",
        close_dialog_stays_dark_with_light_and_dark_application_palettes},
       {"pending control edits cannot export mixed report",
        pending_controls_cannot_export_mixed_report},
       {"explicit mask and hover marker states",
        marker_states_are_not_implicit_full_image_means},
       {"pending settings never relabel old numerical results",
        pending_settings_do_not_relabel_old_results},
       {"exclusive tools stay selected on repeated clicks", tools_are_exclusive_and_repeat_is_noop},
       {"picker options activate picker and hue mode names carry the plane", picker_options_activate_tool_and_hue_names},
       {"moving cursor retains lagged valid highlighters instead of flashing", lagged_hover_keeps_completed_markers_until_replaced},
       {"vector pan refines after settling and rejects stale-center reports", vector_pan_defers_detail_and_report_checks_center},
       {"native surface hover and interrupted gesture recovery", native_surface_hover_and_interrupted_gesture_recover},
       {"shared scope gain Enter-only display contract", scope_gain_commits_only_enter_and_never_analyzes},
       {"new windows Fit while options and existing views survive", preferences_reset_views_without_pixels_or_stale_close_write},
       {"view changes use bounded presentation and idle refinement", view_changes_coalesce_present_and_defer_refinement},
       {"native source pan pinch and gesture cancellation",
        native_pan_pinch_and_cancel_preserve_source_coordinates},
       {"same-size pan refines and report rejects old detail",
        same_resolution_pan_refines_and_report_rejects_old_detail},
       {"capped Vector viewport covers true domain and rejects incomplete reports",
        capped_vector_viewport_refines_and_report_rejects_uncovered_result},
       {"Wave/Parade mode changes replan per-lane detail resolution",
        wave_mode_switch_replans_detail_resolution},
       {"small-screen restoration keeps preferred size",
        small_screen_restore_clamps_without_replacing_large_preference},
       {"newly shown Vector refines its first full domain",
        newly_visible_vector_checks_its_first_full_domain},
       {"all combo indicators render with window-local style",
        combo_indicators_render_and_style_is_local},
       {"header centers and split-tool icon placement", headers_center_controls_and_split_icons},
       {"all combo popups visibly follow hover", popup_hover_is_visible_for_all_combos},
       {"operation bitmap alpha, tools and retained separation", operation_overlay_is_transparent_and_tracks_tools},
       {"false color never changes measurements or scopes",
        false_color_display_never_changes_statistics},
       {"RGB histogram neutral and pairwise overlap pixels",
        rgb_histogram_overlap_is_order_independent_color},
       {"vector target edge text bounds without offscreen labels",
        vector_target_labels_stay_inside_visible_edges},
       {"swatch pairing uses real click events and local updates",
        swatch_pairs_use_real_clicks_and_local_updates},
       {"swatch handles click, drag and curved links",
        swatch_handles_click_drag_and_curve},
       {"deleted swatches clear source, native overlay and report marks",
        deleted_swatches_leave_no_source_or_report_marks},
       {"swatch rebuild keeps scroll and respects a new wheel event",
        swatch_rebuild_preserves_scroll_after_layout},
       {"swatch pair success feedback click drag report and deletion",
        swatch_pair_success_feedback_lifecycle},
       {"swatch pair rows fit 320/480/640 logical-pixel panels",
        swatch_pair_rows_fit_narrow_viewports},
       {"swatch HDR ITP copy and danger actions",
         swatch_itp_copy_and_danger_buttons},
       {"danger palette matches linear Display P3 reference",
         danger_palette_matches_linear_display_p3_reference},
       {"native production-window visual artifacts", visual_artifacts}});
}
