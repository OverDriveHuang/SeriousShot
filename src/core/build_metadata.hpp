#pragma once

#include <string_view>

namespace hdrshot {

// Compiled together, shared by Settings and all export formats/backends.
[[nodiscard]] std::string_view build_timestamp() noexcept;
[[nodiscard]] std::string_view software_identifier() noexcept;

}  // namespace hdrshot
