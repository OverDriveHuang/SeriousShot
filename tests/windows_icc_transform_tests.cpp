#include "platform/windows/windows_icc_transform.hpp"
#include "platform/windows/windows_gpu_source.hpp"
#include "platform/windows/windows_capture_policy.hpp"
#include "test_support.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <system_error>
#include <string>
#include <vector>
#include <windows.h>

using namespace hdrshot;
namespace {
// Project-owned, purpose-built matrix/TRC parser fixtures, not ICC certification
// samples. Fixed big-endian fields avoid any installed-profile dependency.
using Bytes=std::vector<std::uint8_t>;
constexpr std::int32_t pcs_fixed[3][3]={{39322,13107,10761},
                                          {19661,39322,6553},
                                          {1311,5243,47507}};
constexpr const char *parametric_hash="e418b0055ba5e3fdb1f2f89b3bf1972f2dc83ff243447feedffa0ac454c09455";
constexpr const char *sampled1024_hash="cb0dd2d77ff67c94bea9b93109dee6ce4bda498bc7529da47d4e1088bc33bc96";
constexpr const char *sampled256_hash="f27a79605b595ca7b9cde513c82b271a29af66c8cb3a9f396d38aa5209958b6f";
void put16(Bytes &bytes,std::size_t pos,std::uint16_t value) {
  bytes[pos]=static_cast<std::uint8_t>(value>>8);
  bytes[pos+1]=static_cast<std::uint8_t>(value);
}
void put32(Bytes &bytes,std::size_t pos,std::uint32_t value) {
  bytes[pos]=static_cast<std::uint8_t>(value>>24);
  bytes[pos+1]=static_cast<std::uint8_t>(value>>16);
  bytes[pos+2]=static_cast<std::uint8_t>(value>>8);
  bytes[pos+3]=static_cast<std::uint8_t>(value);
}
void signature(Bytes &bytes,std::size_t pos,const char *four) {
  for(int i=0;i<4;++i) bytes[pos+i]=static_cast<std::uint8_t>(four[i]);
}
void append_tag(Bytes &profile,int index,const char *name,const Bytes &payload) {
  while(profile.size()%4) profile.push_back(0);
  const auto offset=static_cast<std::uint32_t>(profile.size());
  const auto table=132+12*index;
  signature(profile,table,name);
  put32(profile,table+4,offset);
  put32(profile,table+8,static_cast<std::uint32_t>(payload.size()));
  profile.insert(profile.end(),payload.begin(),payload.end());
}
Bytes make_profile(const std::array<std::vector<std::uint16_t>,3> &curves,
                   bool parametric=false) {
  Bytes profile(132+6*12,0);
  profile[8]=parametric?4:2;
  // Fixed ICC date (2026-09-28), D50 PCS illuminant and synthetic creator.
  constexpr std::uint16_t date[]={2026,9,28,0,0,0};
  for(int i=0;i<6;++i) put16(profile,24+2*i,date[i]);
  signature(profile,12,"mntr");signature(profile,16,"RGB ");
  signature(profile,20,"XYZ ");signature(profile,36,"acsp");
  put32(profile,68,63190);put32(profile,72,65536);put32(profile,76,54061);
  signature(profile,80,"SSft");
  put32(profile,128,6);
  constexpr const char *xyz_names[]={"rXYZ","gXYZ","bXYZ"};
  constexpr const char *trc_names[]={"rTRC","gTRC","bTRC"};
  for(int channel=0;channel<3;++channel) {
    Bytes xyz(20,0);signature(xyz,0,"XYZ ");
    for(int row=0;row<3;++row)
      put32(xyz,8+row*4,static_cast<std::uint32_t>(pcs_fixed[row][channel]));
    append_tag(profile,channel,xyz_names[channel],xyz);
    Bytes trc;
    if(parametric) {
      // ICC type 4: x<1/4 ? 3x/4+169/4096 : (13x/16+1/8)^2+31/256.
      // All terms are active; branches meet at 1/4 and the upper end is 1.
      trc.resize(40,0);signature(trc,0,"para");put16(trc,8,4);
      constexpr std::uint32_t values[]={2u<<16,13u<<12,1u<<13,3u<<14,
                                         1u<<14,31u<<8,169u<<4};
      for(int i=0;i<7;++i) put32(trc,12+4*i,values[i]);
    } else {
      trc.resize(12+2*curves[channel].size(),0);
      signature(trc,0,"curv");
      put32(trc,8,static_cast<std::uint32_t>(curves[channel].size()));
      for(std::size_t i=0;i<curves[channel].size();++i)
        put16(trc,12+2*i,curves[channel][i]);
    }
    append_tag(profile,3+channel,trc_names[channel],trc);
  }
  put32(profile,0,static_cast<std::uint32_t>(profile.size()));
  return profile;
}
Bytes make_sampled_profile(int count) {
  std::array<std::vector<std::uint16_t>,3> curves{};
  const auto last=std::uint64_t(count-1);
  const auto square=last*last;
  const auto rounded=[](std::uint64_t numerator,std::uint64_t denominator) {
    return static_cast<std::uint16_t>((numerator+denominator/2)/denominator);
  };
  for(int i=0;i<count;++i) {
    const auto index=std::uint64_t(i);
    // Deliberately unequal channels exercise separate GPU texture slices.
    curves[0].push_back(rounded(65535*index,last));
    curves[1].push_back(rounded(65535*index*index,square));
    curves[2].push_back(rounded(65535*(index*last+index*index),2*square));
  }
  return make_profile(curves);
}
class FixtureStore {
public:
  FixtureStore() {
    const auto base=std::filesystem::temp_directory_path();
    const auto tick=std::chrono::steady_clock::now().time_since_epoch().count();
    for(int i=0;i<100;++i) {
      directory_=base/("seriousshot-icc-"+std::to_string(GetCurrentProcessId())+"-"+
                       std::to_string(tick)+"-"+std::to_string(i));
      std::error_code error;
      if(std::filesystem::create_directory(directory_,error)) break;
      directory_.clear();
    }
    HDRSHOT_CHECK(!directory_.empty());
    parametric=write("synthetic-parametric-v4.icc",make_profile({},true));
    sampled1024=write("synthetic-sampled-1024-v2.icc",make_sampled_profile(1024));
    sampled256=write("synthetic-sampled-256-v2.icc",make_sampled_profile(256));
  }
  ~FixtureStore() {
    std::error_code error;
    for(const auto &path:files_) std::filesystem::remove(path,error);
    std::filesystem::remove(directory_,error);
  }
  std::string write(const char *name,const Bytes &bytes) {
    const auto path=directory_/name;
    std::ofstream out(path,std::ios::binary);
    HDRSHOT_CHECK(bool(out));
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    out.close();HDRSHOT_CHECK(bool(out));
    files_.push_back(path);
    return utf8(path);
  }
  std::string absent() const {return utf8(directory_/"absent-profile.icc");}
  static std::string utf8(const std::filesystem::path &path) {
    const auto u=path.u8string();return {u.begin(),u.end()};
  }
  std::string parametric,sampled1024,sampled256;
private:
  std::filesystem::path directory_;
  std::vector<std::filesystem::path> files_;
};
FixtureStore &fixtures() {static FixtureStore store;return store;}
std::shared_ptr<const WindowsIccProfile> synthetic() {
  auto loaded=windows_load_icc_profile(fixtures().parametric);
  HDRSHOT_CHECK(loaded.has_value());
  HDRSHOT_CHECK(loaded.value()->sha256==parametric_hash);
  HDRSHOT_CHECK(loaded.value()->decode_curves[0].type==4);
  return loaded.value();
}
double decode_device(double x) {
  // Independent mathematical oracle for the synthetic type-4 curve.
  x=std::clamp(x,0.0,1.0);
  return x<0.25?3*x/4+169.0/4096:std::pow(13*x/16+1.0/8,2)+31.0/256;
}
std::array<double,3> oracle(std::array<double,3> encoded) {
  // Frozen synthetic PCS matrix. Derive adaptation from Bradford
  // cone responses of D50 and D65, not the production 3x3 adaptation table.
  constexpr double pcs[3][3]={{39322.0/65536,13107.0/65536,10761.0/65536},
                              {19661.0/65536,39322.0/65536,6553.0/65536},
                              {1311.0/65536,5243.0/65536,47507.0/65536}};
  constexpr double cone[3][3]={{0.8951,0.2664,-0.1614},
                                {-0.7502,1.7135,0.0367},
                                {0.0389,-0.0685,1.0296}};
  constexpr double inverse_cone[3][3]={{0.9869929,-0.1470543,0.1599627},
                                        {0.4323053,0.5183603,0.0492912},
                                        {-0.0085287,0.0400428,0.9684867}};
  constexpr double p3[3][3]={{2.493496911941425,-0.9313836179191239,-0.40271078445071684},
                              {-0.8294889695615747,1.7626640603183463,0.0236246858419436},
                              {0.03584583024378447,-0.07617238926804182,0.9568845240076872}};
  const auto adapt=[&](std::array<double,3> xyz) {
    // ICC.1:2022 PCS Table 14; P3 D65 follows xy=(0.3127,0.3290).
    constexpr std::array<double,3> d50{0.9642,1.0,0.8249},
        d65_white{0.9504559270516716,1.0,1.0890577507598784};
    std::array<double,3> cone_xyz{},cone_d50{},cone_d65{},adapted{};
    for(int y=0;y<3;++y) for(int x=0;x<3;++x) {
      cone_xyz[y]+=cone[y][x]*xyz[x];
      cone_d50[y]+=cone[y][x]*d50[x];
      cone_d65[y]+=cone[y][x]*d65_white[x];
    }
    for(int y=0;y<3;++y) cone_xyz[y]*=cone_d65[y]/cone_d50[y];
    for(int y=0;y<3;++y) for(int x=0;x<3;++x) adapted[y]+=inverse_cone[y][x]*cone_xyz[x];
    return adapted;
  };
  std::array<double,3> xyz{},result{};
  for(auto &v:encoded) v=decode_device(v);
  for(int y=0;y<3;++y) for(int x=0;x<3;++x) xyz[y]+=pcs[y][x]*encoded[x];
  const auto d65=adapt(xyz);
  for(int y=0;y<3;++y) for(int x=0;x<3;++x) result[y]+=p3[y][x]*d65[x];
  return result;
}
float eotf(float x) {return x<=0.04045f?x/12.92f:std::pow((x+0.055f)/1.055f,2.4f);}
float oetf(float x) {return x<=0.0031308f?12.92f*x:1.055f*std::pow(x,1.0f/2.4f)-0.055f;}

std::uint16_t to_half(float x) {
  // Round-to-nearest-even IEEE binary16 test fixture generator.
  const auto bits=std::bit_cast<std::uint32_t>(x);
  const std::uint16_t sign=static_cast<std::uint16_t>((bits>>16)&0x8000);
  int exp=int((bits>>23)&255)-127+15;
  std::uint32_t mant=bits&0x7fffff;
  if(exp<=0) {
    if(exp< -10) return sign;
    mant|=0x800000;
    const int shift=14-exp;
    const auto rounded=(mant+((1u<<(shift-1))-1)+((mant>>shift)&1))>>shift;
    return sign|static_cast<std::uint16_t>(rounded);
  }
  if(exp>=31) return sign|0x7c00;
  mant=mant+0xfff+((mant>>13)&1);
  if(mant&0x800000) {mant=0;++exp;}
  return sign|static_cast<std::uint16_t>((exp<<10)|(mant>>13));
}
float from_half(std::uint16_t bits) {
  const float sign=(bits&0x8000)?-1.0f:1.0f;
  const int e=(bits>>10)&31;
  const int f=bits&1023;
  return sign*(e?std::ldexp(1.0f+f/1024.0f,e-15):std::ldexp(f/1024.0f,-14));
}
void parametric_profile_and_independent_oracle() {
  const auto p=synthetic();
  // The synthetic profile's neutral white must remain neutral after PCS D50 to
  // Display P3 D65 adaptation; this catches a reversed adaptation matrix.
  const auto white=windows_icc_device_to_p3_cpu(*p,{1,1,1});
  HDRSHOT_CHECK_NEAR(white[0],white[1],0.001);
  HDRSHOT_CHECK_NEAR(white[1],white[2],0.001);
  for(const auto encoded:{std::array<float,3>{0,0,0},{1,1,1},
                          {0.25f,0.25f,0.25f},{0.5f,0.5f,0.5f},
                          {200/255.0f,100/255.0f,60/255.0f},
                          {60/255.0f,200/255.0f,100/255.0f},
                          {100/255.0f,60/255.0f,200/255.0f}}) {
    const auto expected=oracle({encoded[0],encoded[1],encoded[2]});
    const auto actual=windows_icc_device_to_p3_cpu(*p,encoded);
    for(int c=0;c<3;++c) HDRSHOT_CHECK_NEAR(actual[c],expected[c],2e-6);
    const auto restored=windows_icc_p3_to_device_cpu(*p,actual);
    for(int c=0;c<3;++c) HDRSHOT_CHECK_NEAR(restored[c],encoded[c],1e-5);
  }
}
void gpu_gain_color_and_10bit_steps() {
  const auto p=synthetic();
  constexpr int count=1024;
  std::vector<std::uint16_t> rgba(count*4);
  for(int i=0;i<count;++i) {
    // Gray and three varying channels are covered by cycling fixed chroma.
    const float device=float(i)/1023.0f;
    rgba[i*4+0]=to_half(eotf(device));
    rgba[i*4+1]=to_half(eotf((i%3==0)?device:0.37f));
    rgba[i*4+2]=to_half(eotf((i%3==1)?device:0.69f));
    rgba[i*4+3]=0x7e00; // ignored alpha NaN
  }
  for(float gain:{0.5f,1.0f}) {
    auto source=windows_normalize_scrgb_source({count,1},rgba,1.0f,gain,p);
    HDRSHOT_CHECK(source.has_value());
    auto region=source.value()->read_region({0,0,count,1});
    HDRSHOT_CHECK(region.has_value());
    for(int i=0;i<count;++i) {
      std::array<double,3> encoded{};
      for(int c=0;c<3;++c)
        encoded[c]=oetf(gain*from_half(rgba[i*4+c]));
      const auto expected=oracle(encoded);
      for(int c=0;c<3;++c)
        HDRSHOT_CHECK_NEAR(region.value()[i*4+c],expected[c],3e-5);
      HDRSHOT_CHECK_NEAR(region.value()[i*4+3],1.0,0);
    }
    // The 10-bit input must not collapse to an 8-bit intermediate.
    if(gain==1.0f) {
      for(int i=500;i<503;++i)
        HDRSHOT_CHECK(region.value()[(i+1)*4]!=region.value()[i*4]);
    }
  }
}
void doubled_capture_with_manual_half_gain() {
  const auto p=synthetic();
  constexpr int count=1024;
  std::vector<std::uint16_t> rgba(count*4);
  for(int i=0;i<count;++i) {
    const float code=float(i)/1023.0f;
    rgba[i*4+0]=to_half(2.0f*eotf(code));
    rgba[i*4+1]=to_half(2.0f*eotf((i%2)?0.72f:code));
    rgba[i*4+2]=to_half(2.0f*eotf((i%3)?0.24f:code));
    rgba[i*4+3]=0;
  }
  auto source=windows_normalize_scrgb_source({count,1},rgba,1.0f,0.5,p);
  HDRSHOT_CHECK(source.has_value());
  auto region=source.value()->read_region({0,0,count,1});
  HDRSHOT_CHECK(region.has_value());
  for(int i=0;i<count;++i) {
    std::array<double,3> restored{};
    for(int c=0;c<3;++c) restored[c]=oetf(0.5f*from_half(rgba[i*4+c]));
    const auto reference=oracle(restored);
    for(int c=0;c<3;++c)
      HDRSHOT_CHECK_NEAR(region.value()[i*4+c],reference[c],3e-5);
  }
}
void compatibility_states_preserve_legacy_icc_order() {
  const auto profile=synthetic();
  const std::vector<std::uint16_t> raw{
      0x4000,0x4000,0x4000,0, 0x3400,0x3800,0x3e00,0,
      0xb400,0,0,0, 0,0x4000,0x4a00,0};
  for (bool gain_on : {false,true}) for (bool bypass : {false,true})
    for (double stored_gain : {0.0,0.5,1.0,3.0}) {
      const auto adjustment=resolve_capture_adjustment({gain_on,bypass,stored_gain},false,1.0F);
      HDRSHOT_CHECK(adjustment.has_value());
      auto source=windows_normalize_scrgb_source({4,1},raw,
          adjustment.value().white_scale,adjustment.value().effective_gain,profile);
      HDRSHOT_CHECK(source.has_value());
      auto pixels=source.value()->read_region({0,0,4,1});
      HDRSHOT_CHECK(pixels.has_value());
      const double gain=gain_on?stored_gain:1.0;
      for (int p=0;p<4;++p) {
        std::array<double,3> device{};
        for (int c=0;c<3;++c) device[c]=oetf(static_cast<float>(gain*from_half(raw[p*4+c])));
        const auto expected=oracle(device);
        for (int c=0;c<3;++c) HDRSHOT_CHECK_NEAR(pixels.value()[p*4+c],expected[c],3e-5);
        HDRSHOT_CHECK_NEAR(pixels.value()[p*4+3],1,0);
      }
    }
}
void sampled_v2_curve_tables_and_gpu() {
  auto sampled1024=windows_load_icc_profile(fixtures().sampled1024);
  auto sampled256=windows_load_icc_profile(fixtures().sampled256);
  HDRSHOT_CHECK(sampled1024.has_value());
  HDRSHOT_CHECK(sampled256.has_value());
  HDRSHOT_CHECK(sampled1024.value()->sha256==sampled1024_hash);
  HDRSHOT_CHECK(sampled256.value()->sha256==sampled256_hash);
  HDRSHOT_CHECK(sampled1024.value()->decode_curves[0].samples.size()==1024);
  HDRSHOT_CHECK(sampled256.value()->decode_curves[0].samples.size()==256);
  for(const auto &profile:{sampled1024.value(),sampled256.value()}) {
    const auto &curves=profile->decode_curves;
    const auto midpoint=curves[0].samples.size()/2;
    HDRSHOT_CHECK(curves[0].samples[midpoint]>curves[1].samples[midpoint]);
    HDRSHOT_CHECK(curves[2].samples[midpoint]>curves[1].samples[midpoint]);
    HDRSHOT_CHECK(curves[0].samples[midpoint]>curves[2].samples[midpoint]);
  }
  constexpr int count=1024;
  std::vector<std::uint16_t> rgba(count*4);
  for(int i=0;i<count;++i) {
    const float x=float(i)/1023;
    rgba[i*4+0]=to_half(eotf(x));
    rgba[i*4+1]=to_half(eotf((i%3)?0.31f:x));
    rgba[i*4+2]=to_half(eotf((i%3)?0.73f:x));
    rgba[i*4+3]=0x7e00;
  }
  for(const auto &profile:{sampled1024.value(),sampled256.value()}) {
    auto source=windows_normalize_scrgb_source({count,1},rgba,1.0f,1.0,profile);
    HDRSHOT_CHECK(source.has_value());
    auto pixels=source.value()->read_region({0,0,count,1});
    HDRSHOT_CHECK(pixels.has_value());
    for(int i=0;i<count;++i) {
      std::array<float,3> encoded{};
      for(int c=0;c<3;++c) encoded[c]=oetf(from_half(rgba[i*4+c]));
      const auto expected=windows_icc_device_to_p3_cpu(*profile,encoded);
      for(int c=0;c<3;++c)
        HDRSHOT_CHECK_NEAR(pixels.value()[i*4+c],expected[c],3e-5);
    }
  }
}
void reject_bad_curves_and_luts() {
  const auto original=make_profile({},true);
  // The generated six-tag table has its first TRC at slot 3.
  const auto trc_offset=[](const Bytes &bytes) {
    const auto p=132+3*12+4;
    return (std::uint32_t(bytes[p])<<24)|(std::uint32_t(bytes[p+1])<<16)|
           (std::uint32_t(bytes[p+2])<<8)|bytes[p+3];
  };
  auto reject=[&](const char *name,const Bytes &data) {
    const auto loaded=windows_load_icc_profile(fixtures().write(name,data));
    if(loaded) std::cerr << "Unexpected ICC acceptance: " << name << '\n';
    HDRSHOT_CHECK(!loaded.has_value());
  };
  auto bad=original;signature(bad,trc_offset(bad),"curv");
  reject("bad-curve.icc",bad);
  bad=original;put32(bad,trc_offset(bad)+12,0); // zero gamma
  reject("zero-gamma.icc",bad);
  bad=original;signature(bad,132,"A2B0");
  reject("unsupported-lut.icc",bad);
  bad=original;put32(bad,0,static_cast<std::uint32_t>(bad.size()-1));
  reject("bad-size.icc",bad);
  HDRSHOT_CHECK(!windows_load_icc_profile(fixtures().absent()).has_value());
}
void sampled_curve_forms_and_plateau_policy() {
  auto make=[&](const char *name,const std::vector<std::uint16_t> &values) {
    const std::array<std::vector<std::uint16_t>,3> curves{values,values,values};
    return windows_load_icc_profile(fixtures().write(name,make_profile(curves)));
  };
  auto identity=make("curv-identity.icc",{});
  HDRSHOT_CHECK(identity.has_value());
  HDRSHOT_CHECK(identity.value()->decode_curves[0].type==-1);
  auto gamma=make("curv-gamma.icc",{0x0200});
  HDRSHOT_CHECK(gamma.has_value());
  HDRSHOT_CHECK_NEAR(gamma.value()->decode_curves[0].parameters[0],2.0,0);
  auto plateau=make("curv-plateau.icc",{0,32768,32768,65535});
  HDRSHOT_CHECK(plateau.has_value());
  // Isolate the inverse policy from the ICC matrix: this table value maps
  // exactly to the upper edge of its flat interval.
  std::array<WindowsIccProfile::Curve,3> flat_curves{};
  const float plateau_value=32768.0f/65535.0f;
  for(auto &curve:flat_curves) {
    curve.type=5;
    curve.samples={0.0f,plateau_value,plateau_value,1.0f};
  }
  constexpr std::array<float,9> identity_matrix{1,0,0,0,1,0,0,0,1};
  const WindowsIccProfile flat_fixture("plateau",flat_curves,identity_matrix,identity_matrix);
  const auto upper=windows_icc_p3_to_device_cpu(
      flat_fixture,{plateau_value,plateau_value,plateau_value});
  for(float x:upper) HDRSHOT_CHECK_NEAR(x,2.0f/3.0f,1e-6);
  auto decoded=windows_icc_device_to_p3_cpu(*plateau.value(),{0.5f,0.5f,0.5f});
  auto restored=windows_icc_p3_to_device_cpu(*plateau.value(),decoded);
  // Matrix forward/inverse rounding may choose either neighboring branch.
  // Both 1/3 and 2/3 are valid inverse codes for this flat interval.
  for(float x:restored) HDRSHOT_CHECK(x>=1.0f/3.0f-1e-4f&&x<=2.0f/3.0f+1e-4f);
  HDRSHOT_CHECK(!make("curv-descending.icc",{0,65535,100}).has_value());
}
} // namespace
int main() {
  return test::run({{"synthetic parametric ICC and independent float oracle",parametric_profile_and_independent_oracle},
                    {"compatibility states preserve Legacy ICC order",compatibility_states_preserve_legacy_icc_order},
                    {"ICC GPU gain, chroma and 10-bit steps",gpu_gain_color_and_10bit_steps},
                    {"doubled capture with manual half gain",doubled_capture_with_manual_half_gain},
                    {"synthetic v2 sampled curve tables on GPU",sampled_v2_curve_tables_and_gpu},
                    {"curv identity, gamma, plateau and nonmonotone",sampled_curve_forms_and_plateau_policy},
                    {"bad curve, LUT and file rejection",reject_bad_curves_and_luts}});
}
