#include "platform/windows/windows_icc_transform.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <vector>
#include <bcrypt.h>
#include <windows.h>

namespace hdrshot {
namespace {
Error icc_error(const char *reason, HRESULT hr = E_FAIL) {
  return {ErrorCode::invalid_color_contract, "WindowsIccTransform",
          Retryability::after_recreate,
          {{"reason", reason},
           {"hresult", std::to_string(static_cast<std::uint32_t>(hr))}}};
}

std::uint32_t be32(const std::vector<std::uint8_t> &v, std::size_t p) {
  return (std::uint32_t(v[p]) << 24) | (std::uint32_t(v[p+1]) << 16) |
         (std::uint32_t(v[p+2]) << 8) | v[p+3];
}
std::uint16_t be16(const std::vector<std::uint8_t> &v, std::size_t p) {
  return (std::uint16_t(v[p]) << 8) | v[p+1];
}
double fixed(const std::vector<std::uint8_t> &v, std::size_t p) {
  return static_cast<std::int32_t>(be32(v,p)) / 65536.0;
}
bool within(std::size_t start, std::size_t count, std::size_t limit) {
  return start <= limit && count <= limit-start;
}
std::uint32_t signature(const char *s) {
  return (std::uint32_t(std::uint8_t(s[0]))<<24) |
         (std::uint32_t(std::uint8_t(s[1]))<<16) |
         (std::uint32_t(std::uint8_t(s[2]))<<8) |
         std::uint8_t(s[3]);
}

// CNG is available on supported Windows versions and avoids maintaining
// a local hash implementation for frozen profile identity.
std::string sha256(const std::vector<std::uint8_t> &bytes) {
  BCRYPT_ALG_HANDLE algorithm=nullptr;
  BCRYPT_HASH_HANDLE hash=nullptr;
  ULONG object_size=0,returned=0;
  std::array<std::uint8_t,32> digest{};
  if(!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0))) return {};
  const auto property=BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,
      reinterpret_cast<PUCHAR>(&object_size),sizeof(object_size),&returned,0);
  std::vector<std::uint8_t> object(object_size);
  const bool created=BCRYPT_SUCCESS(property)&&object_size>0&&
      BCRYPT_SUCCESS(BCryptCreateHash(algorithm,&hash,object.data(),object_size,nullptr,0,0));
  const bool ok=created&&BCRYPT_SUCCESS(BCryptHashData(hash,const_cast<PUCHAR>(bytes.data()),
                     static_cast<ULONG>(bytes.size()),0))&&
          BCRYPT_SUCCESS(BCryptFinishHash(hash,digest.data(),static_cast<ULONG>(digest.size()),0));
  if(hash) BCryptDestroyHash(hash);
  BCryptCloseAlgorithmProvider(algorithm,0);
  if(!ok) return {};
  constexpr char hex[]="0123456789abcdef";
  std::string out;out.reserve(64);
  for(auto v:digest) {out.push_back(hex[v>>4]);out.push_back(hex[v&15]);}
  return out;
}

