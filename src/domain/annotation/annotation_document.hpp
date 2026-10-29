#pragma once

#include "core/error.hpp"
#include "core/geometry.hpp"
#include "core/ids.hpp"
#include "core/result.hpp"

#include "domain/annotation/annotation_style_policy.hpp"
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace hdrshot {

enum class AnnotationKind : std::uint8_t {
  rectangle,
  ellipse,
  arrow,
  text,
  line,
  brush
};

struct ShapeStyle {
  std::uint32_t color_srgb_rgb{};
  std::uint16_t line_width_dip{default_shape_size_dip};

  friend bool operator==(const ShapeStyle&, const ShapeStyle&) = default;
};

struct TextStyle {
  std::uint32_t color_srgb_rgb{};
  std::uint16_t font_size_dip{default_text_size_dip};

  friend bool operator==(const TextStyle&, const TextStyle&) = default;
};

struct BrushStyle {
  std::uint32_t color_srgb_rgb{};
  std::uint16_t diameter_dip{default_brush_size_dip};
  friend bool operator==(const BrushStyle &, const BrushStyle &) = default;
};
using AnnotationStyle = std::variant<ShapeStyle, TextStyle, BrushStyle>;
struct StrokePoint {
  double x{}, y{};
  friend bool operator==(const StrokePoint &, const StrokePoint &) = default;
};
struct BrushPath {
  explicit BrushPath(std::vector<StrokePoint>);
  const std::vector<StrokePoint> points;
  const auto &bounds() const { return local_bounds; }
  bool is_valid() const { return valid; }
  const auto &buckets() const { return segment_buckets; }
  const auto &broad() const { return broad_segments; }

private:
  std::array<double, 4> local_bounds{};
  bool valid{};
  std::map<std::pair<int, int>, std::vector<std::size_t>> segment_buckets;
  std::vector<std::size_t> broad_segments;
  friend bool operator==(const BrushPath &a, const BrushPath &b) {
    return a.points == b.points;
  }
};
struct BrushGeometry {
  std::shared_ptr<const BrushPath> path;
  double origin_x{}, origin_y{}, scale_x{1}, scale_y{1};
  friend bool operator==(const BrushGeometry &a, const BrushGeometry &b) {
    return a.origin_x == b.origin_x && a.origin_y == b.origin_y &&
           a.scale_x == b.scale_x && a.scale_y == b.scale_y &&
           (a.path == b.path || (a.path && b.path && *a.path == *b.path));
  }
};

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
  std::optional<BrushGeometry> brush_geometry;

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

struct SetBrushGeometry {
  ObjectId object_id;
  BrushGeometry geometry;
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

using AnnotationCommand =
    std::variant<CreateAnnotation, SelectAnnotation, MoveAnnotation,
                 ResizeAnnotation, SetArrowGeometry, SetBrushGeometry,
                 SetShapeGeometry, RestyleAnnotation, EditAnnotationText,
                 UpdateTextAnnotation, DeleteAnnotation, UndoAnnotation,
                 RedoAnnotation>;

bool valid_style(AnnotationKind kind, const AnnotationStyle &style);
bool valid_geometry(const AnnotationObject &object);

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
