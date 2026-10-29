#include "ui/qt/overlay_editor_widget.hpp"
#include "ports/annotation_font_contract.hpp"
#include "test_support.hpp"

#include <QApplication>
#include <QAbstractItemView>
#include <QListView>
#include <QPointer>
#include "ui/qt/annotation_size_combo.hpp"
#include <QDir>
#include <QFileInfo>
#include <QEnterEvent>
#include "ui/qt/toolbar_style.hpp"
#include <QStyle>
#include <QComboBox>
#include <QCoreApplication>
#include <QColor>
#include <QEvent>
#include <QFrame>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QToolButton>

#include <array>
#include <cstdint>
#include <cmath>
#include <numbers>
#include <string>
#include <vector>

namespace {

using namespace hdrshot;

class TestInputPlatformAdapter final : public QtInputPlatformAdapter {
 public:
  Result<QKeySequence, Error> display_global_hotkey(const std::string&) const override {
    return Result<QKeySequence, Error>::success({});
  }

  Result<std::string, Error> canonical_global_hotkey(
      const QKeySequence&) const override {
    return Result<std::string, Error>::success("Command+Shift+2");
  }

  QString fixed_shortcut_label(UiCommand) const override { return {}; }

  std::optional<UiCommand> fixed_overlay_command(
      const QKeyEvent& event,
      const FocusContext focus,
      const CompletionBindings bindings = {}) const override {
    Key key{Key::unknown};
    if (event.key() == Qt::Key_Escape) key = Key::escape;
    if (event.key() == Qt::Key_Return || event.key() == Qt::Key_Enter) key = Key::enter;
    if (event.key() == Qt::Key_S) key = Key::s;
    if (event.key() == Qt::Key_Y) key = Key::y;
    if (event.key() == Qt::Key_Z) key = Key::z;
    const auto modifiers = event.modifiers();
    return InputMapper::map(KeyEvent{
        focus,
        key,
        KeyModifiers{
            (modifiers & Qt::ControlModifier) != 0,
            (modifiers & Qt::AltModifier) != 0,
            (modifiers & Qt::ShiftModifier) != 0,
        },
    }, bindings);
  }
};

class SolidTextRasterizer final : public TextRasterizerPort {
 public:
  Result<TextCoverageMask, Error> rasterize(const TextRasterRequest& request) override {
    ++call_count;
    if (fail) return Result<TextCoverageMask, Error>::failure(
        Error{ErrorCode::invalid_input, "test", Retryability::never, {}});
    const auto sample_count = static_cast<std::size_t>(request.mask_size_px.width) *
        static_cast<std::size_t>(request.mask_size_px.height);
    return Result<TextCoverageMask, Error>::success(TextCoverageMask{
        request.object_id,
        request.mask_size_px,
        std::vector<std::uint8_t>(sample_count, coverage),
        "test-font",
    });
  }

