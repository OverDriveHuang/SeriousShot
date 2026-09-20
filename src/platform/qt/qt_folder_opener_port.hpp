#pragma once

#include "ports/system_shell_port.hpp"

namespace hdrshot {

class QtFolderOpenerPort final : public FolderOpenerPort {
 public:
  [[nodiscard]] Result<OpenFolderReceipt, Error> open_folder(
      const OpenFolderRequest& request) override;
};

}  // namespace hdrshot
