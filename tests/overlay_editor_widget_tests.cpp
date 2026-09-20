#include "ui/qt/overlay_editor_widget.hpp"
#include "test_support.hpp"

#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QColor>
#include <QEvent>
#include <QFrame>
#include <QKeyEvent>
#include <QLabel>
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

void explicit_completion_commits_text_and_failure_preserves_input() {
  for (const auto* action : {"actionSaveDefault", "actionSaveAs", "actionCopy"}) {
    Fixture fixture;
    fixture.button("toolText")->click();
    send_drag(fixture.widget, {100, 100}, {350, 180});
    auto* input = fixture.widget.findChild<QLineEdit*>("annotationTextEditor");
    input->setText("new text");
    int completions = 0;
    fixture.widget.set_action_requested([&](UiCommand) {
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
  for (const auto* action : {"actionSaveDefault", "actionSaveAs", "actionCopy", "actionCancel"}) {
    Fixture fixture;
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
  HDRSHOT_CHECK(size_combo->currentText().endsWith(QStringLiteral(" px")));
}

void icon_toolbar_preserves_accessibility_and_scopes_press_feedback_to_actions() {
  Fixture fixture;
  constexpr std::array<const char*, 5> tool_names{
      "toolSelect", "toolRectangle", "toolEllipse", "toolArrow", "toolText"};
  constexpr std::array<const char*, 6> action_names{
      "actionUndo", "actionRedo", "actionSaveDefault",
      "actionSaveAs", "actionCopy", "actionCancel"};

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
  HDRSHOT_CHECK(toolbar->styleSheet().contains(
      QStringLiteral("QToolButton:pressed { background: %1; }").arg(hover)));
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
  HDRSHOT_CHECK(widget.selection().desktop_rect == (PixelRect{100, 100, 302, 300}));
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
}  // namespace

int main(int argc, char** argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication application(argc, argv);
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
    widget.resize(1000, 600);
    widget.show();
    QApplication::processEvents();
    widget.findChild<QToolButton*>(QStringLiteral("toolEllipse"))->click();
    send_drag(widget, QPointF(280, 170), QPointF(590, 320));
    widget.findChild<QToolButton*>(QStringLiteral("toolSelect"))->click();
    HDRSHOT_CHECK(widget.annotations().objects.size() == 1U);
    HDRSHOT_CHECK(widget.annotations().objects.front().bounds ==
                  (PixelRect{280, 170, 310, 150}));
    QImage screenshot(widget.size(), QImage::Format_ARGB32_Premultiplied);
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
      {"rotation commits exact pose in one undo",rotation_gesture_commits_exact_preview_and_one_history_step},
      {"rotation at Retina and capture-loss retry",rotation_at_retina_scale_and_cancel_retry},
      {"rotation hot band respects other objects",rotation_hot_zone_does_not_steal_other_objects_or_controls},
      {"rotated crop uses coverage not old local bounds",rotated_crop_can_shrink_past_unrotated_bounds_and_keep_editing},
      {"initial crosshair needs no mouse input", initial_cursor_is_crosshair_without_move_or_press},
      {"manual fallback crosshair transitions on release", initial_cursor_covers_manual_fallback_and_release},
      {"window candidate is not exportable until release", window_hover_is_not_a_selection_or_export_and_click_confirms},
      {"manual window gesture and lost-capture retry", window_manual_drag_uses_anchor_and_cancel_allows_retry},
      {"leaving a candidate clears only preview", leaving_candidate_clears_preview_without_touching_selection},
      {"drawing cursor stays out of toolbar and properties", drawing_cursor_does_not_leak_into_controls},
      {"preview retains grayscale annotation coverage", preview_preserves_partial_coverage},
      {"pending text explicit completion is transactional", explicit_completion_commits_text_and_failure_preserves_input},
      {"input focus blocks fixed completion", pending_input_fixed_keys_do_not_complete_capture},
      {"empty text and cancellation do not add objects", empty_text_completion_and_capture_cancel_do_not_create_objects},
      {"pending text owns property changes", pending_text_owns_property_changes_and_empty_input_is_discarded},
      {"selection properties and toolbar separators", selected_object_exposes_its_own_units_and_toolbar_has_group_separators},
      {"icon toolbar accessibility and press feedback", icon_toolbar_preserves_accessibility_and_scopes_press_feedback_to_actions},
      {"fixed keys use injected platform adapter", fixed_keys_are_interpreted_by_the_injected_platform_adapter},
      {"configured completion actions reach shell", configured_enter_and_double_click_actions_reach_the_shell},
      {"annotations use capture coordinates", annotations_use_capture_coordinates_and_drag_is_clamped_to_selection},
      {"target lock rejects second display", interaction_gate_rejects_a_second_display_after_target_lock},
      {"selection changes only from border", one_selection_changes_only_from_its_border},
      {"selected annotations resize and constrain crop", selected_annotations_resize_and_crop_cannot_exclude_them},
      {"rectangle and ellipse keep all resize handles", rectangle_and_ellipse_keep_all_eight_resize_handles},
      {"invalid selection candidate preserves drawing", invalid_selection_candidate_does_not_poison_later_drawing},
      {"preview rerasterizes only adjusted object", previews_rerasterize_only_the_adjusted_object_and_rebase_selection},
      {"selected annotation move is clamped", selected_annotation_moves_without_moving_selection_and_is_clamped},
      {"arrow endpoints and tiny shape guard", arrow_endpoints_move_independently_and_tiny_shapes_are_discarded},
      {"text finish click is consumed", finishing_text_consumes_the_click_before_another_text_box_can_start},
  });
}
