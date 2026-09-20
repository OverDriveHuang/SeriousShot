#include "domain/annotation/annotation_geometry.hpp"
#include "test_support.hpp"
#include <cmath>
#include <limits>
#include <numbers>

namespace {
using namespace hdrshot;
constexpr double pi=std::numbers::pi;
const PixelRect limits{0,0,1000,1000};
AnnotationObject shape(AnnotationKind kind=AnnotationKind::rectangle) {
  AnnotationObject value;
  value.id={1}; value.kind=kind; value.bounds={300,350,200,100};
  value.style=ShapeStyle{0xFF4D67,4}; return value;
}
void near(double a,double b,double epsilon=1e-7) { HDRSHOT_CHECK(std::abs(a-b)<epsilon); }
void point_near(ShapePoint a,ShapePoint b) { near(a.x,b.x); near(a.y,b.y); }
void center_and_inverse_and_handles() {
  auto value=shape(); value.transform={0.71,0.25,0.75};
  const ShapePoint local{-79,23};
  point_near(shape_to_local(value,shape_to_world(value,local)),local);
  const auto h=shape_handles(value);
  point_near(shape_to_local(value,h[0]),{-100,-50});
  point_near(shape_to_local(value,h[4]),{100,50});
  const auto rotated=rotate_shape_until_boundary(value,0.5,limits);
  point_near(shape_center(rotated),shape_center(value));
  HDRSHOT_CHECK(rotated.bounds==value.bounds);
  HDRSHOT_CHECK(rotated.style==value.style);
}
void hot_zone_is_fixed_and_oriented() {
  for(int scale : {1,2}) {
    auto value=shape(); value.bounds={300*scale,350*scale,200*scale,100*scale};
    value.transform.rotation_radians=0.6;
    const auto hit=[&](double x,double y) { return shape_adjustment_at(value,
        shape_to_world(value,{x*scale,y*scale}),9.0*scale,36.0*scale); };
    HDRSHOT_CHECK(hit(0,0)==AnnotationAdjustment::move);
    HDRSHOT_CHECK(hit(100,0)==AnnotationAdjustment::east);
    HDRSHOT_CHECK(hit(100,50)==AnnotationAdjustment::south_east);
    HDRSHOT_CHECK(hit(115,0)==AnnotationAdjustment::rotate);
    HDRSHOT_CHECK(hit(122,0)==AnnotationAdjustment::rotate);
    HDRSHOT_CHECK(hit(136,0)==AnnotationAdjustment::rotate);
    HDRSHOT_CHECK(hit(144.9,0)==AnnotationAdjustment::rotate);
    HDRSHOT_CHECK(!hit(145.1,0));
    HDRSHOT_CHECK(hit(108.9,0)==AnnotationAdjustment::east);
  }
}
void all_resize_handles_preserve_opposite_anchor() {
  constexpr std::array<AnnotationAdjustment,8> handles{AnnotationAdjustment::north_west,
    AnnotationAdjustment::north,AnnotationAdjustment::north_east,AnnotationAdjustment::east,
    AnnotationAdjustment::south_east,AnnotationAdjustment::south,AnnotationAdjustment::south_west,AnnotationAdjustment::west};
  for(auto kind : {AnnotationKind::rectangle,AnnotationKind::ellipse}) for(std::size_t i=0;i<8;++i) {
    auto value=shape(kind); value.transform.rotation_radians=0.7;
    const auto points=shape_handles(value);
    const auto local=shape_to_local(value,points[i]);
    const auto pointer=shape_to_world(value,{local.x*1.25,local.y*1.5});
    const auto resized=resize_shape(value,handles[i],points[i],pointer,limits,4);
    near(resized.transform.rotation_radians,value.transform.rotation_radians);
    point_near(shape_handles(resized)[(i+4)%8],points[(i+4)%8]);
    HDRSHOT_CHECK(shape_fits(resized,limits));
  }
}
void rotation_stops_at_first_invalid_arc() {
  auto value=shape(); value.bounds={30,80,240,140};
  const PixelRect tight{0,0,300,300};
  HDRSHOT_CHECK(shape_fits(value,tight));
  // End 180 degrees is valid, but a diagonal in between is not.
  value.bounds={15,80,270,140};
  const auto result=rotate_shape_until_boundary(value,pi,tight);
  HDRSHOT_CHECK(result.transform.rotation_radians>0 && result.transform.rotation_radians<pi/2);
  HDRSHOT_CHECK(shape_fits(result,tight));
  auto too_far=result; too_far.transform.rotation_radians+=0.0001;
  HDRSHOT_CHECK(!shape_fits(too_far,tight));
  const auto reversed=rotate_shape_until_boundary(result,-0.1,tight);
  near(reversed.transform.rotation_radians,result.transform.rotation_radians-0.1);
  point_near(shape_center(result),shape_center(value));
}
void ellipse_bounds_are_recomputed_from_contour_not_selection_box() {
  auto value=shape(AnnotationKind::ellipse);
  value.bounds={100,190,200,20};
  value.transform.rotation_radians=pi/4;
  const PixelRect tight{124,124,152,152};
  const auto extent=std::sqrt(5050.0);
  const auto bounds=shape_bounds(value);
  near(bounds.left,200-extent); near(bounds.right,200+extent);
  near(bounds.top,200-extent); near(bounds.bottom,200+extent);
  HDRSHOT_CHECK(shape_fits(value,tight));
  // The rotated editing box has an outside corner, but no visible ellipse
  // stroke is outside. Do not stop rotation at that empty control-box corner.
  const auto handles=shape_handles(value);
  HDRSHOT_CHECK(handles[2].x>tight.right());
  auto axis_aligned=value; axis_aligned.transform.rotation_radians=0;
  HDRSHOT_CHECK(!shape_fits(axis_aligned,tight));
  for (int i=0;i<1000;++i) {
    const auto angle=2*pi*i/1000;
    const auto point=shape_to_world(value,{100*std::cos(angle),10*std::sin(angle)});
    HDRSHOT_CHECK(point.x>=bounds.left-1e-8 && point.x<=bounds.right+1e-8);
    HDRSHOT_CHECK(point.y>=bounds.top-1e-8 && point.y<=bounds.bottom+1e-8);
  }
}
void circle_wrap_and_drag_history() {
  auto value=shape(AnnotationKind::ellipse); value.bounds={300,300,100,100};
  const auto center=shape_center(value);
  const auto pointer=[&](double angle) { return ShapePoint{center.x+100*std::cos(angle),center.y+100*std::sin(angle)}; };
  ShapeRotationDrag drag(value,pointer(pi-0.02));
  const auto result=drag.update(pointer(-pi+0.03),limits);
  near(result.transform.rotation_radians,0.05);
  near(shape_bounds(result).left,shape_bounds(value).left);
  auto document=AnnotationDocument::apply(AnnotationDocument::empty(),CreateAnnotation{value}).value();
  document=AnnotationDocument::apply(document,SetShapeGeometry{value.id,result.bounds,result.transform}).value();
  const auto undone=AnnotationDocument::apply(document,UndoAnnotation{}).value();
  HDRSHOT_CHECK(undone.snapshot().objects.front()==value);
  const auto redone=AnnotationDocument::apply(undone,RedoAnnotation{}).value();
  HDRSHOT_CHECK(redone.snapshot().objects.front()==result);
  const auto restyled=AnnotationDocument::apply(redone,RestyleAnnotation{value.id,ShapeStyle{0xFFFFFF,8}}).value();
  HDRSHOT_CHECK(restyled.snapshot().objects.front().transform==result.transform);
  const auto moved=move_shape(result,{10000,10000},limits);
  HDRSHOT_CHECK(shape_fits(moved,limits));
  near(moved.transform.rotation_radians,result.transform.rotation_radians);
}
void invalid_transform_is_transactional() {
  auto value=shape();
  const auto doc=AnnotationDocument::apply(AnnotationDocument::empty(),CreateAnnotation{value}).value();
  for(auto bad : {std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity(),10.0}) {
    const auto changed=AnnotationDocument::apply(doc,SetShapeGeometry{value.id,value.bounds,{bad,0,0}});
    HDRSHOT_CHECK(!changed);
    HDRSHOT_CHECK(doc.snapshot().objects.front()==value);
  }
  value.kind=AnnotationKind::text; value.transform.rotation_radians=0.2;
  HDRSHOT_CHECK(!valid_shape_transform(value));
}
} // namespace
int main() { return hdrshot::test::run({
  {"center inverse and oriented handles",center_and_inverse_and_handles},
  {"fixed logical hot zone at different scales",hot_zone_is_fixed_and_oriented},
  {"all resized handles preserve opposite anchor",all_resize_handles_preserve_opposite_anchor},
  {"first illegal rotation arc cannot be skipped",rotation_stops_at_first_invalid_arc},
  {"ellipse bounds follow contour not editing box",ellipse_bounds_are_recomputed_from_contour_not_selection_box},
  {"circle wrap and single history command",circle_wrap_and_drag_history},
  {"nonfinite transforms rejected transactionally",invalid_transform_is_transactional}}); }
