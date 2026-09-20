#include "domain/geometry/selection_model.hpp"
#include "test_support.hpp"

namespace {

using hdrshot::PixelPoint;
using hdrshot::PixelRect;
using hdrshot::SelectionModel;
using hdrshot::SelectionSnapshot;

void reverse_drag_matches_reference_vector() {
  const auto result = SelectionModel::update_drag(
      SelectionSnapshot{10, {}},
      PixelPoint{2, 2},
      PixelPoint{-1, -2},
      PixelRect{-4, -4, 8, 8});
  HDRSHOT_CHECK(result.revision == 11);
  HDRSHOT_CHECK(result.desktop_rect == (PixelRect{-1, -2, 3, 4}));
}

void all_drag_directions_normalize_identically() {
  const auto bounds = PixelRect{-10, -10, 20, 20};
  const auto expected = PixelRect{-3, -2, 8, 9};
  HDRSHOT_CHECK(SelectionModel::normalize_drag({-3, -2}, {5, 7}, bounds) == expected);
  HDRSHOT_CHECK(SelectionModel::normalize_drag({5, -2}, {-3, 7}, bounds) == expected);
  HDRSHOT_CHECK(SelectionModel::normalize_drag({-3, 7}, {5, -2}, bounds) == expected);
  HDRSHOT_CHECK(SelectionModel::normalize_drag({5, 7}, {-3, -2}, bounds) == expected);
}

void points_are_clamped_to_desktop_bounds() {
  const auto actual = SelectionModel::normalize_drag(
      PixelPoint{-100, 2},
      PixelPoint{100, 20},
      PixelRect{-4, -3, 10, 8});
  HDRSHOT_CHECK(actual == (PixelRect{-4, 2, 10, 3}));
}

void unchanged_geometry_does_not_increment_revision() {
  const auto previous = SelectionSnapshot{9, PixelRect{1, 2, 3, 4}};
  const auto actual = SelectionModel::update_drag(
      previous,
      PixelPoint{1, 2},
      PixelPoint{4, 6},
      PixelRect{0, 0, 10, 10});
  HDRSHOT_CHECK(actual == previous);
}

void empty_desktop_produces_empty_selection() {
  const auto actual = SelectionModel::normalize_drag({1, 1}, {5, 5}, PixelRect{});
  HDRSHOT_CHECK(actual.empty());
}

void existing_selection_resizes_without_creating_a_second_region() {
  const auto original = SelectionSnapshot{10, PixelRect{100, 100, 400, 300}};
  const auto resized = SelectionModel::adjust(
      original,
      hdrshot::SelectionAdjustment::south_east,
      PixelPoint{500, 400},
      PixelPoint{650, 500},
      PixelRect{0, 0, 1000, 600});
  HDRSHOT_CHECK(resized.desktop_rect == (PixelRect{100, 100, 550, 400}));
}

void selection_edges_cannot_exclude_existing_annotations() {
  const auto original = SelectionSnapshot{20, PixelRect{100, 100, 400, 300}};
  const auto required = PixelRect{180, 160, 160, 120};
  const auto from_left = SelectionModel::adjust(
      original,
      hdrshot::SelectionAdjustment::west,
      PixelPoint{100, 220},
      PixelPoint{260, 220},
      PixelRect{0, 0, 1000, 600},
      2,
      required);
  HDRSHOT_CHECK(from_left.desktop_rect == (PixelRect{180, 100, 320, 300}));

  const auto from_right = SelectionModel::adjust(
      original,
      hdrshot::SelectionAdjustment::east,
      PixelPoint{500, 220},
      PixelPoint{250, 220},
      PixelRect{0, 0, 1000, 600},
      2,
      required);
  HDRSHOT_CHECK(from_right.desktop_rect == (PixelRect{100, 100, 240, 300}));
}

}  // namespace

int main() {
  return hdrshot::test::run({
      {"D3-GEOMETRY-REVERSE-DRAG", reverse_drag_matches_reference_vector},
      {"D3-GEOMETRY-001 all directions", all_drag_directions_normalize_identically},
      {"D3-BOUNDS-001 clamp", points_are_clamped_to_desktop_bounds},
      {"D3-REVISION-001 unchanged", unchanged_geometry_does_not_increment_revision},
      {"D3-BOUNDS-002 empty desktop", empty_desktop_produces_empty_selection},
      {"D3-SELECTION-LOCK-001 adjust one region", existing_selection_resizes_without_creating_a_second_region},
      {"D3-SELECTION-CONTENTS-001 keep annotations inside", selection_edges_cannot_exclude_existing_annotations},
  });
}
