#include "platform/windows/windows_icc_transform.hpp"
#include "platform/windows/windows_gpu_source.hpp"
#include "test_support.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace hdrshot;
namespace {
const std::string dell_path="C:\\Windows\\System32\\spool\\drivers\\color\\DELL UP2516DARGB.icm";
const std::string windows_srgb_path="C:\\Windows\\System32\\spool\\drivers\\color\\sRGB Color Space Profile.icm";
const std::string cal1_path="C:\\Windows\\System32\\spool\\drivers\\color\\01-Windows-UP2516D-CAL1-sRGB-3TRC-Matrix.icm";
constexpr const char *dell_hash="d0d495d54082254e4ea21c9b055a73ca8cc6313d6c146f26e2da9e562efba1c7";

std::shared_ptr<const WindowsIccProfile> dell() {
  auto loaded=windows_load_icc_profile(dell_path);
  HDRSHOT_CHECK(loaded.has_value());
  HDRSHOT_CHECK(loaded.value()->sha256==dell_hash);
  return loaded.value();
}
double decode_device(double x) {
  // Independently recorded Dell type-4 parameters (ICC s15Fixed16 values).
  constexpr double g=2.5692901611328125,a=0.901885986328125,
                   b=0.09808349609375,c=0.148773193359375,
                   d=0.1357269287109375,e=1.52587890625e-5,
                   f=0.0003814697265625;
  x=std::clamp(x,0.0,1.0);
  return x<d?c*x+f:std::pow(a*x+b,g)+e;
}
std::array<double,3> oracle(std::array<double,3> encoded) {
  // Independently frozen PCS matrix from E7. Derive adaptation from Bradford
  // cone responses of D50 and D65, not the production 3x3 adaptation table.
  constexpr double pcs[3][3]={{0.611358642578125,0.1988067626953125,0.1540374755859375},
                              {0.31243896484375,0.628143310546875,0.059417724609375},
                              {0.018280029296875,0.0548095703125,0.752105712890625}};
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
void real_profile_and_independent_oracle() {
  const auto p=dell();
  // The actual profile's neutral white must remain neutral after PCS D50 to
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
  const auto p=dell();
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
  const auto p=dell();
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
void real_v2_curve_tables_and_gpu() {
  auto srgb=windows_load_icc_profile(windows_srgb_path);
  auto cal1=windows_load_icc_profile(cal1_path);
  HDRSHOT_CHECK(srgb.has_value());
  HDRSHOT_CHECK(cal1.has_value());
  HDRSHOT_CHECK(srgb.value()->decode_curves[0].samples.size()==1024);
  HDRSHOT_CHECK(cal1.value()->decode_curves[0].samples.size()==256);
  constexpr int count=1024;
  std::vector<std::uint16_t> rgba(count*4);
  for(int i=0;i<count;++i) {
    const float x=float(i)/1023;
    rgba[i*4+0]=to_half(eotf(x));
    rgba[i*4+1]=to_half(eotf((i%3)?0.31f:x));
    rgba[i*4+2]=to_half(eotf((i%3)?0.73f:x));
    rgba[i*4+3]=0x7e00;
  }
  for(const auto &profile:{srgb.value(),cal1.value()}) {
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
  std::ifstream input(std::filesystem::path(dell_path),std::ios::binary);
  HDRSHOT_CHECK(bool(input));
  std::vector<char> bytes((std::istreambuf_iterator<char>(input)),{});
  auto save=[&](const char *name,const std::vector<char> &data) {
    const auto path=std::filesystem::temp_directory_path()/name;
    std::ofstream out(path,std::ios::binary|std::ios::trunc);
    out.write(data.data(),static_cast<std::streamsize>(data.size()));out.close();
    const auto utf8=path.u8string();
    const auto loaded=windows_load_icc_profile(std::string(utf8.begin(),utf8.end()));
    std::filesystem::remove(path);
    if(loaded) std::cerr << "Unexpected ICC acceptance: " << name << '\n';
    HDRSHOT_CHECK(!loaded.has_value());
  };
  auto bad=bytes;bad[608]='c';bad[609]='u';bad[610]='r';bad[611]='v';
  save("seriousshot_icc_bad_curve.icm",bad);
  bad=bytes;bad[620]=0;bad[621]=0;bad[622]=0;bad[623]=0; // zero gamma
  save("seriousshot_icc_zero_gamma.icm",bad);
  bad=bytes;bad[132]='A';bad[133]='2';bad[134]='B';bad[135]='0';
  save("seriousshot_icc_lut_tag.icm",bad);
  bad=bytes;bad[3]=0;save("seriousshot_icc_bad_size.icm",bad);
  HDRSHOT_CHECK(!windows_load_icc_profile("C:\\absent-profile.icm"));
}
void sampled_curve_forms_and_plateau_policy() {
  std::ifstream input(std::filesystem::path(dell_path),std::ios::binary);
  std::vector<char> original((std::istreambuf_iterator<char>(input)),{});
  auto make=[&](const char *name,const std::vector<std::uint16_t> &values) {
    auto bytes=original;
    bytes[8]=2; // v2 display profile; XYZ matrix remains unchanged.
    auto put32=[&](std::size_t p,std::uint32_t v) {
      bytes[p]=char(v>>24);bytes[p+1]=char(v>>16);bytes[p+2]=char(v>>8);bytes[p+3]=char(v);
    };
    auto put16=[&](std::size_t p,std::uint16_t v) {bytes[p]=char(v>>8);bytes[p+1]=char(v);};
    for(std::size_t tag=0;tag<15;++tag) {
      const auto table=132+tag*12;
      if(bytes[table]!='r'&&bytes[table]!='g'&&bytes[table]!='b') continue;
      if(bytes[table+1]!='T'||bytes[table+2]!='R'||bytes[table+3]!='C') continue;
      const auto offset=(std::uint32_t(std::uint8_t(bytes[table+4]))<<24)|
          (std::uint32_t(std::uint8_t(bytes[table+5]))<<16)|
          (std::uint32_t(std::uint8_t(bytes[table+6]))<<8)|
          std::uint8_t(bytes[table+7]);
      put32(table+8,12+2*static_cast<std::uint32_t>(values.size()));
      bytes[offset]='c';bytes[offset+1]='u';bytes[offset+2]='r';bytes[offset+3]='v';
      put32(offset+8,static_cast<std::uint32_t>(values.size()));
      for(std::size_t j=0;j<values.size();++j) put16(offset+12+j*2,values[j]);
    }
    const auto path=std::filesystem::temp_directory_path()/name;
    std::ofstream out(path,std::ios::binary|std::ios::trunc);
    out.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));out.close();
    const auto u=path.u8string();
    auto loaded=windows_load_icc_profile(std::string(u.begin(),u.end()));
    std::filesystem::remove(path);
    return loaded;
  };
  auto identity=make("seriousshot_icc_curv_identity.icm",{});
  HDRSHOT_CHECK(identity.has_value());
  HDRSHOT_CHECK(identity.value()->decode_curves[0].type==-1);
  auto gamma=make("seriousshot_icc_curv_gamma.icm",{0x0200});
  HDRSHOT_CHECK(gamma.has_value());
  HDRSHOT_CHECK_NEAR(gamma.value()->decode_curves[0].parameters[0],2.0,0);
  auto plateau=make("seriousshot_icc_curv_plateau.icm",{0,32768,32768,65535});
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
  HDRSHOT_CHECK(!make("seriousshot_icc_curv_descending.icm",{0,65535,100}).has_value());
}
} // namespace
int main() {
  return test::run({{"actual Dell profile and independent float oracle",real_profile_and_independent_oracle},
                    {"ICC GPU gain, chroma and 10-bit steps",gpu_gain_color_and_10bit_steps},
                    {"doubled capture with manual half gain",doubled_capture_with_manual_half_gain},
                    {"real Windows v2 curve tables on GPU",real_v2_curve_tables_and_gpu},
                    {"curv identity, gamma, plateau and nonmonotone",sampled_curve_forms_and_plateau_policy},
                    {"bad curve, LUT and file rejection",reject_bad_curves_and_luts}});
}