using Matrix=std::array<double,9>;
Matrix multiply(const Matrix &a,const Matrix &b) {
  Matrix r{};
  for(int y=0;y<3;++y) for(int x=0;x<3;++x)
    for(int k=0;k<3;++k) r[y*3+x]+=a[y*3+k]*b[k*3+x];
  return r;
}
bool inverse(const Matrix &m,Matrix &r) {
  const double det=m[0]*(m[4]*m[8]-m[5]*m[7])-
                   m[1]*(m[3]*m[8]-m[5]*m[6])+
                   m[2]*(m[3]*m[7]-m[4]*m[6]);
  if(!std::isfinite(det)||std::abs(det)<1e-10) return false;
  r={ (m[4]*m[8]-m[5]*m[7])/det,(m[2]*m[7]-m[1]*m[8])/det,(m[1]*m[5]-m[2]*m[4])/det,
      (m[5]*m[6]-m[3]*m[8])/det,(m[0]*m[8]-m[2]*m[6])/det,(m[2]*m[3]-m[0]*m[5])/det,
      (m[3]*m[7]-m[4]*m[6])/det,(m[1]*m[6]-m[0]*m[7])/det,(m[0]*m[4]-m[1]*m[3])/det};
  return true;
}
std::array<float,9> to_float(const Matrix &m) {
  std::array<float,9> r{};
  for(int i=0;i<9;++i) r[i]=static_cast<float>(m[i]);
  return r;
}
float decode(const WindowsIccProfile::Curve &c,float x) {
  x=std::clamp(x,0.0f,1.0f);
  const auto &p=c.parameters;
  if(c.type==-1) return x;
  if(c.type==5) {
    const float u=x*static_cast<float>(c.samples.size()-1);
    const auto i=std::min(static_cast<std::size_t>(u),c.samples.size()-2);
    return c.samples[i]+(u-static_cast<float>(i))*(c.samples[i+1]-c.samples[i]);
  }
  if(c.type==0) return std::pow(x,p[0]);
  if(c.type==1) return x>=-p[2]/p[1]?std::pow(std::max(0.0f,p[1]*x+p[2]),p[0]):0.0f;
  if(c.type==2) return x>=-p[2]/p[1]?std::pow(std::max(0.0f,p[1]*x+p[2]),p[0])+p[3]:p[3];
  if(c.type==3) return x>=p[4]?std::pow(std::max(0.0f,p[1]*x+p[2]),p[0]):p[3]*x;
  return x>=p[4]?std::pow(std::max(0.0f,p[1]*x+p[2]),p[0])+p[5]:p[3]*x+p[6];
}
float encode(const WindowsIccProfile::Curve &c,float y) {
  y=std::clamp(y,0.0f,1.0f);
  const auto &p=c.parameters;
  if(c.type==-1) return y;
  if(c.type==5) {
    if(y<=c.samples.front()) return 0;
    if(y>=c.samples.back()) return 1;
    const auto upper=std::upper_bound(c.samples.begin(),c.samples.end(),y);
    const std::size_t i=static_cast<std::size_t>(upper-c.samples.begin()-1);
    return (static_cast<float>(i)+(y-c.samples[i])/(c.samples[i+1]-c.samples[i]))/
           static_cast<float>(c.samples.size()-1);
  }
  if(c.type==0) return std::pow(y,1.0f/p[0]);
  if(c.type==1) return y<=0?0.0f:std::clamp((std::pow(y,1.0f/p[0])-p[2])/p[1],0.0f,1.0f);
  if(c.type==2) return y<=p[3]?0.0f:std::clamp((std::pow(y-p[3],1.0f/p[0])-p[2])/p[1],0.0f,1.0f);
  const float boundary=c.type==3?p[3]*p[4]:p[3]*p[4]+p[6];
  if(y<boundary) return std::clamp((y-(c.type==4?p[6]:0.0f))/p[3],0.0f,1.0f);
  return std::clamp((std::pow(std::max(0.0f,y-(c.type==4?p[5]:0.0f)),1.0f/p[0])-p[2])/p[1],0.0f,1.0f);
}
std::array<float,3> apply(const std::array<float,9> &m,std::array<float,3> x) {
  return {m[0]*x[0]+m[1]*x[1]+m[2]*x[2],
          m[3]*x[0]+m[4]*x[1]+m[5]*x[2],
          m[6]*x[0]+m[7]*x[1]+m[8]*x[2]};
}

// ICC parametric curves are exact on their supported domain. Discontinuous,
// non-monotone or negative branches cannot be inverted unambiguously.
bool curve_valid(const WindowsIccProfile::Curve &c) {
  const auto &p=c.parameters;
  if(c.type== -1) return true;
  if(c.type==5) {
    if(c.samples.size()<2||c.samples.size()>65536) return false;
    for(std::size_t i=1;i<c.samples.size();++i)
      if(c.samples[i]<c.samples[i-1]) return false;
    return c.samples.front()>=0&&c.samples.back()<=1&&
           c.samples.back()>c.samples.front();
  }
  if(c.type<0||c.type>4||!(p[0]>0)||!std::isfinite(p[0])) return false;
  for(float x:p) if(!std::isfinite(x)) return false;
  if(c.type==0) return true;
  if(!(p[1]>0)) return false;
  if(c.type==1||c.type==2) return p[2]>=0;
  if(!(p[3]>0)||p[4]<0||p[4]>1||p[1]*p[4]+p[2]<0) return false;
  const float lo=c.type==3?p[3]*p[4]:p[3]*p[4]+p[6];
  const float hi=std::pow(p[1]*p[4]+p[2],p[0])+(c.type==4?p[5]:0);
  return std::abs(lo-hi)<2.0f/65536.0f;
}

