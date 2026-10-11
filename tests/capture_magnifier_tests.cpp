#include "domain/capture/magnifier.hpp"
#include "domain/analysis/defaults.hpp"
#include "test_support.hpp"
#include <limits>

namespace {
using namespace hdrshot;
void physical_pixel_boundaries() {
  // Display-local coordinates after the existing desktop-origin transform.
  for (double dpr : {1., 1.25, 1.5, 2.}) {
    const double desktop_left = -1536., desktop_top = -720.;
    const auto p = magnifier_source_point((-1536.+19.75)-desktop_left,
        (-720.+7.25)-desktop_top, dpr, {320,200});
    HDRSHOT_CHECK(p.x == int(std::floor(19.75*dpr)));
    HDRSHOT_CHECK(p.y == int(std::floor(7.25*dpr)));
    const auto before = magnifier_source_point((32.-0.001)/dpr, 0, dpr, {320,200});
    const auto after = magnifier_source_point(32./dpr, 0, dpr, {320,200});
    HDRSHOT_CHECK(before.x == 31);
    HDRSHOT_CHECK(after.x == 32);
  }
  HDRSHOT_CHECK((magnifier_source_point(-.1,-4,2,{320,200}) == PixelPoint{0,0}));
  HDRSHOT_CHECK((magnifier_source_point(999,999,2,{320,200}) == PixelPoint{319,199}));
}
void nearest_neighbor_center_and_missing_edges() {
  for (int zoom : {4,8,12,16}) {
    MagnifierPreview m{true,{0,0},{30,50,7*zoom,5*zoom},zoom,9};
    // A synthetic coordinate ramp distinguishes every source pixel. All
    // destination texels in an enlarged cell must select one exact source.
    for (int row=0;row<5;++row) for(int col=0;col<7;++col)
      for(int dy=0;dy<zoom;++dy) for(int dx=0;dx<zoom;++dx) {
        const auto p=magnifier_texel(m,{30+col*zoom+dx,50+row*zoom+dy});
        HDRSHOT_CHECK((p==PixelPoint{col-3,row-2}));
      }
    HDRSHOT_CHECK((magnifier_texel(m,{30+3*zoom,50+2*zoom}) == PixelPoint{0,0}));
    HDRSHOT_CHECK(magnifier_texel(m,{30,50}).x < 0); // native renderer leaves blank
    m.center={319,199};
    HDRSHOT_CHECK((magnifier_texel(m,{30+6*zoom,50+4*zoom}) == PixelPoint{322,201}));
    for (int width : {zoom,3*zoom,127,256,399}) {
      const int count=magnifier_odd_pixels(width,zoom);
      HDRSHOT_CHECK(count>0 && count%2==1);
      HDRSHOT_CHECK(count*zoom<=width);
      HDRSHOT_CHECK((count+2)*zoom>width);
    }
  }
}
void approved_defaults_and_shared_white() {
  const AnalysisDefaults d;
  HDRSHOT_CHECK(valid_analysis_defaults(d));
  HDRSHOT_CHECK(d.magnifier_enabled && d.magnifier_zoom==8);
  HDRSHOT_CHECK(d.reference_white_nits==203 && d.blur_sigma_px==0 && d.sample_side==1);
  HDRSHOT_CHECK(d.magnifier.selected[0] && d.magnifier.selected[3]);
  HDRSHOT_CHECK(std::count(d.magnifier.selected.begin(),d.magnifier.selected.end(),true)==2);
  HDRSHOT_CHECK(d.analyzer.selected[2] && std::count(d.analyzer.selected.begin(),d.analyzer.selected.end(),true)==1);
  auto alternate=d;alternate.reference_white_nits=100;alternate.blur_sigma_px=8;
  for(bool hdr:{false,true})HDRSHOT_CHECK(analysis_settings(alternate,hdr).blur_sigma_px==8);

}
void invalid_configuration_is_rejected() {
  const AnalysisDefaults baseline;
  for(int zoom : {0,1,7,32}) {auto d=baseline;d.magnifier_zoom=zoom;HDRSHOT_CHECK(!valid_analysis_defaults(d));}
  for(int side : {0,2,7,102}) {auto d=baseline;d.sample_side=side;HDRSHOT_CHECK(!valid_analysis_defaults(d));}
  for(double white : {0.,80.,204.,std::numeric_limits<double>::infinity()}) {auto d=baseline;d.reference_white_nits=white;HDRSHOT_CHECK(!valid_analysis_defaults(d));}
  for(double sigma : {-1.,.25,3.,std::numeric_limits<double>::quiet_NaN()}) {auto d=baseline;d.blur_sigma_px=sigma;HDRSHOT_CHECK(!valid_analysis_defaults(d));}
  auto d=baseline;d.hdr_space=analysis::WorkingSpace::srgb_sdr;HDRSHOT_CHECK(!valid_analysis_defaults(d));
  d=baseline;d.sdr_space=analysis::WorkingSpace::bt2020_pq;HDRSHOT_CHECK(!valid_analysis_defaults(d));
}
}
int main() {return hdrshot::test::run({
  {"physical pixel boundaries at mixed DPR and negative desktop origin",physical_pixel_boundaries},
  {"nearest neighbor exact cells, center, and missing edge coordinates",nearest_neighbor_center_and_missing_edges},
  {"approved defaults share white/space and magnifier bypasses blur",approved_defaults_and_shared_white},
  {"invalid global defaults are rejected",invalid_configuration_is_rejected}});}
