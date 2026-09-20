#include "platform/macos/macos_global_hotkey_port.hpp"

#import <Carbon/Carbon.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace hdrshot {
namespace {

constexpr OSType kHdrShotHotkeySignature = 'HDSH';

Error hotkey_error(
    const ErrorCode code,
    const Retryability retryability,
    std::map<std::string, std::string> context = {}) {
  return Error{code, "MacGlobalHotkeyPort", retryability, std::move(context)};
}

Error status_error(const OSStatus status, const char* operation) {
  const auto code = status == eventHotKeyExistsErr
      ? ErrorCode::hotkey_conflict
      : ErrorCode::state_inconsistent;
  return hotkey_error(
      code,
      status == eventHotKeyExistsErr
          ? Retryability::after_user_action
          : Retryability::after_recreate,
      {{"operation", operation}, {"osStatus", std::to_string(status)}});
}

std::vector<std::string> split_tokens(const std::string& value) {
  std::vector<std::string> tokens;
  std::stringstream stream(value);
  std::string token;
  while (std::getline(stream, token, '+')) {
    if (!token.empty()) {
      tokens.push_back(std::move(token));
    }
  }
  return tokens;
}

std::optional<std::uint32_t> key_code(const char character) {
  switch (character) {
    case 'A': return kVK_ANSI_A;
    case 'B': return kVK_ANSI_B;
    case 'C': return kVK_ANSI_C;
    case 'D': return kVK_ANSI_D;
    case 'E': return kVK_ANSI_E;
    case 'F': return kVK_ANSI_F;
    case 'G': return kVK_ANSI_G;
    case 'H': return kVK_ANSI_H;
    case 'I': return kVK_ANSI_I;
    case 'J': return kVK_ANSI_J;
    case 'K': return kVK_ANSI_K;
    case 'L': return kVK_ANSI_L;
    case 'M': return kVK_ANSI_M;
    case 'N': return kVK_ANSI_N;
    case 'O': return kVK_ANSI_O;
    case 'P': return kVK_ANSI_P;
    case 'Q': return kVK_ANSI_Q;
    case 'R': return kVK_ANSI_R;
    case 'S': return kVK_ANSI_S;
    case 'T': return kVK_ANSI_T;
    case 'U': return kVK_ANSI_U;
    case 'V': return kVK_ANSI_V;
    case 'W': return kVK_ANSI_W;
    case 'X': return kVK_ANSI_X;
    case 'Y': return kVK_ANSI_Y;
    case 'Z': return kVK_ANSI_Z;
    case '0': return kVK_ANSI_0;
    case '1': return kVK_ANSI_1;
    case '2': return kVK_ANSI_2;
    case '3': return kVK_ANSI_3;
    case '4': return kVK_ANSI_4;
    case '5': return kVK_ANSI_5;
    case '6': return kVK_ANSI_6;
    case '7': return kVK_ANSI_7;
    case '8': return kVK_ANSI_8;
    case '9': return kVK_ANSI_9;
    default: return std::nullopt;
  }
}

}  // namespace

struct MacGlobalHotkeyPort::Impl {
  std::mutex mutex;
  EventHandlerRef event_handler{};
  EventHotKeyRef active_hotkey{};
  std::uint32_t active_id{};
  std::uint32_t next_id{1};
  std::string active_name;
  HotkeyTriggerHandler trigger_handler;

  static OSStatus handle_event(
      EventHandlerCallRef,
      EventRef event,
      void* user_data) {
    auto* self = static_cast<Impl*>(user_data);
    EventHotKeyID hotkey_id{};
    const auto status = GetEventParameter(
        event,
        kEventParamDirectObject,
        typeEventHotKeyID,
        nullptr,
        sizeof(hotkey_id),
        nullptr,
        &hotkey_id);
    if (status != noErr || hotkey_id.signature != kHdrShotHotkeySignature) {
      return eventNotHandledErr;
    }

    HotkeyTriggerHandler callback;
    {
      const std::scoped_lock lock(self->mutex);
      if (hotkey_id.id != self->active_id) {
        return eventNotHandledErr;
      }
      callback = self->trigger_handler;
    }
    if (callback) {
      callback();
    }
    return noErr;
  }

  Result<bool, Error> ensure_event_handler() {
    if (event_handler != nullptr) {
      return Result<bool, Error>::success(true);
    }
    const EventTypeSpec event_type{kEventClassKeyboard, kEventHotKeyPressed};
    const auto status = InstallEventHandler(
        GetApplicationEventTarget(),
        &Impl::handle_event,
        1,
        &event_type,
        this,
        &event_handler);
    if (status != noErr) {
      return Result<bool, Error>::failure(status_error(status, "install_event_handler"));
    }
    return Result<bool, Error>::success(true);
  }

  Result<std::pair<EventHotKeyRef, std::uint32_t>, Error> register_native(
      const MacHotkeyDescriptor descriptor) {
    const auto handler = ensure_event_handler();
    if (!handler) {
      return Result<std::pair<EventHotKeyRef, std::uint32_t>, Error>::failure(handler.error());
    }
    const auto id = next_id++;
    const EventHotKeyID hotkey_id{kHdrShotHotkeySignature, id};
    EventHotKeyRef hotkey{};
    const auto status = RegisterEventHotKey(
        descriptor.carbon_key_code,
        descriptor.carbon_modifiers,
        hotkey_id,
        GetApplicationEventTarget(),
        0,
        &hotkey);
    if (status != noErr || hotkey == nullptr) {
      return Result<std::pair<EventHotKeyRef, std::uint32_t>, Error>::failure(
          status_error(status, "register"));
    }
    return Result<std::pair<EventHotKeyRef, std::uint32_t>, Error>::success({hotkey, id});
  }
};