struct Tag {std::size_t offset=0,length=0;bool present=false;};
} // namespace

WindowsIccProfile::WindowsIccProfile(std::string hash,std::array<Curve,3> curves,
                                     std::array<float,9> forward,std::array<float,9> inverse)
    :sha256(std::move(hash)),decode_curves(curves),device_to_p3(forward),p3_to_device(inverse) {}

Result<std::shared_ptr<const WindowsIccProfile>,Error>
windows_load_icc_profile(const std::string &utf8_path) {
  if(utf8_path.empty()) return Result<std::shared_ptr<const WindowsIccProfile>,Error>::failure(icc_error("empty_path"));
  const int length=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,utf8_path.data(),
                                      static_cast<int>(utf8_path.size()),nullptr,0);
  if(length<=0) return Result<std::shared_ptr<const WindowsIccProfile>,Error>::failure(icc_error("invalid_utf8"));
  std::wstring wide(length,L'\0');
  MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,utf8_path.data(),
                      static_cast<int>(utf8_path.size()),wide.data(),length);
  std::ifstream stream(std::filesystem::path(wide),std::ios::binary|std::ios::ate);
  if(!stream) return Result<std::shared_ptr<const WindowsIccProfile>,Error>::failure(icc_error("open_failed"));
  const auto end=stream.tellg();
  if(end<132||end>16*1024*1024) return Result<std::shared_ptr<const WindowsIccProfile>,Error>::failure(icc_error("size_invalid"));
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
  stream.seekg(0);
  if(!stream.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size())))
    return Result<std::shared_ptr<const WindowsIccProfile>,Error>::failure(icc_error("read_failed"));
  if(be32(bytes,0)!=bytes.size()||be32(bytes,36)!=signature("acsp")||
     (bytes[8]!=2&&bytes[8]!=4)||be32(bytes,12)!=signature("mntr")||
     be32(bytes,16)!=signature("RGB ")||
     be32(bytes,20)!=signature("XYZ "))
    return Result<std::shared_ptr<const WindowsIccProfile>,Error>::failure(icc_error("unsupported_header"));
  const std::size_t count=be32(bytes,128);
  if(count>4096||!within(132,count*12,bytes.size()))
    return Result<std::shared_ptr<const WindowsIccProfile>,Error>::failure(icc_error("tag_table_invalid"));
  auto find=[&](const char *name)->Tag {
    Tag result{};
    for(std::size_t i=0;i<count;++i) {
      const auto p=132+i*12;
      if(be32(bytes,p)==signature(name)) {
        if(result.present) return {bytes.size(),1,true};
        result={be32(bytes,p+4),be32(bytes,p+8),true};
      }
    }
    return result;
  };
  for(auto name:{"A2B0","A2B1","A2B2","B2A0","B2A1","B2A2","D2B0","B2D0","mAB ","mBA "})
    if(find(name).present) return Result<std::shared_ptr<const WindowsIccProfile>,Error>::failure(icc_error("unsupported_lut_profile"));
  Matrix pcs{};
  std::array<WindowsIccProfile::Curve,3> curves{};
  const char *xyz_tags[]{"rXYZ","gXYZ","bXYZ"};
  const char *trc_tags[]{"rTRC","gTRC","bTRC"};
  for(int c=0;c<3;++c) {
    const auto xyz=find(xyz_tags[c]),trc=find(trc_tags[c]);
    if(!xyz.present||!trc.present||!within(xyz.offset,xyz.length,bytes.size())||
       !within(trc.offset,trc.length,bytes.size())||xyz.length<20||trc.length<12||
       be32(bytes,xyz.offset)!=signature("XYZ ")||xyz.length!=20)
      return Result<std::shared_ptr<const WindowsIccProfile>,Error>::failure(icc_error("matrix_trc_missing_or_invalid"));
    for(int row=0;row<3;++row) pcs[row*3+c]=fixed(bytes,xyz.offset+8+row*4);
    const auto curve_tag_type=be32(bytes,trc.offset);
    if(curve_tag_type==signature("para")) {
      const auto kind=be16(bytes,trc.offset+8);
      constexpr int arg_count[]{1,3,4,5,7};
      if(kind>4||trc.length!=std::size_t(12+arg_count[kind]*4))
        return Result<std::shared_ptr<const WindowsIccProfile>,Error>::failure(icc_error("curve_length_invalid"));
      curves[c].type=kind;
      for(int j=0;j<arg_count[kind];++j)
        curves[c].parameters[j]=static_cast<float>(fixed(bytes,trc.offset+12+j*4));
    } else if(curve_tag_type==signature("curv")) {
      const std::size_t entries=be32(bytes,trc.offset+8);
      if(entries>65536||entries>(trc.length-12)/2||
         trc.length>12+2*entries+3)
        return Result<std::shared_ptr<const WindowsIccProfile>,Error>::failure(icc_error("curve_length_invalid"));
      if(entries==0) curves[c].type=-1;
      else if(entries==1) {
        curves[c].type=0;
        curves[c].parameters[0]=be16(bytes,trc.offset+12)/256.0f;
      } else {
        curves[c].type=5;
        curves[c].samples.resize(entries);
        for(std::size_t j=0;j<entries;++j)
          curves[c].samples[j]=be16(bytes,trc.offset+12+2*j)/65535.0f;
      }
    } else {
      return Result<std::shared_ptr<const WindowsIccProfile>,Error>::failure(icc_error("unsupported_curve_type"));
    }
    if(!curve_valid(curves[c]))
      return Result<std::shared_ptr<const WindowsIccProfile>,Error>::failure(icc_error("curve_noninvertible"));
  }
  // Bradford D50 PCS -> D65, then XYZ D65 -> Display P3 linear RGB.
  // ICC.1:2022 PCS illuminant D50 is (0.9642, 1, 0.8249); target white is
  // Display P3 xy=(0.3127,0.3290). These are deliberately not the nearby
  // CIE daylight/D65 rounded XYZ values used in generic Bradford tables.
  constexpr Matrix d50_to_d65={0.9555125889283473,-0.023072975173043474,0.06330908471330186,
                              -0.02832475931011885,1.0099429264007955,0.02105443875138901,
                               0.012328703174821268,-0.0205353076543033,1.3307136899175889};
  constexpr Matrix xyz_to_p3={2.493496911941425,-0.9313836179191239,-0.40271078445071684,
                             -0.8294889695615747,1.7626640603183463,0.0236246858419436,
                              0.03584583024378447,-0.07617238926804182,0.9568845240076872};
  const auto forward=multiply(multiply(xyz_to_p3,d50_to_d65),pcs);
  Matrix backward{};
  if(!inverse(forward,backward))
    return Result<std::shared_ptr<const WindowsIccProfile>,Error>::failure(icc_error("matrix_singular"));
  for(double v:forward) if(!std::isfinite(v)||std::abs(v)>16)
    return Result<std::shared_ptr<const WindowsIccProfile>,Error>::failure(icc_error("matrix_invalid"));
  const auto hash=sha256(bytes);
  if(hash.size()!=64)
    return Result<std::shared_ptr<const WindowsIccProfile>,Error>::failure(icc_error("hash_failed"));
  return Result<std::shared_ptr<const WindowsIccProfile>,Error>::success(
      std::make_shared<const WindowsIccProfile>(hash,curves,to_float(forward),to_float(backward)));
}

