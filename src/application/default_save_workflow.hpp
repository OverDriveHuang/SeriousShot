#pragma once

#include "core/error.hpp"
#include "core/result.hpp"
#include "ports/export_ports.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace hdrshot {

struct DefaultSaveRequest {
  std::span<const std::uint8_t> encoded_bytes;
  std::string default_folder;
  std::size_t maximum_collision_attempts{10000};
  SaveFormat save_format{SaveFormat::png_display_p3_dual_range};
};

class FilenamePolicy {
 public:
  [[nodiscard]] static Result<std::string, Error> make_png_name(
      const LocalDateTime& timestamp,
      std::size_t collision_index);
  [[nodiscard]] static Result<std::string, Error> make_name(
      const LocalDateTime& timestamp,
      std::size_t collision_index,
      SaveFormat save_format);
  [[nodiscard]] static Result<std::string, Error> join_folder(
      const std::string& folder,
      const std::string& file_name);
};

class DefaultSaveWorkflow {
 public:
  [[nodiscard]] static Result<FileReceipt, Error> save(
      const DefaultSaveRequest& request,
      const ClockPort& clock,
      FileStorePort& file_store);
};

}  // namespace hdrshot
