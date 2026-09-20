#pragma once

#include "ports/export_ports.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace hdrshot {

struct MacHotkeyDescriptor {
  std::uint32_t carbon_modifiers{};
  std::uint32_t carbon_key_code{};

  friend bool operator==(const MacHotkeyDescriptor&, const MacHotkeyDescriptor&) = default;
};

class MacGlobalHotkeyPort final : public GlobalHotkeyPort {
 public:
  MacGlobalHotkeyPort();
  ~MacGlobalHotkeyPort() override;

  MacGlobalHotkeyPort(const MacGlobalHotkeyPort&) = delete;
  MacGlobalHotkeyPort& operator=(const MacGlobalHotkeyPort&) = delete;

  void set_trigger_handler(HotkeyTriggerHandler handler) override;
  [[nodiscard]] Result<HotkeyReceipt, Error> register_hotkey(std::string hotkey) override;
  [[nodiscard]] Result<HotkeyReceipt, Error> replace_hotkey(std::string hotkey) override;
  [[nodiscard]] Result<HotkeyReceipt, Error> unregister_hotkey() override;

  [[nodiscard]] static Result<MacHotkeyDescriptor, Error> parse_hotkey(
      const std::string& hotkey);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace hdrshot
