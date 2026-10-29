#include "application/annotation_edit_service.hpp"
#include "domain/annotation/brush_stroke_builder.hpp"
#include "domain/annotation/annotation_geometry.hpp"
#include "domain/annotation/brush_coverage_kernel.hpp"
#include <limits>
#include "test_support.hpp"
#include <random>
using namespace hdrshot;
namespace {
template<class T> T take(Result<T, Error> v) { HDRSHOT_CHECK(v.has_value()); return std::move(v.value()); }
std::vector<unsigned char> unpack(const AnnotationCoverageLayer& layer, int w,int h) {
  std::vector<unsigned char> out(std::size_t(w)*h);
  for(const auto& row:layer.spans) for(std::size_t i=0;i<row.length();++i) {
    HDRSHOT_CHECK(row.x>=0 && row.y>=0 && row.y<h && row.x+int(i)<w);
    out[std::size_t(row.y)*w+row.x+i]=row.coverage_at(i);
  }
  return out;
}
void randomized_independent_distance_oracle() {
  std::mt19937 rng(0x6410427);
  for(double factor:{1.,1.25,1.5,2.}) for(int diameter:{2,4,24,32,48,72}) {
    auto scale=take(AnnotationScale::from(factor));
    for(int trial=0;trial<5;++trial) {
      std::vector<StrokePoint> points;
      for(int i=0;i<(trial==0?1:8);++i)
        points.push_back({17+double(rng()%6401)/100.,29+double(rng()%4801)/100.});
      if(trial==1) points={{17,29},{81,29},{81,77},{17,77},{17,29}};
      if(trial==2) points={{17,53},{81,53},{17,53},{81,53}};
      if(trial==3) points={{49,29},{49,77},{49,29},{49,77}};
      BrushStrokeBuilder builder;
      take(builder.begin({1},{0x111111,std::uint16_t(diameter)},points[0],{17,29,64,48},scale));
      for(std::size_t i=1;i<points.size();++i) take(builder.append(std::span(&points[i],1)));
      auto completed=take(builder.finish(points.back()));
      auto got=unpack(completed.preview.layer,64,48);
      auto doc=take(AnnotationDocument::apply(AnnotationDocument::empty(),CreateAnnotation{completed.object}));
      auto full=take(AnnotationRenderPlanner::build(doc.snapshot(),{17,29,64,48},factor));
      HDRSHOT_CHECK(got==unpack(full.ordered_layers[0],64,48));
      for(int y=0;y<48;++y) for(int x=0;x<64;++x) {
        double best=1e30;
        for(std::size_t i=0;i<points.size();++i) {
          auto a=points[i?i-1:0],b=points[i];
          const double vx=b.x-a.x,vy=b.y-a.y,px=x+17.5-a.x,py=y+29.5-a.y;
          const double length=vx*vx+vy*vy;
          const double t=length?std::clamp((px*vx+py*vy)/length,0.,1.):0.;
          best=std::min(best,std::sqrt((px-t*vx)*(px-t*vx)+(py-t*vy)*(py-t*vy)));
        }
        auto expected=static_cast<unsigned char>(std::round(std::clamp(diameter*factor/2.+.5-best,0.,1.)*255));
        HDRSHOT_CHECK(got[std::size_t(y)*64+x]==expected);
      }
    }
  }
}
void invalid_batch_and_lifetime() {
  auto scale=take(AnnotationScale::from(1));
  std::weak_ptr<const BrushPath> weak;
  {
    BrushStrokeBuilder builder;
    take(builder.begin({1},{0x111111,48},{40,40},{0,0,400,300},scale));
    auto before=take(builder.snapshot());
    StrokePoint invalid[]{{70,50},{NAN,60}};
    HDRSHOT_CHECK(!builder.append(invalid));
    HDRSHOT_CHECK(take(builder.snapshot()).layer==before.layer);
    auto completed=take(builder.finish({200,150}));
    weak=completed.object.brush_geometry->path;
    auto doc=take(AnnotationDocument::apply(AnnotationDocument::empty(),CreateAnnotation{completed.object}));
    auto second=completed.object; second.id={2}; second.brush_geometry->origin_x=10;
    second.bounds=take(brush_geometry_bounds(*second.brush_geometry));
    auto next=take(AnnotationDocument::apply(doc,CreateAnnotation{second}));
    HDRSHOT_CHECK(next.snapshot().objects[0].brush_geometry->path==next.snapshot().objects[1].brush_geometry->path);
    builder.cancel();
    HDRSHOT_CHECK(!weak.expired());
    HDRSHOT_CHECK(doc.snapshot().objects[0]==completed.object);
  }
  HDRSHOT_CHECK(weak.expired());
}
void clip_and_translation_match_full() {
  auto scale=take(AnnotationScale::from(1.5));
  for(auto start:std::vector<StrokePoint>{{0,0},{100,0},{0,80},{100,80},{100,20}}) {
    BrushStrokeBuilder builder;
    take(builder.begin({1},{0x36A3FF,48},start,{0,0,100,80},scale));
    auto completed=take(builder.finish(start));
    auto doc=take(AnnotationDocument::apply(AnnotationDocument::empty(),CreateAnnotation{completed.object}));
    auto plan=take(AnnotationRenderPlanner::build(doc.snapshot(),{0,0,100,80},1.5));
    AnnotationEditContext context{{1,{0,0,100,80}},scale};
    auto expanded=take(prepare_selection_change(doc,&plan,{2,{-50,-50,200,180}},context));
    auto full=take(AnnotationRenderPlanner::build(doc.snapshot(),{-50,-50,200,180},1.5));
    HDRSHOT_CHECK(*expanded.plan==full);
  }
}
void overflowing_clip_is_rejected_before_arithmetic() {
  auto scale=take(AnnotationScale::from(1));
  for (auto clip:std::vector<PixelRect>{{1000000000,0,2000000000,1},
                                      {0,1000000000,1,2000000000}}) {
    BrushStrokeBuilder builder;
    HDRSHOT_CHECK(!builder.begin({1},{0x111111,4},
                                 {double(clip.x),double(clip.y)},clip,scale));
  }
}
void finite_extremes_reject_whole_kernel_batch() {
  BrushCoverageAccumulator accumulator;
  const PixelRect clip{0,0,4,4};
  const StrokeSegment initial{{.5,.5},{1.5,.5}};
  take(accumulate_brush_segments(accumulator,std::span(&initial,1),1,clip));
  const auto before=freeze_brush_coverage(accumulator,{1},0x111111,clip);
  const auto counters=accumulator.counters;
  const double huge=std::numeric_limits<double>::max();
  for (const auto invalid:std::vector<StrokeSegment>{
      {{-huge,.5},{huge,.5}},{{.5,-huge},{.5,huge}},
      {{0,.5},{huge,.5}},{{0,0},{1e200,1e200}}}) {
    const StrokeSegment batch[]{{{2.5,2.5},{3.5,2.5}},invalid};
    HDRSHOT_CHECK(!accumulate_brush_segments(accumulator,batch,1,clip));
    HDRSHOT_CHECK(freeze_brush_coverage(accumulator,{1},0x111111,clip)==before);
    HDRSHOT_CHECK(accumulator.counters.segments_processed==counters.segments_processed);
    HDRSHOT_CHECK(accumulator.counters.sample_evaluations==counters.sample_evaluations);
    HDRSHOT_CHECK(accumulator.counters.frozen_rows==counters.frozen_rows);
    HDRSHOT_CHECK(accumulator.counters.copied_samples==counters.copied_samples);
  }
}
void transformed_brush_keeps_diameter_and_distance_coverage() {
  const std::vector<StrokePoint> points{{10.2,10.4},{40.6,10.4},{40.6,35.8},{10.2,10.4}};
  for (double factor:{1.,1.25,1.5,2.}) {
    auto scale=take(AnnotationScale::from(factor));
    BrushStrokeBuilder builder;
    take(builder.begin({1},{0x111111,4},points[0],{0,0,96,96},scale));
    take(builder.append(std::span(points).subspan(1)));
    auto object=take(builder.finish(points.back())).object;
    object.brush_geometry->origin_x=4.3; object.brush_geometry->origin_y=8.1;
    object.brush_geometry->scale_x=1.27; object.brush_geometry->scale_y=.73;
    object.bounds=take(brush_geometry_bounds(*object.brush_geometry));
    auto got=unpack(take(AnnotationRenderPlanner::rasterize_shape(object,{96,96},scale)),96,96);
    for (int y=0;y<96;++y) for(int x=0;x<96;++x) {
      double nearest=1e30;
      for(std::size_t i=1;i<points.size();++i) {
        const double ax=4.3+points[i-1].x*1.27,ay=8.1+points[i-1].y*.73;
        const double bx=4.3+points[i].x*1.27,by=8.1+points[i].y*.73;
        const double dx=bx-ax,dy=by-ay,px=x+.5-ax,py=y+.5-ay;
        const double t=std::clamp((px*dx+py*dy)/(dx*dx+dy*dy),0.,1.);
        nearest=std::min(nearest,std::sqrt((px-t*dx)*(px-t*dx)+(py-t*dy)*(py-t*dy)));
      }
      auto expected=static_cast<unsigned char>(std::round(std::clamp(2*factor+.5-nearest,0.,1.)*255));
      HDRSHOT_CHECK(got[std::size_t(y)*96+x]==expected);
    }
  }
}
}
int main() { return test::run({{"independent random and boundary distance oracle",randomized_independent_distance_oracle},{"invalid batch atomicity and shared path release",invalid_batch_and_lifetime},{"all corner ROI expansion matches full",clip_and_translation_match_full},{"overflowing clip rejected before arithmetic",overflowing_clip_is_rejected_before_arithmetic},{"finite extremes reject entire kernel batch",finite_extremes_reject_whole_kernel_batch},{"transformed brush radius and independent oracle",transformed_brush_keeps_diameter_and_distance_coverage}}); }
