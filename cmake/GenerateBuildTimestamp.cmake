if(NOT DEFINED OUTPUT_FILE OR OUTPUT_FILE STREQUAL "")
  message(FATAL_ERROR "OUTPUT_FILE is required")
endif()

get_filename_component(output_directory "${OUTPUT_FILE}" DIRECTORY)
file(MAKE_DIRECTORY "${output_directory}")
string(TIMESTAMP hdrshot_build_timestamp "%Y-%m-%d %H:%M:%S")
file(WRITE "${OUTPUT_FILE}"
    "#include \"core/build_metadata.hpp\"\n\n"
    "namespace hdrshot {\n"
    "std::string_view build_timestamp() noexcept {\n"
    "  return \"${hdrshot_build_timestamp}\";\n"
    "}\n"
    "std::string_view software_identifier() noexcept {\n"
    "  return \"SeriousShot; build ${hdrshot_build_timestamp}\";\n"
    "}\n"
    "}  // namespace hdrshot\n")
