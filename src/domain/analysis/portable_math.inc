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
inline float srgb_encode(float x) {
  float v=x<0.0f?-x:x;
  float e=v<=0.0031308f?12.92f*v:1.055f*a_pow(v,1.0f/2.4f)-0.055f;
  return a_copysign(e,x);
}
inline float srgb_decode(float x) {
  float v=x<0.0f?-x:x;
  float e=v<=0.04045f?v/12.92f:a_pow((v+0.055f)/1.055f,2.4f);
  return a_copysign(e,x);
}
inline float pq_encode(float nit) {
  float p=a_pow(bound(nit,0.0f,10000.0f)/10000.0f,2610.0f/16384.0f);
  return a_pow((3424.0f/4096.0f+(2413.0f/128.0f)*p)/(1.0f+(2392.0f/128.0f)*p),2523.0f/32.0f);
}
inline float pq_decode(float signal) {
  float p=a_pow(bound(signal,0.0f,1.0f),32.0f/2523.0f);
  return 10000.0f*a_pow(bound(p-3424.0f/4096.0f,0.0f,1.0f)/((2413.0f/128.0f)-(2392.0f/128.0f)*p),16384.0f/2610.0f);
}
inline A3 xyz_from_rgb(A3 v, unsigned int gamut) {
  if(gamut==0u) return a3(
    dot3(a3(0.4123907992659595f,0.3575843393838780f,0.1804807884018343f),v),
    dot3(a3(0.2126390058715104f,0.7151686787677560f,0.0721923153607337f),v),
    dot3(a3(0.0193308187155918f,0.1191947797946260f,0.9505321522496607f),v));
  if(gamut==2u) return a3(
    dot3(a3(0.6369580483012914f,0.1446169035862083f,0.1688809751641721f),v),
    dot3(a3(0.2627002120112671f,0.6779980715188708f,0.0593017164698620f),v),
    dot3(a3(0.0f,0.0280726930490874f,1.0609850577107910f),v));
  return a3(
    dot3(a3(0.4865709486482162f,0.2656676931690931f,0.1982172852343625f),v),
    dot3(a3(0.2289745640697488f,0.6917385218365064f,0.0792869140937450f),v),
    dot3(a3(0.0f,0.0451133818589026f,1.0439443689009757f),v));
}
inline A3 rgb_from_xyz(A3 v,unsigned int gamut) {
  if(gamut==0u) return a3(
    dot3(a3(3.2409699419045226f,-1.5373831775700940f,-0.4986107602930034f),v),
    dot3(a3(-0.9692436362808796f,1.8759675015077202f,0.0415550574071756f),v),
    dot3(a3(0.0556300796969937f,-0.2039769588889765f,1.0569715142428786f),v));
  if(gamut==2u) return a3(
    dot3(a3(1.716651187971268f,-0.355670783776392f,-0.253366281373660f),v),
    dot3(a3(-0.666684351832489f,1.616481236634939f,0.015768545813911f),v),
    dot3(a3(0.017639857445311f,-0.042770613257809f,0.942103121235474f),v));
  return a3(
    dot3(a3(2.493496911941425f,-0.931383617919124f,-0.402710784450717f),v),
    dot3(a3(-0.829488969561575f,1.762664060318346f,0.023624685841944f),v),
    dot3(a3(0.035845830243784f,-0.076172389268041f,0.956884524007687f),v));
}
inline unsigned int working_gamut(unsigned int space) { return space==0u?0u:(space==3u?2u:1u); }
inline A3 y_coefficients(unsigned int gamut) { return a3(
  xyz_from_rgb(a3(1,0,0),gamut).y,xyz_from_rgb(a3(0,1,0),gamut).y,xyz_from_rgb(a3(0,0,1),gamut).y); }
