#include "ports/export_ports.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

using hdrshot::ChooseSavePathOutcome;
using hdrshot::ChooseSavePathRequest;
using hdrshot::ChosenPath;
using hdrshot::ClipboardPort;
using hdrshot::ClipboardReceipt;
using hdrshot::ClipboardWriteRequest;
using hdrshot::ClockPort;
using hdrshot::Error;
using hdrshot::ErrorCode;
using hdrshot::FileDialogPort;
using hdrshot::FileReceipt;
using hdrshot::FileStorePort;
using hdrshot::GlobalHotkeyPort;
using hdrshot::HotkeyReceipt;
using hdrshot::LocalDateTime;
using hdrshot::Result;
using hdrshot::Retryability;
using hdrshot::SettingsPatch;
using hdrshot::SettingsReceipt;
using hdrshot::SettingsSnapshot;
using hdrshot::SettingsStorePort;
using hdrshot::UserCancelled;
using hdrshot::WriteFileRequest;

Error fake_error(const ErrorCode code, std::string module, const Retryability retryability) {
  return Error{code, std::move(module), retryability, {}};
}

class FakeClipboard final : public ClipboardPort {
 public:
  bool reject{};
  std::vector<std::uint8_t> last_bytes;
  std::vector<std::string> last_mime_types;

  Result<ClipboardReceipt, Error> write(const ClipboardWriteRequest& request) override {
    if (reject) {
      return Result<ClipboardReceipt, Error>::failure(
          fake_error(ErrorCode::clipboard_rejected, "FakeClipboard", Retryability::same_input));
    }
    last_bytes.assign(request.bytes.begin(), request.bytes.end());
    last_mime_types = request.mime_types;
    return Result<ClipboardReceipt, Error>::success(
        ClipboardReceipt{last_bytes.size(), last_mime_types});
  }
};

class FakeFileStore final : public FileStorePort {
 public:
  std::optional<ErrorCode> injected_failure;
  std::map<std::string, std::vector<std::uint8_t>> files;

  Result<FileReceipt, Error> write(const WriteFileRequest& request) override {
    if (injected_failure.has_value()) {
      const auto code = *injected_failure;
      return Result<FileReceipt, Error>::failure(
          fake_error(code, "FakeFileStore", Retryability::after_user_action));
    }
    const auto existing = files.find(request.exact_path);
    if (existing != files.end() && !request.overwrite) {
      return Result<FileReceipt, Error>::failure(fake_error(
          ErrorCode::path_already_exists, "FakeFileStore", Retryability::same_input));
    }
    files[request.exact_path] = std::vector<std::uint8_t>(request.bytes.begin(), request.bytes.end());
    return Result<FileReceipt, Error>::success(
        FileReceipt{request.exact_path, request.bytes.size()});
  }
};

class FakeFileDialog final : public FileDialogPort {
 public:
  ChooseSavePathOutcome outcome{UserCancelled{}};
  bool fail{};
  std::optional<ChooseSavePathRequest> last_request;

  Result<ChooseSavePathOutcome, Error> choose_save_path(
      const ChooseSavePathRequest& request) override {
    last_request = request;
    if (fail) {
      return Result<ChooseSavePathOutcome, Error>::failure(
          fake_error(ErrorCode::dialog_failed, "FakeFileDialog", Retryability::after_user_action));
    }
    return Result<ChooseSavePathOutcome, Error>::success(outcome);
  }
};

class FakeSettingsStore final : public SettingsStorePort {
 public:
  SettingsSnapshot snapshot{1, 0, "Ctrl+Shift+S", "/Pictures"};
  bool corrupt{};

  Result<SettingsSnapshot, Error> load() override {
    if (corrupt) {
      return Result<SettingsSnapshot, Error>::failure(
          fake_error(ErrorCode::settings_corrupt, "FakeSettingsStore", Retryability::after_user_action));
    }
    return Result<SettingsSnapshot, Error>::success(snapshot);
  }

  Result<SettingsReceipt, Error> save(const SettingsPatch& patch) override {
    if (corrupt) {
      return Result<SettingsReceipt, Error>::failure(
          fake_error(ErrorCode::settings_corrupt, "FakeSettingsStore", Retryability::after_user_action));
    }
    if (patch.global_capture_hotkey.has_value()) {
      snapshot.global_capture_hotkey = *patch.global_capture_hotkey;
    }
    if (patch.default_save_folder.has_value()) {
      snapshot.default_save_folder = *patch.default_save_folder;
    }
    ++snapshot.revision;
    return Result<SettingsReceipt, Error>::success(SettingsReceipt{snapshot.revision});
  }
};

