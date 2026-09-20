# Apply to the same generated jpegr TU as the ICC patch. That patch validates
# the complete fetched v2.0.2 source SHA before calling this function; never
# modify the fetched tree or replace the upstream tone/gain math.
function(hdrshot_uhdr_honor_gainmap_preset input_variable output_variable)
  set(_source "${${input_variable}}")
  set(_old "  mEncPreset = UHDR_USAGE_REALTIME;  // overriding the config option")
  string(FIND "${_source}" "${_old}" _first)
  if(_first LESS 0)
    message(FATAL_ERROR "libultrahdr HDR-only preset override changed: review two-pass patch")
  endif()
  string(LENGTH "${_old}" _length)
  math(EXPR _after "${_first} + ${_length}")
  string(SUBSTRING "${_source}" ${_after} -1 _remaining)
  string(FIND "${_remaining}" "${_old}" _duplicate)
  if(NOT _duplicate LESS 0)
    message(FATAL_ERROR "libultrahdr HDR-only preset override is not unique")
  endif()
  set(_new "  // Honor the shared adapter's BEST_QUALITY preset for HDR-only input.
  // Upstream two-pass merges RGB ranges before quantization when XMP is enabled.")
  string(REPLACE "${_old}" "${_new}" _source "${_source}")
  set(${output_variable} "${_source}" PARENT_SCOPE)
endfunction()
