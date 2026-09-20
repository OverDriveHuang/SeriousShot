# CMake's MSVC compiler identification can assume English /showIncludes even
# when only localized MSVC resources are installed. An incorrect prefix makes
# Ninja silently omit header dependencies. Detect the actual tool's prefix.
set(_hdrshot_include_probe "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/hdrshot_msvc_include_probe.cpp")
file(WRITE "${_hdrshot_include_probe}" "#include <stddef.h>\n")
execute_process(
  COMMAND "${CMAKE_CXX_COMPILER}" /nologo /showIncludes /EP "${_hdrshot_include_probe}"
  OUTPUT_VARIABLE _hdrshot_include_stdout ERROR_VARIABLE _hdrshot_include_stderr
  RESULT_VARIABLE _hdrshot_include_result
)
if(_hdrshot_include_result EQUAL 0)
  string(REGEX MATCH "[^\r\n]* +[A-Za-z]:[/\\\\][^\r\n]*stddef.h"
      _hdrshot_include_line "${_hdrshot_include_stdout}\n${_hdrshot_include_stderr}")
  string(REGEX REPLACE " +[A-Za-z]:[/\\\\].*$" "" _hdrshot_include_prefix "${_hdrshot_include_line}")
  if(NOT _hdrshot_include_prefix STREQUAL "")
    set(CMAKE_CL_SHOWINCLUDES_PREFIX "${_hdrshot_include_prefix}")
    message(STATUS "MSVC dependency prefix: ${CMAKE_CL_SHOWINCLUDES_PREFIX}")
  endif()
endif()
