# v2.0.2 omits the secondary (alternate HDR) ICC whenever XMP is enabled.
# Keep ISO's explicit alternate color declaration even when also writing XMP.
# Generate a single modified translation unit; never mutate the fetched tree.
set(_uhdr_jpegr "${ultrahdr_SOURCE_DIR}/lib/src/jpegr.cpp")
file(SHA256 "${_uhdr_jpegr}" _uhdr_jpegr_sha256)
if(NOT _uhdr_jpegr_sha256 STREQUAL
    "c9e0048dbeb313f1d916b4a454e1f1b6d8af108d09743f2533a9cd96fd60e850")
  message(FATAL_ERROR "libultrahdr source changed: review the alternate ICC patch")
endif()
file(READ "${_uhdr_jpegr}" _uhdr_original)
set(_uhdr_old "if (!kWriteXmpMetadata) {")
set(_uhdr_new "if (!kWriteXmpMetadata || kWriteIso21496_1Metadata) {")
string(REGEX MATCHALL "if \\(!kWriteXmpMetadata\\) \\{" _uhdr_matches "${_uhdr_original}")
list(LENGTH _uhdr_matches _uhdr_match_count)
if(NOT _uhdr_match_count EQUAL 1)
  message(FATAL_ERROR "libultrahdr alternate ICC patch is not unique")
endif()
string(REPLACE "${_uhdr_old}" "${_uhdr_new}" _uhdr_patched "${_uhdr_original}")
set(_uhdr_icc_old "IccHelper::writeIccProfile(gainmap_img->ct, gainmap_img->cg)")
set(_uhdr_icc_new "IccHelper::writeIccProfile(
        // ISO alternate profile describes reconstructed HDR, not gain-map codes.
        // Keep the linear HDR input/math, use upstream P3/PQ for Apple interop.
        gainmap_img->cg == UHDR_CG_DISPLAY_P3 && gainmap_img->ct == UHDR_CT_LINEAR
            ? UHDR_CT_PQ : gainmap_img->ct, gainmap_img->cg)")
string(FIND "${_uhdr_patched}" "${_uhdr_icc_old}" _uhdr_icc_position)
if(_uhdr_icc_position LESS 0)
  message(FATAL_ERROR "libultrahdr alternate ICC call changed")
endif()
string(REPLACE "${_uhdr_icc_old}" "${_uhdr_icc_new}" _uhdr_patched "${_uhdr_patched}")
hdrshot_uhdr_honor_gainmap_preset(_uhdr_patched _uhdr_patched)
set(_uhdr_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/ultrahdr/jpegr.cpp")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/ultrahdr")
# configure_file COPYONLY preserves the mtime on subsequent configurations.
file(WRITE "${_uhdr_generated}.tmp" "${_uhdr_patched}")
configure_file("${_uhdr_generated}.tmp" "${_uhdr_generated}" COPYONLY)
get_target_property(_uhdr_core_sources core SOURCES)
list(FIND _uhdr_core_sources "${_uhdr_jpegr}" _uhdr_source_index)
if(_uhdr_source_index LESS 0)
  message(FATAL_ERROR "libultrahdr core source list changed: patch not installed")
endif()
list(REMOVE_AT _uhdr_core_sources ${_uhdr_source_index})
list(APPEND _uhdr_core_sources "${_uhdr_generated}")
set_property(TARGET core PROPERTY SOURCES "${_uhdr_core_sources}")

# v2.0.2's mAB/mBA writer returns after a SUCCESSFUL first B-curve write,
# leaving the remaining curves and CLUT zero-filled. ColorSync rejects this
# PQ profile. First correct the inverted error check; the independent PQ
# direction correction below retains the upstream ICC tone mapping and PCS.
set(_uhdr_icc_source "${ultrahdr_SOURCE_DIR}/lib/src/icc.cpp")
file(SHA256 "${_uhdr_icc_source}" _uhdr_icc_sha256)
if(NOT _uhdr_icc_sha256 STREQUAL
    "c9d009952d79ba9d61e2afd5b0205221f3b1cd843ab5da69d142d8f87895e8c5")
  message(FATAL_ERROR "libultrahdr ICC source changed: review the PQ curve write fix")
endif()
file(READ "${_uhdr_icc_source}" _uhdr_icc_original)
set(_uhdr_curve_old "if (dataStruct->write(b_curves_data[i]->getData(), b_curves_data[i]->getLength())) {")
set(_uhdr_curve_new "if (!dataStruct->write(b_curves_data[i]->getData(), b_curves_data[i]->getLength())) {")
string(FIND "${_uhdr_icc_original}" "${_uhdr_curve_old}" _uhdr_curve_position)
if(_uhdr_curve_position LESS 0)
  message(FATAL_ERROR "libultrahdr ICC B-curve write changed")
endif()
string(REPLACE "${_uhdr_curve_old}" "${_uhdr_curve_new}" _uhdr_icc_patched "${_uhdr_icc_original}")

