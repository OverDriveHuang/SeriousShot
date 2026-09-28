add_library(hdrshot_windows_analysis STATIC
    src/platform/windows/windows_analysis_backend.cpp
    src/platform/windows/windows_analysis_presenter.cpp)
target_link_libraries(hdrshot_windows_analysis PUBLIC hdrshot_analysis_core
    hdrshot_windows_capture hdrshot_export_workflow
    PRIVATE d3d11 dxgi d3dcompiler dcomp windowsapp)
target_compile_definitions(hdrshot_windows_analysis PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
hdrshot_enable_warnings(hdrshot_windows_analysis)
if(BUILD_TESTING)
  foreach(module IN ITEMS analysis analysis_presenter)
    add_executable(hdrshot_windows_${module}_tests tests/windows_${module}_tests.cpp)
    target_link_libraries(hdrshot_windows_${module}_tests PRIVATE hdrshot_windows_analysis hdrshot_analysis_cpu)
    target_include_directories(hdrshot_windows_${module}_tests PRIVATE tests)
    target_compile_definitions(hdrshot_windows_${module}_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    hdrshot_enable_warnings(hdrshot_windows_${module}_tests)
    add_test(NAME hdrshot_windows_${module}_tests COMMAND hdrshot_windows_${module}_tests)
    set_tests_properties(hdrshot_windows_${module}_tests PROPERTIES TIMEOUT 120)
  endforeach()
endif()