std::array<float,3> windows_icc_device_to_p3_cpu(const WindowsIccProfile &p,std::array<float,3> encoded) {
  for(int c=0;c<3;++c) encoded[c]=decode(p.decode_curves[c],encoded[c]);
  return apply(p.device_to_p3,encoded);
}
std::array<float,3> windows_icc_p3_to_device_cpu(const WindowsIccProfile &p,std::array<float,3> linear_p3) {
  auto encoded=apply(p.p3_to_device,linear_p3);
  for(int c=0;c<3;++c) encoded[c]=encode(p.decode_curves[c],encoded[c]);
  return encoded;
}

namespace {
struct alignas(16) GpuConstants {
  float forward[3][4]{};
  float backward[3][4]{};
  float curve[3][2][4]{};
  float kind[4]{};
};
}
WindowsIccGpuTransform::WindowsIccGpuTransform(std::shared_ptr<const WindowsIccProfile> p,
                                               winrt::com_ptr<ID3D11Buffer> constants,
                                               winrt::com_ptr<ID3D11ShaderResourceView> decode_table,
                                               winrt::com_ptr<ID3D11ShaderResourceView> encode_table)
    :profile_(std::move(p)),constants_(std::move(constants)),
     decode_table_(std::move(decode_table)),encode_table_(std::move(encode_table)) {}
