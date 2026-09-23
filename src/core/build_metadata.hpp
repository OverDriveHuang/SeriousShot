#pragma once

#include <string_view>

namespace hdrshot {

// Immutable identity of this package's code, not its compilation or upload time.
// Empty commit/time means provenance is unavailable, never "built just now".
[[nodiscard]] std::string_view product_version() noexcept;
[[nodiscard]] std::string_view source_commit() noexcept;
[[nodiscard]] std::string_view source_commit_timestamp() noexcept;
[[nodiscard]] bool source_modified() noexcept;

// Compiled together, shared by Settings and all export formats/backends.
[[nodiscard]] std::string_view build_timestamp() noexcept;
[[nodiscard]] std::string_view software_identifier() noexcept;

}  // namespace hdrshot
