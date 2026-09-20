#pragma once

#include "ports/export_ports.hpp"

#include <string>

namespace hdrshot {

class QtSettingsStorePort final : public SettingsStorePort {
 public:
  QtSettingsStorePort(
      std::string exact_path,
      std::string default_hotkey,
      std::string default_save_folder);

  [[nodiscard]] Result<SettingsSnapshot, Error> load() override;
  [[nodiscard]] Result<SettingsReceipt, Error> save(const SettingsPatch& patch) override;

 private:
  std::string exact_path_;
  std::string default_hotkey_;
  std::string default_save_folder_;
};

}  // namespace hdrshot
