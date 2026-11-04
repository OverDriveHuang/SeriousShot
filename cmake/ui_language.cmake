# Use one release invocation for all executables and reuse the same two QMs.
set(_hdrshot_ui_consumers)
foreach(_target IN ITEMS
    hdrshot_macos_qt_overlay_poc
    hdrshot_windows_app
    hdrshot_ui_language_tests
    hdrshot_application_restart_tests
    hdrshot_qt_settings_store_tests
    hdrshot_qt_capture_runtime_tests
    hdrshot_capture_error_message_tests
    hdrshot_analyzer_ui_tests
    hdrshot_analyzer_scope_regression_tests
    hdrshot_update_runtime_tests
    hdrshot_settings_window_tests
    hdrshot_overlay_editor_widget_tests
    hdrshot_qt_overlay_host_tests
    hdrshot_pinned_screenshot_tests
    hdrshot_save_success_feedback_tests
    hdrshot_macos_pinned_screenshot_tests
    hdrshot_macos_analyzer_integration_tests
    hdrshot_analyzer_ui_native_surface_tests
    hdrshot_macos_qt_overlay_window_tests
    hdrshot_macos_qt_input_platform_adapter_tests
    hdrshot_windows_capture_acceptance_tests
    hdrshot_windows_pinned_screenshot_tests
    hdrshot_windows_overlay_session_tests
    hdrshot_windows_qt_ports_tests)
  if(TARGET "${_target}")
    list(APPEND _hdrshot_ui_consumers "${_target}")
    target_link_libraries("${_target}" PRIVATE hdrshot_qt_ui_language)
    # In Makefile builds, finish the shared outputs before any consumer's rcc.
    add_dependencies("${_target}" hdrshot_i18n_lrelease)
  endif()
endforeach()

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ui_language_sources.txt" _hdrshot_ui_sources
  REGEX "^(src|tools)/")
list(REMOVE_ITEM _hdrshot_ui_sources src/ui/qt/ui_language_runtime.cpp)
list(TRANSFORM _hdrshot_ui_sources PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")

# Look in the same Qt SDK translation directory as MERGE_QT_TRANSLATIONS.
set(_hdrshot_qt_translations "${QT_INTERNAL_ABSOLUTE_TRANSLATIONS_DIR}")
if(NOT _hdrshot_qt_translations)
  set(_hdrshot_qt_translations "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_TRANSLATIONS}")
endif()
set(_hdrshot_ui_ts)
foreach(_locale IN ITEMS en zh_CN)
  set(_ts "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/qt/i18n/seriousshot_${_locale}.ts")
  if(NOT EXISTS "${_ts}")
    message(FATAL_ERROR "Missing SeriousShot source catalog: ${_ts}")
  endif()
  if(NOT EXISTS "${_hdrshot_qt_translations}/qtbase_${_locale}.qm")
    message(FATAL_ERROR
      "Missing Qt standard catalog qtbase_${_locale}.qm under ${_hdrshot_qt_translations}; "
      "install Qt Translations for the configured Qt SDK.")
  endif()
  list(APPEND _hdrshot_ui_ts "${_ts}")
endforeach()

if(_hdrshot_ui_consumers)
  qt_add_translations(
    TARGETS ${_hdrshot_ui_consumers}
    SOURCE_TARGETS hdrshot_qt_ui_language
    SOURCES ${_hdrshot_ui_sources}
    INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/src"
    TS_FILES ${_hdrshot_ui_ts}
    NO_GENERATE_PLURALS_TS_FILE
    RESOURCE_PREFIX /seriousshot/i18n
    MERGE_QT_TRANSLATIONS
    QT_TRANSLATION_CATALOGS qtbase
    LUPDATE_TARGET hdrshot_i18n_lupdate
    LUPDATE_OPTIONS -no-obsolete -locations none -source-language zh_CN
    LRELEASE_TARGET hdrshot_i18n_lrelease
    LRELEASE_OPTIONS -fail-on-unfinished)
endif()

if(BUILD_TESTING)
  add_test(NAME hdrshot_ui_catalog_check
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-ui-catalogs.py"
      --lupdate "$<TARGET_FILE:Qt6::lupdate>")
  foreach(_target IN ITEMS hdrshot_capture_error_message_tests hdrshot_analyzer_ui_tests
      hdrshot_update_runtime_tests)
    if(TARGET "${_target}")
      add_test(NAME "${_target}_english" COMMAND "${_target}" --english)
      set_tests_properties("${_target}_english" PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 90)
    endif()
  endforeach()
endif()
