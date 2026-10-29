#include "domain/annotation/annotation_allocation_checkpoint.hpp"
#include "domain/annotation/annotation_document.hpp"
#include "domain/annotation/annotation_geometry.hpp"

#include <algorithm>
#include <limits>
#include <string>
#include <type_traits>

namespace hdrshot {
namespace {

[[nodiscard]] Error object_error(const ErrorCode code, const ObjectId id) {
  return Error{
      code,
      "AnnotationDocument",
      Retryability::never,
      {{"objectId", std::to_string(id.value)}},
  };
}

template <typename Objects>
[[nodiscard]] auto find_object(Objects& objects, const ObjectId id) {
  return std::find_if(objects.begin(), objects.end(), [id](const AnnotationObject& object) {
    return object.id == id;
  });
}

[[nodiscard]] PixelRect bounds_from_arrow(const ArrowGeometry geometry) {
  const auto left = std::min(geometry.start.x, geometry.end.x);
  const auto top = std::min(geometry.start.y, geometry.end.y);
  return PixelRect{
      left,
      top,
      static_cast<int>(std::max<std::int64_t>(
          1, std::int64_t(std::max(geometry.start.x, geometry.end.x)) - left)),
      static_cast<int>(std::max<std::int64_t>(
          1, std::int64_t(std::max(geometry.start.y, geometry.end.y)) - top)),
  };
}

[[nodiscard]] std::int32_t scale_coordinate(
    const std::int32_t value,
    const std::int32_t old_origin,
    const std::int32_t old_extent,
    const std::int32_t new_origin,
    const std::int32_t new_extent) {
  const auto relative = static_cast<double>(value - old_origin) /
      static_cast<double>(old_extent);
  return new_origin + static_cast<std::int32_t>(relative * static_cast<double>(new_extent));
}

}  // namespace

bool valid_style(AnnotationKind kind, const AnnotationStyle &style) {
  const auto colors = annotation_palette_srgb();
  const auto sizes = annotation_sizes_dip(kind);
  std::uint32_t color;
  std::uint16_t size;
  if (kind == AnnotationKind::text) {
    auto p = std::get_if<TextStyle>(&style);
    if (!p)
      return false;
    color = p->color_srgb_rgb;
    size = p->font_size_dip;
  } else if (kind == AnnotationKind::brush) {
    auto p = std::get_if<BrushStyle>(&style);
    if (!p)
      return false;
    color = p->color_srgb_rgb;
    size = p->diameter_dip;
  } else {
    auto p = std::get_if<ShapeStyle>(&style);
    if (!p)
      return false;
    color = p->color_srgb_rgb;
    size = p->line_width_dip;
  }
  return std::find(colors.begin(), colors.end(), color) != colors.end() &&
         std::find(sizes.begin(), sizes.end(), size) != sizes.end();
}
bool valid_geometry(const AnnotationObject &object) {
  if (object.bounds.empty() || !valid_shape_transform(object) ||
      std::int64_t(object.bounds.x) + object.bounds.width >
          std::numeric_limits<int>::max() ||
      std::int64_t(object.bounds.y) + object.bounds.height >
          std::numeric_limits<int>::max())
    return false;
  if (object.kind == AnnotationKind::brush) {
    if (!object.brush_geometry || object.arrow_geometry || !object.text.empty())
      return false;
    auto bounds = brush_geometry_bounds(*object.brush_geometry);
    return bounds && bounds.value() == object.bounds;
  }
  if (object.brush_geometry)
    return false;
  if (object.kind == AnnotationKind::arrow ||
      object.kind == AnnotationKind::line) {
    if (!object.arrow_geometry ||
        object.arrow_geometry->start == object.arrow_geometry->end)
      return false;
    auto g = *object.arrow_geometry;
    return std::abs(std::int64_t(g.start.x) - g.end.x) <=
               std::numeric_limits<int>::max() &&
           std::abs(std::int64_t(g.start.y) - g.end.y) <=
               std::numeric_limits<int>::max();
  }
  return !object.arrow_geometry;
}

AnnotationDocument AnnotationDocument::empty() {
  return AnnotationDocument{};
}

void AnnotationDocument::refresh_history_flags() {
  current_.undo_available = !undo_stack_.empty();
  current_.redo_available = !redo_stack_.empty();
}

Result<AnnotationDocument, Error> AnnotationDocument::apply(
    const AnnotationDocument& document,
    const AnnotationCommand& command) {
  try {
  annotation_allocation_checkpoint(AnnotationAllocationStage::history_candidate);
  if (std::holds_alternative<UndoAnnotation>(command)) {
    if (document.undo_stack_.empty()) {
      return Result<AnnotationDocument, Error>::failure(Error{
          ErrorCode::precondition_failed,
          "AnnotationDocument",
          Retryability::never,
          {{"command", "undo"}},
      });
    }
    auto next = document;
    const auto next_revision = document.current_.revision + 1;
    next.redo_stack_.push_back(document.current_);
    next.current_ = next.undo_stack_.back();
    next.undo_stack_.pop_back();
    next.current_.revision = next_revision;
    next.refresh_history_flags();
    return Result<AnnotationDocument, Error>::success(std::move(next));
  }

  if (std::holds_alternative<RedoAnnotation>(command)) {
    if (document.redo_stack_.empty()) {
      return Result<AnnotationDocument, Error>::failure(Error{
          ErrorCode::precondition_failed,
          "AnnotationDocument",
          Retryability::never,
          {{"command", "redo"}},
      });
    }
    auto next = document;
    const auto next_revision = document.current_.revision + 1;
    next.undo_stack_.push_back(document.current_);
    next.current_ = next.redo_stack_.back();
    next.redo_stack_.pop_back();
    next.current_.revision = next_revision;
    next.refresh_history_flags();
    return Result<AnnotationDocument, Error>::success(std::move(next));
  }

  if (const auto* select = std::get_if<SelectAnnotation>(&command); select != nullptr) {
    if (select->object_id.has_value() &&
        find_object(document.current_.objects, select->object_id.value()) ==
            document.current_.objects.end()) {
      return Result<AnnotationDocument, Error>::failure(
          object_error(ErrorCode::object_not_found, select->object_id.value()));
    }
    auto next = document;
    next.current_.selected_object_id = select->object_id;
    next.current_.revision = document.current_.revision + 1;
    next.refresh_history_flags();
    return Result<AnnotationDocument, Error>::success(std::move(next));
  }

  auto next = document;
  auto candidate = document.current_;
  const auto mutation = std::visit(
      [&candidate](const auto& typed_command) -> std::optional<Error> {
        using Command = std::decay_t<decltype(typed_command)>;
        if constexpr (std::is_same_v<Command, CreateAnnotation>) {
          if (find_object(candidate.objects, typed_command.object.id) != candidate.objects.end()) {
            return object_error(ErrorCode::precondition_failed, typed_command.object.id);
          }
          if (!valid_geometry(typed_command.object) ||
              !valid_style(typed_command.object.kind, typed_command.object.style)) {
            return object_error(ErrorCode::invalid_input, typed_command.object.id);
          }
          candidate.objects.push_back(typed_command.object);
          candidate.selected_object_id = typed_command.object.id;
        } else if constexpr (std::is_same_v<Command, MoveAnnotation>) {
          const auto object = find_object(candidate.objects, typed_command.object_id);
          if (object == candidate.objects.end()) {
            return object_error(ErrorCode::object_not_found, typed_command.object_id);
          }
          if (object->brush_geometry) {
            auto geometry = *object->brush_geometry;
            geometry.origin_x += typed_command.delta.x;
            geometry.origin_y += typed_command.delta.y;
            auto bounds = brush_geometry_bounds(geometry);
            if (!bounds)
              return bounds.error();
            object->brush_geometry = std::move(geometry);
            object->bounds = bounds.value();
            candidate.selected_object_id = typed_command.object_id;
            return std::nullopt;
          }
          const auto safe_add = [](int a, int b) {
            auto sum = std::int64_t(a) + b;
            return sum >= std::numeric_limits<int>::min() &&
                   sum <= std::numeric_limits<int>::max();
          };
          if (!safe_add(object->bounds.x, typed_command.delta.x) ||
              !safe_add(object->bounds.y, typed_command.delta.y) ||
              !safe_add(object->bounds.right(), typed_command.delta.x) ||
              !safe_add(object->bounds.bottom(), typed_command.delta.y))
            return object_error(ErrorCode::invalid_input,
                                typed_command.object_id);
          object->bounds.x += typed_command.delta.x;
          object->bounds.y += typed_command.delta.y;
          if (object->arrow_geometry.has_value()) {
            object->arrow_geometry->start.x += typed_command.delta.x;
            object->arrow_geometry->start.y += typed_command.delta.y;
            object->arrow_geometry->end.x += typed_command.delta.x;
            object->arrow_geometry->end.y += typed_command.delta.y;
          }
          candidate.selected_object_id = typed_command.object_id;
        } else if constexpr (std::is_same_v<Command, ResizeAnnotation>) {
          const auto object = find_object(candidate.objects, typed_command.object_id);
          if (object == candidate.objects.end()) {
            return object_error(ErrorCode::object_not_found, typed_command.object_id);
          }
          if (object->kind == AnnotationKind::brush)
            return object_error(ErrorCode::precondition_failed,
                                typed_command.object_id);
          if (typed_command.bounds.empty()) {
            return object_error(ErrorCode::invalid_input, typed_command.object_id);
          }
          if (object->arrow_geometry.has_value()) {
            const auto old_bounds = object->bounds;
            object->arrow_geometry->start = PixelPoint{
                scale_coordinate(
                    object->arrow_geometry->start.x,
                    old_bounds.x,
                    old_bounds.width,
                    typed_command.bounds.x,
                    typed_command.bounds.width),
                scale_coordinate(
                    object->arrow_geometry->start.y,
                    old_bounds.y,
                    old_bounds.height,
                    typed_command.bounds.y,
                    typed_command.bounds.height),
            };
            object->arrow_geometry->end = PixelPoint{
                scale_coordinate(
                    object->arrow_geometry->end.x,
                    old_bounds.x,
                    old_bounds.width,
                    typed_command.bounds.x,
                    typed_command.bounds.width),
                scale_coordinate(
                    object->arrow_geometry->end.y,
                    old_bounds.y,
                    old_bounds.height,
                    typed_command.bounds.y,
                    typed_command.bounds.height),
            };
          }
          object->bounds = typed_command.bounds;
          candidate.selected_object_id = typed_command.object_id;
        } else if constexpr (std::is_same_v<Command, SetBrushGeometry>) {
          const auto object =
              find_object(candidate.objects, typed_command.object_id);
          if (object == candidate.objects.end())
            return object_error(ErrorCode::object_not_found,
                                typed_command.object_id);
          if (object->kind != AnnotationKind::brush ||
              !object->brush_geometry ||
              object->brush_geometry->path != typed_command.geometry.path)
            return object_error(ErrorCode::precondition_failed,
                                typed_command.object_id);
          auto bounds = brush_geometry_bounds(typed_command.geometry);
          if (!bounds)
            return bounds.error();
          object->brush_geometry = typed_command.geometry;
          object->bounds = bounds.value();
          candidate.selected_object_id = typed_command.object_id;
        } else if constexpr (std::is_same_v<Command, SetShapeGeometry>) {
          const auto object = find_object(candidate.objects, typed_command.object_id);
          if (object == candidate.objects.end())
            return object_error(ErrorCode::object_not_found, typed_command.object_id);
          auto updated = *object;
          updated.bounds = typed_command.bounds;
          updated.transform = typed_command.transform;
          if (!is_rotatable(updated) || !valid_geometry(updated))
            return object_error(ErrorCode::invalid_input, typed_command.object_id);
          *object = std::move(updated);
          candidate.selected_object_id = typed_command.object_id;
        } else if constexpr (std::is_same_v<Command, SetArrowGeometry>) {
          const auto object = find_object(candidate.objects, typed_command.object_id);
          if (object == candidate.objects.end()) {
            return object_error(ErrorCode::object_not_found, typed_command.object_id);
          }
          if ((object->kind != AnnotationKind::arrow &&
               object->kind != AnnotationKind::line) ||
              typed_command.geometry.start == typed_command.geometry.end) {
            return object_error(ErrorCode::precondition_failed, typed_command.object_id);
          }
          const auto g = typed_command.geometry;
          if (std::abs(std::int64_t(g.start.x) - g.end.x) >
                  std::numeric_limits<int>::max() ||
              std::abs(std::int64_t(g.start.y) - g.end.y) >
                  std::numeric_limits<int>::max())
            return object_error(ErrorCode::invalid_input,
                                typed_command.object_id);
          object->arrow_geometry = typed_command.geometry;
          object->bounds = bounds_from_arrow(typed_command.geometry);
          if (!valid_geometry(*object))
            return object_error(ErrorCode::invalid_input,
                                typed_command.object_id);
          candidate.selected_object_id = typed_command.object_id;
        } else if constexpr (std::is_same_v<Command, RestyleAnnotation>) {
          const auto object = find_object(candidate.objects, typed_command.object_id);
          if (object == candidate.objects.end()) {
            return object_error(ErrorCode::object_not_found, typed_command.object_id);
          }
          if (!valid_style(object->kind, typed_command.style)) {
            return object_error(ErrorCode::invalid_input, typed_command.object_id);
          }
          object->style = typed_command.style;
          candidate.selected_object_id = typed_command.object_id;
        } else if constexpr (std::is_same_v<Command, EditAnnotationText>) {
          const auto object = find_object(candidate.objects, typed_command.object_id);
          if (object == candidate.objects.end()) {
            return object_error(ErrorCode::object_not_found, typed_command.object_id);
          }
          if (object->kind != AnnotationKind::text) {
            return object_error(ErrorCode::precondition_failed, typed_command.object_id);
          }
          object->text = typed_command.text;
          candidate.selected_object_id = typed_command.object_id;
        } else if constexpr (std::is_same_v<Command, UpdateTextAnnotation>) {
          const auto object = find_object(candidate.objects, typed_command.object_id);
          if (object == candidate.objects.end()) {
            return object_error(ErrorCode::object_not_found, typed_command.object_id);
          }
          if (object->kind != AnnotationKind::text || typed_command.bounds.empty() ||
              typed_command.text.empty() ||
              !valid_style(AnnotationKind::text, typed_command.style)) {
            return object_error(ErrorCode::invalid_input, typed_command.object_id);
          }
          object->bounds = typed_command.bounds;
          object->style = typed_command.style;
          object->text = typed_command.text;
          candidate.selected_object_id = typed_command.object_id;
        } else if constexpr (std::is_same_v<Command, DeleteAnnotation>) {
          const auto object = find_object(candidate.objects, typed_command.object_id);
          if (object == candidate.objects.end()) {
            return object_error(ErrorCode::object_not_found, typed_command.object_id);
          }
          candidate.objects.erase(object);
          candidate.selected_object_id.reset();
        }
        return std::nullopt;
      },
      command);

  if (mutation.has_value()) {
    return Result<AnnotationDocument, Error>::failure(mutation.value());
  }

  next.undo_stack_.push_back(document.current_);
  next.redo_stack_.clear();
  candidate.revision = document.current_.revision + 1;
  next.current_ = std::move(candidate);
  next.refresh_history_flags();
  return Result<AnnotationDocument, Error>::success(std::move(next));
  } catch (const std::bad_alloc&) {
    return Result<AnnotationDocument,Error>::failure({ErrorCode::state_inconsistent,
        "AnnotationDocument",Retryability::same_input,{{"reason","allocation_failed"}}});
  }
}

}  // namespace hdrshot
