#pragma once
#include "domain/analysis/math.hpp"
#include <regex>
#include <string>

namespace hdrshot {
// Mechanical syntax adaptation only: the shared include remains the single
// authority for color formulas, constants, ranges and binning on every backend.
inline std::string windows_analysis_math_hlsl() {
  static const std::string source = [] {
    std::string math = analysis_math::metal_source;
    math = std::regex_replace(
        math, std::regex(R"(return A3\s*\{x\s*,\s*y\s*,\s*z\s*\};)"),
        "A3 result; result.x=x; result.y=y; result.z=z; return result;");
    math = std::regex_replace(math, std::regex(R"(Signals out\s*\{\s*\};)"),
                              "Signals out=(Signals)0;");
    // HLSL reserves 'out' as a parameter qualifier.
    math = std::regex_replace(math, std::regex(R"(\bout\b)"), "result_signals");
    math = std::regex_replace(
        math,
        std::regex(
            R"(A3 v\s*=\s*space\s*<\s*2u\s*\?\s*clip3\(source,\s*1.0f\)\s*:\s*source;)"),
        "A3 v=source; if(space<2u) v=clip3(source,1.0f);");
    return std::string(R"HLSL(
float a_pow(float x,float y){return pow(x,y);}
float a_sqrt(float x){return sqrt(x);}
float a_atan2(float y,float x){return atan2(y,x);}
float a_floor(float x){return floor(x);}
float a_log2(float x){return log2(x);}
float a_copysign(float x,float y){return asfloat((asuint(x)&0x7fffffffu)|(asuint(y)&0x80000000u));}
bool a_finite(float x){return (asuint(x)&0x7f800000u)!=0x7f800000u;}
)HLSL") + math;
  }();
  return source;
}
} // namespace hdrshot
