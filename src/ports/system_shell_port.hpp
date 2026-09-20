#pragma once

#include "core/error.hpp"
#include "core/result.hpp"

#include <string>

namespace hdrshot {

struct OpenFolderRequest {
  std::string exact_path;
  bool create_if_missing{true};
};

struct OpenFolderReceipt {
  std::string exact_path;

  friend bool operator==(const OpenFolderReceipt&, const OpenFolderReceipt&) = default;
};

class FolderOpenerPort {
 public:
  virtual ~FolderOpenerPort() = default;
  [[nodiscard]] virtual Result<OpenFolderReceipt, Error> open_folder(
      const OpenFolderRequest& request) = 0;
};

}  // namespace hdrshot