# PQ code -> normalized linear light is EOTF (named pqInvOetf upstream).
# Only change A2B0 CLUT construction, not the image/gain-map encoder or the
# subsequent ICC tone map / RGB -> PCS transform. Applied on ALL platforms.
# The complete source SHA above and unique context below fail closed on drift.
set(_uhdr_pq_old "  // Convert the source signal to linear.
  for (size_t i = 0; i < kNumChannels; ++i) {
    rgb[i] = pqOetf(rgb[i]);
  }")
set(_uhdr_pq_new "  // Decode PQ code to normalized linear light (EOTF).
  // XDRShot: preserve the following upstream ICC tone map and PCS transform.
  for (size_t i = 0; i < kNumChannels; ++i) {
    rgb[i] = pqInvOetf(rgb[i]);
  }")
string(FIND "${_uhdr_icc_patched}" "${_uhdr_pq_old}" _uhdr_pq_first)
if(_uhdr_pq_first LESS 0)
  message(FATAL_ERROR "libultrahdr ICC PQ linearization changed: review A2B0 direction fix")
endif()
string(LENGTH "${_uhdr_pq_old}" _uhdr_pq_length)
math(EXPR _uhdr_pq_after "${_uhdr_pq_first} + ${_uhdr_pq_length}")
string(SUBSTRING "${_uhdr_icc_patched}" ${_uhdr_pq_after} -1 _uhdr_pq_remaining)
string(FIND "${_uhdr_pq_remaining}" "${_uhdr_pq_old}" _uhdr_pq_duplicate)
if(NOT _uhdr_pq_duplicate LESS 0)
  message(FATAL_ERROR "libultrahdr ICC PQ linearization is not unique")
endif()
string(REPLACE "${_uhdr_pq_old}" "${_uhdr_pq_new}" _uhdr_icc_patched "${_uhdr_icc_patched}")
message(STATUS "libultrahdr: installed shared A2B0 PQ EOTF correction (ICC tone map retained)")

# Match canonical Display P3's fixed-point D50 matrix and equivalent sRGB
# type-3 curve. Apple's named-space recognition needs both, not the description.
# These are numeric color-space parameters, NOT an embedded Apple ICC/HAGC.
string(REPLACE "toXYZD50 = kDisplayP3;" "toXYZD50 = {{{33759.0f / 65536.0f, 19135.0f / 65536.0f, 10296.0f / 65536.0f},
                       {15807.0f / 65536.0f, 45367.0f / 65536.0f, 4363.0f / 65536.0f},
                       {-69.0f / 65536.0f, 2745.0f / 65536.0f, 51385.0f / 65536.0f}}};"
    _uhdr_icc_patched "${_uhdr_icc_patched}")
set(_uhdr_srgb_old "write_trc_tag(kSRGB_TransFun)")
set(_uhdr_srgb_new "(gamut == UHDR_CG_DISPLAY_P3 ? writeDisplayP3SrgbCurve() : write_trc_tag(kSRGB_TransFun))")
string(REPLACE "${_uhdr_srgb_old}" "${_uhdr_srgb_new}" _uhdr_icc_patched "${_uhdr_icc_patched}")
set(_uhdr_profile_entry "std::shared_ptr<DataStruct> IccHelper::writeIccProfile(")
set(_uhdr_p3_curve "static std::shared_ptr<DataStruct> writeDisplayP3SrgbCurve() {
  // ICC type 3: (aX+b)^g above d, cX below d. e=f=0 from type 4.
  const uint32_t values[] = {Endian_SwapBE32(0x70617261), 0,
      Endian_SwapBE32(0x00030000), Endian_SwapBE32(0x00026666),
      Endian_SwapBE32(0x0000f2a7), Endian_SwapBE32(0x00000d59),
      Endian_SwapBE32(0x000013d0), Endian_SwapBE32(0x00000a5b)};
  auto curve = std::make_shared<DataStruct>(sizeof(values));
  curve->write(values, sizeof(values));
  return curve;
}

${_uhdr_profile_entry}")
string(REPLACE "${_uhdr_profile_entry}" "${_uhdr_p3_curve}" _uhdr_icc_patched "${_uhdr_icc_patched}")
set(_uhdr_icc_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/ultrahdr/icc.cpp")
file(WRITE "${_uhdr_icc_generated}.tmp" "${_uhdr_icc_patched}")
configure_file("${_uhdr_icc_generated}.tmp" "${_uhdr_icc_generated}" COPYONLY)
get_target_property(_uhdr_core_sources core SOURCES)
list(FIND _uhdr_core_sources "${_uhdr_icc_source}" _uhdr_icc_source_index)
if(_uhdr_icc_source_index LESS 0)
  message(FATAL_ERROR "libultrahdr ICC source is missing from core")
endif()
list(REMOVE_AT _uhdr_core_sources ${_uhdr_icc_source_index})
list(APPEND _uhdr_core_sources "${_uhdr_icc_generated}")
set_property(TARGET core PROPERTY SOURCES "${_uhdr_core_sources}")
