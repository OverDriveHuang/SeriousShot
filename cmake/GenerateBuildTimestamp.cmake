if(NOT DEFINED OUTPUT_FILE OR OUTPUT_FILE STREQUAL "")
  message(FATAL_ERROR "OUTPUT_FILE is required")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/SourceIdentity.cmake")
hdrshot_source_identity("${SOURCE_DIR}")
if(REQUIRE_CLEAN_SOURCE AND (HDRSHOT_SOURCE_COMMIT STREQUAL "" OR HDRSHOT_SOURCE_MODIFIED))
  message(FATAL_ERROR "Release requires known, committed SeriousShot source inputs")
endif()
if(NOT PRODUCT_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "PRODUCT_VERSION must be the numeric project version")
endif()

get_filename_component(output_directory "${OUTPUT_FILE}" DIRECTORY)
file(MAKE_DIRECTORY "${output_directory}")
string(TIMESTAMP hdrshot_build_timestamp "%Y-%m-%d %H:%M:%S")
file(WRITE "${OUTPUT_FILE}"
    "#include \"core/build_metadata.hpp\"\n\n"
    "namespace hdrshot {\n"
    "std::string_view product_version() noexcept { return \"${PRODUCT_VERSION}\"; }\n"
    "std::string_view source_commit() noexcept { return \"${HDRSHOT_SOURCE_COMMIT}\"; }\n"
    "std::string_view source_commit_timestamp() noexcept { return \"${HDRSHOT_SOURCE_COMMIT_TIME}\"; }\n"
    "bool source_modified() noexcept { return ${HDRSHOT_SOURCE_MODIFIED}; }\n"
    "std::string_view build_timestamp() noexcept {\n"
    "  return \"${hdrshot_build_timestamp}\";\n"
    "}\n"
    "std::string_view software_identifier() noexcept {\n"
    "  return \"SeriousShot; build ${hdrshot_build_timestamp}\";\n"
    "}\n"
    "}  // namespace hdrshot\n")