class FixedClock final : public ClockPort {
 public:
  explicit FixedClock(const LocalDateTime value) : value_(value) {}
  LocalDateTime now_local() const override { return value_; }

 private:
  LocalDateTime value_;
};

class FakeGlobalHotkey final : public GlobalHotkeyPort {
 public:
  std::optional<std::string> active;
  std::string conflicting_hotkey;

  void set_trigger_handler(hdrshot::HotkeyTriggerHandler handler) override {
    trigger_handler = std::move(handler);
  }

  Result<HotkeyReceipt, Error> register_hotkey(std::string hotkey) override {
    if (hotkey == conflicting_hotkey) {
      return conflict();
    }
    if (active.has_value()) {
      return Result<HotkeyReceipt, Error>::failure(
          fake_error(ErrorCode::precondition_failed, "FakeGlobalHotkey", Retryability::never));
    }
    active = std::move(hotkey);
    return Result<HotkeyReceipt, Error>::success(HotkeyReceipt{*active});
  }

  Result<HotkeyReceipt, Error> replace_hotkey(std::string hotkey) override {
    if (!active.has_value()) {
      return Result<HotkeyReceipt, Error>::failure(
          fake_error(ErrorCode::precondition_failed, "FakeGlobalHotkey", Retryability::never));
    }
    if (hotkey == conflicting_hotkey) {
      return conflict();
    }
    active = std::move(hotkey);
    return Result<HotkeyReceipt, Error>::success(HotkeyReceipt{*active});
  }

  Result<HotkeyReceipt, Error> unregister_hotkey() override {
    if (!active.has_value()) {
      return Result<HotkeyReceipt, Error>::failure(
          fake_error(ErrorCode::precondition_failed, "FakeGlobalHotkey", Retryability::never));
    }
    active.reset();
    return Result<HotkeyReceipt, Error>::success(HotkeyReceipt{});
  }

  hdrshot::HotkeyTriggerHandler trigger_handler;

 private:
  static Result<HotkeyReceipt, Error> conflict() {
    return Result<HotkeyReceipt, Error>::failure(
        fake_error(ErrorCode::hotkey_conflict, "FakeGlobalHotkey", Retryability::after_user_action));
  }
};

void clipboard_contract() {
  FakeClipboard port;
  const std::vector<std::uint8_t> bytes{1, 2, 3, 4};
  const auto receipt = port.write(ClipboardWriteRequest{bytes, {"image/png"}});
  HDRSHOT_CHECK(receipt.has_value());
  HDRSHOT_CHECK(receipt.value().bytes_written == bytes.size());
  HDRSHOT_CHECK(port.last_bytes == bytes);
  HDRSHOT_CHECK(port.last_mime_types == std::vector<std::string>{"image/png"});

  port.reject = true;
  const auto rejected = port.write(ClipboardWriteRequest{bytes, {"image/png"}});
  HDRSHOT_CHECK(!rejected.has_value());
  HDRSHOT_CHECK(rejected.error().code == ErrorCode::clipboard_rejected);
}

void file_store_create_new_contract() {
  FakeFileStore port;
  const std::vector<std::uint8_t> first{1, 2};
  const std::vector<std::uint8_t> second{9, 9};
  const auto created = port.write(WriteFileRequest{first, "/Pictures/capture.png", false});
  HDRSHOT_CHECK(created.has_value());

  const auto collision = port.write(WriteFileRequest{second, "/Pictures/capture.png", false});
  HDRSHOT_CHECK(!collision.has_value());
  HDRSHOT_CHECK(collision.error().code == ErrorCode::path_already_exists);
  HDRSHOT_CHECK(port.files.at("/Pictures/capture.png") == first);
}

void file_store_failure_has_no_success_receipt() {
  FakeFileStore port;
  port.injected_failure = ErrorCode::storage_full;
  const std::vector<std::uint8_t> bytes{1, 2};
  const auto result = port.write(WriteFileRequest{bytes, "/Pictures/capture.png", false});
  HDRSHOT_CHECK(!result.has_value());
  HDRSHOT_CHECK(result.error().code == ErrorCode::storage_full);
  HDRSHOT_CHECK(port.files.empty());
}

