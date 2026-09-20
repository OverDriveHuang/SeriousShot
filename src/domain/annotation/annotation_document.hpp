#pragma once

#include "core/error.hpp"
#include "core/geometry.hpp"
#include "core/ids.hpp"
#include "core/result.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace hdrshot {

enum class AnnotationKind : std::uint8_t { rectangle, ellipse, arrow, text };

struct ShapeStyle {
  std::uint32_t color_srgb_rgb{};
  std::uint16_t line_width_px{4};

  friend bool operator==(const ShapeStyle&, const ShapeStyle&) = default;
};

struct TextStyle {
  std::uint32_t color_srgb_rgb{};
  std::uint16_t font_size_pt{20};

  friend bool operator==(const TextStyle&, const TextStyle&) = default;
};

using AnnotationStyle = std::variant<ShapeStyle, TextStyle>;

struct ArrowGeometry {
  PixelPoint start{};
  PixelPoint end{};

  friend bool operator==(const ArrowGeometry&, const ArrowGeometry&) = default;
};

struct ShapeTransform {
  double rotation_radians{};
  // Subpixel center remainder; bounds stays the unrotated local rectangle.
  double offset_x{};
  double offset_y{};
  friend bool operator==(const ShapeTransform&, const ShapeTransform&) = default;
};

struct AnnotationObject {
  ObjectId id{};
  AnnotationKind kind{AnnotationKind::rectangle};
  PixelRect bounds{};
  AnnotationStyle style{ShapeStyle{}};
  std::string text;
  std::optional<ArrowGeometry> arrow_geometry;
  ShapeTransform transform{};

  friend bool operator==(const AnnotationObject&, const AnnotationObject&) = default;
};

struct AnnotationDocumentSnapshot {
  DocumentRevision revision{};
  std::vector<AnnotationObject> objects;
  std::optional<ObjectId> selected_object_id;
  bool undo_available{};
  bool redo_available{};

  friend bool operator==(const AnnotationDocumentSnapshot&, const AnnotationDocumentSnapshot&) = default;
};

struct CreateAnnotation {
  AnnotationObject object;
};

struct MoveAnnotation {
  ObjectId object_id;
  PixelPoint delta;
};

struct SelectAnnotation {
  std::optional<ObjectId> object_id;
};

struct ResizeAnnotation {
  ObjectId object_id;
  PixelRect bounds;
};

struct SetArrowGeometry {
  ObjectId object_id;
  ArrowGeometry geometry;
};

struct SetShapeGeometry {
  ObjectId object_id;
  PixelRect bounds;
  ShapeTransform transform;
};

struct RestyleAnnotation {
  ObjectId object_id;
  AnnotationStyle style;
};

struct EditAnnotationText {
  ObjectId object_id;
  std::string text;
};

struct UpdateTextAnnotation {
  ObjectId object_id;
  PixelRect bounds;
  TextStyle style;
  std::string text;
};

struct DeleteAnnotation {
  ObjectId object_id;
};

struct UndoAnnotation {};
struct RedoAnnotation {};

using AnnotationCommand = std::variant<
    CreateAnnotation,
    SelectAnnotation,
    MoveAnnotation,
    ResizeAnnotation,
    SetArrowGeometry,
    SetShapeGeometry,
    RestyleAnnotation,
    EditAnnotationText,
    UpdateTextAnnotation,
    DeleteAnnotation,
    UndoAnnotation,
    RedoAnnotation>;

class AnnotationDocument {
 public:
  [[nodiscard]] static AnnotationDocument empty();

  [[nodiscard]] const AnnotationDocumentSnapshot& snapshot() const noexcept { return current_; }

  [[nodiscard]] static Result<AnnotationDocument, Error> apply(
      const AnnotationDocument& document,
      const AnnotationCommand& command);

 private:
  AnnotationDocumentSnapshot current_;
  std::vector<AnnotationDocumentSnapshot> undo_stack_;
  std::vector<AnnotationDocumentSnapshot> redo_stack_;

  void refresh_history_flags();
};

}  // namespace hdrshot
