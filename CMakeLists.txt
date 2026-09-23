cmake_minimum_required(VERSION 3.24)

if(APPLE AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "15.5" CACHE STRING
      "Minimum supported macOS version")
endif()

project(HDRShot VERSION 0.1.0 LANGUAGES CXX)

if(MSVC)
  include(cmake/windows_msvc.cmake)
endif()

option(HDRSHOT_ENABLE_QT_HOST "Build the production Qt/native host POC" OFF)

if(APPLE)
  enable_language(OBJCXX)
endif()

include(CTest)
find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)

include(FetchContent)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static third-party libraries" FORCE)
set(UHDR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(UHDR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(UHDR_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
set(UHDR_BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
set(UHDR_BUILD_DEPS ON CACHE BOOL "" FORCE)
set(UHDR_BUILD_JAVA OFF CACHE BOOL "" FORCE)
set(UHDR_BUILD_PACKAGING OFF CACHE BOOL "" FORCE)
set(UHDR_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(UHDR_ENABLE_GLES OFF CACHE BOOL "" FORCE)
set(UHDR_ENABLE_HEIF OFF CACHE BOOL "" FORCE)
set(UHDR_WRITE_XMP ON CACHE BOOL "" FORCE)
set(UHDR_WRITE_ISO ON CACHE BOOL "" FORCE)
set(UHDR_MAX_DIMENSION 16384 CACHE STRING "Maximum Ultra HDR image dimension" FORCE)
if(MSVC)
  # Upstream's BUILD_FOR_WINUI switch selects the parent's DLL CRT for a
  # static codec. It does not add WinUI; Qt and bundled libjpeg use /MD too.
  set(BUILD_FOR_WINUI ON)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()
FetchContent_Declare(
  ultrahdr
  GIT_REPOSITORY https://github.com/google/libultrahdr.git
  GIT_TAG e5f5a022fe96fc4dc2ee35c19f733a50df807abe
  GIT_SHALLOW FALSE
  GIT_CONFIG core.autocrlf=false core.eol=lf
)
FetchContent_MakeAvailable(ultrahdr)
include(cmake/libultrahdr_gainmap.cmake)
include(cmake/libultrahdr_p3_alternate.cmake)
include(cmake/libultrahdr_ycbcr.cmake)
if(TARGET uhdr)
  set(HDRSHOT_ULTRAHDR_TARGET uhdr)
elseif(TARGET uhdr-static)
  set(HDRSHOT_ULTRAHDR_TARGET uhdr-static)
else()
  message(FATAL_ERROR "libultrahdr did not provide a supported CMake target")
endif()

set(HDRSHOT_DISPLAY_P3_ICC_PATH
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/color_profiles/DisplayP3-v4.icc")
set(HDRSHOT_DISPLAY_P3_ICC_SHA256
    "cb51de38e482ee974c0c76b9689e16aad04bad16e226fed2f30c842d15ff3a3d")
file(SHA256 "${HDRSHOT_DISPLAY_P3_ICC_PATH}" HDRSHOT_DISPLAY_P3_ICC_ACTUAL_SHA256)
if(NOT HDRSHOT_DISPLAY_P3_ICC_ACTUAL_SHA256 STREQUAL HDRSHOT_DISPLAY_P3_ICC_SHA256)
  message(FATAL_ERROR
      "Bundled DisplayP3-v4.icc hash mismatch: ${HDRSHOT_DISPLAY_P3_ICC_ACTUAL_SHA256}")
endif()
file(READ "${HDRSHOT_DISPLAY_P3_ICC_PATH}" HDRSHOT_DISPLAY_P3_ICC_HEX HEX)
string(LENGTH "${HDRSHOT_DISPLAY_P3_ICC_HEX}" HDRSHOT_DISPLAY_P3_ICC_HEX_LENGTH)
math(EXPR HDRSHOT_DISPLAY_P3_ICC_SIZE "${HDRSHOT_DISPLAY_P3_ICC_HEX_LENGTH} / 2")
string(REGEX REPLACE "([0-9A-Fa-f][0-9A-Fa-f])" "0x\\1,"
       HDRSHOT_DISPLAY_P3_ICC_BYTES "${HDRSHOT_DISPLAY_P3_ICC_HEX}")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(
  cmake/display_p3_icc_profile.cpp.in
  "${CMAKE_CURRENT_BINARY_DIR}/generated/display_p3_icc_profile.cpp"
  @ONLY
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

function(hdrshot_enable_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive-)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
  endif()
endfunction()

add_library(hdrshot_core STATIC
  src/core/error.cpp
)
target_include_directories(hdrshot_core PUBLIC src)
hdrshot_enable_warnings(hdrshot_core)

# Separate reproducible source identity from diagnostic build time.
option(HDRSHOT_RELEASE_BUILD "Require known, clean source identity for distribution" OFF)
set(HDRSHOT_BUILD_TIMESTAMP_SOURCE
    "${CMAKE_CURRENT_BINARY_DIR}/generated/hdrshot_build_timestamp.cpp")
add_custom_target(hdrshot_generate_build_timestamp
  COMMAND "${CMAKE_COMMAND}"
    "-DOUTPUT_FILE=${HDRSHOT_BUILD_TIMESTAMP_SOURCE}"
    "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
    "-DPRODUCT_VERSION=${PROJECT_VERSION}"
    "-DREQUIRE_CLEAN_SOURCE=${HDRSHOT_RELEASE_BUILD}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateBuildTimestamp.cmake"
  BYPRODUCTS "${HDRSHOT_BUILD_TIMESTAMP_SOURCE}"
  COMMENT "Generating SeriousShot source identity and build timestamp"
  VERBATIM
)
set_source_files_properties("${HDRSHOT_BUILD_TIMESTAMP_SOURCE}" PROPERTIES GENERATED TRUE)
add_library(hdrshot_build_metadata STATIC "${HDRSHOT_BUILD_TIMESTAMP_SOURCE}")
add_dependencies(hdrshot_build_metadata hdrshot_generate_build_timestamp)
target_include_directories(hdrshot_build_metadata PUBLIC src)
hdrshot_enable_warnings(hdrshot_build_metadata)

add_library(hdrshot_input_mapper STATIC
  src/application/input_mapper.cpp
)
target_link_libraries(hdrshot_input_mapper PUBLIC hdrshot_core)
target_include_directories(hdrshot_input_mapper PUBLIC src)
hdrshot_enable_warnings(hdrshot_input_mapper)

add_library(hdrshot_interaction_controller STATIC
  src/application/interaction_controller.cpp
)
target_link_libraries(hdrshot_interaction_controller PUBLIC hdrshot_input_mapper hdrshot_core)
target_include_directories(hdrshot_interaction_controller PUBLIC src)
hdrshot_enable_warnings(hdrshot_interaction_controller)

add_library(hdrshot_frame_pipeline STATIC
  src/domain/frame/frame_pipeline.cpp
)
target_link_libraries(hdrshot_frame_pipeline PUBLIC hdrshot_core hdrshot_color)
target_include_directories(hdrshot_frame_pipeline PUBLIC src)
hdrshot_enable_warnings(hdrshot_frame_pipeline)

add_library(hdrshot_frame_cropper STATIC
  src/domain/frame/frame_cropper.cpp
)
target_link_libraries(hdrshot_frame_cropper PUBLIC hdrshot_core hdrshot_color)
target_include_directories(hdrshot_frame_cropper PUBLIC src)
hdrshot_enable_warnings(hdrshot_frame_cropper)

add_library(hdrshot_annotation_rasterizer STATIC
  src/domain/annotation/annotation_pixel_plan_validator.cpp
  src/domain/annotation/annotation_render_plan.cpp
)
target_link_libraries(hdrshot_annotation_rasterizer
  PUBLIC hdrshot_core hdrshot_annotation_model hdrshot_ports hdrshot_color
)
target_include_directories(hdrshot_annotation_rasterizer PUBLIC src)
hdrshot_enable_warnings(hdrshot_annotation_rasterizer)

add_library(hdrshot_annotation_renderer STATIC
  src/domain/annotation/annotation_renderer.cpp
)
target_link_libraries(hdrshot_annotation_renderer
  PUBLIC hdrshot_core hdrshot_annotation_rasterizer hdrshot_frame_cropper hdrshot_color hdrshot_output_policy
)
target_include_directories(hdrshot_annotation_renderer PUBLIC src)
hdrshot_enable_warnings(hdrshot_annotation_renderer)

add_library(hdrshot_capture_session STATIC
  src/application/capture_session.cpp
)
target_link_libraries(hdrshot_capture_session
  PUBLIC hdrshot_core hdrshot_ports hdrshot_frame_pipeline hdrshot_annotation_model
)
target_include_directories(hdrshot_capture_session PUBLIC src)
hdrshot_enable_warnings(hdrshot_capture_session)

add_library(hdrshot_multi_display_preview STATIC
  src/application/multi_display_preview_presenter.cpp
)
target_link_libraries(hdrshot_multi_display_preview PUBLIC hdrshot_core hdrshot_ports)
target_include_directories(hdrshot_multi_display_preview PUBLIC src)
hdrshot_enable_warnings(hdrshot_multi_display_preview)

add_library(hdrshot_export_workflow STATIC
  src/application/clean_content_cache.cpp
  src/application/export_workflow.cpp
  src/application/export_completion_coordinator.cpp
)
target_link_libraries(hdrshot_export_workflow
  PUBLIC
    hdrshot_core
    hdrshot_ports
    hdrshot_default_save_workflow
    hdrshot_build_metadata
    hdrshot_frame_cropper
    hdrshot_annotation_renderer
    hdrshot_png
    hdrshot_output_policy
)
target_include_directories(hdrshot_export_workflow PUBLIC src)
hdrshot_enable_warnings(hdrshot_export_workflow)

add_library(hdrshot_analysis_application STATIC
  src/application/analysis_session.cpp
  src/application/analysis_workflow.cpp
)
target_link_libraries(hdrshot_analysis_application PUBLIC hdrshot_export_workflow Threads::Threads)
target_include_directories(hdrshot_analysis_application PUBLIC src)
hdrshot_enable_warnings(hdrshot_analysis_application)

add_library(hdrshot_analysis_core STATIC
  src/domain/analysis/math.cpp src/domain/analysis/engine.cpp
  src/domain/analysis/color_difference.cpp src/domain/analysis/swatch_pairs.cpp
  src/domain/analysis/gamut_boundary.cpp)
# Generated reference geometry must not silently drift from the shared math.
# Normalize Git CRLF checkouts so this gate is identical on Windows and macOS.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/domain/analysis/portable_math.inc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/domain/analysis/portable_color_math.inc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/domain/analysis/gamut_boundary_data.inc")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/domain/analysis/portable_math.inc" HDRSHOT_GAMUT_MATH)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/domain/analysis/portable_color_math.inc" HDRSHOT_GAMUT_COLOR_MATH)
string(REPLACE "\r\n" "\n" HDRSHOT_GAMUT_MATH "${HDRSHOT_GAMUT_MATH}")
string(REPLACE "\r\n" "\n" HDRSHOT_GAMUT_COLOR_MATH "${HDRSHOT_GAMUT_COLOR_MATH}")
string(CONCAT HDRSHOT_GAMUT_MATH_INPUTS "${HDRSHOT_GAMUT_MATH}" "${HDRSHOT_GAMUT_COLOR_MATH}")
string(SHA256 HDRSHOT_GAMUT_MATH_SHA "${HDRSHOT_GAMUT_MATH_INPUTS}")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/src/domain/analysis/gamut_boundary_data.inc"
  HDRSHOT_GAMUT_DATA_SHA REGEX "portable color math inputs SHA-256:")
if(NOT HDRSHOT_GAMUT_DATA_SHA STREQUAL "// portable color math inputs SHA-256: ${HDRSHOT_GAMUT_MATH_SHA}")
  message(FATAL_ERROR "Analyzer gamut tables are stale: run scripts/generate_analyzer_gamut.py and apply its patch")
endif()
target_link_libraries(hdrshot_analysis_core PUBLIC hdrshot_core hdrshot_color)
target_include_directories(hdrshot_analysis_core PUBLIC src)
hdrshot_enable_warnings(hdrshot_analysis_core)
add_library(hdrshot_analysis_cpu STATIC src/platform/cpu/cpu_analysis_port.cpp)
target_link_libraries(hdrshot_analysis_cpu PUBLIC hdrshot_analysis_core
  hdrshot_frame_cropper hdrshot_annotation_rasterizer hdrshot_ports)
target_include_directories(hdrshot_analysis_cpu PUBLIC src)
hdrshot_enable_warnings(hdrshot_analysis_cpu)
if(BUILD_TESTING)
  add_executable(hdrshot_analyzer_core_tests tests/analyzer_core_tests.cpp)
  target_link_libraries(hdrshot_analyzer_core_tests PRIVATE hdrshot_analysis_cpu)
  target_include_directories(hdrshot_analyzer_core_tests PRIVATE tests)
  hdrshot_enable_warnings(hdrshot_analyzer_core_tests)
  add_test(NAME hdrshot_analyzer_core_tests COMMAND hdrshot_analyzer_core_tests)
  add_executable(hdrshot_swatch_pair_tests tests/swatch_pair_tests.cpp)
  target_link_libraries(hdrshot_swatch_pair_tests PRIVATE hdrshot_analysis_core)
  target_include_directories(hdrshot_swatch_pair_tests PRIVATE tests)
  hdrshot_enable_warnings(hdrshot_swatch_pair_tests)
  add_test(NAME hdrshot_swatch_pair_tests COMMAND hdrshot_swatch_pair_tests)
  add_executable(hdrshot_analyzer_gamut_tests tests/analyzer_gamut_tests.cpp)
  target_link_libraries(hdrshot_analyzer_gamut_tests PRIVATE hdrshot_analysis_core)
  target_include_directories(hdrshot_analyzer_gamut_tests PRIVATE tests)
  hdrshot_enable_warnings(hdrshot_analyzer_gamut_tests)
  add_test(NAME hdrshot_analyzer_gamut_tests COMMAND hdrshot_analyzer_gamut_tests)
  add_executable(hdrshot_analysis_application_tests tests/analysis_application_tests.cpp)
  target_link_libraries(hdrshot_analysis_application_tests PRIVATE hdrshot_analysis_application hdrshot_analysis_cpu)
  target_include_directories(hdrshot_analysis_application_tests PRIVATE tests)
  hdrshot_enable_warnings(hdrshot_analysis_application_tests)
  add_test(NAME hdrshot_analysis_application_tests COMMAND hdrshot_analysis_application_tests)
  set_tests_properties(hdrshot_analysis_application_tests PROPERTIES TIMEOUT 60)
endif()

add_library(hdrshot_headless_capture STATIC src/application/headless_capture.cpp)
target_link_libraries(hdrshot_headless_capture
  PUBLIC hdrshot_frame_pipeline hdrshot_export_workflow hdrshot_geometry)
target_include_directories(hdrshot_headless_capture PUBLIC src)
hdrshot_enable_warnings(hdrshot_headless_capture)

add_library(hdrshot_default_save_workflow STATIC
  src/application/default_save_workflow.cpp
)
target_link_libraries(hdrshot_default_save_workflow PUBLIC hdrshot_core hdrshot_ports)
target_include_directories(hdrshot_default_save_workflow PUBLIC src)
hdrshot_enable_warnings(hdrshot_default_save_workflow)

add_library(hdrshot_settings_workflow STATIC
  src/application/settings_workflow.cpp
)
target_link_libraries(hdrshot_settings_workflow PUBLIC hdrshot_core hdrshot_ports)
target_include_directories(hdrshot_settings_workflow PUBLIC src)
hdrshot_enable_warnings(hdrshot_settings_workflow)

add_library(hdrshot_geometry STATIC
  src/domain/geometry/selection_model.cpp
  src/domain/geometry/window_selection.cpp
)
target_link_libraries(hdrshot_geometry PUBLIC hdrshot_core)
target_include_directories(hdrshot_geometry PUBLIC src)
hdrshot_enable_warnings(hdrshot_geometry)

add_library(hdrshot_annotation_model STATIC
  src/domain/annotation/annotation_document.cpp
  src/domain/annotation/annotation_geometry.cpp
)
target_link_libraries(hdrshot_annotation_model PUBLIC hdrshot_core)
target_include_directories(hdrshot_annotation_model PUBLIC src)
hdrshot_enable_warnings(hdrshot_annotation_model)

add_library(hdrshot_color STATIC
  src/domain/color/bt2020_srgb_icc_profile.cpp
  "${CMAKE_CURRENT_BINARY_DIR}/generated/display_p3_icc_profile.cpp"
  src/domain/color/extended_p3_mapper.cpp
  src/domain/color/pq_reference_white_mapper.cpp
  src/domain/color/pq_to_srgb_mapper.cpp
)
target_link_libraries(hdrshot_color PUBLIC hdrshot_core)
target_include_directories(hdrshot_color PUBLIC src "${CMAKE_CURRENT_BINARY_DIR}/generated")
hdrshot_enable_warnings(hdrshot_color)

add_library(hdrshot_png STATIC
  src/domain/png/png_encoder.cpp
  src/domain/png/streaming_png_encoder.cpp
)
target_link_libraries(hdrshot_png PUBLIC hdrshot_core ZLIB::ZLIB)
target_include_directories(hdrshot_png PUBLIC src)
hdrshot_enable_warnings(hdrshot_png)

add_library(hdrshot_output_policy STATIC
  src/domain/output/output_classifier.cpp
  src/domain/output/output_row_producer.cpp
  src/domain/output/source_range_probe.cpp
)
target_link_libraries(hdrshot_output_policy
  PUBLIC hdrshot_core hdrshot_annotation_rasterizer hdrshot_frame_cropper hdrshot_color
)
target_include_directories(hdrshot_output_policy PUBLIC src)
hdrshot_enable_warnings(hdrshot_output_policy)

add_library(hdrshot_ultra_hdr_input STATIC
  src/domain/output/ultra_hdr_input_renderer.cpp
)
target_link_libraries(hdrshot_ultra_hdr_input
  PUBLIC hdrshot_core hdrshot_color hdrshot_frame_cropper hdrshot_annotation_rasterizer
)
target_include_directories(hdrshot_ultra_hdr_input PUBLIC src)
hdrshot_enable_warnings(hdrshot_ultra_hdr_input)

add_library(hdrshot_ultra_hdr_codec STATIC
  src/adapters/shared/libultrahdr_encoder.cpp
)
target_link_libraries(hdrshot_ultra_hdr_codec
  PUBLIC hdrshot_core hdrshot_ports
  PRIVATE hdrshot_color ${HDRSHOT_ULTRAHDR_TARGET}
)
target_include_directories(hdrshot_ultra_hdr_codec PUBLIC src)
target_include_directories(hdrshot_ultra_hdr_codec SYSTEM PRIVATE
  "$<TARGET_PROPERTY:core,INCLUDE_DIRECTORIES>")
hdrshot_enable_warnings(hdrshot_ultra_hdr_codec)

add_library(hdrshot_ports INTERFACE)
target_link_libraries(hdrshot_ports INTERFACE hdrshot_core)
target_include_directories(hdrshot_ports INTERFACE src)

if(HDRSHOT_ENABLE_QT_HOST)
  find_package(Qt6 REQUIRED COMPONENTS Widgets)
  find_package(Qt6FreetypePrivate REQUIRED)
  find_package(Qt6BundledFreetype REQUIRED)

  add_library(hdrshot_qt_analyzer_ui STATIC
    src/ui/qt/analyzer_layout.cpp src/ui/qt/analyzer_source_view.cpp
    src/ui/qt/analyzer_scope_plot.cpp src/ui/qt/analyzer_swatch_panel.cpp
    src/ui/qt/analyzer_window.cpp)
  target_link_libraries(hdrshot_qt_analyzer_ui PUBLIC hdrshot_analysis_core Qt6::Widgets)
  target_include_directories(hdrshot_qt_analyzer_ui PUBLIC src)
  hdrshot_enable_warnings(hdrshot_qt_analyzer_ui)
  if(BUILD_TESTING)
    add_executable(hdrshot_analyzer_ui_tests tests/analyzer_ui_tests.cpp)
    target_link_libraries(hdrshot_analyzer_ui_tests PRIVATE hdrshot_qt_analyzer_ui hdrshot_analysis_cpu)
    target_include_directories(hdrshot_analyzer_ui_tests PRIVATE tests)
    hdrshot_enable_warnings(hdrshot_analyzer_ui_tests)
    add_test(NAME hdrshot_analyzer_ui_tests COMMAND hdrshot_analyzer_ui_tests)
    set_tests_properties(hdrshot_analyzer_ui_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 90)
    add_executable(hdrshot_analyzer_scope_regression_tests tests/analyzer_scope_regression_tests.cpp)
    target_link_libraries(hdrshot_analyzer_scope_regression_tests PRIVATE hdrshot_qt_analyzer_ui)
    target_include_directories(hdrshot_analyzer_scope_regression_tests PRIVATE tests)
    hdrshot_enable_warnings(hdrshot_analyzer_scope_regression_tests)
    add_test(NAME hdrshot_analyzer_scope_regression_tests COMMAND hdrshot_analyzer_scope_regression_tests)
    set_tests_properties(hdrshot_analyzer_scope_regression_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 90)
  endif()

  add_library(hdrshot_freetype_text_rasterizer STATIC
    src/platform/freetype/freetype_text_rasterizer_port.cpp
  )
  target_link_libraries(hdrshot_freetype_text_rasterizer
    PUBLIC hdrshot_ports
    PRIVATE Qt6::FreetypePrivate Qt6::BundledFreetype
  )
  target_include_directories(hdrshot_freetype_text_rasterizer PUBLIC src)
  hdrshot_enable_warnings(hdrshot_freetype_text_rasterizer)

  add_library(hdrshot_qt_overlay_ui STATIC
    src/ui/qt/overlay_editor_widget.cpp
  )
  target_link_libraries(hdrshot_qt_overlay_ui
    PUBLIC hdrshot_annotation_model hdrshot_annotation_rasterizer hdrshot_geometry hdrshot_input_mapper hdrshot_capture_session hdrshot_export_workflow Qt6::Widgets
  )
  target_include_directories(hdrshot_qt_overlay_ui PUBLIC src)
  hdrshot_enable_warnings(hdrshot_qt_overlay_ui)

  add_library(hdrshot_qt_settings STATIC
    src/platform/qt/qt_folder_opener_port.cpp
    src/platform/qt/qt_settings_store_port.cpp
    src/ui/qt/settings_window.cpp
  )
  target_link_libraries(hdrshot_qt_settings
    PUBLIC hdrshot_settings_workflow hdrshot_ports hdrshot_build_metadata Qt6::Widgets
  )
  target_include_directories(hdrshot_qt_settings PUBLIC src)
  hdrshot_enable_warnings(hdrshot_qt_settings)

  add_library(hdrshot_qt_diagnostics STATIC
    src/platform/qt/qt_json_lines_diagnostics_port.cpp
    src/platform/qt/qt_session_diagnostics_port.cpp
  )
  target_link_libraries(hdrshot_qt_diagnostics PUBLIC hdrshot_core Qt6::Widgets)
  target_include_directories(hdrshot_qt_diagnostics PUBLIC src)
  hdrshot_enable_warnings(hdrshot_qt_diagnostics)

  if(BUILD_TESTING)
    add_executable(hdrshot_qt_overlay_host_tests tests/qt_overlay_host_tests.cpp)
    target_link_libraries(hdrshot_qt_overlay_host_tests PRIVATE hdrshot_qt_overlay_ui hdrshot_interaction_controller)
    target_include_directories(hdrshot_qt_overlay_host_tests PRIVATE tests)
    hdrshot_enable_warnings(hdrshot_qt_overlay_host_tests)
    add_test(NAME hdrshot_qt_overlay_host_tests COMMAND hdrshot_qt_overlay_host_tests)
    set_tests_properties(hdrshot_qt_overlay_host_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)

    add_executable(hdrshot_qt_settings_store_tests
      tests/qt_settings_store_port_tests.cpp
    )
    target_link_libraries(hdrshot_qt_settings_store_tests PRIVATE hdrshot_qt_settings)
    target_include_directories(hdrshot_qt_settings_store_tests PRIVATE tests)
    hdrshot_enable_warnings(hdrshot_qt_settings_store_tests)
    add_test(NAME hdrshot_qt_settings_store_tests COMMAND hdrshot_qt_settings_store_tests)

    add_executable(hdrshot_qt_diagnostics_tests
      tests/qt_json_lines_diagnostics_port_tests.cpp
    )
    target_link_libraries(hdrshot_qt_diagnostics_tests PRIVATE hdrshot_qt_diagnostics)
    target_include_directories(hdrshot_qt_diagnostics_tests PRIVATE tests)
    hdrshot_enable_warnings(hdrshot_qt_diagnostics_tests)
    add_test(NAME hdrshot_qt_diagnostics_tests COMMAND hdrshot_qt_diagnostics_tests)

    add_executable(hdrshot_session_diagnostics_tests tests/qt_session_diagnostics_port_tests.cpp)
    target_link_libraries(hdrshot_session_diagnostics_tests PRIVATE hdrshot_qt_diagnostics)
    target_include_directories(hdrshot_session_diagnostics_tests PRIVATE tests)
    hdrshot_enable_warnings(hdrshot_session_diagnostics_tests)
    add_test(NAME hdrshot_session_diagnostics_tests COMMAND hdrshot_session_diagnostics_tests)

    add_executable(hdrshot_qt_export_task_executor_tests tests/qt_export_task_executor_tests.cpp)
    target_link_libraries(hdrshot_qt_export_task_executor_tests PRIVATE hdrshot_ports Qt6::Core)
    target_include_directories(hdrshot_qt_export_task_executor_tests PRIVATE tests)
    hdrshot_enable_warnings(hdrshot_qt_export_task_executor_tests)
    add_test(NAME hdrshot_qt_export_task_executor_tests COMMAND hdrshot_qt_export_task_executor_tests)
    set_tests_properties(hdrshot_qt_export_task_executor_tests PROPERTIES TIMEOUT 10)

    add_executable(hdrshot_settings_window_tests
      tests/settings_window_tests.cpp
    )
    target_link_libraries(hdrshot_settings_window_tests PRIVATE
      hdrshot_qt_settings
      hdrshot_input_mapper
    )
    target_include_directories(hdrshot_settings_window_tests PRIVATE tests)
    hdrshot_enable_warnings(hdrshot_settings_window_tests)
    add_test(NAME hdrshot_settings_window_tests COMMAND hdrshot_settings_window_tests)

    add_executable(hdrshot_overlay_editor_widget_tests
      tests/overlay_editor_widget_tests.cpp
    )
    target_link_libraries(hdrshot_overlay_editor_widget_tests PRIVATE hdrshot_qt_overlay_ui)
    target_include_directories(hdrshot_overlay_editor_widget_tests PRIVATE tests)
    hdrshot_enable_warnings(hdrshot_overlay_editor_widget_tests)
    add_test(
      NAME hdrshot_overlay_editor_widget_tests
      COMMAND hdrshot_overlay_editor_widget_tests
    )

    add_executable(hdrshot_freetype_text_rasterizer_tests
      tests/freetype_text_rasterizer_tests.cpp
    )
    target_link_libraries(hdrshot_freetype_text_rasterizer_tests
      PRIVATE hdrshot_freetype_text_rasterizer
    )
    target_include_directories(hdrshot_freetype_text_rasterizer_tests PRIVATE tests)
    target_compile_definitions(hdrshot_freetype_text_rasterizer_tests PRIVATE
      HDRSHOT_TEST_FONT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/NotoSansSC-VF.ttf"
    )
    hdrshot_enable_warnings(hdrshot_freetype_text_rasterizer_tests)
    add_test(
      NAME hdrshot_freetype_text_rasterizer_tests
      COMMAND hdrshot_freetype_text_rasterizer_tests
    )
  endif()
endif()

if(APPLE)
  add_library(hdrshot_macos_linear_storage STATIC src/platform/macos/macos_linear_storage.mm)
  target_include_directories(hdrshot_macos_linear_storage PUBLIC src)
  target_compile_options(hdrshot_macos_linear_storage PRIVATE -fobjc-arc)
  target_link_libraries(hdrshot_macos_linear_storage PUBLIC "-framework Metal")
  add_library(hdrshot_macos_gpu_source STATIC src/platform/macos/macos_gpu_source.mm)
  add_library(hdrshot_macos_analysis STATIC src/platform/macos/macos_analysis_backend.mm)
  target_include_directories(hdrshot_macos_analysis PUBLIC src)
  target_compile_options(hdrshot_macos_analysis PRIVATE -fobjc-arc)
  target_link_libraries(hdrshot_macos_analysis PUBLIC hdrshot_analysis_core
    hdrshot_macos_gpu_source hdrshot_frame_cropper hdrshot_annotation_rasterizer
    "-framework AppKit" "-framework Metal" "-framework QuartzCore" "-framework Foundation")
  hdrshot_enable_warnings(hdrshot_macos_analysis)
  if(BUILD_TESTING)
    add_executable(hdrshot_macos_analysis_tests tests/macos_analysis_tests.mm)
    target_compile_options(hdrshot_macos_analysis_tests PRIVATE -fobjc-arc)
    target_include_directories(hdrshot_macos_analysis_tests PRIVATE tests)
    target_link_libraries(hdrshot_macos_analysis_tests PRIVATE hdrshot_macos_analysis hdrshot_analysis_cpu)
    hdrshot_enable_warnings(hdrshot_macos_analysis_tests)
    add_test(NAME hdrshot_macos_analysis_tests COMMAND hdrshot_macos_analysis_tests)
    set_tests_properties(hdrshot_macos_analysis_tests PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
  endif()
  target_include_directories(hdrshot_macos_gpu_source PUBLIC src)
  target_compile_options(hdrshot_macos_gpu_source PRIVATE -fobjc-arc)
  target_link_libraries(hdrshot_macos_gpu_source PUBLIC hdrshot_color hdrshot_macos_linear_storage "-framework Metal" "-framework Foundation")

  add_library(hdrshot_macos_capture_adapter STATIC
    src/platform/macos/screen_capture_kit_adapter.mm
  )
  target_link_libraries(hdrshot_macos_capture_adapter
    PUBLIC hdrshot_core
    PRIVATE
      "-framework CoreGraphics"
      "-framework AppKit"
      "-framework CoreMedia"
      "-framework CoreVideo"
      "-framework Foundation"
      "-framework ScreenCaptureKit"
  )
  target_include_directories(hdrshot_macos_capture_adapter PUBLIC src)
  target_compile_options(hdrshot_macos_capture_adapter PRIVATE -fobjc-arc)
  set_target_properties(hdrshot_macos_capture_adapter PROPERTIES
    OBJCXX_STANDARD 20
    OBJCXX_STANDARD_REQUIRED ON
    OSX_DEPLOYMENT_TARGET 15.5
  )
  hdrshot_enable_warnings(hdrshot_macos_capture_adapter)

  add_library(hdrshot_macos_capture_ports STATIC
    src/platform/macos/macos_capture_ports.mm
    src/platform/macos/macos_window_catalog.mm
  )
  target_link_libraries(hdrshot_macos_capture_ports
    PUBLIC hdrshot_ports hdrshot_macos_capture_adapter hdrshot_geometry
  )
  target_include_directories(hdrshot_macos_capture_ports PUBLIC src)
  target_compile_options(hdrshot_macos_capture_ports PRIVATE -fobjc-arc)
  set_target_properties(hdrshot_macos_capture_ports PROPERTIES
    OBJCXX_STANDARD 20
    OBJCXX_STANDARD_REQUIRED ON
    OSX_DEPLOYMENT_TARGET 15.5
  )
  hdrshot_enable_warnings(hdrshot_macos_capture_ports)

  add_executable(hdrshot_macos_window_catalog_probe tools/macos_window_catalog_probe.mm)
  target_link_libraries(hdrshot_macos_window_catalog_probe PRIVATE hdrshot_macos_capture_ports)
  target_compile_options(hdrshot_macos_window_catalog_probe PRIVATE -fobjc-arc)
  set_target_properties(hdrshot_macos_window_catalog_probe PROPERTIES
    OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON OSX_DEPLOYMENT_TARGET 15.5)
  hdrshot_enable_warnings(hdrshot_macos_window_catalog_probe)

  add_library(hdrshot_macos_metal_presenter STATIC
    src/platform/macos/metal_edr_presenter.mm
  )
  target_link_libraries(hdrshot_macos_metal_presenter
    PUBLIC hdrshot_core
    PRIVATE
      "-framework CoreGraphics"
      "-framework Foundation"
      "-framework Metal"
      "-framework QuartzCore"
  )
  target_include_directories(hdrshot_macos_metal_presenter PUBLIC src)
  target_compile_options(hdrshot_macos_metal_presenter PRIVATE -fobjc-arc)
  set_target_properties(hdrshot_macos_metal_presenter PROPERTIES
    OBJCXX_STANDARD 20
    OBJCXX_STANDARD_REQUIRED ON
    OSX_DEPLOYMENT_TARGET 15.5
  )
  hdrshot_enable_warnings(hdrshot_macos_metal_presenter)

  add_library(hdrshot_macos_preview_presenter_port STATIC
    src/platform/macos/macos_preview_presenter_port.cpp
  )
  target_link_libraries(hdrshot_macos_preview_presenter_port
    PUBLIC hdrshot_ports hdrshot_macos_metal_presenter
  )
  target_include_directories(hdrshot_macos_preview_presenter_port PUBLIC src)
  hdrshot_enable_warnings(hdrshot_macos_preview_presenter_port)

  add_library(hdrshot_macos_export_pixel_processor STATIC
    src/platform/macos/macos_metal_export_pixel_processor.mm
  )
  target_link_libraries(hdrshot_macos_export_pixel_processor
    PUBLIC hdrshot_ports hdrshot_output_policy
    PRIVATE
      "-framework Foundation"
      "-framework Metal"
  )
  target_include_directories(hdrshot_macos_export_pixel_processor PUBLIC src)
  target_compile_options(hdrshot_macos_export_pixel_processor PRIVATE -fobjc-arc)
  set_target_properties(hdrshot_macos_export_pixel_processor PROPERTIES
    OBJCXX_STANDARD 20
    OBJCXX_STANDARD_REQUIRED ON
    OSX_DEPLOYMENT_TARGET 15.5
  )
  hdrshot_enable_warnings(hdrshot_macos_export_pixel_processor)

  add_library(hdrshot_macos_export_ports STATIC
    src/platform/macos/macos_export_ports.mm
  )
  target_link_libraries(hdrshot_macos_export_ports
    PUBLIC hdrshot_ports
    PRIVATE
      "-framework AppKit"
      "-framework Foundation"
      "-framework UniformTypeIdentifiers"
  )
  target_include_directories(hdrshot_macos_export_ports PUBLIC src)
  target_compile_options(hdrshot_macos_export_ports PRIVATE -fobjc-arc)
  set_target_properties(hdrshot_macos_export_ports PROPERTIES
    OBJCXX_STANDARD 20
    OBJCXX_STANDARD_REQUIRED ON
    OSX_DEPLOYMENT_TARGET 15.5
  )
  hdrshot_enable_warnings(hdrshot_macos_export_ports)

  add_library(hdrshot_macos_global_hotkey_port STATIC
    src/platform/macos/macos_global_hotkey_port.mm
  )
  target_link_libraries(hdrshot_macos_global_hotkey_port
    PUBLIC hdrshot_ports
    PRIVATE "-framework Carbon"
  )
  target_include_directories(hdrshot_macos_global_hotkey_port PUBLIC src)
  target_compile_options(hdrshot_macos_global_hotkey_port PRIVATE -fobjc-arc)
  set_target_properties(hdrshot_macos_global_hotkey_port PROPERTIES
    OBJCXX_STANDARD 20
    OBJCXX_STANDARD_REQUIRED ON
    OSX_DEPLOYMENT_TARGET 15.5
  )
  hdrshot_enable_warnings(hdrshot_macos_global_hotkey_port)

  add_executable(hdrshot_macos_capture_smoke tools/macos_capture_smoke.cpp)
  target_link_libraries(hdrshot_macos_capture_smoke PRIVATE hdrshot_macos_capture_adapter)
  target_include_directories(hdrshot_macos_capture_smoke PRIVATE src)
  hdrshot_enable_warnings(hdrshot_macos_capture_smoke)

  add_executable(hdrshot_macos_global_hotkey_smoke tools/macos_global_hotkey_smoke.mm)
  target_link_libraries(hdrshot_macos_global_hotkey_smoke PRIVATE
    hdrshot_macos_global_hotkey_port
    "-framework AppKit"
  )
  target_compile_options(hdrshot_macos_global_hotkey_smoke PRIVATE -fobjc-arc)
  set_target_properties(hdrshot_macos_global_hotkey_smoke PROPERTIES
    OBJCXX_STANDARD 20
    OBJCXX_STANDARD_REQUIRED ON
    OSX_DEPLOYMENT_TARGET 15.5
  )
  hdrshot_enable_warnings(hdrshot_macos_global_hotkey_smoke)

  add_executable(hdrshot_macos_atomic_region_probe tools/macos_atomic_region_probe.mm)
  target_link_libraries(hdrshot_macos_atomic_region_probe PRIVATE
    "-framework CoreGraphics"
    "-framework Foundation"
    "-framework ScreenCaptureKit"
  )
  target_compile_options(hdrshot_macos_atomic_region_probe PRIVATE -fobjc-arc)
  set_target_properties(hdrshot_macos_atomic_region_probe PROPERTIES
    OBJCXX_STANDARD 20
    OBJCXX_STANDARD_REQUIRED ON
    OSX_DEPLOYMENT_TARGET 15.2
  )
  hdrshot_enable_warnings(hdrshot_macos_atomic_region_probe)

  add_executable(hdrshot_macos_metal_overlay_poc tools/macos_metal_overlay_poc.mm)
  target_link_libraries(hdrshot_macos_metal_overlay_poc PRIVATE
    hdrshot_macos_metal_presenter
    "-framework AppKit"
    "-framework QuartzCore"
  )
  target_compile_options(hdrshot_macos_metal_overlay_poc PRIVATE -fobjc-arc)
  set_target_properties(hdrshot_macos_metal_overlay_poc PROPERTIES
    OBJCXX_STANDARD 20
    OBJCXX_STANDARD_REQUIRED ON
    OSX_DEPLOYMENT_TARGET 15.5
  )
  hdrshot_enable_warnings(hdrshot_macos_metal_overlay_poc)

  add_executable(hdrshot_macos_capture_overlay_smoke tools/macos_capture_overlay_smoke.mm)
  target_link_libraries(hdrshot_macos_capture_overlay_smoke PRIVATE
    hdrshot_macos_capture_adapter
    hdrshot_macos_metal_presenter
    "-framework AppKit"
    "-framework QuartzCore"
  )
  target_compile_options(hdrshot_macos_capture_overlay_smoke PRIVATE -fobjc-arc)
  set_target_properties(hdrshot_macos_capture_overlay_smoke PROPERTIES
    OBJCXX_STANDARD 20
    OBJCXX_STANDARD_REQUIRED ON
    OSX_DEPLOYMENT_TARGET 15.5
  )
  hdrshot_enable_warnings(hdrshot_macos_capture_overlay_smoke)

  if(HDRSHOT_ENABLE_QT_HOST)
    # Explicit opt-in desktop fixture; not an unattended CTest entry.
    add_executable(hdrshot_macos_linear_capture_probe EXCLUDE_FROM_ALL
      tools/macos_linear_capture_probe.mm)
    target_link_libraries(hdrshot_macos_linear_capture_probe PRIVATE
      hdrshot_macos_capture_ports hdrshot_qt_diagnostics hdrshot_build_metadata hdrshot_color
      "-framework AppKit" "-framework Metal" "-framework QuartzCore")
    target_compile_options(hdrshot_macos_linear_capture_probe PRIVATE -fobjc-arc)
    set_target_properties(hdrshot_macos_linear_capture_probe PROPERTIES
      OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON OSX_DEPLOYMENT_TARGET 15.5)
    add_library(hdrshot_macos_qt_input_platform_adapter STATIC
      src/platform/macos/macos_qt_input_platform_adapter.cpp
    )
    target_link_libraries(hdrshot_macos_qt_input_platform_adapter
      PUBLIC hdrshot_input_mapper Qt6::Widgets
    )
    target_include_directories(hdrshot_macos_qt_input_platform_adapter PUBLIC src)
    hdrshot_enable_warnings(hdrshot_macos_qt_input_platform_adapter)

    add_library(hdrshot_macos_qt_analyzer STATIC
      src/platform/macos/macos_qt_analyzer_controller.mm)
    target_compile_options(hdrshot_macos_qt_analyzer PRIVATE -fobjc-arc)
    target_link_libraries(hdrshot_macos_qt_analyzer PUBLIC hdrshot_analysis_application
      hdrshot_qt_analyzer_ui hdrshot_macos_analysis hdrshot_macos_export_pixel_processor)
    hdrshot_enable_warnings(hdrshot_macos_qt_analyzer)
    if(BUILD_TESTING)
      add_executable(hdrshot_analyzer_ui_native_surface_tests tests/analyzer_ui_native_surface_tests.mm)
      add_executable(hdrshot_analyzer_interaction_probe
        artifacts/2026-09-19_analyzer_interaction_fixes/checks/interaction_probe.mm)
      target_compile_options(hdrshot_analyzer_interaction_probe PRIVATE -fobjc-arc)
      target_link_libraries(hdrshot_analyzer_interaction_probe PRIVATE
        hdrshot_qt_analyzer_ui hdrshot_macos_analysis hdrshot_analysis_cpu hdrshot_analysis_application)
      target_compile_options(hdrshot_analyzer_ui_native_surface_tests PRIVATE -fobjc-arc)
      target_link_libraries(hdrshot_analyzer_ui_native_surface_tests PRIVATE hdrshot_qt_analyzer_ui
        hdrshot_macos_analysis hdrshot_analysis_cpu)
      target_include_directories(hdrshot_analyzer_ui_native_surface_tests PRIVATE tests)
      hdrshot_enable_warnings(hdrshot_analyzer_ui_native_surface_tests)
      add_test(NAME hdrshot_analyzer_ui_native_surface_tests COMMAND hdrshot_analyzer_ui_native_surface_tests)
      set_tests_properties(hdrshot_analyzer_ui_native_surface_tests PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)

      add_executable(hdrshot_macos_analyzer_integration_tests tests/macos_analyzer_integration_tests.mm)
      target_compile_options(hdrshot_macos_analyzer_integration_tests PRIVATE -fobjc-arc)
      target_link_libraries(hdrshot_macos_analyzer_integration_tests PRIVATE hdrshot_macos_qt_analyzer
        hdrshot_macos_export_ports hdrshot_ultra_hdr_codec ${HDRSHOT_ULTRAHDR_TARGET})
      target_include_directories(hdrshot_macos_analyzer_integration_tests PRIVATE tests)
      target_include_directories(hdrshot_macos_analyzer_integration_tests SYSTEM PRIVATE
        "$<TARGET_PROPERTY:core,INCLUDE_DIRECTORIES>")
      hdrshot_enable_warnings(hdrshot_macos_analyzer_integration_tests)
      add_test(NAME hdrshot_macos_analyzer_integration_tests COMMAND hdrshot_macos_analyzer_integration_tests)
      set_tests_properties(hdrshot_macos_analyzer_integration_tests PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
    endif()

    add_executable(hdrshot_macos_qt_overlay_poc tools/macos_qt_overlay_poc.mm
      src/platform/macos/macos_capture_cli.mm
      src/platform/macos/macos_qt_window_activation.mm
      src/platform/macos/macos_qt_overlay_window.mm)
    target_link_libraries(hdrshot_macos_qt_overlay_poc PRIVATE
      hdrshot_capture_session
      hdrshot_macos_qt_analyzer
      hdrshot_headless_capture
      hdrshot_interaction_controller
      hdrshot_multi_display_preview
      hdrshot_export_workflow
      hdrshot_annotation_model
      hdrshot_geometry
      hdrshot_macos_capture_ports
      hdrshot_macos_export_ports
      hdrshot_macos_global_hotkey_port
      hdrshot_macos_qt_input_platform_adapter
      hdrshot_freetype_text_rasterizer
      hdrshot_macos_preview_presenter_port
      hdrshot_macos_export_pixel_processor
      hdrshot_ultra_hdr_codec
      hdrshot_qt_overlay_ui
      hdrshot_qt_diagnostics
      hdrshot_qt_settings
      Qt6::Widgets
      "-framework AppKit"
      "-framework QuartzCore"
    )
    target_compile_options(hdrshot_macos_qt_overlay_poc PRIVATE -fobjc-arc)
    set_target_properties(hdrshot_macos_qt_overlay_poc PROPERTIES
      OBJCXX_STANDARD 20
      OBJCXX_STANDARD_REQUIRED ON
      OSX_DEPLOYMENT_TARGET 15.5
      MACOSX_BUNDLE TRUE
      MACOSX_BUNDLE_BUNDLE_NAME "SeriousShot"
      MACOSX_BUNDLE_GUI_IDENTIFIER "dev.hdrshot.desktop"
      MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
      MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
      MACOSX_BUNDLE_INFO_PLIST
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/macos/HDRShotInfo.plist.in"
      OUTPUT_NAME "SeriousShot"
    )
    hdrshot_enable_warnings(hdrshot_macos_qt_overlay_poc)
    target_sources(hdrshot_macos_qt_overlay_poc PRIVATE assets/icons/SeriousShot.icns)
    set_source_files_properties(assets/icons/SeriousShot.icns PROPERTIES
      MACOSX_PACKAGE_LOCATION Resources)
    add_custom_command(TARGET hdrshot_macos_qt_overlay_poc POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:hdrshot_macos_qt_overlay_poc>/assets/fonts"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/NotoSansSC-VF.ttf"
        "$<TARGET_FILE_DIR:hdrshot_macos_qt_overlay_poc>/assets/fonts/NotoSansSC-VF.ttf"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/OFL.txt"
        "$<TARGET_FILE_DIR:hdrshot_macos_qt_overlay_poc>/assets/fonts/OFL.txt"
      COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_BUNDLE_DIR:hdrshot_macos_qt_overlay_poc>/Contents/Resources/licenses"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
        "$<TARGET_BUNDLE_DIR:hdrshot_macos_qt_overlay_poc>/Contents/Resources/THIRD_PARTY_NOTICES.md"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ultrahdr_SOURCE_DIR}/LICENSE"
        "$<TARGET_BUNDLE_DIR:hdrshot_macos_qt_overlay_poc>/Contents/Resources/licenses/libultrahdr-LICENSE.txt"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ultrahdr_SOURCE_DIR}/adobe-hdr-gain-map-license/NOTICE"
        "$<TARGET_BUNDLE_DIR:hdrshot_macos_qt_overlay_poc>/Contents/Resources/licenses/libultrahdr-Adobe-NOTICE.txt"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ultrahdr_SOURCE_DIR}/third_party/turbojpeg/LICENSE.md"
        "$<TARGET_BUNDLE_DIR:hdrshot_macos_qt_overlay_poc>/Contents/Resources/licenses/libjpeg-turbo-LICENSE.md"
    )

    if(BUILD_TESTING)
      add_executable(hdrshot_macos_qt_overlay_window_tests
        tests/macos_qt_overlay_window_tests.mm
        src/platform/macos/macos_qt_overlay_window.mm)
      target_link_libraries(hdrshot_macos_qt_overlay_window_tests PRIVATE
        Qt6::Widgets "-framework AppKit" "-framework QuartzCore")
      target_include_directories(hdrshot_macos_qt_overlay_window_tests PRIVATE src tests)
      target_compile_options(hdrshot_macos_qt_overlay_window_tests PRIVATE -fobjc-arc)
      set_target_properties(hdrshot_macos_qt_overlay_window_tests PROPERTIES
        OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON OSX_DEPLOYMENT_TARGET 15.5)
      hdrshot_enable_warnings(hdrshot_macos_qt_overlay_window_tests)
      add_test(NAME hdrshot_macos_qt_overlay_window_tests
        COMMAND hdrshot_macos_qt_overlay_window_tests)
      # Native attributes cannot be tested with Qt's offscreen platform plugin.
      # These tests create hidden windows only; no show/event loop/capture/input.
      set_tests_properties(hdrshot_macos_qt_overlay_window_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=cocoa" TIMEOUT 10 SKIP_RETURN_CODE 77)

      add_test(
        NAME hdrshot_macos_deployment_target_contract
        COMMAND
          "${CMAKE_CURRENT_SOURCE_DIR}/scripts/verify_macos_deployment_target.sh"
          "$<TARGET_BUNDLE_DIR:hdrshot_macos_qt_overlay_poc>"
          "${CMAKE_OSX_DEPLOYMENT_TARGET}"
      )

      add_executable(hdrshot_macos_qt_input_platform_adapter_tests
        tests/macos_qt_input_platform_adapter_tests.cpp
      )
      target_link_libraries(
        hdrshot_macos_qt_input_platform_adapter_tests PRIVATE
          hdrshot_macos_qt_input_platform_adapter
      )
      target_include_directories(
        hdrshot_macos_qt_input_platform_adapter_tests PRIVATE tests
      )
      hdrshot_enable_warnings(hdrshot_macos_qt_input_platform_adapter_tests)
      add_test(
        NAME hdrshot_macos_qt_input_platform_adapter_tests
        COMMAND hdrshot_macos_qt_input_platform_adapter_tests
      )

    endif()
  endif()
endif()

if(BUILD_TESTING)
  add_executable(hdrshot_headless_capture_tests tests/headless_capture_tests.cpp)
  # Python is optional for test tooling, never a runtime/build requirement.
  find_package(Python3 QUIET COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME hdrshot_source_identity_tests
      COMMAND "${CMAKE_COMMAND}" -E env "HDRSHOT_TEST_CMAKE=${CMAKE_COMMAND}"
        "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/source_identity_tests.py")
  endif()
  target_link_libraries(hdrshot_headless_capture_tests PRIVATE hdrshot_headless_capture)
  target_include_directories(hdrshot_headless_capture_tests PRIVATE tests)
  hdrshot_enable_warnings(hdrshot_headless_capture_tests)
  add_test(NAME hdrshot_headless_capture_tests COMMAND hdrshot_headless_capture_tests)
  add_executable(hdrshot_export_completion_tests tests/export_completion_tests.cpp)
  target_link_libraries(hdrshot_export_completion_tests PRIVATE hdrshot_export_workflow)
  target_include_directories(hdrshot_export_completion_tests PRIVATE tests)
  hdrshot_enable_warnings(hdrshot_export_completion_tests)
  add_test(NAME hdrshot_export_completion_tests COMMAND hdrshot_export_completion_tests)
  add_executable(hdrshot_input_mapper_tests tests/input_mapper_tests.cpp)
  target_link_libraries(hdrshot_input_mapper_tests PRIVATE hdrshot_input_mapper)
  target_include_directories(hdrshot_input_mapper_tests PRIVATE tests)
  hdrshot_enable_warnings(hdrshot_input_mapper_tests)
  add_test(NAME hdrshot_input_mapper_tests COMMAND hdrshot_input_mapper_tests)

  add_executable(hdrshot_interaction_tests tests/interaction_controller_tests.cpp)
  target_link_libraries(hdrshot_interaction_tests PRIVATE hdrshot_interaction_controller)
  target_include_directories(hdrshot_interaction_tests PRIVATE tests)
  hdrshot_enable_warnings(hdrshot_interaction_tests)
  add_test(NAME hdrshot_interaction_tests COMMAND hdrshot_interaction_tests)

  add_executable(hdrshot_frame_pipeline_tests tests/frame_pipeline_tests.cpp)
  target_link_libraries(hdrshot_frame_pipeline_tests PRIVATE hdrshot_frame_pipeline)
  target_include_directories(hdrshot_frame_pipeline_tests PRIVATE tests)
  hdrshot_enable_warnings(hdrshot_frame_pipeline_tests)
  add_test(NAME hdrshot_frame_pipeline_tests COMMAND hdrshot_frame_pipeline_tests)

  add_executable(hdrshot_capture_session_tests tests/capture_session_tests.cpp)
  target_link_libraries(hdrshot_capture_session_tests PRIVATE hdrshot_capture_session)
  target_include_directories(hdrshot_capture_session_tests PRIVATE tests)
  hdrshot_enable_warnings(hdrshot_capture_session_tests)
  add_test(NAME hdrshot_capture_session_tests COMMAND hdrshot_capture_session_tests)

  add_executable(hdrshot_multi_display_preview_tests
    tests/multi_display_preview_presenter_tests.cpp
  )
  target_link_libraries(hdrshot_multi_display_preview_tests PRIVATE hdrshot_multi_display_preview)
  target_include_directories(hdrshot_multi_display_preview_tests PRIVATE tests)
  hdrshot_enable_warnings(hdrshot_multi_display_preview_tests)
  add_test(NAME hdrshot_multi_display_preview_tests COMMAND hdrshot_multi_display_preview_tests)

  add_executable(hdrshot_frame_cropper_tests tests/frame_cropper_tests.cpp)
  target_link_libraries(hdrshot_frame_cropper_tests PRIVATE hdrshot_frame_cropper)
  target_include_directories(hdrshot_frame_cropper_tests PRIVATE tests)
  hdrshot_enable_warnings(hdrshot_frame_cropper_tests)
  add_test(NAME hdrshot_frame_cropper_tests COMMAND hdrshot_frame_cropper_tests)

  add_executable(hdrshot_annotation_renderer_tests tests/annotation_renderer_tests.cpp)
  add_executable(hdrshot_annotation_antialiasing_tests tests/annotation_antialiasing_tests.cpp)
  target_link_libraries(hdrshot_annotation_antialiasing_tests PRIVATE hdrshot_output_policy hdrshot_ultra_hdr_input)
  target_include_directories(hdrshot_annotation_antialiasing_tests PRIVATE tests)
  hdrshot_enable_warnings(hdrshot_annotation_antialiasing_tests)
  add_test(NAME hdrshot_annotation_antialiasing_tests COMMAND hdrshot_annotation_antialiasing_tests)
  target_link_libraries(hdrshot_annotation_renderer_tests PRIVATE hdrshot_annotation_renderer)
  target_include_directories(hdrshot_annotation_renderer_tests PRIVATE tests)
  hdrshot_enable_warnings(hdrshot_annotation_renderer_tests)
  add_test(NAME hdrshot_annotation_renderer_tests COMMAND hdrshot_annotation_renderer_tests)

  add_executable(hdrshot_export_workflow_tests tests/export_workflow_tests.cpp)
  target_link_libraries(hdrshot_export_workflow_tests PRIVATE hdrshot_export_workflow)
  target_include_directories(hdrshot_export_workflow_tests PRIVATE tests)
  hdrshot_enable_warnings(hdrshot_export_workflow_tests)
  add_test(NAME hdrshot_export_workflow_tests COMMAND hdrshot_export_workflow_tests)

  add_executable(hdrshot_geometry_tests tests/selection_model_tests.cpp)
  target_link_libraries(hdrshot_geometry_tests PRIVATE hdrshot_geometry)
  target_include_directories(hdrshot_geometry_tests PRIVATE tests)
  hdrshot_enable_warnings(hdrshot_geometry_tests)
  add_test(NAME hdrshot_geometry_tests COMMAND hdrshot_geometry_tests)

  add_executable(hdrshot_window_selection_tests tests/window_selection_tests.cpp)
  target_link_libraries(hdrshot_window_selection_tests PRIVATE hdrshot_geometry)
  target_include_directories(hdrshot_window_selection_tests PRIVATE tests)
  hdrshot_enable_warnings(hdrshot_window_selection_tests)
  add_test(NAME hdrshot_window_selection_tests COMMAND hdrshot_window_selection_tests)
  set_tests_properties(hdrshot_window_selection_tests PROPERTIES TIMEOUT 10)

  add_executable(hdrshot_overlay_input_recovery_tests tests/overlay_input_recovery_tests.cpp)
  target_include_directories(hdrshot_overlay_input_recovery_tests PRIVATE src tests)
  hdrshot_enable_warnings(hdrshot_overlay_input_recovery_tests)
  add_test(NAME hdrshot_overlay_input_recovery_tests COMMAND hdrshot_overlay_input_recovery_tests)

  add_executable(hdrshot_annotation_model_tests tests/annotation_document_tests.cpp)
  target_link_libraries(hdrshot_annotation_model_tests PRIVATE hdrshot_annotation_model)
  target_include_directories(hdrshot_annotation_model_tests PRIVATE tests)
  hdrshot_enable_warnings(hdrshot_annotation_model_tests)
  add_test(NAME hdrshot_annotation_model_tests COMMAND hdrshot_annotation_model_tests)

  add_executable(hdrshot_annotation_geometry_tests tests/annotation_geometry_tests.cpp)
  target_link_libraries(hdrshot_annotation_geometry_tests PRIVATE hdrshot_annotation_model)
  target_include_directories(hdrshot_annotation_geometry_tests PRIVATE tests)
  hdrshot_enable_warnings(hdrshot_annotation_geometry_tests)
  add_test(NAME hdrshot_annotation_geometry_tests COMMAND hdrshot_annotation_geometry_tests)

  add_executable(hdrshot_color_tests tests/pq_reference_white_mapper_tests.cpp)
  target_link_libraries(hdrshot_color_tests PRIVATE hdrshot_color)
  target_include_directories(hdrshot_color_tests PRIVATE tests)
  hdrshot_enable_warnings(hdrshot_color_tests)
  add_test(NAME hdrshot_color_tests COMMAND hdrshot_color_tests)

  add_executable(hdrshot_png_tests tests/png_encoder_tests.cpp)
  target_link_libraries(hdrshot_png_tests PRIVATE hdrshot_png hdrshot_color)
  target_include_directories(hdrshot_png_tests PRIVATE tests)
  hdrshot_enable_warnings(hdrshot_png_tests)
  add_test(NAME hdrshot_png_tests COMMAND hdrshot_png_tests)

  add_executable(hdrshot_png_corpus_benchmark tools/png_corpus_benchmark.cpp)
  target_link_libraries(hdrshot_png_corpus_benchmark PRIVATE hdrshot_png hdrshot_color)
  if(APPLE)
    target_link_libraries(hdrshot_png_corpus_benchmark PRIVATE "-lcompression")
  endif()
  hdrshot_enable_warnings(hdrshot_png_corpus_benchmark)
  add_test(NAME hdrshot_png_corpus_contract COMMAND hdrshot_png_corpus_benchmark)
  set_tests_properties(hdrshot_png_corpus_contract PROPERTIES TIMEOUT 30)

  # Diagnostic only: compile the encoder with adaptive switching disabled and public zlib init
  # parameters. The production hdrshot_png target has no override definition.
  if(APPLE)
    add_executable(hdrshot_png_parameter_benchmark tools/png_corpus_benchmark.cpp
      src/domain/png/streaming_png_encoder.cpp src/domain/png/png_encoder.cpp)
    target_compile_definitions(hdrshot_png_parameter_benchmark PRIVATE
      HDRSHOT_PNG_PARAMETER_SWEEP=1 HDRSHOT_PNG_FIXED_COMPRESSION_PROBE=1
      deflateInit2_=hdrshot_probe_deflate_init)
    target_link_libraries(hdrshot_png_parameter_benchmark PRIVATE hdrshot_color ZLIB::ZLIB)
    target_link_libraries(hdrshot_png_parameter_benchmark PRIVATE "-lcompression")
    hdrshot_enable_warnings(hdrshot_png_parameter_benchmark)
  endif()

  add_executable(hdrshot_output_policy_tests tests/output_classifier_tests.cpp)
  target_link_libraries(hdrshot_output_policy_tests PRIVATE hdrshot_output_policy)
  target_include_directories(hdrshot_output_policy_tests PRIVATE tests)
  hdrshot_enable_warnings(hdrshot_output_policy_tests)
  add_test(NAME hdrshot_output_policy_tests COMMAND hdrshot_output_policy_tests)

  add_executable(hdrshot_source_range_probe_tests tests/source_range_probe_tests.cpp)
  target_link_libraries(hdrshot_source_range_probe_tests PRIVATE hdrshot_output_policy)
  target_include_directories(hdrshot_source_range_probe_tests PRIVATE tests)
  hdrshot_enable_warnings(hdrshot_source_range_probe_tests)
  add_test(NAME hdrshot_source_range_probe_tests COMMAND hdrshot_source_range_probe_tests)

  add_executable(hdrshot_output_row_producer_tests tests/output_row_producer_tests.cpp)
  target_link_libraries(hdrshot_output_row_producer_tests PRIVATE hdrshot_output_policy)
  target_include_directories(hdrshot_output_row_producer_tests PRIVATE tests)
  hdrshot_enable_warnings(hdrshot_output_row_producer_tests)
  add_test(NAME hdrshot_output_row_producer_tests COMMAND hdrshot_output_row_producer_tests)

  add_executable(hdrshot_ultra_hdr_tests tests/ultra_hdr_tests.cpp)
  target_link_libraries(hdrshot_ultra_hdr_tests PRIVATE
    hdrshot_ultra_hdr_input hdrshot_ultra_hdr_codec hdrshot_annotation_rasterizer
    ${HDRSHOT_ULTRAHDR_TARGET}
  )
  target_include_directories(hdrshot_ultra_hdr_tests PRIVATE tests)
  target_include_directories(hdrshot_ultra_hdr_tests SYSTEM PRIVATE
    "$<TARGET_PROPERTY:core,INCLUDE_DIRECTORIES>")
  hdrshot_enable_warnings(hdrshot_ultra_hdr_tests)
  add_test(NAME hdrshot_ultra_hdr_tests COMMAND hdrshot_ultra_hdr_tests)

  add_executable(hdrshot_software_metadata_tests tests/software_metadata_tests.cpp)
  target_link_libraries(hdrshot_software_metadata_tests PRIVATE
    hdrshot_build_metadata hdrshot_png hdrshot_ultra_hdr_codec
    ${HDRSHOT_ULTRAHDR_TARGET})
  target_include_directories(hdrshot_software_metadata_tests PRIVATE tests)
  target_include_directories(hdrshot_software_metadata_tests SYSTEM PRIVATE
    "$<TARGET_PROPERTY:core,INCLUDE_DIRECTORIES>")
  hdrshot_enable_warnings(hdrshot_software_metadata_tests)
  add_test(NAME hdrshot_software_metadata_tests COMMAND hdrshot_software_metadata_tests)

  add_executable(hdrshot_ultra_hdr_icc_tests tests/ultra_hdr_icc_tests.cpp)
  target_include_directories(hdrshot_ultra_hdr_icc_tests PRIVATE tests)
  target_include_directories(hdrshot_ultra_hdr_icc_tests SYSTEM PRIVATE
    "$<TARGET_PROPERTY:core,INCLUDE_DIRECTORIES>")
  target_link_libraries(hdrshot_ultra_hdr_icc_tests PRIVATE ${HDRSHOT_ULTRAHDR_TARGET})
  hdrshot_enable_warnings(hdrshot_ultra_hdr_icc_tests)
  add_test(NAME hdrshot_ultra_hdr_icc_tests COMMAND hdrshot_ultra_hdr_icc_tests)

  add_executable(hdrshot_ultra_hdr_color_probe tools/ultra_hdr_color_probe.cpp)
  target_include_directories(hdrshot_ultra_hdr_color_probe SYSTEM PRIVATE
    "$<TARGET_PROPERTY:core,INCLUDE_DIRECTORIES>")
  target_link_libraries(hdrshot_ultra_hdr_color_probe PRIVATE
    hdrshot_ultra_hdr_input hdrshot_ultra_hdr_codec ${HDRSHOT_ULTRAHDR_TARGET})
  hdrshot_enable_warnings(hdrshot_ultra_hdr_color_probe)

  add_executable(hdrshot_ultra_hdr_matrix_tests tests/ultra_hdr_matrix_tests.cpp)
  target_include_directories(hdrshot_ultra_hdr_matrix_tests PRIVATE tests
    "$<TARGET_PROPERTY:core,INCLUDE_DIRECTORIES>")
  target_link_libraries(hdrshot_ultra_hdr_matrix_tests PRIVATE ${HDRSHOT_ULTRAHDR_TARGET})
  if(UHDR_ENABLE_INTRINSICS)
    target_compile_definitions(hdrshot_ultra_hdr_matrix_tests PRIVATE UHDR_ENABLE_INTRINSICS)
  endif()
  hdrshot_enable_warnings(hdrshot_ultra_hdr_matrix_tests)
  add_test(NAME hdrshot_ultra_hdr_matrix_tests COMMAND hdrshot_ultra_hdr_matrix_tests)

  add_executable(hdrshot_port_contract_tests tests/port_contract_tests.cpp)
  target_link_libraries(hdrshot_port_contract_tests PRIVATE hdrshot_ports)
  target_include_directories(hdrshot_port_contract_tests PRIVATE tests)
  hdrshot_enable_warnings(hdrshot_port_contract_tests)
  add_test(NAME hdrshot_port_contract_tests COMMAND hdrshot_port_contract_tests)

  add_executable(hdrshot_default_save_workflow_tests tests/default_save_workflow_tests.cpp)
  target_link_libraries(hdrshot_default_save_workflow_tests PRIVATE hdrshot_default_save_workflow)
  target_include_directories(hdrshot_default_save_workflow_tests PRIVATE tests)
  hdrshot_enable_warnings(hdrshot_default_save_workflow_tests)
  add_test(NAME hdrshot_default_save_workflow_tests COMMAND hdrshot_default_save_workflow_tests)

  add_executable(hdrshot_settings_workflow_tests tests/settings_workflow_tests.cpp)
  target_link_libraries(hdrshot_settings_workflow_tests PRIVATE hdrshot_settings_workflow)
  target_include_directories(hdrshot_settings_workflow_tests PRIVATE tests)
  hdrshot_enable_warnings(hdrshot_settings_workflow_tests)
  add_test(NAME hdrshot_settings_workflow_tests COMMAND hdrshot_settings_workflow_tests)

  if(APPLE)
    add_executable(hdrshot_macos_icc_profile_tests tests/macos_icc_profile_tests.mm)
    target_link_libraries(
      hdrshot_macos_icc_profile_tests
      PRIVATE hdrshot_color "-framework CoreFoundation" "-framework CoreGraphics"
    )
    target_include_directories(hdrshot_macos_icc_profile_tests PRIVATE tests)
    set_target_properties(hdrshot_macos_icc_profile_tests PROPERTIES
      OBJCXX_STANDARD 20
      OBJCXX_STANDARD_REQUIRED ON
      OSX_DEPLOYMENT_TARGET 15.5
    )
    hdrshot_enable_warnings(hdrshot_macos_icc_profile_tests)
    add_test(NAME hdrshot_macos_icc_profile_tests COMMAND hdrshot_macos_icc_profile_tests)

    add_executable(hdrshot_macos_export_ports_tests tests/macos_export_ports_tests.cpp)
    target_link_libraries(hdrshot_macos_export_ports_tests PRIVATE hdrshot_macos_export_ports)
    target_include_directories(hdrshot_macos_export_ports_tests PRIVATE tests)
    hdrshot_enable_warnings(hdrshot_macos_export_ports_tests)
    add_test(NAME hdrshot_macos_export_ports_tests COMMAND hdrshot_macos_export_ports_tests)

    add_executable(hdrshot_macos_clipboard_tests tests/macos_clipboard_tests.mm)
    target_link_libraries(hdrshot_macos_clipboard_tests PRIVATE
      hdrshot_macos_export_ports hdrshot_export_workflow hdrshot_ultra_hdr_codec
      hdrshot_ultra_hdr_input ${HDRSHOT_ULTRAHDR_TARGET})
    target_include_directories(hdrshot_macos_clipboard_tests PRIVATE tests)
    target_include_directories(hdrshot_macos_clipboard_tests SYSTEM PRIVATE
      "$<TARGET_PROPERTY:core,INCLUDE_DIRECTORIES>")
    target_compile_options(hdrshot_macos_clipboard_tests PRIVATE -fobjc-arc)
    set_target_properties(hdrshot_macos_clipboard_tests PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON)
    hdrshot_enable_warnings(hdrshot_macos_clipboard_tests)
    add_test(NAME hdrshot_macos_clipboard_tests COMMAND hdrshot_macos_clipboard_tests)
    set_tests_properties(hdrshot_macos_clipboard_tests PROPERTIES LABELS "macos_desktop;clipboard")

    add_executable(
      hdrshot_macos_ultra_hdr_fixture_tests
      tests/macos_ultra_hdr_fixture_tests.mm
    )
    target_link_libraries(
      hdrshot_macos_ultra_hdr_fixture_tests
      PRIVATE
        hdrshot_ultra_hdr_input
        hdrshot_ultra_hdr_codec
        hdrshot_annotation_rasterizer
        ${HDRSHOT_ULTRAHDR_TARGET}
        "-framework CoreGraphics"
        "-framework Foundation"
        "-framework ImageIO"
    )
    target_include_directories(
      hdrshot_macos_ultra_hdr_fixture_tests PRIVATE tests
    )
    target_include_directories(hdrshot_macos_ultra_hdr_fixture_tests SYSTEM PRIVATE
      "$<TARGET_PROPERTY:core,INCLUDE_DIRECTORIES>")
    target_compile_options(
      hdrshot_macos_ultra_hdr_fixture_tests PRIVATE -fobjc-arc
    )
    set_target_properties(
      hdrshot_macos_ultra_hdr_fixture_tests PROPERTIES
      OBJCXX_STANDARD 20
      OBJCXX_STANDARD_REQUIRED ON
      OSX_DEPLOYMENT_TARGET 15.5
    )
    hdrshot_enable_warnings(hdrshot_macos_ultra_hdr_fixture_tests)
    add_test(
      NAME hdrshot_macos_ultra_hdr_fixture_tests
      COMMAND hdrshot_macos_ultra_hdr_fixture_tests
    )
    set_tests_properties(
      hdrshot_macos_ultra_hdr_fixture_tests PROPERTIES SKIP_RETURN_CODE 77
    )

  add_executable(hdrshot_macos_capture_adapter_tests tests/macos_capture_adapter_tests.cpp)
    target_link_libraries(hdrshot_macos_capture_adapter_tests PRIVATE hdrshot_macos_capture_adapter)
    target_include_directories(hdrshot_macos_capture_adapter_tests PRIVATE tests)
    hdrshot_enable_warnings(hdrshot_macos_capture_adapter_tests)
  add_test(NAME hdrshot_macos_capture_adapter_tests COMMAND hdrshot_macos_capture_adapter_tests)

    add_executable(hdrshot_macos_window_catalog_tests tests/macos_window_catalog_tests.cpp)
    target_link_libraries(hdrshot_macos_window_catalog_tests PRIVATE hdrshot_macos_capture_ports)
    target_include_directories(hdrshot_macos_window_catalog_tests PRIVATE tests)
    hdrshot_enable_warnings(hdrshot_macos_window_catalog_tests)
    add_test(NAME hdrshot_macos_window_catalog_tests COMMAND hdrshot_macos_window_catalog_tests)
    set_tests_properties(hdrshot_macos_window_catalog_tests PROPERTIES TIMEOUT 10)

  add_executable(hdrshot_macos_global_hotkey_port_tests tests/macos_global_hotkey_port_tests.cpp)
  target_link_libraries(hdrshot_macos_global_hotkey_port_tests PRIVATE hdrshot_macos_global_hotkey_port)
  target_include_directories(hdrshot_macos_global_hotkey_port_tests PRIVATE tests)
  hdrshot_enable_warnings(hdrshot_macos_global_hotkey_port_tests)
  add_test(NAME hdrshot_macos_global_hotkey_port_tests COMMAND hdrshot_macos_global_hotkey_port_tests)

    add_executable(hdrshot_macos_metal_presenter_tests tests/macos_metal_presenter_tests.cpp)
    target_link_libraries(hdrshot_macos_metal_presenter_tests PRIVATE hdrshot_macos_metal_presenter)
    target_include_directories(hdrshot_macos_metal_presenter_tests PRIVATE tests)
    hdrshot_enable_warnings(hdrshot_macos_metal_presenter_tests)
    add_test(NAME hdrshot_macos_metal_presenter_tests COMMAND hdrshot_macos_metal_presenter_tests)
    set_tests_properties(hdrshot_macos_metal_presenter_tests PROPERTIES SKIP_RETURN_CODE 77)

    add_executable(hdrshot_macos_export_benchmark tools/macos_export_benchmark.mm)
    target_link_libraries(hdrshot_macos_export_benchmark PRIVATE
      hdrshot_macos_export_pixel_processor hdrshot_ultra_hdr_codec hdrshot_png)
    target_compile_options(hdrshot_macos_export_benchmark PRIVATE -fobjc-arc)
    hdrshot_enable_warnings(hdrshot_macos_export_benchmark)

    add_executable(hdrshot_macos_imageio_png_benchmark tools/macos_imageio_png_benchmark.mm)
    target_link_libraries(hdrshot_macos_imageio_png_benchmark PRIVATE
      hdrshot_png hdrshot_color "-framework Foundation" "-framework ImageIO" "-framework CoreGraphics")
    target_compile_options(hdrshot_macos_imageio_png_benchmark PRIVATE -fobjc-arc)
    hdrshot_enable_warnings(hdrshot_macos_imageio_png_benchmark)
    add_test(NAME hdrshot_macos_imageio_png_contract COMMAND hdrshot_macos_imageio_png_benchmark)
    set_tests_properties(hdrshot_macos_imageio_png_contract PROPERTIES TIMEOUT 60)

    add_executable(
      hdrshot_macos_metal_export_pixel_processor_tests
      tests/macos_metal_export_pixel_processor_tests.cpp
    )
    target_link_libraries(
      hdrshot_macos_metal_export_pixel_processor_tests
      PRIVATE
        hdrshot_macos_export_pixel_processor
        hdrshot_output_policy
        hdrshot_ultra_hdr_input
    )
    target_include_directories(
      hdrshot_macos_metal_export_pixel_processor_tests PRIVATE tests
    )
    hdrshot_enable_warnings(hdrshot_macos_metal_export_pixel_processor_tests)
    add_test(
      NAME hdrshot_macos_metal_export_pixel_processor_tests
      COMMAND hdrshot_macos_metal_export_pixel_processor_tests
    )
    set_tests_properties(
      hdrshot_macos_metal_export_pixel_processor_tests
      PROPERTIES SKIP_RETURN_CODE 77
    )

    add_executable(
      hdrshot_macos_preview_presenter_port_tests
      tests/macos_preview_presenter_port_tests.cpp
    )
    target_link_libraries(
      hdrshot_macos_preview_presenter_port_tests
      PRIVATE hdrshot_macos_preview_presenter_port
    )
    target_include_directories(hdrshot_macos_preview_presenter_port_tests PRIVATE tests)
    hdrshot_enable_warnings(hdrshot_macos_preview_presenter_port_tests)
    add_test(
      NAME hdrshot_macos_preview_presenter_port_tests
      COMMAND hdrshot_macos_preview_presenter_port_tests
    )
  endif()
endif()

if(WIN32)
  include(cmake/windows.cmake)
endif()

if(BUILD_TESTING)
  add_executable(hdrshot_clean_content_tests tests/clean_content_tests.cpp)
  target_link_libraries(hdrshot_clean_content_tests PRIVATE hdrshot_export_workflow hdrshot_frame_pipeline hdrshot_ultra_hdr_input)
  target_include_directories(hdrshot_clean_content_tests PRIVATE tests)
  if(APPLE)
    target_link_libraries(hdrshot_clean_content_tests PRIVATE hdrshot_macos_export_pixel_processor hdrshot_macos_metal_presenter)
  endif()
  add_test(NAME hdrshot_clean_content_tests COMMAND hdrshot_clean_content_tests)
endif()

if(APPLE)
  add_executable(hdrshot_clean_content_benchmark tools/clean_content_benchmark.mm)
  add_executable(hdrshot_macos_gpu_source_benchmark tools/macos_gpu_source_benchmark.mm)
  if(BUILD_TESTING)
    add_executable(hdrshot_macos_gpu_source_tests tests/macos_gpu_source_tests.mm)
    target_compile_options(hdrshot_macos_gpu_source_tests PRIVATE -fobjc-arc)
    hdrshot_enable_warnings(hdrshot_macos_gpu_source_tests)
    target_include_directories(hdrshot_macos_gpu_source_tests PRIVATE tests)
    target_link_libraries(hdrshot_macos_gpu_source_tests PRIVATE hdrshot_export_workflow
      hdrshot_frame_pipeline hdrshot_ultra_hdr_input hdrshot_macos_export_pixel_processor
      hdrshot_macos_metal_presenter)
    add_test(NAME hdrshot_macos_gpu_source_tests COMMAND hdrshot_macos_gpu_source_tests)
    set_tests_properties(hdrshot_macos_gpu_source_tests PROPERTIES SKIP_RETURN_CODE 77)
  endif()
  target_compile_options(hdrshot_macos_gpu_source_benchmark PRIVATE -fobjc-arc)
  target_link_libraries(hdrshot_macos_gpu_source_benchmark PRIVATE hdrshot_export_workflow
      hdrshot_frame_pipeline hdrshot_macos_export_pixel_processor hdrshot_macos_metal_presenter)
  target_compile_options(hdrshot_clean_content_benchmark PRIVATE -fobjc-arc)
  target_link_libraries(hdrshot_clean_content_benchmark PRIVATE hdrshot_export_workflow hdrshot_frame_pipeline
      hdrshot_macos_export_pixel_processor hdrshot_macos_metal_presenter "-framework Foundation")
  if(HDRSHOT_T21_LEGACY_DIR)
    target_sources(hdrshot_clean_content_benchmark PRIVATE "${HDRSHOT_T21_LEGACY_DIR}/legacy_metal.mm")
    target_include_directories(hdrshot_clean_content_benchmark PRIVATE "${HDRSHOT_T21_LEGACY_DIR}")
    target_compile_definitions(hdrshot_clean_content_benchmark PRIVATE HDRSHOT_T21_BASELINE=1)
  endif()
endif()

if(APPLE)
  target_link_libraries(hdrshot_macos_capture_ports PUBLIC hdrshot_macos_linear_storage hdrshot_macos_gpu_source)
  target_link_libraries(hdrshot_clean_content_benchmark PRIVATE hdrshot_macos_linear_storage)
  if(BUILD_TESTING)
    target_link_libraries(hdrshot_clean_content_tests PRIVATE hdrshot_macos_linear_storage)
  endif()
endif()

if(APPLE)
  target_link_libraries(hdrshot_macos_metal_presenter PUBLIC hdrshot_macos_linear_storage hdrshot_macos_gpu_source)
  target_link_libraries(hdrshot_macos_export_pixel_processor PUBLIC hdrshot_macos_linear_storage hdrshot_macos_gpu_source)
endif()
