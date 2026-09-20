#pragma once

#include <cstddef>
#include <string>

namespace hdrshot {

struct RangeFitResult {
  bool fits_sdr{};
  std::size_t source_visible_pixel_count{};
  std::size_t skipped_annotation_pixel_count{};
  std::size_t scanned_component_count{};
  std::string provenance;

  friend bool operator==(const RangeFitResult&, const RangeFitResult&) = default;
};

}  // namespace hdrshot