inline A3 working_rgb(A3 source,unsigned int space,float white) {
  A3 v=space<2u?clip3(source,1.0f):source;
  unsigned int gamut=working_gamut(space);
  if(gamut!=1u) v=rgb_from_xyz(xyz_from_rgb(v,1u),gamut);
  return clip3(v,space<2u?1.0f:10000.0f/white);
}
inline A3 lms_from_rgb(A3 v,unsigned int gamut) {
  if(gamut==1u) return a3(
    dot3(a3(0.334550623220059f,0.576391094387109f,0.089058282392832f),v),
    dot3(a3(0.158520235234873f,0.713612932646569f,0.127866832118557f),v),
    dot3(a3(0.020581197388829f,0.091695662564561f,0.887723140046610f),v));
  if(gamut!=2u) v=rgb_from_xyz(xyz_from_rgb(v,gamut),2u);
  return a3(dot3(a3(1688.0f/4096,2146.0f/4096,262.0f/4096),v),
    dot3(a3(683.0f/4096,2951.0f/4096,462.0f/4096),v),
    dot3(a3(99.0f/4096,309.0f/4096,3688.0f/4096),v));
}
inline A3 itp_from_lms(A3 v) {
  A3 p=a3(pq_encode(v.x),pq_encode(v.y),pq_encode(v.z));
  float lm=p.x-p.y,sm=p.z-p.y;
  return a3(0.5f*(p.x+p.y),
    (6610.0f*lm+7003.0f*sm)/8192.0f,
    (17933.0f*lm-543.0f*sm)/4096.0f);
}
inline float lab_f(float v) { return v>216.0f/24389.0f?a_pow(v,1.0f/3.0f):(24389.0f/27.0f*v+16.0f)/116.0f; }
inline A3 lab_from_xyz(A3 v) {
  float x=lab_f(v.x/0.9504559270516716f), y=lab_f(v.y), z=lab_f(v.z/1.0890577507598784f);
  return a3(116.0f*y-16.0f,500.0f*(x-y),200.0f*(y-z));
}
inline A3 ncl_from_signal(A3 s,unsigned int gamut) {
  A3 k=y_coefficients(gamut); float y=dot3(k,s);
  return a3(y,(s.z-y)/(2.0f*(1.0f-k.z)),(s.x-y)/(2.0f*(1.0f-k.x)));
}
inline float adobe_coordinate(float edr) {
  if(edr<=1.0f) return (4.0f/9.0f)*srgb_encode(bound(edr,0.0f,1.0f));
  return 4.0f/9.0f+a_log2(edr)/9.0f;
}
inline float adobe_axis(float edr) { return bound(adobe_coordinate(edr),0.0f,1.0f); }
inline float adobe_inverse(float u) { return u<=4.0f/9.0f?srgb_decode(u*9.0f/4.0f):a_pow(2.0f,u*9.0f-4.0f); }
inline float hue_degrees(float x,float y) {
  float angle=a_atan2(y,x)*57.29577951308232f;
  if(angle<0.0f) angle+=360.0f;
  return angle>=360.0f?0.0f:angle;
}
inline bool hue_defined(float chroma,unsigned int space) {
  return a_finite(chroma)&&chroma>(space>=2u?1.0f/720.0f:1.0f);
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
struct Signals {
  A3 encoded; A3 perceptual; A3 ncl; A3 adobe;
  float y_nit; float intensity; float hue; float chroma;
  bool valid; bool hue_valid;
};
inline Signals signals_from_work(A3 w,unsigned int space,float white) {
  Signals out{}; out.valid=finite3(w); if(!out.valid) return out;
  unsigned int gamut=working_gamut(space);
  out.y_nit=dot3(y_coefficients(gamut),w)*white;
  if(space>=2u) {
    A3 a=scale3(w,white),lms=lms_from_rgb(a,gamut);
    out.valid=finite3(lms)&&lms.x>=0.0f&&lms.y>=0.0f&&lms.z>=0.0f;
    if(!out.valid) return out;
    out.encoded=a3(pq_encode(a.x),pq_encode(a.y),pq_encode(a.z));
    out.perceptual=itp_from_lms(lms); out.intensity=out.perceptual.x;
  } else {
    out.encoded=a3(srgb_encode(w.x),srgb_encode(w.y),srgb_encode(w.z));
    out.perceptual=lab_from_xyz(xyz_from_rgb(w,gamut));
    out.intensity=dot3(y_coefficients(gamut),out.encoded);
  }
  out.ncl=ncl_from_signal(out.encoded,gamut);
  out.adobe=a3(adobe_axis(w.x),adobe_axis(w.y),adobe_axis(w.z));
  out.chroma=a_sqrt(out.perceptual.y*out.perceptual.y+out.perceptual.z*out.perceptual.z);
  out.hue_valid=hue_defined(out.chroma,space);
  out.hue=out.hue_valid?hue_degrees(out.perceptual.y,out.perceptual.z):0.0f;
  return out;
}
)