void file_dialog_exact_path_and_cancel_contract() {
  FakeFileDialog port;
  port.outcome = ChosenPath{"/tmp/exact.png"};
  const auto chosen = port.choose_save_path(
      ChooseSavePathRequest{"capture.png", "/Pictures", hdrshot::DisplayId{7}});
  HDRSHOT_CHECK(chosen.has_value());
  HDRSHOT_CHECK(std::get<ChosenPath>(chosen.value()).exact_path == "/tmp/exact.png");
  HDRSHOT_CHECK(port.last_request->initial_folder == "/Pictures");
  HDRSHOT_CHECK(port.last_request->owner_display_id == hdrshot::DisplayId{7});

  port.outcome = UserCancelled{};
  const auto cancelled = port.choose_save_path(ChooseSavePathRequest{"capture.png", "/Pictures"});
  HDRSHOT_CHECK(cancelled.has_value());
  HDRSHOT_CHECK(std::holds_alternative<UserCancelled>(cancelled.value()));
}

void settings_roundtrip_contract() {
  FakeSettingsStore port;
  const auto initial = port.load();
  HDRSHOT_CHECK(initial.has_value());
  HDRSHOT_CHECK(initial.value().revision == 0);

  SettingsPatch patch;
  patch.global_capture_hotkey = "Command+Shift+4";
  patch.default_save_folder = "/Screenshots";
  const auto receipt = port.save(patch);
  HDRSHOT_CHECK(receipt.has_value());
  HDRSHOT_CHECK(receipt.value().revision == 1);

  const auto reloaded = port.load();
  HDRSHOT_CHECK(reloaded.has_value());
  HDRSHOT_CHECK(reloaded.value().global_capture_hotkey == "Command+Shift+4");
  HDRSHOT_CHECK(reloaded.value().default_save_folder == "/Screenshots");
}

void settings_corruption_is_explicit() {
  FakeSettingsStore port;
  port.corrupt = true;
  const auto result = port.load();
  HDRSHOT_CHECK(!result.has_value());
  HDRSHOT_CHECK(result.error().code == ErrorCode::settings_corrupt);
}

void fixed_clock_contract() {
  const LocalDateTime fixed{2026, 8, 28, 14, 30, 45, 480};
  const FixedClock port(fixed);
  HDRSHOT_CHECK(port.now_local() == fixed);
  HDRSHOT_CHECK(port.now_local() == port.now_local());
}

void global_hotkey_lifecycle_and_conflict_contract() {
  FakeGlobalHotkey port;
  port.conflicting_hotkey = "Ctrl+X";
  const auto registered = port.register_hotkey("Ctrl+Shift+S");
  HDRSHOT_CHECK(registered.has_value());
  HDRSHOT_CHECK(port.active == "Ctrl+Shift+S");

  const auto conflict = port.replace_hotkey("Ctrl+X");
  HDRSHOT_CHECK(!conflict.has_value());
  HDRSHOT_CHECK(conflict.error().code == ErrorCode::hotkey_conflict);
  HDRSHOT_CHECK(port.active == "Ctrl+Shift+S");

  const auto replaced = port.replace_hotkey("Ctrl+Alt+A");
  HDRSHOT_CHECK(replaced.has_value());
  HDRSHOT_CHECK(port.active == "Ctrl+Alt+A");

  const auto unregistered = port.unregister_hotkey();
  HDRSHOT_CHECK(unregistered.has_value());
  HDRSHOT_CHECK(!port.active.has_value());
}

}  // namespace

int main() {
  using hdrshot::test::TestCase;
  return hdrshot::test::run(std::vector<TestCase>{
      {"clipboard write and rejection", clipboard_contract},
      {"file store atomic create-new", file_store_create_new_contract},
      {"file store injected failure", file_store_failure_has_no_success_receipt},
      {"file dialog exact path and cancel", file_dialog_exact_path_and_cancel_contract},
      {"settings roundtrip", settings_roundtrip_contract},
      {"settings corruption", settings_corruption_is_explicit},
      {"fixed clock", fixed_clock_contract},
      {"global hotkey lifecycle and conflict", global_hotkey_lifecycle_and_conflict_contract},
  });
}