  int call_count{};
  std::uint8_t coverage{255U};
  bool fail{};
};

void send_drag(OverlayEditorWidget& widget, const QPointF start, const QPointF end) {
  QMouseEvent press(
      QEvent::MouseButtonPress, start, start, start,
      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(&widget, &press);
  QMouseEvent move(
      QEvent::MouseMove, end, end, end,
      Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(&widget, &move);
  QMouseEvent release(
      QEvent::MouseButtonRelease, end, end, end,
      Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(&widget, &release);
  QApplication::processEvents();
}

void send_move(OverlayEditorWidget& widget, const QPointF point) {
  QMouseEvent move(
      QEvent::MouseMove, point, point, point,
      Qt::NoButton, Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(&widget, &move);
  QApplication::processEvents();
}

void confirm_text(QLineEdit& editor) {
  QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
  QApplication::sendEvent(&editor, &enter);
  QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  QApplication::processEvents();
}

struct Fixture {
  TestInputPlatformAdapter input_platform;
  SolidTextRasterizer text_rasterizer;
  OverlayEditorWidget widget{
      PixelSize{1000, 600},
      1.0,
      &text_rasterizer,
      "",
      input_platform,
      SelectionSnapshot{SelectionRevision{1}, PixelRect{0, 0, 1000, 600}}};

  Fixture() {
    widget.resize(1000, 600);
    widget.show();
    QApplication::processEvents();
  }

  QToolButton* button(const char* name) {
    auto* result = widget.findChild<QToolButton*>(QString::fromLatin1(name));
    HDRSHOT_CHECK(result != nullptr);
    return result;
  }
};

void brush_preserves_all_events_and_final_double_coordinates() {
  Fixture fixture;
  fixture.widget.set_annotation_preview_coalescing(true);
  fixture.button("toolBrush")->click();
  auto *sizes = fixture.widget.findChild<QComboBox *>();
  HDRSHOT_CHECK(sizes->count() == 6);
  HDRSHOT_CHECK(sizes->currentText() == "24 DIP");
  const auto send = [&](QEvent::Type type, QPointF point) {
    QMouseEvent event(type, point, point, point,
                      type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                      type == QEvent::MouseButtonRelease ? Qt::NoButton
                                                         : Qt::LeftButton,
                      Qt::NoModifier);
    QApplication::sendEvent(&fixture.widget, &event);
  };
  send(QEvent::MouseButtonPress, {100.1, 100.2});
  send(QEvent::MouseMove, {150.2, 100.3});
  send(QEvent::MouseMove, {150.4, 150.5});
  send(QEvent::MouseMove, {100.3, 150.7});
  send(QEvent::MouseButtonRelease, {100.6, 150.9});
  auto snapshot = fixture.widget.annotations();
  HDRSHOT_CHECK(snapshot.objects.size() == 1);
  const auto path = snapshot.objects.front().brush_geometry->path;
  HDRSHOT_CHECK(path->points.size() == 5);
  HDRSHOT_CHECK_NEAR(path->points.back().x, 100.6, 1e-10);
  HDRSHOT_CHECK_NEAR(path->points[1].y, 100.3, 1e-10);
  int actions = 0;
  fixture.widget.set_action_requested([&](auto) { ++actions; });
  send(QEvent::MouseButtonDblClick, {250.2, 220.3});
  send(QEvent::MouseButtonRelease, {250.2, 220.3});
  HDRSHOT_CHECK(fixture.widget.annotations().objects.size() == 2);
  HDRSHOT_CHECK(actions == 0);
  fixture.button("actionUndo")->click();
  HDRSHOT_CHECK(fixture.widget.annotations().objects.size() == 1);
  send(QEvent::MouseButtonPress, {300.1, 300.2});
  send(QEvent::MouseMove, {350.4, 350.7});
  fixture.button("actionSaveDefault")->click();
  HDRSHOT_CHECK(actions == 1);
  HDRSHOT_CHECK(fixture.widget.annotations().objects.size() == 2);
  HDRSHOT_CHECK(fixture.widget.annotations()
                    .objects.back()
                    .brush_geometry->path->points.size() == 2);
}

void fractional_capture_scale_preserves_subpixel_samples() {
  for (double scale : {1., 1.25, 1.5, 2.}) {
    TestInputPlatformAdapter input;
    SolidTextRasterizer text;
    const PixelSize capture{int(1000 * scale), int(600 * scale)};
    OverlayEditorWidget widget{capture, scale, &text, "", input,
        SelectionSnapshot{1, {0, 0, capture.width, capture.height}}};
    widget.resize(1000, 600);
    widget.show();
    QApplication::processEvents();
    widget.findChild<QToolButton *>("toolBrush")->click();
    const auto send = [&](QEvent::Type type, QPointF p) {
      QMouseEvent event(type, p, p, p,
          type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
          type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
          Qt::NoModifier);
      QApplication::sendEvent(&widget, &event);
    };
    send(QEvent::MouseButtonPress, {100.1, 100.1});
    send(QEvent::MouseMove, {100.2, 100.2});
    send(QEvent::MouseButtonRelease, {100.3, 100.3});
    const auto &points = widget.annotations().objects[0].brush_geometry->path->points;
    HDRSHOT_CHECK(points.size() == 3);
    HDRSHOT_CHECK(int(points[0].x) == int(points[1].x));
    HDRSHOT_CHECK(points[0] != points[1]);
    HDRSHOT_CHECK_NEAR(points[0].x, 100.1 * scale, 1e-9);
    HDRSHOT_CHECK_NEAR(points.back().y, 100.3 * scale, 1e-9);
  }
}
void three_dip_size_groups_and_palette_are_independent() {
  Fixture fixture;
  auto *sizes=fixture.widget.findChild<QComboBox *>();
  const auto verify=[&](const char *tool,std::initializer_list<int> values,int selected) {
    fixture.button(tool)->click();
    HDRSHOT_CHECK(sizes->count()==int(values.size()));
    int i=0;
    for(auto value:values) {
      HDRSHOT_CHECK(sizes->itemData(i).toInt()==value);
      HDRSHOT_CHECK(sizes->itemText(i)==QString::number(value)+" DIP");
      ++i;
    }
    HDRSHOT_CHECK(sizes->currentData().toInt()==selected);
  };
  verify("toolBrush",{2,4,24,32,48,72},24);
  sizes->setCurrentIndex(5);
  verify("toolLine",{1,2,3,4,6},2);
  sizes->setCurrentIndex(4);
  verify("toolText",{12,16,20,24,32,48},20);
  sizes->setCurrentIndex(5);
  verify("toolBrush",{2,4,24,32,48,72},72);
  verify("toolArrow",{1,2,3,4,6},6);
  verify("toolText",{12,16,20,24,32,48},48);
  for(const char *color:{"COLORFF4D67","COLORFFC83D","COLORFF8A34","COLOR25C06D","COLOR36A3FF","COLORFFFFFF","COLOR111111"})
    HDRSHOT_CHECK(fixture.button(color)->isVisible());
}

void brush_actions_and_interruptions() {
  for (const char *action : {"actionSaveDefault", "actionSaveAs", "actionCopy",
                             "actionAnalyze", "actionPinScreenshot"}) {
    Fixture fixture;
    fixture.widget.set_analysis_available(true);
    fixture.widget.set_pin_available(true);
    fixture.button("toolBrush")->click();
    int calls = 0;
    fixture.widget.set_action_requested([&](auto) {
      ++calls;
      HDRSHOT_CHECK(fixture.widget.annotations().objects.size() == 1);
      HDRSHOT_CHECK(fixture.widget.annotations()
                        .objects.front()
                        .brush_geometry->path->points.back() ==
                    (StrokePoint{170.4, 150.7}));
    });
    QMouseEvent press(QEvent::MouseButtonPress, {100.2, 100.3}, {100.2, 100.3},
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent move(QEvent::MouseMove, {170.4, 150.7}, {170.4, 150.7},
                     Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&fixture.widget, &press);
    QApplication::sendEvent(&fixture.widget, &move);
    fixture.button(action)->click();
    HDRSHOT_CHECK(calls == 1);
  }
  for (int interruption = 0; interruption < 5; ++interruption) {
    Fixture fixture;
    fixture.widget.set_annotation_preview_coalescing(true);
    fixture.button("toolBrush")->click();
    QMouseEvent press(QEvent::MouseButtonPress, {100, 100}, {100, 100},
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent move(QEvent::MouseMove, {180, 140}, {180, 140}, Qt::NoButton,
                     Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&fixture.widget, &press);
    QApplication::sendEvent(&fixture.widget, &move);
    const auto epoch = fixture.widget.annotation_preview_epoch();
    if (interruption == 0)
      fixture.button("actionUndo")->click();
    else if (interruption == 1)
      fixture.button("toolLine")->click();
    else {
      QEvent event(interruption == 2   ? QEvent::WindowDeactivate
                   : interruption == 3 ? QEvent::Hide
                                       : QEvent::UngrabMouse);
      QApplication::sendEvent(&fixture.widget, &event);
    }
    QApplication::processEvents();
    HDRSHOT_CHECK(fixture.widget.annotations().objects.empty());
    HDRSHOT_CHECK(fixture.widget.annotation_preview_epoch() > epoch);
  }
}

void brush_transform_is_exact_and_undoable() {
  Fixture fixture;
  fixture.button("toolBrush")->click();
  send_drag(fixture.widget, {100.2, 100.3}, {180.4, 150.6});
  fixture.button("toolSelect")->click();
  auto original = fixture.widget.annotations().objects.front();
  send_drag(fixture.widget, {140.3, 125.45}, {140.55, 126.2});
  auto moved = fixture.widget.annotations().objects.front();
  HDRSHOT_CHECK(moved.brush_geometry->path == original.brush_geometry->path);
  HDRSHOT_CHECK_NEAR(moved.brush_geometry->origin_x, .25, 1e-10);
  HDRSHOT_CHECK_NEAR(moved.brush_geometry->origin_y, .75, 1e-10);
  fixture.button("actionUndo")->click();
  HDRSHOT_CHECK(fixture.widget.annotations().objects.front() == original);
  send_drag(fixture.widget, {180.4, 150.6}, {220.7, 180.9});
  const auto resized = fixture.widget.annotations().objects.front();
  HDRSHOT_CHECK(resized.brush_geometry->path == original.brush_geometry->path);
  HDRSHOT_CHECK(resized.brush_geometry->scale_x > 1 &&
                resized.brush_geometry->scale_y > 1);
  HDRSHOT_CHECK(std::get<BrushStyle>(resized.style).diameter_dip == 24);
  fixture.button("actionUndo")->click();
  HDRSHOT_CHECK(fixture.widget.annotations().objects.front() == original);
  fixture.button("COLOR25C06D")->click();
  auto *sizes = fixture.widget.findChild<QComboBox *>();
  sizes->setCurrentIndex(5);
  auto styled = fixture.widget.annotations().objects.front();
  HDRSHOT_CHECK(std::get<BrushStyle>(styled.style).color_srgb_rgb == 0x25C06D);
  HDRSHOT_CHECK(std::get<BrushStyle>(styled.style).diameter_dip == 72);
  fixture.button("actionUndo")->click();
  fixture.button("actionUndo")->click();
  HDRSHOT_CHECK(fixture.widget.annotations().objects.front() == original);
}

void drawing_cursor_does_not_leak_into_controls() {
  Fixture fixture;
  auto* toolbar=fixture.widget.findChild<QFrame*>("editorToolbar");
  auto* properties=fixture.widget.findChild<QFrame*>("propertyBar");
  HDRSHOT_CHECK(toolbar!=nullptr && properties!=nullptr);
  for(const char* tool:{"toolRectangle","toolArrow"}) {
    fixture.button(tool)->click();
    send_move(fixture.widget,{200,200});
    HDRSHOT_CHECK(fixture.widget.cursor().shape()==Qt::CrossCursor);
    for(auto* bar:{toolbar,properties}) {
      HDRSHOT_CHECK(bar->cursor().shape()==Qt::ArrowCursor);
      for(auto* child:bar->findChildren<QWidget*>()) {
        HDRSHOT_CHECK(child->cursor().shape()!=Qt::CrossCursor);
      }
      QMouseEvent hover(QEvent::MouseMove,QPointF(4,4),QPointF(bar->mapToGlobal(QPoint(4,4))),
          Qt::NoButton,Qt::NoButton,Qt::NoModifier);
      QApplication::sendEvent(bar,&hover);
      HDRSHOT_CHECK(bar->cursor().shape()==Qt::ArrowCursor);
    }
    fixture.button("toolSelect")->click();
    send_move(fixture.widget,{200,200});
    HDRSHOT_CHECK(fixture.widget.cursor().shape()==Qt::ArrowCursor);
  }
}

void preview_preserves_partial_coverage() {
  Fixture fixture;
  fixture.text_rasterizer.coverage=128;
  fixture.button("toolText")->click();
  send_drag(fixture.widget,{100,100},{350,180});
  auto* editor=fixture.widget.findChild<QLineEdit*>("annotationTextEditor");
  HDRSHOT_CHECK(editor!=nullptr);
  editor->setText("AA");
  confirm_text(*editor);
  const auto bounds=fixture.widget.annotations().objects.front().bounds;
  QImage pixels(fixture.widget.size(),QImage::Format_ARGB32);
  pixels.fill(Qt::transparent);
  QPainter painter(&pixels);
  fixture.widget.render(&painter);
  painter.end();
  // The fake glyph fills its bounds: sample far from selection handles/UI.
  HDRSHOT_CHECK(qAlpha(pixels.pixel(bounds.x+bounds.width/2,bounds.y+bounds.height/2))==128);
}

void pending_text_requests_the_fixed_variable_font_instance() {
  Fixture fixture;
  fixture.button("toolText")->click();
  send_drag(fixture.widget, {100,100}, {350,180});
  auto* editor = fixture.widget.findChild<QLineEdit*>("annotationTextEditor");
  HDRSHOT_CHECK(editor != nullptr);
  const auto font = editor->font();
  HDRSHOT_CHECK(font.isVariableAxisSet("wght"));
  HDRSHOT_CHECK(font.variableAxisValue("wght") == annotation_font::weight);
  HDRSHOT_CHECK(font.hintingPreference() == QFont::PreferNoHinting);
  HDRSHOT_CHECK((font.styleStrategy() & QFont::NoFontMerging) != 0);
}

void explicit_completion_commits_text_and_failure_preserves_input() {
  for (const auto* action : {"actionSaveDefault", "actionSaveAs", "actionCopy",
                             "actionAnalyze", "actionPinScreenshot"}) {
    Fixture fixture;
    fixture.widget.set_analysis_available(true);
    fixture.widget.set_pin_available(true);
    fixture.button("toolText")->click();
    send_drag(fixture.widget, {100, 100}, {350, 180});
    auto* input = fixture.widget.findChild<QLineEdit*>("annotationTextEditor");
    input->setText("new text");
    int completions = 0;
    fixture.widget.set_action_requested([&](UiCommand command) {
      if (std::string(action) == "actionPinScreenshot") {
        HDRSHOT_CHECK(command == UiCommand::pin_screenshot);
      }
      ++completions;
      HDRSHOT_CHECK(fixture.widget.annotations().objects.size() == 1);
      HDRSHOT_CHECK(fixture.widget.annotations().objects.front().text == "new text");
      HDRSHOT_CHECK(fixture.widget.annotation_render_plan()->source_document_revision ==
          fixture.widget.annotations().revision);
    });
    fixture.text_rasterizer.fail = true;
    fixture.button(action)->click();
    HDRSHOT_CHECK(completions == 0);
    HDRSHOT_CHECK(fixture.widget.annotations().objects.empty());
    HDRSHOT_CHECK(input->text() == "new text");
    fixture.text_rasterizer.fail = false;
    fixture.button(action)->click();
    HDRSHOT_CHECK(completions == 1);
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    HDRSHOT_CHECK(fixture.widget.findChild<QLineEdit*>("annotationTextEditor") == nullptr);
    // A destination chooser may cancel: the View and committed text survive.
    send_drag(fixture.widget, {140, 130}, {140, 130});
    input = fixture.widget.findChild<QLineEdit*>("annotationTextEditor");
    HDRSHOT_CHECK(input != nullptr);
    input->setText("updated text");
    fixture.widget.set_action_requested([&](UiCommand) {
      ++completions;
      HDRSHOT_CHECK(fixture.widget.annotations().objects.front().text == "updated text");
    });
    fixture.button(action)->click();
    HDRSHOT_CHECK(completions == 2);
  }
}

void pending_input_fixed_keys_do_not_complete_capture() {
  Fixture fixture;
  fixture.button("toolText")->click();
  send_drag(fixture.widget, {100, 100}, {350, 180});
  auto* input = fixture.widget.findChild<QLineEdit*>("annotationTextEditor");
  input->setText("pending");
  input->setFocus();
  int actions = 0;
  fixture.widget.set_action_requested([&](UiCommand) { ++actions; });
  for (const auto modifiers : std::array<Qt::KeyboardModifiers, 2>{Qt::ControlModifier, Qt::ControlModifier | Qt::AltModifier}) {
    QKeyEvent key(QEvent::KeyPress, Qt::Key_S, modifiers);
    QApplication::sendEvent(input, &key);
  }
  HDRSHOT_CHECK(actions == 0);
  confirm_text(*input);
  HDRSHOT_CHECK(actions == 0);
}

void empty_text_completion_and_capture_cancel_do_not_create_objects() {
  for (const auto* action : {"actionSaveDefault", "actionSaveAs", "actionCopy",
                             "actionAnalyze", "actionPinScreenshot", "actionCancel"}) {
    Fixture fixture;
    fixture.widget.set_analysis_available(true);
    fixture.widget.set_pin_available(true);
    fixture.button("toolText")->click();
    send_drag(fixture.widget, {100, 100}, {350, 180});
    auto* input = fixture.widget.findChild<QLineEdit*>("annotationTextEditor");
    HDRSHOT_CHECK(input != nullptr);
    if (std::string(action) == "actionCancel") input->setText("discard this");
    int actions = 0;
    fixture.widget.set_action_requested([&](UiCommand) { ++actions; });
    fixture.button(action)->click();
    HDRSHOT_CHECK(actions == 1);
    HDRSHOT_CHECK(fixture.widget.annotations().objects.empty());
    if (std::string(action) != "actionCancel") {
      QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
      HDRSHOT_CHECK(fixture.widget.findChild<QLineEdit*>("annotationTextEditor") == nullptr);
    }
  }
}

void pending_text_owns_property_changes_and_empty_input_is_discarded() {
  Fixture fixture;
  fixture.button("toolRectangle")->click();
  send_drag(fixture.widget, QPointF(80, 80), QPointF(220, 180));
  HDRSHOT_CHECK(fixture.widget.annotations().objects.size() == 1);
  const auto original_rectangle = fixture.widget.annotations().objects.front();

  fixture.button("toolText")->click();
  send_drag(fixture.widget, QPointF(300, 100), QPointF(620, 180));
  auto* editor = fixture.widget.findChild<QLineEdit*>(QStringLiteral("annotationTextEditor"));
  HDRSHOT_CHECK(editor != nullptr);
  HDRSHOT_CHECK(editor->font().weight() == 450);
  HDRSHOT_CHECK(!fixture.widget.annotations().selected_object_id.has_value());

  fixture.button("COLOR25C06D")->click();
  HDRSHOT_CHECK(fixture.widget.annotations().objects.front().style == original_rectangle.style);
  HDRSHOT_CHECK(editor->styleSheet().contains(QStringLiteral("25c06d"), Qt::CaseInsensitive));
  editor->setText(QStringLiteral("当前文字"));
  QMetaObject::invokeMethod(editor, "returnPressed", Qt::DirectConnection);
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  HDRSHOT_CHECK(fixture.widget.annotations().objects.size() == 2);
  const auto& created_text = fixture.widget.annotations().objects.back();
  HDRSHOT_CHECK(created_text.kind == AnnotationKind::text);
  HDRSHOT_CHECK(created_text.bounds == (PixelRect{300, 100, 320, 80}));
  HDRSHOT_CHECK(std::get<TextStyle>(created_text.style).color_srgb_rgb == 0x25C06D);

  send_drag(fixture.widget, QPointF(300, 220), QPointF(620, 300));
  HDRSHOT_CHECK(fixture.widget.findChild<QLineEdit*>(
      QStringLiteral("annotationTextEditor")) != nullptr);
  fixture.button("toolArrow")->click();
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  HDRSHOT_CHECK(fixture.widget.findChild<QLineEdit*>(
      QStringLiteral("annotationTextEditor")) == nullptr);
  HDRSHOT_CHECK(fixture.widget.annotations().objects.size() == 2);
}

void selected_object_exposes_its_own_units_and_toolbar_has_group_separators() {
  Fixture fixture;
  HDRSHOT_CHECK(fixture.widget.findChild<QFrame*>(
      QStringLiteral("separatorAfterSelect")) != nullptr);
  HDRSHOT_CHECK(fixture.widget.findChild<QFrame*>(
      QStringLiteral("separatorBeforeUndo")) != nullptr);
  HDRSHOT_CHECK(fixture.widget.findChild<QFrame*>(
      QStringLiteral("separatorBeforeSave")) != nullptr);

  fixture.button("toolRectangle")->click();
  send_drag(fixture.widget, QPointF(80, 80), QPointF(220, 180));
  fixture.button("toolSelect")->click();
  send_drag(fixture.widget, QPointF(120, 120), QPointF(120, 120));
  auto* property_bar = fixture.widget.findChild<QFrame*>(QStringLiteral("propertyBar"));
  auto* size_combo = fixture.widget.findChild<QComboBox*>();
  HDRSHOT_CHECK(property_bar != nullptr);
  HDRSHOT_CHECK(size_combo != nullptr);
  HDRSHOT_CHECK(property_bar->isVisible());
  HDRSHOT_CHECK(size_combo->currentText().endsWith(QStringLiteral(" DIP")));
}

void icon_toolbar_preserves_accessibility_and_scopes_press_feedback_to_actions() {
  Fixture fixture;
  constexpr std::array<const char *, 7> tool_names{
      "toolSelect", "toolRectangle", "toolEllipse", "toolLine",
      "toolArrow",  "toolText",      "toolBrush"};
  constexpr std::array<const char*, 8> action_names{
      "actionUndo", "actionRedo", "actionSaveDefault",
      "actionSaveAs", "actionCopy", "actionAnalyze", "actionPinScreenshot", "actionCancel"};

  for (const auto* name : tool_names) {
    const auto* button = fixture.button(name);
    HDRSHOT_CHECK(button->text().isEmpty());
    HDRSHOT_CHECK(!button->toolTip().isEmpty());
    HDRSHOT_CHECK(!button->accessibleName().isEmpty());
    HDRSHOT_CHECK(!button->property("toolbarSymbol").toString().isEmpty());
    HDRSHOT_CHECK(!button->property("actionPressFeedback").toBool());
  }
  for (const auto* name : action_names) {
    const auto* button = fixture.button(name);
    HDRSHOT_CHECK(button->text().isEmpty());
    HDRSHOT_CHECK(!button->toolTip().isEmpty());
    HDRSHOT_CHECK(!button->accessibleName().isEmpty());
    HDRSHOT_CHECK(!button->property("toolbarSymbol").toString().isEmpty());
    HDRSHOT_CHECK(button->property("actionPressFeedback").toBool());
    HDRSHOT_CHECK(button->property("pressedSymbolScalePercent").toInt() == 85);
    HDRSHOT_CHECK(button->property("pressedSymbolOffsetPx").toInt() == 2);
  }

  HDRSHOT_CHECK(fixture.button("actionCopy")->property("primary").toBool());
  HDRSHOT_CHECK(fixture.button("actionCancel")->property("cancelAction").toBool());
  const auto* toolbar = fixture.widget.findChild<QFrame*>(QStringLiteral("editorToolbar"));
  HDRSHOT_CHECK(toolbar != nullptr);
  const bool dark = fixture.widget.palette().color(QPalette::Window).lightness() < 128;
  const auto hover = dark ? QStringLiteral("#4D525A") : QStringLiteral("#CBCED3");
  HDRSHOT_CHECK(toolbar->styleSheet().contains(hover));
  // Rendered state regressions below require distinct hover/pressed colors;
  // the previous shared hover color is no longer the interaction contract.
}

void copy_action_has_distinct_rendered_feedback(bool dark) {
  struct PaletteRestore {
    QPalette palette=QApplication::palette();
    ~PaletteRestore() { QApplication::setPalette(palette); }
  } restore;
  auto palette=QApplication::palette();
  palette.setColor(QPalette::Window,dark ? QColor("#26292E") : QColor("#F5F6F8"));
  QApplication::setPalette(palette);
  Fixture fixture;
  auto *button=fixture.button("actionCopy");
  auto background=[&](bool hover,bool pressed) {
    button->setAttribute(Qt::WA_UnderMouse,hover);
    button->setDown(pressed);
    auto *parent=button->parentWidget();
    const auto point=button->mapTo(parent,QPoint(3,button->height()/2));
    const auto image=parent->grab().toImage();
    // Read the composed background away from the centered glyph.
    return image.pixelColor(int(point.x()*image.devicePixelRatio()),
                            int(point.y()*image.devicePixelRatio()));
  };
  const auto normal=background(false,false);
  const auto hover=background(true,false);
  const auto pressed=background(true,true);
  std::cout<<"overlay copy "<<(dark ? "dark" : "light")<<" background "
           <<normal.name().toStdString()<<" / "<<hover.name().toStdString()
           <<" / "<<pressed.name().toStdString()<<'\n';
  HDRSHOT_CHECK(normal!=hover);
  HDRSHOT_CHECK(normal!=pressed);
  HDRSHOT_CHECK(hover!=pressed);
  HDRSHOT_CHECK(background(false,false)==normal);
}

void pin_action_availability_preserves_toolbar_groups_and_dispatches_command() {
  Fixture fixture;
  auto* toolbar = fixture.widget.findChild<QFrame*>("editorToolbar");
  auto* pin = fixture.button("actionPinScreenshot");
  auto* before_pin = fixture.widget.findChild<QFrame*>("separatorBeforePin");
  auto* before_cancel = fixture.widget.findChild<QFrame*>("separatorBeforeCancel");
  HDRSHOT_CHECK(toolbar && before_pin && before_cancel);
  HDRSHOT_CHECK(pin->isHidden() && before_pin->isHidden());
  HDRSHOT_CHECK(before_cancel->isVisible());
  const auto original_width = toolbar->width();

  fixture.widget.set_pin_available(true);
  HDRSHOT_CHECK(pin->isVisible() && before_pin->isVisible());
  HDRSHOT_CHECK(toolbar->width() > original_width);
  HDRSHOT_CHECK(fixture.button("actionAnalyze")->isHidden());
  HDRSHOT_CHECK(pin->toolTip() == QStringLiteral("新建贴图窗口"));
  HDRSHOT_CHECK(pin->accessibleName() == QStringLiteral("新建贴图窗口"));
  HDRSHOT_CHECK(pin->property("toolbarSymbol").toString() == "pin_create");
  HDRSHOT_CHECK(!pin->isCheckable());
  HDRSHOT_CHECK(pin->size() == QSize(38, 38));
  for (auto* separator : {before_pin, before_cancel}) {
    HDRSHOT_CHECK(separator->frameShape() == QFrame::VLine);
    HDRSHOT_CHECK(separator->property("toolbarSeparator").toBool());
    HDRSHOT_CHECK(separator->isVisible());
  }

  fixture.widget.set_analysis_available(true);
  const std::vector<QString> expected{"toolSelect",
                                      "separatorAfterSelect",
                                      "toolRectangle",
                                      "toolEllipse",
                                      "toolLine",
                                      "toolArrow",
                                      "toolText",
                                      "toolBrush",
                                      "separatorBeforeUndo",
                                      "actionUndo",
                                      "actionRedo",
                                      "separatorBeforeAnalyze",
                                      "actionAnalyze",
                                      "separatorBeforeSave",
                                      "actionSaveDefault",
                                      "actionSaveAs",
                                      "actionCopy",
                                      "separatorBeforePin",
                                      "actionPinScreenshot",
                                      "separatorBeforeCancel",
                                      "actionCancel"};
  std::vector<QString> actual;
  for (int index = 0; index < toolbar->layout()->count(); ++index) {
    const auto* item = toolbar->layout()->itemAt(index)->widget();
    HDRSHOT_CHECK(item && item->isVisible());
    actual.push_back(item->objectName());
  }
  HDRSHOT_CHECK(actual == expected);
  HDRSHOT_CHECK(fixture.button("actionCopy")->property("primary").toBool());
  HDRSHOT_CHECK(fixture.button("actionAnalyze")->property("analysisAction").toBool());
  HDRSHOT_CHECK(fixture.button("actionCancel")->property("cancelAction").toBool());

  int calls = 0;
  fixture.widget.set_action_requested([&](UiCommand command) {
    HDRSHOT_CHECK(command == UiCommand::pin_screenshot);
    ++calls;
  });
  fixture.widget.setEnabled(false);
  HDRSHOT_CHECK(!pin->isEnabled());
  pin->click();
  HDRSHOT_CHECK(calls == 0);
  fixture.widget.setEnabled(true);
  pin->click();
  HDRSHOT_CHECK(calls == 1);
  HDRSHOT_CHECK(!pin->isChecked());

  fixture.widget.set_pin_available(false);
  HDRSHOT_CHECK(pin->isHidden() && before_pin->isHidden());
  HDRSHOT_CHECK(before_cancel->isVisible());
  HDRSHOT_CHECK(fixture.button("actionAnalyze")->isVisible());
  fixture.widget.set_analysis_available(false);
  HDRSHOT_CHECK(toolbar->width() == original_width);
}

void pin_action_requires_a_committed_selection() {
  TestInputPlatformAdapter input_platform;
  SolidTextRasterizer text_rasterizer;
  OverlayEditorWidget widget({1000, 600}, 1.0, &text_rasterizer, "", input_platform,
                             {1, {}});
  widget.set_pin_available(true);
  int calls = 0;
  widget.set_action_requested([&](UiCommand) { ++calls; });
  widget.findChild<QToolButton*>("actionPinScreenshot")->click();
  HDRSHOT_CHECK(calls == 0);
  HDRSHOT_CHECK(widget.selection().desktop_rect.empty());
}

void fixed_keys_are_interpreted_by_the_injected_platform_adapter() {
  Fixture fixture;
  std::optional<UiCommand> action;
  fixture.widget.set_action_requested([&action](const UiCommand command) {
    action = command;
  });
  // In Qt/Cocoa, physical Command is ControlModifier. The widget does not
  // interpret that fact itself; the injected adapter maps it to the shared
  // save-as command.
  QKeyEvent save_as(
      QEvent::KeyPress,
      Qt::Key_S,
      Qt::ControlModifier | Qt::AltModifier);
  QApplication::sendEvent(&fixture.widget, &save_as);
  HDRSHOT_CHECK(action == UiCommand::save_as);
}

void configured_enter_and_double_click_actions_reach_the_shell() {
  Fixture fixture;
  std::vector<UiCommand> actions;
  fixture.widget.set_action_requested([&actions](const UiCommand command) {
    actions.push_back(command);
  });
  fixture.widget.set_completion_bindings(CompletionBindings{
      CompletionAction::save_default,
      CompletionAction::save_as,
  });

  QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
  QApplication::sendEvent(&fixture.widget, &enter);
  QMouseEvent double_click(
      QEvent::MouseButtonDblClick,
      QPointF(300, 200),
      QPointF(300, 200),
      QPointF(300, 200),
      Qt::LeftButton,
      Qt::LeftButton,
      Qt::NoModifier);
  QApplication::sendEvent(&fixture.widget, &double_click);

  HDRSHOT_CHECK(actions == std::vector<UiCommand>({
      UiCommand::save_default,
      UiCommand::save_as,
  }));
}

void annotations_use_capture_coordinates_and_drag_is_clamped_to_selection() {
  TestInputPlatformAdapter input_platform;
  OverlayEditorWidget widget{
      PixelSize{1000, 600},
      1.0,
      nullptr,
      "",
      input_platform,
      SelectionSnapshot{SelectionRevision{1}, PixelRect{100, 100, 400, 300}}};
  widget.resize(1000, 600);
  widget.show();
  QApplication::processEvents();
  auto* rectangle = widget.findChild<QToolButton*>(QStringLiteral("toolRectangle"));
  HDRSHOT_CHECK(rectangle != nullptr);
  rectangle->click();

  send_drag(widget, QPointF(40, 40), QPointF(80, 80));
  HDRSHOT_CHECK(widget.annotations().objects.empty());

  std::shared_ptr<const AnnotationRenderPlan> callback_plan;
  widget.set_preview_changed(
      [&callback_plan](
          const SelectionSnapshot&,
          const AnnotationDocumentSnapshot&,
          std::shared_ptr<const AnnotationRenderPlan> plan) {
        callback_plan = std::move(plan);
      });
  send_drag(widget, QPointF(200, 200), QPointF(800, 500));
  HDRSHOT_CHECK(widget.annotations().objects.size() == 1U);
  const auto& bounds = widget.annotations().objects.front().bounds;
  HDRSHOT_CHECK((bounds == PixelRect{200, 200, 300, 200}));
  HDRSHOT_CHECK(bounds.right() <= 500);
  HDRSHOT_CHECK(bounds.bottom() <= 400);
  HDRSHOT_CHECK(callback_plan != nullptr);
  HDRSHOT_CHECK(callback_plan == widget.annotation_render_plan());
  HDRSHOT_CHECK(callback_plan->source_document_revision == widget.annotations().revision);
}

void interaction_gate_rejects_a_second_display_after_target_lock() {
  TestInputPlatformAdapter input_platform;
  OverlayEditorWidget widget{
      PixelSize{1000, 600},
      1.0,
      nullptr,
      "",
      input_platform,
      SelectionSnapshot{SelectionRevision{1}, {}}};
  widget.resize(1000, 600);
  widget.show();
  QApplication::processEvents();
  HDRSHOT_CHECK(widget.annotation_render_plan() != nullptr);
  HDRSHOT_CHECK(widget.annotation_render_plan()->output_size_px == PixelSize{});

  int gate_calls = 0;
  widget.set_interaction_started([&gate_calls] {
    ++gate_calls;
    return false;
  });
  send_drag(widget, QPointF(100, 100), QPointF(500, 350));
  HDRSHOT_CHECK(gate_calls == 1);
  HDRSHOT_CHECK(widget.selection().desktop_rect.empty());
}

void one_selection_changes_only_from_its_border() {
  TestInputPlatformAdapter input_platform;
  OverlayEditorWidget widget{
      PixelSize{1000, 600},
      1.0,
      nullptr,
      "",
      input_platform,
      SelectionSnapshot{SelectionRevision{1}, PixelRect{100, 100, 400, 300}}};
  widget.resize(1000, 600);
  widget.show();
  QApplication::processEvents();

  send_drag(widget, QPointF(200, 200), QPointF(250, 230));
  HDRSHOT_CHECK(widget.selection().desktop_rect == (PixelRect{100, 100, 400, 300}));
  send_drag(widget, QPointF(500, 280), QPointF(620, 280));
  HDRSHOT_CHECK(widget.selection().desktop_rect == (PixelRect{100, 100, 520, 300}));
  const auto adjusted = widget.selection();
  send_drag(widget, QPointF(800, 500), QPointF(900, 550));
  HDRSHOT_CHECK(widget.selection() == adjusted);
}

void selected_annotations_resize_and_crop_cannot_exclude_them() {
  TestInputPlatformAdapter input_platform;
  OverlayEditorWidget widget{
      PixelSize{1000, 600},
      1.0,
      nullptr,
      "",
      input_platform,
      SelectionSnapshot{SelectionRevision{1}, PixelRect{100, 100, 400, 300}}};
  widget.resize(1000, 600);
  widget.show();
  QApplication::processEvents();
  widget.findChild<QToolButton*>(QStringLiteral("toolRectangle"))->click();
  send_drag(widget, QPointF(200, 200), QPointF(350, 300));
  widget.findChild<QToolButton*>(QStringLiteral("toolSelect"))->click();

  send_move(widget, QPointF(350, 250));
  HDRSHOT_CHECK(widget.cursor().shape() == Qt::SizeHorCursor);
  auto* toolbar = widget.findChild<QFrame*>(QStringLiteral("editorToolbar"));
  HDRSHOT_CHECK(toolbar != nullptr);
  HDRSHOT_CHECK(toolbar->y() >= 400);
  HDRSHOT_CHECK(toolbar->geometry().bottom() < widget.height());

  send_drag(widget, QPointF(350, 250), QPointF(400, 250));
  HDRSHOT_CHECK(widget.annotations().objects.front().bounds ==
                (PixelRect{200, 200, 200, 100}));

  send_drag(widget, QPointF(500, 180), QPointF(250, 180));
  HDRSHOT_CHECK(widget.selection().desktop_rect == (PixelRect{100, 100, 301, 300}));
  HDRSHOT_CHECK(widget.annotations().objects.front().bounds.right() <
                widget.selection().desktop_rect.right());

  auto* delete_button = widget.findChild<QToolButton*>(
      QStringLiteral("deleteSelectedAnnotation"));
  HDRSHOT_CHECK(delete_button != nullptr);
  HDRSHOT_CHECK(delete_button->isVisible());
  HDRSHOT_CHECK(delete_button->styleSheet().contains(QStringLiteral("background:")));
  delete_button->click();
  HDRSHOT_CHECK(widget.annotations().objects.empty());
}

void rectangle_and_ellipse_keep_all_eight_resize_handles() {
  struct ResizeCase {
    QPointF start;
    QPointF end;
    PixelRect expected;
  };
  const std::array<ResizeCase, 8> cases{{
      {{200, 200}, {180, 180}, {180, 180, 220, 140}},
      {{300, 200}, {300, 180}, {200, 180, 200, 140}},
      {{400, 200}, {420, 180}, {200, 180, 220, 140}},
      {{400, 260}, {420, 260}, {200, 200, 220, 120}},
      {{400, 320}, {420, 340}, {200, 200, 220, 140}},
      {{300, 320}, {300, 340}, {200, 200, 200, 140}},
      {{200, 320}, {180, 340}, {180, 200, 220, 140}},
      {{200, 260}, {180, 260}, {180, 200, 220, 120}},
  }};

  for (const auto* tool_name : {"toolRectangle", "toolEllipse"}) {
    for (const auto& test_case : cases) {
      Fixture fixture;
      fixture.button(tool_name)->click();
      send_drag(fixture.widget, QPointF(200, 200), QPointF(400, 320));
      fixture.button("toolSelect")->click();
      send_drag(fixture.widget, test_case.start, test_case.end);
      HDRSHOT_CHECK(fixture.widget.annotations().objects.size() == 1U);
      HDRSHOT_CHECK(
          fixture.widget.annotations().objects.front().bounds == test_case.expected);
    }
  }
}

void initial_empty_selection_accepts_roi_then_brush() {
  TestInputPlatformAdapter input_platform;
  SolidTextRasterizer text_rasterizer;
  OverlayEditorWidget widget{PixelSize{1000,600},1.,&text_rasterizer,"",input_platform,
      SelectionSnapshot{SelectionRevision{1},{}}};
  widget.resize(1000,600); widget.show(); QApplication::processEvents();
  send_drag(widget,{100,100},{700,500});
  HDRSHOT_CHECK(widget.selection().desktop_rect==PixelRect(100,100,600,400));
  HDRSHOT_CHECK(widget.annotation_render_plan()!=nullptr);
  HDRSHOT_CHECK(widget.annotation_render_plan()->source_selection_rect_px==widget.selection().desktop_rect);
  widget.findChild<QToolButton*>(QStringLiteral("toolBrush"))->click();
  send_drag(widget,{200,200},{400,300});
  HDRSHOT_CHECK(widget.annotations().objects.size()==1);
  HDRSHOT_CHECK(widget.annotations().objects.front().kind==AnnotationKind::brush);
}

void invalid_selection_candidate_does_not_poison_later_drawing() {
  TestInputPlatformAdapter input_platform;
  SolidTextRasterizer text_rasterizer;
  OverlayEditorWidget widget{
      PixelSize{1000, 600},
      1.0,
      &text_rasterizer,
      "",
      input_platform,
      SelectionSnapshot{SelectionRevision{1}, PixelRect{100, 100, 600, 400}}};
  widget.resize(1000, 600);
  widget.show();
  QApplication::processEvents();

  widget.findChild<QToolButton*>(QStringLiteral("toolEllipse"))->click();
  send_drag(widget, QPointF(250, 200), QPointF(450, 320));
  widget.findChild<QToolButton*>(QStringLiteral("toolSelect"))->click();
  const auto plan_before_crop = widget.annotation_render_plan();
  HDRSHOT_CHECK(plan_before_crop != nullptr);

  // The requested right edge would exclude the ellipse. The selection must
  // stop at the authoritative coverage boundary without exposing a failed
  // candidate as live state.
  send_drag(widget, QPointF(700, 350), QPointF(300, 350));
  HDRSHOT_CHECK(widget.selection().desktop_rect.right() >=
                widget.annotations().objects.front().bounds.right());
  HDRSHOT_CHECK(widget.annotation_render_plan() != nullptr);
  HDRSHOT_CHECK(widget.annotation_render_plan()->source_selection_rect_px ==
                widget.selection().desktop_rect);
  HDRSHOT_CHECK(widget.annotation_render_plan()->source_document_revision ==
                widget.annotations().revision);
  const auto* status = widget.findChild<QLabel*>(QStringLiteral("exportStatus"));
  HDRSHOT_CHECK(status != nullptr);
  HDRSHOT_CHECK(!status->isVisible());

  widget.findChild<QToolButton*>(QStringLiteral("toolRectangle"))->click();
  send_drag(widget, QPointF(120, 120), QPointF(190, 170));
  HDRSHOT_CHECK(widget.annotations().objects.size() == 2U);
}

void previews_rerasterize_only_the_adjusted_object_and_rebase_selection() {
  Fixture fixture;
  fixture.button("toolText")->click();
  send_drag(fixture.widget, QPointF(80, 80), QPointF(180, 130));
  auto* editor = fixture.widget.findChild<QLineEdit*>(
      QStringLiteral("annotationTextEditor"));
  HDRSHOT_CHECK(editor != nullptr);
  editor->setText(QStringLiteral("stable"));
  confirm_text(*editor);

  fixture.button("toolEllipse")->click();
  send_drag(fixture.widget, QPointF(250, 180), QPointF(700, 430));
  fixture.button("toolSelect")->click();
  send_drag(fixture.widget, QPointF(400, 300), QPointF(400, 300));
  const auto calls_before_resize = fixture.text_rasterizer.call_count;
  send_drag(fixture.widget, QPointF(700, 430), QPointF(760, 470));
  // Pointer preview and final commit both reuse the unchanged text coverage.
  HDRSHOT_CHECK(fixture.text_rasterizer.call_count == calls_before_resize);

  const auto calls_before_selection = fixture.text_rasterizer.call_count;
  send_drag(fixture.widget, QPointF(1000, 500), QPointF(900, 500));
  HDRSHOT_CHECK(fixture.text_rasterizer.call_count == calls_before_selection);
}

void selected_annotation_moves_without_moving_selection_and_is_clamped() {
  TestInputPlatformAdapter input_platform;
  OverlayEditorWidget widget{
      PixelSize{1000, 600},
      1.0,
      nullptr,
      "",
      input_platform,
      SelectionSnapshot{SelectionRevision{1}, PixelRect{100, 100, 400, 300}}};
  widget.resize(1000, 600);
  widget.show();
  QApplication::processEvents();
  widget.findChild<QToolButton*>(QStringLiteral("toolRectangle"))->click();
  send_drag(widget, QPointF(200, 200), QPointF(350, 300));
  widget.findChild<QToolButton*>(QStringLiteral("toolSelect"))->click();
  const auto selection_before = widget.selection();

  send_move(widget, QPointF(275, 250));
  HDRSHOT_CHECK(widget.cursor().shape() == Qt::SizeAllCursor);
  send_drag(widget, QPointF(275, 250), QPointF(0, 0));

  HDRSHOT_CHECK(widget.selection() == selection_before);
  HDRSHOT_CHECK(widget.annotations().objects.front().bounds ==
                (PixelRect{100, 100, 150, 100}));
}

void arrow_endpoints_move_independently_and_tiny_shapes_are_discarded() {
  Fixture fixture;
  fixture.button("toolRectangle")->click();
  send_drag(fixture.widget, QPointF(100, 100), QPointF(102, 102));
  HDRSHOT_CHECK(fixture.widget.annotations().objects.empty());

  fixture.button("toolArrow")->click();
  send_drag(fixture.widget, QPointF(100, 100), QPointF(300, 200));
  HDRSHOT_CHECK(fixture.widget.annotations().objects.size() == 1U);
  fixture.button("toolSelect")->click();
  send_move(fixture.widget, QPointF(100, 100));
  HDRSHOT_CHECK(fixture.widget.cursor().shape() == Qt::CrossCursor);
  send_drag(fixture.widget, QPointF(100, 100), QPointF(150, 80));
  const auto geometry = fixture.widget.annotations().objects.front().arrow_geometry;
  HDRSHOT_CHECK(geometry.has_value());
  HDRSHOT_CHECK(geometry->start == (PixelPoint{150, 80}));
  HDRSHOT_CHECK(geometry->end == (PixelPoint{300, 200}));
}

void finishing_text_consumes_the_click_before_another_text_box_can_start() {
  Fixture fixture;
  fixture.button("toolText")->click();
  send_drag(fixture.widget, QPointF(200, 160), QPointF(200, 160));
  auto* editor = fixture.widget.findChild<QLineEdit*>(QStringLiteral("annotationTextEditor"));
  HDRSHOT_CHECK(editor != nullptr);
  editor->setText(QStringLiteral("existing text"));
  confirm_text(*editor);
  HDRSHOT_CHECK(fixture.widget.annotations().objects.size() == 1U);

  send_drag(fixture.widget, QPointF(220, 180), QPointF(220, 180));
  editor = fixture.widget.findChild<QLineEdit*>(QStringLiteral("annotationTextEditor"));
  HDRSHOT_CHECK(editor != nullptr);
  HDRSHOT_CHECK(editor->text() == QStringLiteral("existing text"));

  send_drag(fixture.widget, QPointF(700, 420), QPointF(700, 420));
  QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  HDRSHOT_CHECK(fixture.widget.annotations().objects.size() == 1U);
  HDRSHOT_CHECK(fixture.widget.findChild<QLineEdit*>(
      QStringLiteral("annotationTextEditor")) == nullptr);

  send_drag(fixture.widget, QPointF(700, 420), QPointF(700, 420));
  HDRSHOT_CHECK(fixture.widget.findChild<QLineEdit*>(
      QStringLiteral("annotationTextEditor")) != nullptr);
  HDRSHOT_CHECK(fixture.widget.annotations().objects.size() == 1U);
}

struct WindowFixture {
  TestInputPlatformAdapter input;
  SolidTextRasterizer raster;
  OverlayEditorWidget widget{{1000, 600}, 2, &raster, "", input, {1, {}}};
  WindowFixture() {
    widget.resize(500, 300);
    widget.set_window_candidates(std::make_shared<const WindowSnapshot>(WindowSnapshot{{1}, {2}, 3,
        {{5, {7}, {100, 100, 600, 300}, 0}}}), {7});
    widget.show();
    QApplication::processEvents();
  }
};
void send_button(OverlayEditorWidget& widget, QEvent::Type type, QPointF point) {
  QMouseEvent event(type, point, point, point, Qt::LeftButton,
      type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(&widget, &event);
}
QPointF widget_point(const OverlayEditorWidget& widget, ShapePoint point, int scale=1) {
  (void)widget;
  return {point.x/scale,point.y/scale};
}

void rotation_gesture_commits_exact_preview_and_one_history_step() {
  for(const auto* tool : {"toolRectangle","toolEllipse"}) {
    Fixture fixture;
    auto& widget=fixture.widget;
    fixture.button(tool)->click();
    send_drag(widget,{300,180},{500,300});
    fixture.button("toolSelect")->click();
    const auto original=widget.annotations().objects.front();
    const auto center=shape_center(original);
    const QPointF start(center.x+115,center.y),end(center.x,center.y+115);
    send_move(widget,start);
    HDRSHOT_CHECK(widget.cursor().shape()==Qt::BitmapCursor);
    send_button(widget,QEvent::MouseButtonPress,start);
    send_move(widget,end);
    HDRSHOT_CHECK(widget.annotations().objects.front()==original); // transient only
    const auto red_coverage = [&]() {
      QImage image(widget.size(),QImage::Format_ARGB32_Premultiplied);
      image.fill(Qt::transparent);
      QPainter painter(&image); widget.render(&painter); painter.end();
      std::vector<QRgb> pixels;
      for(int y=110;y<360;++y) for(int x=270;x<540;++x) {
        const auto pixel=image.pixel(x,y);
        pixels.push_back(qRed(pixel)>qGreen(pixel)*1.5 && qRed(pixel)>qBlue(pixel)*1.2 ? pixel : 0);
      }
      return pixels;
    };
    const auto visible_preview=red_coverage();
    send_button(widget,QEvent::MouseButtonRelease,end);
    HDRSHOT_CHECK(red_coverage()==visible_preview);
    const auto rotated=widget.annotations().objects.front();
    HDRSHOT_CHECK(std::abs(rotated.transform.rotation_radians-std::numbers::pi/2)<1e-7);
    HDRSHOT_CHECK(rotated.bounds==original.bounds);
    const auto expected=AnnotationRenderPlanner::build(widget.annotations(),widget.selection().desktop_rect,1);
    HDRSHOT_CHECK(expected && *widget.annotation_render_plan()==expected.value());
    fixture.button("actionUndo")->click();
    HDRSHOT_CHECK(widget.annotations().objects.front()==original);
    fixture.button("actionRedo")->click();
    HDRSHOT_CHECK(widget.annotations().objects.front()==rotated);
    const auto handles=shape_handles(rotated);
    send_move(widget,widget_point(widget,handles[3]));
    HDRSHOT_CHECK(widget.cursor().shape()==Qt::SizeVerCursor);
    const auto moved_handle=shape_to_world(rotated,{130,0});
    send_drag(widget,widget_point(widget,handles[3]),widget_point(widget,moved_handle));
    const auto resized=widget.annotations().objects.front();
    HDRSHOT_CHECK(resized.bounds.width==230 && resized.bounds.height==120);
    HDRSHOT_CHECK(resized.transform.rotation_radians==rotated.transform.rotation_radians);
    const auto opposite=shape_handles(resized)[7];
    HDRSHOT_CHECK(std::hypot(opposite.x-handles[7].x,opposite.y-handles[7].y)<1e-6);
    const auto image=qgetenv("HDRSHOT_ROTATION_SCREENSHOT");
    if (!image.isEmpty() && tool==std::string("toolEllipse")) {
      const auto radius=resized.bounds.width*0.5+15;
      send_drag(widget,widget_point(widget,shape_to_world(resized,{radius,0})),
          widget_point(widget,shape_to_world(resized,{radius*std::cos(-0.6),radius*std::sin(-0.6)})));
      QImage visual(widget.size(),QImage::Format_ARGB32_Premultiplied);
      visual.fill(QColor(70,70,70));
      QPainter painter(&visual); widget.render(&painter); painter.end();
      HDRSHOT_CHECK(visual.save(QString::fromUtf8(image)));
    }
  }
}

void rotation_at_retina_scale_and_cancel_retry() {
  TestInputPlatformAdapter adapter;
  OverlayEditorWidget widget{{2000,1200},2,nullptr,"",adapter,{1,{0,0,2000,1200}}};
  widget.resize(1000,600); widget.show(); QApplication::processEvents();
  widget.findChild<QToolButton*>("toolEllipse")->click();
  send_drag(widget,{300,200},{500,300});
  widget.findChild<QToolButton*>("toolSelect")->click();
  const auto original=widget.annotations().objects.front();
  send_move(widget,{515,250}); HDRSHOT_CHECK(widget.cursor().shape()==Qt::BitmapCursor);
  send_move(widget,{524,250}); HDRSHOT_CHECK(widget.cursor().shape()==Qt::BitmapCursor);
  send_move(widget,{536,250}); HDRSHOT_CHECK(widget.cursor().shape()==Qt::BitmapCursor);
  send_move(widget,{546,250}); HDRSHOT_CHECK(widget.cursor().shape()!=Qt::BitmapCursor);
  send_move(widget,{505,250}); HDRSHOT_CHECK(widget.cursor().shape()==Qt::SizeHorCursor);
  send_button(widget,QEvent::MouseButtonPress,{536,250});
  send_move(widget,{400,386});
  QEvent lost(QEvent::UngrabMouse); QApplication::sendEvent(&widget,&lost);
  send_button(widget,QEvent::MouseButtonRelease,{400,386});
  HDRSHOT_CHECK(widget.annotations().objects.front()==original);
  send_drag(widget,{536,250},{400,386});
  HDRSHOT_CHECK(std::abs(widget.annotations().objects.front().transform.rotation_radians-std::numbers::pi/2)<1e-7);
  HDRSHOT_CHECK(widget.annotations().objects.front().bounds==original.bounds);
}

void rotation_hot_zone_does_not_steal_other_objects_or_controls() {
  Fixture fixture;
  fixture.button("toolRectangle")->click();
  send_drag(fixture.widget,{300,180},{500,300});
  send_drag(fixture.widget,{510,210},{580,290});
  fixture.button("toolSelect")->click();
  send_drag(fixture.widget,{390,220},{390,220}); // select first
  send_move(fixture.widget,{520,250}); // first's rotation band, second's content
  HDRSHOT_CHECK(fixture.widget.cursor().shape()!=Qt::BitmapCursor);
  send_drag(fixture.widget,{530,250},{530,250});
  HDRSHOT_CHECK(fixture.widget.annotations().selected_object_id==ObjectId{2});
}

void rotated_crop_can_shrink_past_unrotated_bounds_and_keep_editing() {
  Fixture fixture;
  auto& widget=fixture.widget;
  fixture.button("toolEllipse")->click();
  send_drag(widget,{300,180},{500,300});
  fixture.button("toolSelect")->click();
  send_drag(widget,{515,240},{400,355});
  const auto rotated=widget.annotations().objects.front();
  send_drag(widget,{1000,400},{440,400});
  HDRSHOT_CHECK(widget.selection().desktop_rect.right()<rotated.bounds.right());
  HDRSHOT_CHECK(widget.annotations().objects.front()==rotated);
  const auto expected=AnnotationRenderPlanner::build(widget.annotations(),widget.selection().desktop_rect,1);
  HDRSHOT_CHECK(expected.has_value());
  HDRSHOT_CHECK(AnnotationRenderPlanner::build_pixel_plan(expected.value()).value()==
      AnnotationRenderPlanner::build_pixel_plan(*widget.annotation_render_plan()).value());
  fixture.button("toolRectangle")->click();
  send_drag(widget,{120,380},{230,430});
  HDRSHOT_CHECK(widget.annotations().objects.size()==2);
}

void window_hover_is_not_a_selection_or_export_and_click_confirms() {
  WindowFixture fixture;
  auto& widget = fixture.widget;
  int exports = 0;
  widget.set_action_requested([&](UiCommand) { ++exports; });
  send_move(widget, {100, 100});
  HDRSHOT_CHECK(widget.selection().desktop_rect.empty());
  HDRSHOT_CHECK(widget.initial_highlight().rect == (PixelRect{100, 100, 600, 300}));
  HDRSHOT_CHECK(widget.annotation_render_plan()->source_selection_rect_px.empty());
  HDRSHOT_CHECK(!widget.findChild<QFrame*>("editorToolbar")->isVisible());
  widget.findChild<QToolButton*>("actionCopy")->click();
  QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
  QApplication::sendEvent(&widget, &enter);
  HDRSHOT_CHECK(exports == 0 && fixture.raster.call_count == 0);
  send_button(widget, QEvent::MouseButtonPress, {100, 100});
  HDRSHOT_CHECK(widget.selection().desktop_rect.empty());
  send_button(widget, QEvent::MouseButtonRelease, {102, 101});
  HDRSHOT_CHECK(widget.selection().desktop_rect == (PixelRect{100, 100, 600, 300}));
  HDRSHOT_CHECK(widget.initial_highlight().rect.empty());
  HDRSHOT_CHECK(widget.annotation_render_plan()->source_selection_rect_px == widget.selection().desktop_rect);
  HDRSHOT_CHECK(widget.findChild<QFrame*>("editorToolbar")->isVisible());
  send_button(widget, QEvent::MouseButtonDblClick, {102, 101});
  HDRSHOT_CHECK(exports == 0);
  send_button(widget, QEvent::MouseButtonRelease, {102, 101});
  // A later complete click sequence retains the configured double-click action.
  send_button(widget, QEvent::MouseButtonPress, {180, 130});
  send_button(widget, QEvent::MouseButtonRelease, {180, 130});
  send_button(widget, QEvent::MouseButtonDblClick, {180, 130});
  HDRSHOT_CHECK(exports == 1);
}
void initial_cursor_is_crosshair_without_move_or_press() {
  WindowFixture fixture;
  auto& widget = fixture.widget;
  HDRSHOT_CHECK(widget.cursor().shape() == Qt::CrossCursor);
  // A candidate edge is not yet a resizable selection.
  widget.refresh_window_candidate({50, 50});
  HDRSHOT_CHECK(widget.cursor().shape() == Qt::CrossCursor);
  QEvent leave(QEvent::Leave);
  QApplication::sendEvent(&widget, &leave);
  QEnterEvent enter({50, 50}, {50, 50}, {50, 50});
  QApplication::sendEvent(&widget, &enter);
  HDRSHOT_CHECK(widget.cursor().shape() == Qt::CrossCursor);
  widget.restore_interaction_focus({100, 100});
  HDRSHOT_CHECK(widget.cursor().shape() == Qt::CrossCursor);
  send_button(widget, QEvent::MouseButtonPress, {100, 100});
  HDRSHOT_CHECK(widget.cursor().shape() == Qt::CrossCursor);
  send_button(widget, QEvent::MouseButtonRelease, {100, 100});
  HDRSHOT_CHECK(!widget.selection().desktop_rect.empty());
  HDRSHOT_CHECK(widget.cursor().shape() == Qt::ArrowCursor);
}

void initial_cursor_covers_manual_fallback_and_release() {
  TestInputPlatformAdapter input;
  SolidTextRasterizer raster;
  for (bool with_empty_catalog : {false, true}) {
    OverlayEditorWidget widget{{1000, 600}, 2, &raster, "", input, {1, {}}};
    widget.resize(500, 300);
    if (with_empty_catalog) widget.set_window_candidates(
        std::make_shared<const WindowSnapshot>(WindowSnapshot{{1}, {2}, 3, {}}), {7});
    widget.show();
    HDRSHOT_CHECK(widget.cursor().shape() == Qt::CrossCursor);
    send_drag(widget, {100, 100}, {100, 100});
    HDRSHOT_CHECK(widget.selection().desktop_rect.empty());
    HDRSHOT_CHECK(widget.cursor().shape() == Qt::CrossCursor);
    send_button(widget, QEvent::MouseButtonPress, {100, 100});
    send_move(widget, {180, 180});
    HDRSHOT_CHECK(widget.cursor().shape() == Qt::CrossCursor);
    send_button(widget, QEvent::MouseButtonRelease, {180, 180});
    HDRSHOT_CHECK(!widget.selection().desktop_rect.empty());
    HDRSHOT_CHECK(widget.cursor().shape() == Qt::SizeFDiagCursor);
  }
}
void window_manual_drag_uses_anchor_and_cancel_allows_retry() {
  WindowFixture fixture;
  auto& widget = fixture.widget;
  send_button(widget, QEvent::MouseButtonPress, {100, 100});
  send_move(widget, {180, 180});
  HDRSHOT_CHECK(widget.selection().desktop_rect.empty());
  HDRSHOT_CHECK(widget.initial_highlight().rect == (PixelRect{200, 200, 160, 160}));
  QEvent lost(QEvent::UngrabMouse);
  QApplication::sendEvent(&widget, &lost);
  HDRSHOT_CHECK(widget.initial_highlight().rect.empty());
  send_button(widget, QEvent::MouseButtonRelease, {180, 180});
  HDRSHOT_CHECK(widget.selection().desktop_rect.empty());
  send_drag(widget, {100, 100}, {200, 160});
  HDRSHOT_CHECK(widget.selection().desktop_rect == (PixelRect{200, 200, 200, 120}));
  // Existing annotation creation and crop protection operate on this same ROI.
  widget.findChild<QToolButton*>("toolRectangle")->click();
  send_drag(widget, {120, 120}, {150, 140});
  HDRSHOT_CHECK(widget.annotations().objects.size() == 1);
  HDRSHOT_CHECK(widget.annotations().objects[0].bounds == (PixelRect{240, 240, 60, 40}));
}
void leaving_candidate_clears_preview_without_touching_selection() {
  WindowFixture fixture;
  send_move(fixture.widget, {100, 100});
  const auto revision = fixture.widget.initial_highlight().revision;
  QEvent leave(QEvent::Leave);
  QApplication::sendEvent(&fixture.widget, &leave);
  HDRSHOT_CHECK(fixture.widget.initial_highlight().rect.empty());
  HDRSHOT_CHECK(fixture.widget.initial_highlight().revision > revision);
  HDRSHOT_CHECK(fixture.widget.selection().desktop_rect.empty());
  send_move(fixture.widget, {100, 100});
  HDRSHOT_CHECK(!fixture.widget.initial_highlight().rect.empty());
  QApplication::sendEvent(&fixture.widget, &leave);
  QEnterEvent enter({100,100}, {100,100}, {100,100});
  QApplication::sendEvent(&fixture.widget, &enter);
  HDRSHOT_CHECK(!fixture.widget.initial_highlight().rect.empty());
}
void annotation_moves_coalesce_and_release_keeps_final_coordinate() {
  TestInputPlatformAdapter input;
  SolidTextRasterizer text;
  OverlayEditorWidget editor({600, 400}, 1.0, &text, "", input,
                             {1, {0, 0, 600, 400}});
  editor.resize(600, 400);
  editor.set_clean_content_presented(true);
  editor.set_annotation_preview_coalescing(true);
  editor.show();
  QApplication::processEvents();
  int previews = 0;
  editor.set_preview_changed([&](auto&, auto&, auto) { ++previews; });
  editor.findChild<QToolButton*>("toolArrow")->click();
  const auto mouse = [&](QEvent::Type type, QPointF p) {
    QMouseEvent e(
        type, p, p, type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
        type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
        Qt::NoModifier);
    QApplication::sendEvent(&editor, &e);
  };
  mouse(QEvent::MouseButtonPress, {60, 60});
  for (int i = 0; i < 10; ++i)
    mouse(QEvent::MouseMove, {double(100 + i * 10), double(90 + i * 5)});
  HDRSHOT_CHECK(previews == 0);
  QApplication::processEvents();
  HDRSHOT_CHECK(previews == 1);
  mouse(QEvent::MouseMove, {250, 170});
  mouse(QEvent::MouseButtonRelease, {350, 220});
  HDRSHOT_CHECK(editor.annotations().objects.size() == 1);
  HDRSHOT_CHECK((editor.annotations().objects.front().arrow_geometry->end ==
                 PixelPoint{350, 220}));
  const auto committed = previews;
  QApplication::processEvents();
  HDRSHOT_CHECK(previews == committed);
  mouse(QEvent::MouseButtonPress, {100, 100});
  mouse(QEvent::MouseMove, {400, 250});
  editor.interrupt_pointer_gesture();
  const auto interrupted = previews;
  QApplication::processEvents();
  HDRSHOT_CHECK(previews == interrupted);
  HDRSHOT_CHECK(editor.annotations().objects.size() == 1);
}

}  // namespace

void trial_brush_cursor_area_and_cleanup() {
  TestInputPlatformAdapter input;
  SolidTextRasterizer rasterizer;
  OverlayEditorWidget widget{{1000, 600}, 1.0, &rasterizer, "", input,
      SelectionSnapshot{SelectionRevision{1}, PixelRect{100, 100, 500, 300}}};
  widget.resize(1000, 600);
  widget.show();
  QApplication::processEvents();
  auto* brush = widget.findChild<QToolButton*>("toolBrush");
  auto* line = widget.findChild<QToolButton*>("toolLine");
  brush->click();
  send_move(widget, {650, 200});
  const auto clear = widget.grab().toImage().copy(180, 180, 40, 40);
  const auto erased = [&] { return widget.grab().toImage().copy(180, 180, 40, 40) == clear; };
  send_move(widget, {200, 200});
  HDRSHOT_CHECK(widget.cursor().shape() == Qt::CrossCursor);
  HDRSHOT_CHECK(!erased());
  send_move(widget, {650, 200});
  HDRSHOT_CHECK(widget.cursor().shape() == Qt::ArrowCursor);
  HDRSHOT_CHECK(erased());
  send_button(widget, QEvent::MouseButtonPress, {650, 200});
  send_button(widget, QEvent::MouseButtonRelease, {650, 200});
  HDRSHOT_CHECK(widget.annotations().objects.empty());
  send_move(widget, {200, 200});
  QEvent leave(QEvent::Leave);
  QApplication::sendEvent(&widget, &leave);
  HDRSHOT_CHECK(erased());
  send_move(widget, {200, 200});
  const auto child_point = brush->mapTo(&widget, brush->rect().center());
  send_move(widget, child_point);
  HDRSHOT_CHECK(erased());
  send_move(widget, {200, 200});
  widget.set_interaction_locked(true);
  HDRSHOT_CHECK(widget.cursor().shape() == Qt::ArrowCursor);
  HDRSHOT_CHECK(erased());
  widget.set_interaction_locked(false);
  send_move(widget, {200, 200});
  line->click();
  HDRSHOT_CHECK(erased());
  brush->click();
  send_move(widget, {600, 400});  // existing inclusive capture boundary
  HDRSHOT_CHECK(widget.cursor().shape() == Qt::CrossCursor);
  send_move(widget, {600.25, 400});
  HDRSHOT_CHECK(widget.cursor().shape() == Qt::ArrowCursor);
}

void trial_popup_width_and_input() {
  Fixture fixture;
  fixture.button("toolBrush")->click();
  auto* combo = fixture.widget.findChild<QComboBox*>();
  const auto width = combo->width();
  const int initial = combo->currentIndex();
  QKeyEvent down(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
  QApplication::sendEvent(combo, &down);
  HDRSHOT_CHECK(combo->currentIndex() == initial + 1);
  combo->showPopup();
  QApplication::processEvents();
  auto* shell = QApplication::activePopupWidget();
  HDRSHOT_CHECK(shell != nullptr && shell->objectName() == "annotationSizePopup");
  auto* view = shell->findChild<QListView*>();
  HDRSHOT_CHECK(view != nullptr && view->model() == combo->model());
  int widest_text = 0;
  for (int row = 0; row < combo->count(); ++row) {
    auto font = combo->font();
    const auto role = combo->itemData(row, Qt::FontRole);
    if (role.canConvert<QFont>()) font = qvariant_cast<QFont>(role).resolve(font);
    const auto advance = QFontMetrics(font).horizontalAdvance(combo->itemText(row));
    std::cout << "size popup text " << combo->itemText(row).toStdString() << " width=" << advance << '\n';
    widest_text = std::max(widest_text, advance);
  }
  HDRSHOT_CHECK(shell->width() == widest_text + 32 + 18);
  QImage rendered(shell->size(), QImage::Format_ARGB32_Premultiplied);
  rendered.fill(Qt::transparent);
  shell->render(&rendered, QPoint(), QRegion(), QWidget::DrawChildren);
  HDRSHOT_CHECK(rendered.pixelColor(0,0).alpha() == 0);
  HDRSHOT_CHECK(rendered.pixelColor(rendered.width()-1,rendered.height()-1).alpha() == 0);
  const auto point = view->visualRect(combo->model()->index(4, 0)).center();
  const auto global = view->viewport()->mapToGlobal(point);
  QMouseEvent press(QEvent::MouseButtonPress, point, global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  QMouseEvent release(QEvent::MouseButtonRelease, point, global, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(view->viewport(), &press);
  QApplication::sendEvent(view->viewport(), &release);
  HDRSHOT_CHECK(combo->currentData().toInt() == 48);
  combo->hidePopup();
  HDRSHOT_CHECK(combo->width() == width);
  combo->showPopup();
  shell = QApplication::activePopupWidget();
  view = shell->findChild<QListView*>();
  QKeyEvent up(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
  QApplication::sendEvent(view, &up);
  QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
  QApplication::sendEvent(view, &escape);
  HDRSHOT_CHECK(!shell->isVisible() && combo->currentData().toInt() == 48);
  fixture.button("toolLine")->click();
  HDRSHOT_CHECK(combo->count() == 5);
  combo->showPopup();
  shell = QApplication::activePopupWidget();
  view = shell->findChild<QListView*>();
  HDRSHOT_CHECK(view->model()->rowCount() == 5);
  combo->hidePopup();
  fixture.button("toolText")->click();
  HDRSHOT_CHECK(combo->count() == 6);
  combo->showPopup();
  shell = QApplication::activePopupWidget();
  HDRSHOT_CHECK(shell != nullptr);
  fixture.widget.hide();
  HDRSHOT_CHECK(!shell->isVisible());
  combo->setEnabled(false);
  combo->showPopup();
  HDRSHOT_CHECK(!shell->isVisible());
  auto* temporary = new AnnotationSizeCombo;
  temporary->addItem("24 DIP", 24);
  temporary->show();
  temporary->showPopup();
  QPointer<QWidget> owned_popup = QApplication::activePopupWidget();
  HDRSHOT_CHECK(owned_popup != nullptr);
  delete temporary;
  HDRSHOT_CHECK(owned_popup.isNull());
}

void closed_picker_approved_paint_and_state_recovery() {
  const auto output = qEnvironmentVariable("SERIOUSSHOT_CLOSED_PICKER_IMAGES");
  const auto reference = qEnvironmentVariable("SERIOUSSHOT_CLOSED_PICKER_REFERENCE_DIR");
  if (!reference.isEmpty()) {
    for (const auto* theme : {"light", "dark"}) for (int value : {2, 24}) for (int state : {0, 1, 3}) {
      const auto path = QDir(reference).filePath(QString("%1_%2_%3_candidate.png").arg(theme).arg(value).arg(state));
      const QFileInfo file(path);
      const QImage expected(path);
      if (!file.isFile() || !file.isReadable() || expected.isNull()) {
        std::cerr << "closed picker reference missing, unreadable or invalid: " << path.toStdString()
                  << "; set SERIOUSSHOT_CLOSED_PICKER_REFERENCE_DIR to the approved reference directory\n";
        HDRSHOT_CHECK(false);
      }
    }
  }
  if (!output.isEmpty()) QDir().mkpath(output);
  for (bool dark : {false, true}) {
    QFrame context;
    context.setObjectName("propertyBar");
    auto palette = QApplication::palette();
    palette.setColor(QPalette::Window, dark ? QColor("#26292E") : QColor("#F5F6F8"));
    context.setPalette(palette);
    context.setStyleSheet(toolbar_style::sheet(dark));
    AnnotationSizeCombo combo(&context);
    for (int value : {2, 4, 24, 32, 48, 72}) combo.addItem(QString::number(value) + " DIP", value);
    combo.setMinimumWidth(76); combo.setFixedHeight(28); combo.resize(76, 28);
    context.ensurePolished(); combo.ensurePolished();
    const auto capture = [&](int value, int state) {
      QImage image(combo.size() * 2, QImage::Format_ARGB32_Premultiplied);
      image.setDevicePixelRatio(2); image.fill(Qt::transparent);
      combo.render(&image, QPoint(), QRegion(), QWidget::DrawChildren);
      const auto stem = QString("%1_%2_%3").arg(dark ? "dark" : "light").arg(value).arg(state);
      if (!output.isEmpty()) HDRSHOT_CHECK(image.save(output + "/" + stem + "_production.png"));
      if (!reference.isEmpty() && (state == 0 || state == 1 || state == 3)) {
        QImage approved(QDir(reference).filePath(stem + "_candidate.png"));
        HDRSHOT_CHECK(!approved.isNull() && approved.size() == image.size());
        approved = approved.convertToFormat(image.format());
        std::size_t differences = 0;
        for (int y = 0; y < image.height(); ++y)
          for (int x = 0; x < image.width(); ++x)
            differences += image.pixel(x, y) != approved.pixel(x, y);
        std::cout << "approved closed pixels " << stem.toStdString() << " differences=" << differences << '\n';
        HDRSHOT_CHECK(differences == 0);
      }
      return image;
    };
    for (int value : {2, 24}) {
      combo.setCurrentText(QString::number(value) + " DIP");
      QEvent leave(QEvent::Leave); QApplication::sendEvent(&combo, &leave);
      const auto normal = capture(value, 0);
      QEnterEvent enter(QPointF(4,4), QPointF(4,4), QPointF(4,4));
      QApplication::sendEvent(&combo, &enter);
      capture(value, 1);
      const QPointF press_point(65, 14);
      QMouseEvent press(QEvent::MouseButtonPress, press_point, combo.mapToGlobal(press_point),
                        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
      QApplication::sendEvent(&combo, &press);
      auto* popup = QApplication::activePopupWidget();
      HDRSHOT_CHECK(popup != nullptr && popup->objectName() == "annotationSizePopup");
      capture(value, 3);
      auto* list = popup->findChild<QListView*>();
      QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
      QApplication::sendEvent(list, &escape);  // no release was delivered to the combo
      HDRSHOT_CHECK(!popup->isVisible() && combo.currentData().toInt() == value);
      HDRSHOT_CHECK(capture(value, 4) == normal);
      QKeyEvent open(QEvent::KeyPress, Qt::Key_Down, Qt::AltModifier);
      QApplication::sendEvent(&combo, &open);
      popup = QApplication::activePopupWidget();
      HDRSHOT_CHECK(popup != nullptr);
      popup->hide();  // same hideEvent path as Qt outside-dismiss; not native input verification
      HDRSHOT_CHECK(capture(value, 5) == normal);
      combo.showPopup();
      combo.setEnabled(false);
      HDRSHOT_CHECK(QApplication::activePopupWidget() == nullptr);
      combo.setEnabled(true);
      HDRSHOT_CHECK(capture(value, 6) == normal);
    }
  }
}

void trial_popup_visuals(const QString& output) {
  QDir().mkpath(output);
  const auto original = QApplication::palette();
  for (bool dark : {false, true}) {
    auto palette = original;
    palette.setColor(QPalette::Window, dark ? QColor("#26292E") : QColor("#F5F6F8"));
    QApplication::setPalette(palette);
    Fixture fixture;
    for (const char* tool : {"toolBrush", "toolLine", "toolText"}) {
      fixture.button(tool)->click();
      auto* combo = fixture.widget.findChild<QComboBox*>();
      const auto closed_width = combo->width();
      for (int selected = 0; selected < combo->count(); ++selected) {
        combo->setCurrentIndex(selected);
        combo->showPopup();
        QApplication::processEvents();
        auto* shell = QApplication::activePopupWidget();
        HDRSHOT_CHECK(shell != nullptr);
        auto* view = shell->findChild<QListView*>();
        HDRSHOT_CHECK(view != nullptr);
        std::cout << "POPUP " << dark << ' ' << tool << ' ' << selected
                  << " closed=" << closed_width << " popup=" << shell->width()
                  << " natural=" << view->sizeHintForColumn(0)
                  << " dpr=" << combo->devicePixelRatioF() << '\n';
        const auto stem = QString("%1_%2_%3").arg(dark ? "dark" : "light").arg(tool).arg(selected);
        HDRSHOT_CHECK(shell->grab().save(output + "/" + stem + "_popup.png"));
        if (selected == 2) {
          const auto hover_point = view->visualRect(combo->model()->index(3, 0)).center();
          QMouseEvent move(QEvent::MouseMove, hover_point, view->viewport()->mapToGlobal(hover_point),
                           Qt::NoButton, Qt::NoButton, Qt::NoModifier);
          QApplication::sendEvent(view->viewport(), &move);
          QApplication::processEvents();
          HDRSHOT_CHECK(combo->currentIndex() == selected);
          HDRSHOT_CHECK(shell->grab().save(output + "/" + stem + "_hover_other.png"));
        }
        combo->hidePopup();
        HDRSHOT_CHECK(combo->grab().save(output + "/" + stem + "_closed.png"));
        HDRSHOT_CHECK(combo->width() == closed_width);
      }
    }
  }
  QApplication::setPalette(original);
}

int main(int argc, char** argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication application(argc, argv);
  if (qEnvironmentVariableIsSet("SERIOUSSHOT_TRIAL_UI_CHECKS"))
    return hdrshot::test::run({{"approved closed picker paint and state recovery", closed_picker_approved_paint_and_state_recovery},
                               {"brush cursor area and cleanup", trial_brush_cursor_area_and_cleanup},
                               {"popup natural width and existing keyboard/mouse input", trial_popup_width_and_input}});
  const auto images = qEnvironmentVariable("SERIOUSSHOT_TRIAL_UI_IMAGES");
  if (!images.isEmpty()) { trial_popup_visuals(images); return 0; }
  if (qEnvironmentVariableIsSet("HDRSHOT_OVERLAY_DARK")) {
    auto palette = application.palette();
    palette.setColor(QPalette::Window, QColor(30, 30, 30));
    palette.setColor(QPalette::WindowText, QColor(240, 240, 240));
    palette.setColor(QPalette::Base, QColor(42, 42, 42));
    palette.setColor(QPalette::Text, QColor(240, 240, 240));
    palette.setColor(QPalette::Button, QColor(48, 48, 48));
    palette.setColor(QPalette::ButtonText, QColor(240, 240, 240));
    application.setPalette(palette);
  }
  const auto screenshot_path = qgetenv("HDRSHOT_OVERLAY_SCREENSHOT");
  if (!screenshot_path.isEmpty()) {
    TestInputPlatformAdapter input_platform;
    SolidTextRasterizer text_rasterizer;
    OverlayEditorWidget widget{
        PixelSize{1000, 600},
        1.0,
        &text_rasterizer,
        "",
        input_platform,
        SelectionSnapshot{SelectionRevision{1}, PixelRect{140, 90, 680, 330}}};
    widget.set_analysis_available(true);
    widget.set_pin_available(true);
    widget.resize(1000, 600);
    widget.show();
    QApplication::processEvents();
    const bool brush_screenshot = qEnvironmentVariableIsSet("HDRSHOT_OVERLAY_SCREENSHOT_BRUSH");
    widget.findChild<QToolButton*>(brush_screenshot ? QStringLiteral("toolBrush") : QStringLiteral("toolEllipse"))->click();
    if (brush_screenshot) {
      auto *sizes = widget.findChild<QComboBox *>();
      HDRSHOT_CHECK(sizes->count() == 6);
      sizes->setCurrentIndex(sizes->findData(72));
    }
    send_drag(widget, QPointF(280, 170), QPointF(590, 320));
    if (!brush_screenshot)
      widget.findChild<QToolButton*>(QStringLiteral("toolSelect"))->click();
    else
      send_move(widget, QPointF(680, 270));
    HDRSHOT_CHECK(widget.annotations().objects.size() == 1U);
    HDRSHOT_CHECK(widget.annotations().objects.front().bounds ==
                  (PixelRect{280, 170, 310, 150}));
    const auto screenshot_dpr = widget.devicePixelRatioF();
    QImage screenshot(widget.size() * screenshot_dpr, QImage::Format_ARGB32_Premultiplied);
    screenshot.setDevicePixelRatio(screenshot_dpr);
    std::size_t annotation_pixels = 0;
    for (int attempt = 0; attempt < 10 && annotation_pixels < 200U; ++attempt) {
      QCoreApplication::sendPostedEvents();
      widget.repaint();
      QApplication::processEvents();
      screenshot.fill(Qt::transparent);
      QPainter screenshot_painter(&screenshot);
      widget.render(&screenshot_painter);
      screenshot_painter.end();
      annotation_pixels = 0;
      for (int y = 0; y < screenshot.height(); ++y) {
        const auto* row = reinterpret_cast<const QRgb*>(screenshot.constScanLine(y));
        for (int x = 0; x < screenshot.width(); ++x) {
          const auto pixel = row[x];
          if (qAlpha(pixel) > 0 && qRed(pixel) > 200 &&
              qGreen(pixel) < 120 && qBlue(pixel) < 160) {
            ++annotation_pixels;
          }
        }
      }
    }
    HDRSHOT_CHECK(annotation_pixels >= 200U);
    if (!screenshot.save(QString::fromUtf8(screenshot_path))) {
      return 1;
    }
  }
  return hdrshot::test::run({
      {"approved closed picker paint and state recovery", closed_picker_approved_paint_and_state_recovery},
      {"brush cursor area and cleanup", trial_brush_cursor_area_and_cleanup},
      {"popup width and existing keyboard/mouse selection", trial_popup_width_and_input},
      {"brush preserves events and final coordinates",
       brush_preserves_all_events_and_final_double_coordinates},
      {"fractional capture scale preserves same-pixel samples",
       fractional_capture_scale_preserves_subpixel_samples},
      {"three DIP size groups retain independent values",three_dip_size_groups_and_palette_are_independent},
      {"brush precise transform undo", brush_transform_is_exact_and_undoable},
      {"brush actions and interruption cleanup",
       brush_actions_and_interruptions},
      {"latest annotation moves, final release and interruption",
       annotation_moves_coalesce_and_release_keeps_final_coordinate},
      {"rotation commits exact pose in one undo",
       rotation_gesture_commits_exact_preview_and_one_history_step},
      {"rotation at Retina and capture-loss retry",
       rotation_at_retina_scale_and_cancel_retry},
      {"rotation hot band respects other objects",
       rotation_hot_zone_does_not_steal_other_objects_or_controls},
      {"rotated crop uses coverage not old local bounds",
       rotated_crop_can_shrink_past_unrotated_bounds_and_keep_editing},
      {"initial crosshair needs no mouse input",
       initial_cursor_is_crosshair_without_move_or_press},
      {"manual fallback crosshair transitions on release",
       initial_cursor_covers_manual_fallback_and_release},
      {"window candidate is not exportable until release",
       window_hover_is_not_a_selection_or_export_and_click_confirms},
      {"manual window gesture and lost-capture retry",
       window_manual_drag_uses_anchor_and_cancel_allows_retry},
      {"leaving a candidate clears only preview",
       leaving_candidate_clears_preview_without_touching_selection},
      {"drawing cursor stays out of toolbar and properties",
       drawing_cursor_does_not_leak_into_controls},
      {"preview retains grayscale annotation coverage",
       preview_preserves_partial_coverage},
      {"pending input requests fixed wght instance",
       pending_text_requests_the_fixed_variable_font_instance},
      {"pending text explicit completion is transactional",
       explicit_completion_commits_text_and_failure_preserves_input},
      {"input focus blocks fixed completion",
       pending_input_fixed_keys_do_not_complete_capture},
      {"empty text and cancellation do not add objects",
       empty_text_completion_and_capture_cancel_do_not_create_objects},
      {"pending text owns property changes",
       pending_text_owns_property_changes_and_empty_input_is_discarded},
      {"selection properties and toolbar separators",
       selected_object_exposes_its_own_units_and_toolbar_has_group_separators},
      {"icon toolbar accessibility and press feedback",
       icon_toolbar_preserves_accessibility_and_scopes_press_feedback_to_actions},
      {"copy rendered feedback light",
       [] { copy_action_has_distinct_rendered_feedback(false); }},
      {"copy rendered feedback dark",
       [] { copy_action_has_distinct_rendered_feedback(true); }},
      {"pin toolbar availability, grouping and command",
       pin_action_availability_preserves_toolbar_groups_and_dispatches_command},
      {"pin requires committed selection",
       pin_action_requires_a_committed_selection},
      {"fixed keys use injected platform adapter",
       fixed_keys_are_interpreted_by_the_injected_platform_adapter},
      {"configured completion actions reach shell",
       configured_enter_and_double_click_actions_reach_the_shell},
      {"annotations use capture coordinates",
       annotations_use_capture_coordinates_and_drag_is_clamped_to_selection},
      {"target lock rejects second display",
       interaction_gate_rejects_a_second_display_after_target_lock},
      {"selection changes only from border",
       one_selection_changes_only_from_its_border},
      {"selected annotations resize and constrain crop",
       selected_annotations_resize_and_crop_cannot_exclude_them},
      {"rectangle and ellipse keep all resize handles",
       rectangle_and_ellipse_keep_all_eight_resize_handles},
      {"initial empty selection accepts ROI and brush",
       initial_empty_selection_accepts_roi_then_brush},
      {"invalid selection candidate preserves drawing",
       invalid_selection_candidate_does_not_poison_later_drawing},
      {"preview rerasterizes only adjusted object",
       previews_rerasterize_only_the_adjusted_object_and_rebase_selection},
      {"selected annotation move is clamped",
       selected_annotation_moves_without_moving_selection_and_is_clamped},
      {"arrow endpoints and tiny shape guard",
       arrow_endpoints_move_independently_and_tiny_shapes_are_discarded},
      {"text finish click is consumed",
       finishing_text_consumes_the_click_before_another_text_box_can_start},
  });
}
