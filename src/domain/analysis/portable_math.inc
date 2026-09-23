// Intentionally included twice: once as C++, once stringized as Metal source.
// Intrinsics a_pow/a_sqrt/a_atan2/a_floor/a_log2/a_finite/a_copysign are supplied by the
// executor. All formulas, constants, clipping, bins and mask decisions live here.
ANALYSIS_MATH_SOURCE(
struct A3 { float x; float y; float z; };
inline A3 a3(float x, float y, float z) { return A3{x,y,z}; }
inline A3 add3(A3 a, A3 b) { return a3(a.x+b.x,a.y+b.y,a.z+b.z); }
inline A3 scale3(A3 a, float s) { return a3(a.x*s,a.y*s,a.z*s); }
inline float dot3(A3 a, A3 b) { return (a.x*b.x+a.y*b.y)+a.z*b.z; }
inline bool finite3(A3 v) { return a_finite(v.x)&&a_finite(v.y)&&a_finite(v.z); }
inline float bound(float x,float lo,float hi) { return x<lo?lo:(x>hi?hi:x); }
inline A3 clip3(A3 v,float hi) { return a3(bound(v.x,0.0f,hi),bound(v.y,0.0f,hi),bound(v.z,0.0f,hi)); }
)
#include "domain/analysis/portable_color_math.inc"
ANALYSIS_MATH_SOURCE(
inline A3 working_rgb(A3 source,unsigned int space,float white) {
  A3 v=space<2u?clip3(source,1.0f):source;
  unsigned int gamut=working_gamut(space);
  if(gamut!=1u) v=rgb_from_xyz(xyz_from_rgb(v,1u),gamut);
  return clip3(v,space<2u?1.0f:10000.0f/white);
}
inline unsigned int signal_bin(float value,unsigned int bins) {
  float scaled=a_floor(bound(value,0.0f,1.0f)*float(bins));
  return scaled>=float(bins)?bins-1u:(unsigned int)(scaled);
}
// Source dimensions <=16384 and waveform width <=4096 keep this exact uint
// product in range. Avoid periodic CPU/GPU float boundary disagreements.
inline unsigned int source_column_bin(unsigned int x,unsigned int width,unsigned int bins) {
  return ((2u*x+1u)*bins)/(2u*width);
}
inline bool mask_contains(unsigned int kind,float x,float y,float left,float top,float width,float height) {
  if(kind==0u) return true;
  if(!(width>0.0f&&height>0.0f)||x<left||y<top||x>=left+width||y>=top+height) return false;
  if(kind==1u) return true;
  float dx=(x-left)/width*2.0f-1.0f,dy=(y-top)/height*2.0f-1.0f;
  return dx*dx+dy*dy<=1.0f;
}
inline unsigned int false_color_index(float y) {
  const float thresholds[14]={0.0028125f,0.00795495128834866f,0.0159099025766973f,
    0.0318198051533946f,0.0636396103067893f,0.1070286403502449f,
    0.1513613547456686f,0.2140572807004898f,0.3027227094913372f,
    0.5091168824543142f,1.0182337649086284f,2.0364675298172568f,
    4.0729350596345135f,11.52f};
  unsigned int i=0u; while(i<14u&&y>=thresholds[i]) ++i; return i;
}
inline A3 ui_rgb_to_linear_p3(unsigned int rgb) {
  A3 s=a3(srgb_decode(float((rgb>>16u)&255u)/255.0f),srgb_decode(float((rgb>>8u)&255u)/255.0f),srgb_decode(float(rgb&255u)/255.0f));
  return rgb_from_xyz(xyz_from_rgb(s,0u),1u);
}
inline A3 false_color_rgb(float y) {
  const unsigned int palette[15]={0x000000u,0x702090u,0x141898u,0x2058f0u,0x005828u,
    0x00a438u,0x70e068u,0x767676u,0xe4d000u,0xfff470u,0xf08000u,0xffb858u,
    0xd01818u,0xff7070u,0xffffffu};
  return ui_rgb_to_linear_p3(palette[false_color_index(y)]);
}
inline A3 display_srgb_from_work(A3 work,unsigned int space) {
  A3 c=clip3(rgb_from_xyz(xyz_from_rgb(work,working_gamut(space)),0u),1.0f);
  return a3(srgb_encode(c.x),srgb_encode(c.y),srgb_encode(c.z));
}
)
