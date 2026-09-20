# Shared codec correction for locked libultrahdr v2.0.2. P3 primaries do NOT
# imply P3 luminance coefficients for JPEG YCbCr: scalar/decoder use BT.601.
# Upstream also interchanges the BT.2100 and P3 cases in its ARM NEON path.
# Keep SIMD and 4:4:4; generate one patched TU without modifying fetched code.
set(_uhdr_neon "${ultrahdr_SOURCE_DIR}/lib/src/dsp/arm/gainmapmath_neon.cpp")
file(SHA256 "${_uhdr_neon}" _uhdr_neon_sha)
if(NOT _uhdr_neon_sha STREQUAL
    "e0dedba60d3f28982a09eb1b5ee83107d6d4433dbe356fc3ee3267173540f2f6")
  message(FATAL_ERROR "libultrahdr NEON source changed: review JPEG matrix patch")
endif()
file(READ "${_uhdr_neon}" _uhdr_neon_text)
set(_uhdr_neon_coeff_old [=[// RGB Display P3 -> Yuv Display P3
// Y = 0.2289746 * R + 0.6917385 * G + 0.0792869 * B
// U = -0.124346335 * R + -0.375653665 * G + 0.5 * B
// V = 0.5 * R + -0.448583471 * G + -0.051416529 * B
ALIGNED(16)
const uint16_t kRgbDispP3ToYuv_coeffs_neon[8] = {3752, 11333, 1299, 2037, 6155, 8192, 7350, 842};]=])
set(_uhdr_neon_coeff_new [=[// Display P3 JPEG uses full-range BT.601 YCbCr, matching p3RgbToYuv
// and p3YuvToRgb. This matrix does not change the RGB primaries.
// Y=.299R+.587G+.114B; Cb=(B-Y)/1.772; Cr=(R-Y)/1.402.
// Rounded to 2^14; sums preserve the neutral axis and chroma bias.
ALIGNED(16)
const uint16_t kRgbDispP3ToYuv_coeffs_neon[8] = {4899, 9617, 1868, 2765, 5427, 8192, 6860, 1332};]=])
set(_uhdr_neon_switch_old [=[} else if (src->cg == UHDR_CG_BT_2100) {
      coeffs_ptr = kRgbDispP3ToYuv_coeffs_neon;
    } else if (src->cg == UHDR_CG_DISPLAY_P3) {
      coeffs_ptr = kRgb2100ToYuv_coeffs_neon;]=])
set(_uhdr_neon_switch_new [=[} else if (src->cg == UHDR_CG_BT_2100) {
      coeffs_ptr = kRgb2100ToYuv_coeffs_neon;
    } else if (src->cg == UHDR_CG_DISPLAY_P3) {
      coeffs_ptr = kRgbDispP3ToYuv_coeffs_neon;]=])
foreach(_kind coeff switch)
  string(FIND "${_uhdr_neon_text}" "${_uhdr_neon_${_kind}_old}" _match)
  if(_match LESS 0)
    message(FATAL_ERROR "libultrahdr NEON ${_kind} patch no longer matches")
  endif()
  string(REPLACE "${_uhdr_neon_${_kind}_old}" "${_uhdr_neon_${_kind}_new}"
      _uhdr_neon_text "${_uhdr_neon_text}")
endforeach()
get_target_property(_uhdr_neon_core_sources core SOURCES)
list(FIND _uhdr_neon_core_sources "${_uhdr_neon}" _uhdr_neon_index)
# Scalar/x64 builds have no NEON translation unit; keep the same shared policy.
if(NOT _uhdr_neon_index LESS 0)
  set(_uhdr_neon_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/ultrahdr/gainmapmath_neon.cpp")
  file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/ultrahdr")
  file(WRITE "${_uhdr_neon_generated}.tmp" "${_uhdr_neon_text}")
  configure_file("${_uhdr_neon_generated}.tmp" "${_uhdr_neon_generated}" COPYONLY)
  list(REMOVE_AT _uhdr_neon_core_sources ${_uhdr_neon_index})
  list(APPEND _uhdr_neon_core_sources "${_uhdr_neon_generated}")
  set_property(TARGET core PROPERTY SOURCES "${_uhdr_neon_core_sources}")
  message(STATUS "libultrahdr: installed shared NEON JPEG matrix correction")
endif()
