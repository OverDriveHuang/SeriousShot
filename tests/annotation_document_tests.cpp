#include "domain/annotation/annotation_document.hpp"
#include "test_support.hpp"

#include <cstdint>

namespace {

using namespace hdrshot;

constexpr std::uint32_t kRed = 0xFF4D67;
constexpr std::uint32_t kBlue = 0x36A3FF;

AnnotationDocument unwrap(Result<AnnotationDocument, Error> result) {
  HDRSHOT_CHECK(result.has_value());
  return std::move(result.value());
}

AnnotationObject rectangle() {
  return AnnotationObject{
      ObjectId{1},
      AnnotationKind::rectangle,
      PixelRect{10, 20, 30, 40},
      ShapeStyle{kRed, 4},
      {},
      std::nullopt,
  };
}

void create_move_restyle_undo_redo() {
  auto document = AnnotationDocument::empty();
  document = unwrap(AnnotationDocument::apply(document, CreateAnnotation{rectangle()}));
  HDRSHOT_CHECK(document.snapshot().revision == 1);
  HDRSHOT_CHECK(document.snapshot().objects.size() == 1);
  HDRSHOT_CHECK(document.snapshot().selected_object_id == ObjectId{1});

  document = unwrap(AnnotationDocument::apply(document, MoveAnnotation{ObjectId{1}, {5, -2}}));
  HDRSHOT_CHECK(document.snapshot().revision == 2);
  HDRSHOT_CHECK(document.snapshot().objects.front().bounds == (PixelRect{15, 18, 30, 40}));

  document = unwrap(AnnotationDocument::apply(
      document,
      RestyleAnnotation{ObjectId{1}, ShapeStyle{kBlue, 6}}));
  HDRSHOT_CHECK(document.snapshot().revision == 3);
  HDRSHOT_CHECK(std::get<ShapeStyle>(document.snapshot().objects.front().style).color_srgb_rgb == kBlue);
  HDRSHOT_CHECK(std::get<ShapeStyle>(document.snapshot().objects.front().style).line_width_px == 6);

  document = unwrap(AnnotationDocument::apply(document, UndoAnnotation{}));
  HDRSHOT_CHECK(document.snapshot().revision == 4);
  HDRSHOT_CHECK(std::get<ShapeStyle>(document.snapshot().objects.front().style).color_srgb_rgb == kRed);
  HDRSHOT_CHECK(std::get<ShapeStyle>(document.snapshot().objects.front().style).line_width_px == 4);
  HDRSHOT_CHECK(document.snapshot().redo_available);

  document = unwrap(AnnotationDocument::apply(document, RedoAnnotation{}));
  HDRSHOT_CHECK(document.snapshot().revision == 5);
  HDRSHOT_CHECK(std::get<ShapeStyle>(document.snapshot().objects.front().style).color_srgb_rgb == kBlue);
  HDRSHOT_CHECK(std::get<ShapeStyle>(document.snapshot().objects.front().style).line_width_px == 6);
  HDRSHOT_CHECK(!document.snapshot().redo_available);
}

void undo_then_new_command_clears_redo() {
  auto document = AnnotationDocument::empty();
  document = unwrap(AnnotationDocument::apply(document, CreateAnnotation{rectangle()}));
  document = unwrap(AnnotationDocument::apply(document, MoveAnnotation{ObjectId{1}, {5, 0}}));
  document = unwrap(AnnotationDocument::apply(document, UndoAnnotation{}));
  HDRSHOT_CHECK(document.snapshot().redo_available);
  document = unwrap(AnnotationDocument::apply(
      document,
      RestyleAnnotation{ObjectId{1}, ShapeStyle{kBlue, 8}}));
  HDRSHOT_CHECK(!document.snapshot().redo_available);
  const auto redo = AnnotationDocument::apply(document, RedoAnnotation{});
  HDRSHOT_CHECK(!redo.has_value());
  HDRSHOT_CHECK(redo.error().code == ErrorCode::precondition_failed);
}

void missing_object_preserves_document() {
  const auto document = AnnotationDocument::empty();
  const auto result = AnnotationDocument::apply(document, MoveAnnotation{ObjectId{404}, {1, 1}});
  HDRSHOT_CHECK(!result.has_value());
  HDRSHOT_CHECK(result.error().code == ErrorCode::object_not_found);
  HDRSHOT_CHECK(document.snapshot() == AnnotationDocument::empty().snapshot());
}

void text_edit_requires_text_object() {
  auto document = AnnotationDocument::empty();
  document = unwrap(AnnotationDocument::apply(document, CreateAnnotation{rectangle()}));
  const auto bad = AnnotationDocument::apply(
      document,
      EditAnnotationText{ObjectId{1}, "not allowed"});
  HDRSHOT_CHECK(!bad.has_value());
  HDRSHOT_CHECK(bad.error().code == ErrorCode::precondition_failed);

  auto text = rectangle();
  text.id = ObjectId{2};
  text.kind = AnnotationKind::text;
  text.style = TextStyle{kRed, 20};
  text.text = "before";
  document = unwrap(AnnotationDocument::apply(document, CreateAnnotation{text}));
  document = unwrap(AnnotationDocument::apply(
      document,
      EditAnnotationText{ObjectId{2}, "after"}));
  HDRSHOT_CHECK(document.snapshot().objects.back().text == "after");
}

void select_resize_and_style_domains_are_enforced() {
  auto document = AnnotationDocument::empty();
  document = unwrap(AnnotationDocument::apply(document, CreateAnnotation{rectangle()}));
  document = unwrap(AnnotationDocument::apply(
      document,
      ResizeAnnotation{ObjectId{1}, PixelRect{2, 3, 80, 90}}));
  HDRSHOT_CHECK(document.snapshot().objects.front().bounds == (PixelRect{2, 3, 80, 90}));
  document = unwrap(AnnotationDocument::apply(document, SelectAnnotation{std::nullopt}));
  HDRSHOT_CHECK(!document.snapshot().selected_object_id.has_value());
  document = unwrap(AnnotationDocument::apply(document, SelectAnnotation{ObjectId{1}}));
  HDRSHOT_CHECK(document.snapshot().selected_object_id == ObjectId{1});

  const auto bad_width = AnnotationDocument::apply(
      document,
      RestyleAnnotation{ObjectId{1}, ShapeStyle{kBlue, 5}});
  HDRSHOT_CHECK(!bad_width.has_value());
  HDRSHOT_CHECK(bad_width.error().code == ErrorCode::invalid_input);
  const auto wrong_style_domain = AnnotationDocument::apply(
      document,
      RestyleAnnotation{ObjectId{1}, TextStyle{kBlue, 20}});
  HDRSHOT_CHECK(!wrong_style_domain.has_value());
  HDRSHOT_CHECK(wrong_style_domain.error().code == ErrorCode::invalid_input);
}

void delete_is_undoable() {
  auto document = AnnotationDocument::empty();
  document = unwrap(AnnotationDocument::apply(document, CreateAnnotation{rectangle()}));
  document = unwrap(AnnotationDocument::apply(document, DeleteAnnotation{ObjectId{1}}));
  HDRSHOT_CHECK(document.snapshot().objects.empty());
  document = unwrap(AnnotationDocument::apply(document, UndoAnnotation{}));
  HDRSHOT_CHECK(document.snapshot().objects.size() == 1);
  HDRSHOT_CHECK(document.snapshot().objects.front().id == ObjectId{1});
}

void selection_is_not_an_undo_step_and_preserves_redo() {
  auto document = AnnotationDocument::empty();
  document = unwrap(AnnotationDocument::apply(document, CreateAnnotation{rectangle()}));
  document = unwrap(AnnotationDocument::apply(document, MoveAnnotation{ObjectId{1}, {5, 0}}));
  document = unwrap(AnnotationDocument::apply(document, UndoAnnotation{}));
  HDRSHOT_CHECK(document.snapshot().redo_available);

  document = unwrap(AnnotationDocument::apply(document, SelectAnnotation{std::nullopt}));
  HDRSHOT_CHECK(!document.snapshot().selected_object_id.has_value());
  HDRSHOT_CHECK(document.snapshot().redo_available);

  document = unwrap(AnnotationDocument::apply(document, RedoAnnotation{}));
  HDRSHOT_CHECK(document.snapshot().objects.front().bounds.x == 15);
  HDRSHOT_CHECK(!document.snapshot().redo_available);
}

void arrow_direction_survives_move_resize_and_undo() {
  auto document = AnnotationDocument::empty();
  const AnnotationObject arrow{
      ObjectId{9},
      AnnotationKind::arrow,
      PixelRect{10, 20, 30, 40},
      ShapeStyle{kRed, 4},
      {},
      ArrowGeometry{PixelPoint{10, 60}, PixelPoint{40, 20}},
  };
  document = unwrap(AnnotationDocument::apply(document, CreateAnnotation{arrow}));
  document = unwrap(AnnotationDocument::apply(
      document, MoveAnnotation{ObjectId{9}, PixelPoint{5, -5}}));
  const auto moved = std::optional<ArrowGeometry>{ArrowGeometry{{15, 55}, {45, 15}}};
  HDRSHOT_CHECK(document.snapshot().objects.front().arrow_geometry == moved);

  document = unwrap(AnnotationDocument::apply(
      document, ResizeAnnotation{ObjectId{9}, PixelRect{0, 0, 60, 80}}));
  const auto resized = std::optional<ArrowGeometry>{ArrowGeometry{{0, 80}, {60, 0}}};
  HDRSHOT_CHECK(document.snapshot().objects.front().arrow_geometry == resized);

  document = unwrap(AnnotationDocument::apply(document, UndoAnnotation{}));
  HDRSHOT_CHECK(document.snapshot().objects.front().arrow_geometry == moved);
}

void arrow_requires_direction_geometry() {
  auto invalid = rectangle();
  invalid.id = ObjectId{8};
  invalid.kind = AnnotationKind::arrow;
  const auto result = AnnotationDocument::apply(
      AnnotationDocument::empty(), CreateAnnotation{invalid});
  HDRSHOT_CHECK(!result.has_value());
  HDRSHOT_CHECK(result.error().code == ErrorCode::invalid_input);
}

void arrow_endpoints_and_text_bounds_update_atomically() {
  auto document = AnnotationDocument::empty();
  const AnnotationObject arrow{
      ObjectId{9},
      AnnotationKind::arrow,
      PixelRect{10, 20, 30, 40},
      ShapeStyle{kRed, 4},
      {},
      ArrowGeometry{PixelPoint{10, 60}, PixelPoint{40, 20}},
  };
  document = unwrap(AnnotationDocument::apply(document, CreateAnnotation{arrow}));
  document = unwrap(AnnotationDocument::apply(
      document,
      SetArrowGeometry{ObjectId{9}, ArrowGeometry{{20, 25}, {90, 70}}}));
  const auto expected_arrow =
      std::optional<ArrowGeometry>{ArrowGeometry{{20, 25}, {90, 70}}};
  HDRSHOT_CHECK(document.snapshot().objects.front().arrow_geometry == expected_arrow);
  HDRSHOT_CHECK(document.snapshot().objects.front().bounds == (PixelRect{20, 25, 70, 45}));

  auto text = rectangle();
  text.id = ObjectId{10};
  text.kind = AnnotationKind::text;
  text.style = TextStyle{kRed, 20};
  text.text = "before";
  document = unwrap(AnnotationDocument::apply(document, CreateAnnotation{text}));
  document = unwrap(AnnotationDocument::apply(
      document,
      UpdateTextAnnotation{
          ObjectId{10}, PixelRect{30, 40, 160, 60}, TextStyle{kBlue, 24}, "after"}));
  const auto& updated = document.snapshot().objects.back();
  HDRSHOT_CHECK(updated.bounds == (PixelRect{30, 40, 160, 60}));
  const auto expected_style = AnnotationStyle{TextStyle{kBlue, 24}};
  HDRSHOT_CHECK(updated.style == expected_style);
  HDRSHOT_CHECK(updated.text == "after");
}

}  // namespace

int main() {
  return hdrshot::test::run({
      {"D5-ANNOTATION-UNDO-REDO", create_move_restyle_undo_redo},
      {"D5-BRANCH-001", undo_then_new_command_clears_redo},
      {"D5-INVALID-001", missing_object_preserves_document},
      {"D5-TEXT-001", text_edit_requires_text_object},
      {"D5-DELETE-001", delete_is_undoable},
      {"D5-SELECT-RESIZE-STYLE-001", select_resize_and_style_domains_are_enforced},
      {"D5-SELECT-HISTORY-001", selection_is_not_an_undo_step_and_preserves_redo},
      {"D5-ARROW-GEOMETRY-001", arrow_direction_survives_move_resize_and_undo},
      {"D5-ARROW-GEOMETRY-INVALID", arrow_requires_direction_geometry},
      {"D5-DIRECT-MANIPULATION-001", arrow_endpoints_and_text_bounds_update_atomically},
  });
}