MacGlobalHotkeyPort::MacGlobalHotkeyPort() : impl_(std::make_unique<Impl>()) {}

MacGlobalHotkeyPort::~MacGlobalHotkeyPort() {
  if (impl_->active_hotkey != nullptr) {
    UnregisterEventHotKey(impl_->active_hotkey);
  }
  if (impl_->event_handler != nullptr) {
    RemoveEventHandler(impl_->event_handler);
  }
}

void MacGlobalHotkeyPort::set_trigger_handler(HotkeyTriggerHandler handler) {
  const std::scoped_lock lock(impl_->mutex);
  impl_->trigger_handler = std::move(handler);
}

Result<HotkeyReceipt, Error> MacGlobalHotkeyPort::register_hotkey(std::string hotkey) {
  const auto parsed = parse_hotkey(hotkey);
  if (!parsed) {
    return Result<HotkeyReceipt, Error>::failure(parsed.error());
  }
  const std::scoped_lock lock(impl_->mutex);
  if (impl_->active_hotkey != nullptr) {
    return Result<HotkeyReceipt, Error>::failure(hotkey_error(
        ErrorCode::precondition_failed,
        Retryability::never,
        {{"reason", "hotkey_already_registered"}}));
  }
  const auto registered = impl_->register_native(parsed.value());
  if (!registered) {
    return Result<HotkeyReceipt, Error>::failure(registered.error());
  }
  impl_->active_hotkey = registered.value().first;
  impl_->active_id = registered.value().second;
  impl_->active_name = std::move(hotkey);
  return Result<HotkeyReceipt, Error>::success(HotkeyReceipt{impl_->active_name});
}

Result<HotkeyReceipt, Error> MacGlobalHotkeyPort::replace_hotkey(std::string hotkey) {
  const auto parsed = parse_hotkey(hotkey);
  if (!parsed) {
    return Result<HotkeyReceipt, Error>::failure(parsed.error());
  }
  const std::scoped_lock lock(impl_->mutex);
  if (impl_->active_hotkey == nullptr) {
    return Result<HotkeyReceipt, Error>::failure(hotkey_error(
        ErrorCode::precondition_failed,
        Retryability::never,
        {{"reason", "no_registered_hotkey"}}));
  }
  const auto replacement = impl_->register_native(parsed.value());
  if (!replacement) {
    return Result<HotkeyReceipt, Error>::failure(replacement.error());
  }
  const auto status = UnregisterEventHotKey(impl_->active_hotkey);
  if (status != noErr) {
    UnregisterEventHotKey(replacement.value().first);
    return Result<HotkeyReceipt, Error>::failure(status_error(status, "replace_unregister_old"));
  }
  impl_->active_hotkey = replacement.value().first;
  impl_->active_id = replacement.value().second;
  impl_->active_name = std::move(hotkey);
  return Result<HotkeyReceipt, Error>::success(HotkeyReceipt{impl_->active_name});
}

Result<HotkeyReceipt, Error> MacGlobalHotkeyPort::unregister_hotkey() {
  const std::scoped_lock lock(impl_->mutex);
  if (impl_->active_hotkey == nullptr) {
    return Result<HotkeyReceipt, Error>::failure(hotkey_error(
        ErrorCode::precondition_failed,
        Retryability::never,
        {{"reason", "no_registered_hotkey"}}));
  }
  const auto status = UnregisterEventHotKey(impl_->active_hotkey);
  if (status != noErr) {
    return Result<HotkeyReceipt, Error>::failure(status_error(status, "unregister"));
  }
  impl_->active_hotkey = nullptr;
  impl_->active_id = 0;
  impl_->active_name.clear();
  return Result<HotkeyReceipt, Error>::success(HotkeyReceipt{});
}

Result<MacHotkeyDescriptor, Error> MacGlobalHotkeyPort::parse_hotkey(
    const std::string& hotkey) {
  const auto tokens = split_tokens(hotkey);
  if (tokens.size() < 2) {
    return Result<MacHotkeyDescriptor, Error>::failure(hotkey_error(
        ErrorCode::invalid_input,
        Retryability::never,
        {{"reason", "modifier_and_key_required"}}));
  }

  std::uint32_t modifiers = 0;
  std::optional<std::uint32_t> parsed_key;
  for (const auto& token : tokens) {
    if (token == "Command") {
      if ((modifiers & cmdKey) != 0) {
        parsed_key.reset();
        break;
      }
      modifiers |= cmdKey;
    } else if (token == "Shift") {
      if ((modifiers & shiftKey) != 0) {
        parsed_key.reset();
        break;
      }
      modifiers |= shiftKey;
    } else if (token == "Option") {
      if ((modifiers & optionKey) != 0) {
        parsed_key.reset();
        break;
      }
      modifiers |= optionKey;
    } else if (token == "Control") {
      if ((modifiers & controlKey) != 0) {
        parsed_key.reset();
        break;
      }
      modifiers |= controlKey;
    } else if (token.size() == 1 && !parsed_key.has_value()) {
      parsed_key = key_code(static_cast<char>(
          std::toupper(static_cast<unsigned char>(token.front()))));
    } else {
      parsed_key.reset();
      break;
    }
  }
  if (modifiers == 0 || !parsed_key.has_value()) {
    return Result<MacHotkeyDescriptor, Error>::failure(hotkey_error(
        ErrorCode::invalid_input,
        Retryability::never,
        {{"reason", "unsupported_hotkey"}}));
  }
  return Result<MacHotkeyDescriptor, Error>::success(
      MacHotkeyDescriptor{modifiers, *parsed_key});
}

}  // namespace hdrshot