Result<std::shared_ptr<const WindowsIccGpuTransform>,Error>
WindowsIccGpuTransform::create(ID3D11Device *device,std::shared_ptr<const WindowsIccProfile> profile) {
  if(!device||!profile) return Result<std::shared_ptr<const WindowsIccGpuTransform>,Error>::failure(icc_error("invalid_gpu_input"));
  GpuConstants data{};
  for(int r=0;r<3;++r) {
    for(int c=0;c<3;++c) {
      data.forward[r][c]=profile->device_to_p3[r*3+c];
      data.backward[r][c]=profile->p3_to_device[r*3+c];
    }
    for(int i=0;i<7;++i) data.curve[r][i/4][i%4]=profile->decode_curves[r].parameters[i];
    data.kind[r]=static_cast<float>(profile->decode_curves[r].type);
  }
  D3D11_BUFFER_DESC desc{};
  desc.ByteWidth=sizeof(data);desc.Usage=D3D11_USAGE_IMMUTABLE;desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
  const D3D11_SUBRESOURCE_DATA initial{&data,0,0};
  winrt::com_ptr<ID3D11Buffer> buffer;
  const auto hr=device->CreateBuffer(&desc,&initial,buffer.put());
  if(FAILED(hr)) return Result<std::shared_ptr<const WindowsIccGpuTransform>,Error>::failure(icc_error("gpu_constants",hr));
  winrt::com_ptr<ID3D11ShaderResourceView> decode_view,encode_view;
  const bool sampled=std::any_of(profile->decode_curves.begin(),profile->decode_curves.end(),
                                 [](const auto &c){return c.type==5;});
  if(sampled) {
    constexpr std::size_t length=65536;
    std::vector<float> decode_data(length*3),encode_data(length*3);
    for(int c=0;c<3;++c) for(std::size_t i=0;i<length;++i) {
      const float x=static_cast<float>(i)/65535.0f;
      decode_data[c*length+i]=decode(profile->decode_curves[c],x);
      encode_data[c*length+i]=encode(profile->decode_curves[c],x);
    }
    D3D11_TEXTURE2D_DESC texture_desc{};
    texture_desc.Width=256;texture_desc.Height=256;texture_desc.MipLevels=1;
    texture_desc.ArraySize=3;texture_desc.Format=DXGI_FORMAT_R32_FLOAT;
    texture_desc.SampleDesc.Count=1;texture_desc.Usage=D3D11_USAGE_IMMUTABLE;
    texture_desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    auto make_view=[&](const std::vector<float> &pixels,winrt::com_ptr<ID3D11ShaderResourceView> &view)->HRESULT {
      D3D11_SUBRESOURCE_DATA subresources[3]{};
      for(int c=0;c<3;++c) {
        subresources[c].pSysMem=pixels.data()+c*length;
        subresources[c].SysMemPitch=256*sizeof(float);
        subresources[c].SysMemSlicePitch=length*sizeof(float);
      }
      winrt::com_ptr<ID3D11Texture2D> texture;
      auto result=device->CreateTexture2D(&texture_desc,subresources,texture.put());
      if(SUCCEEDED(result)) result=device->CreateShaderResourceView(texture.get(),nullptr,view.put());
      return result;
    };
    auto result=make_view(decode_data,decode_view);
    if(SUCCEEDED(result)) result=make_view(encode_data,encode_view);
    if(FAILED(result)) return Result<std::shared_ptr<const WindowsIccGpuTransform>,Error>::failure(icc_error("gpu_curve_tables",result));
  }
  return Result<std::shared_ptr<const WindowsIccGpuTransform>,Error>::success(
      std::shared_ptr<const WindowsIccGpuTransform>(new WindowsIccGpuTransform(
          std::move(profile),std::move(buffer),std::move(decode_view),std::move(encode_view))));
}
void WindowsIccGpuTransform::bind_cs(ID3D11DeviceContext *context) const {
  ID3D11Buffer *buffers[]={constants_.get()};context->CSSetConstantBuffers(2,1,buffers);
  ID3D11ShaderResourceView *views[]={decode_table_.get(),encode_table_.get()};
  context->CSSetShaderResources(8,2,views);
}
void WindowsIccGpuTransform::bind_ps(ID3D11DeviceContext *context) const {
  ID3D11Buffer *buffers[]={constants_.get()};context->PSSetConstantBuffers(2,1,buffers);
  ID3D11ShaderResourceView *views[]={decode_table_.get(),encode_table_.get()};
  context->PSSetShaderResources(8,2,views);
}
std::string_view WindowsIccGpuTransform::hlsl_source() {
  return R"HLSL(
Texture2DArray<float> icc_decode_table : register(t8);
Texture2DArray<float> icc_encode_table : register(t9);
cbuffer WindowsIccParams : register(b2) {
  float4 icc_forward0,icc_forward1,icc_forward2;
  float4 icc_backward0,icc_backward1,icc_backward2;
  float4 icc_curve_r0,icc_curve_r1;
  float4 icc_curve_g0,icc_curve_g1;
  float4 icc_curve_b0,icc_curve_b1;
  float4 icc_curve_kind;
};
float windows_icc_lookup(Texture2DArray<float> table,float x,int channel) {
  float pos=saturate(x)*65535.0;
  uint i=(uint)floor(pos);
  uint j=min(i+1,65535);
  float a=table.Load(int4(i&255,i>>8,channel,0));
  float b=table.Load(int4(j&255,j>>8,channel,0));
  return lerp(a,b,pos-i);
}
float windows_icc_decode_channel(float x,float4 p,float4 q,int kind,int channel) {
  x=saturate(x);
  if(kind<0) return x;
  if(kind==5) return windows_icc_lookup(icc_decode_table,x,channel);
  if(kind==0) return pow(x,p.x);
  if(kind==1) return x>=-p.z/p.y?pow(max(0.0,p.y*x+p.z),p.x):0.0;
  if(kind==2) return x>=-p.z/p.y?pow(max(0.0,p.y*x+p.z),p.x)+p.w:p.w;
  if(kind==3) return x>=q.x?pow(max(0.0,p.y*x+p.z),p.x):p.w*x;
  return x>=q.x?pow(max(0.0,p.y*x+p.z),p.x)+q.y:p.w*x+q.z;
}
float windows_icc_encode_channel(float y,float4 p,float4 q,int kind,int channel) {
  y=saturate(y);
  if(kind<0) return y;
  if(kind==5) return windows_icc_lookup(icc_encode_table,y,channel);
  if(kind==0) return pow(y,1.0/p.x);
  if(kind==1) return y<=0.0?0.0:saturate((pow(y,1.0/p.x)-p.z)/p.y);
  if(kind==2) return y<=p.w?0.0:saturate((pow(y-p.w,1.0/p.x)-p.z)/p.y);
  float boundary=p.w*q.x+(kind==4?q.z:0.0);
  if(y<boundary) return saturate((y-(kind==4?q.z:0.0))/p.w);
  return saturate((pow(max(0.0,y-(kind==4?q.y:0.0)),1.0/p.x)-p.z)/p.y);
}
float3 windows_icc_device_to_p3(float3 encoded) {
  float3 v=float3(windows_icc_decode_channel(encoded.r,icc_curve_r0,icc_curve_r1,(int)icc_curve_kind.x,0),
                  windows_icc_decode_channel(encoded.g,icc_curve_g0,icc_curve_g1,(int)icc_curve_kind.y,1),
                  windows_icc_decode_channel(encoded.b,icc_curve_b0,icc_curve_b1,(int)icc_curve_kind.z,2));
  return float3(dot(icc_forward0.xyz,v),dot(icc_forward1.xyz,v),dot(icc_forward2.xyz,v));
}
float3 windows_icc_p3_to_device(float3 p3) {
  float3 v=float3(dot(icc_backward0.xyz,p3),dot(icc_backward1.xyz,p3),dot(icc_backward2.xyz,p3));
  return float3(windows_icc_encode_channel(v.r,icc_curve_r0,icc_curve_r1,(int)icc_curve_kind.x,0),
                windows_icc_encode_channel(v.g,icc_curve_g0,icc_curve_g1,(int)icc_curve_kind.y,1),
                windows_icc_encode_channel(v.b,icc_curve_b0,icc_curve_b1,(int)icc_curve_kind.z,2));
}
)HLSL";
}
} // namespace hdrshot
