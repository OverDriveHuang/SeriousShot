#pragma once

#include "core/error.hpp"
#include "core/byte_sink.hpp"
#include "core/ids.hpp"
#include "core/result.hpp"
#include "domain/color/hdr_pq_precision.hpp"
#include "domain/input/completion_action.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace hdrshot {

enum class SaveFormat : std::uint8_t {
  png_display_p3_dual_range,
  ultra_hdr_jpeg,
};

enum class UltraHdrJpegQuality : std::uint8_t {
  compact = 85,
  balanced = 95,
  maximum = 100,
};

[[nodiscard]] constexpr bool valid_ultra_hdr_jpeg_quality(
    const UltraHdrJpegQuality value) noexcept {
  return value == UltraHdrJpegQuality::compact ||
      value == UltraHdrJpegQuality::balanced ||
      value == UltraHdrJpegQuality::maximum;
}

[[nodiscard]] constexpr int ultra_hdr_jpeg_quality_value(
    const UltraHdrJpegQuality value) noexcept {
  return static_cast<int>(value);
}

enum class PqDiffuseWhite : std::uint16_t {
  nits_100 = 100,
  nits_203 = 203,
};

[[nodiscard]] constexpr double pq_diffuse_white_nits(
    const PqDiffuseWhite value) noexcept {
  return static_cast<double>(value);
}

struct ClipboardWriteRequest {
  std::span<const std::uint8_t> bytes;
  std::vector<std::string> mime_types;
};

struct ClipboardReceipt {
  std::size_t bytes_written{};
  std::vector<std::string> published_mime_types;

  friend bool operator==(const ClipboardReceipt&, const ClipboardReceipt&) = default;
};

class ClipboardPort {
 public:
  virtual ~ClipboardPort() = default;
  [[nodiscard]] virtual Result<ClipboardReceipt, Error> write(
      const ClipboardWriteRequest& request) = 0;
};

struct WriteFileRequest {
  std::span<const std::uint8_t> bytes;
  std::string exact_path;
  bool overwrite{};
};

struct FileReceipt {
  std::string exact_path;
  std::size_t bytes_written{};

  friend bool operator==(const FileReceipt&, const FileReceipt&) = default;
};

struct OpenAtomicFileRequest {
  std::string exact_path;
  bool overwrite{};
};

// The destination becomes visible as the final file only after commit().
// Destroying or aborting an uncommitted sink must remove its pending file.
class AtomicFileSink : public ByteSink {
 public:
  ~AtomicFileSink() override = default;
  [[nodiscard]] virtual Result<FileReceipt, Error> commit() = 0;
  virtual void abort() noexcept = 0;
};

class FileStorePort {
 public:
  virtual ~FileStorePort() = default;
  virtual Result<bool, Error> prepare_directory(const std::string&) {
    return Result<bool, Error>::failure(Error{ErrorCode::path_not_writable,
        "FileStorePort", Retryability::after_user_action,
        {{"reason", "directory_preparation_not_implemented"}}});
  }
  [[nodiscard]] virtual Result<FileReceipt, Error> write(const WriteFileRequest& request) = 0;
  [[nodiscard]] virtual Result<std::unique_ptr<AtomicFileSink>, Error> open_atomic(
      const OpenAtomicFileRequest&) {
    return Result<std::unique_ptr<AtomicFileSink>, Error>::failure(Error{
        ErrorCode::unsupported_encoding,
        "FileStorePort",
        Retryability::never,
        {{"reason", "atomic_streaming_not_implemented"}},
    });
  }
};

struct ChooseSavePathRequest {
  std::string suggested_name;
  std::string initial_folder;
  DisplayId owner_display_id{};
  SaveFormat save_format{SaveFormat::png_display_p3_dual_range};
};

struct ChosenPath {
  std::string exact_path;

  friend bool operator==(const ChosenPath&, const ChosenPath&) = default;
};

struct UserCancelled {
  friend bool operator==(const UserCancelled&, const UserCancelled&) = default;
};

using ChooseSavePathOutcome = std::variant<ChosenPath, UserCancelled>;

class FileDialogPort {
 public:
  virtual ~FileDialogPort() = default;
  [[nodiscard]] virtual Result<ChooseSavePathOutcome, Error> choose_save_path(
      const ChooseSavePathRequest& request) = 0;
};

struct SettingsSnapshot {
  std::uint32_t schema_version{5};
  std::uint64_t revision{};
  std::string global_capture_hotkey;
  std::string default_save_folder;
  SaveFormat save_format{SaveFormat::png_display_p3_dual_range};
  PqDiffuseWhite pq_diffuse_white{PqDiffuseWhite::nits_203};
  HdrPqPrecision hdr_pq_precision{HdrPqPrecision::bits_10};
  UltraHdrJpegQuality ultra_hdr_jpeg_quality{UltraHdrJpegQuality::balanced};
  CompletionAction enter_completion_action{CompletionAction::copy_to_clipboard};
  CompletionAction double_click_completion_action{CompletionAction::copy_to_clipboard};
  bool initial_settings_presented{};
  bool detailed_logging{};
  std::string analyzer_preferences_json{}; // UI options only, no pixels/masks/sample positions

  friend bool operator==(const SettingsSnapshot&, const SettingsSnapshot&) = default;
};

struct SettingsPatch {
  std::optional<std::string> global_capture_hotkey;
  std::optional<std::string> default_save_folder;
  std::optional<SaveFormat> save_format;
  std::optional<PqDiffuseWhite> pq_diffuse_white;
  std::optional<HdrPqPrecision> hdr_pq_precision;
  std::optional<UltraHdrJpegQuality> ultra_hdr_jpeg_quality;
  std::optional<CompletionAction> enter_completion_action;
  std::optional<CompletionAction> double_click_completion_action;
  std::optional<bool> initial_settings_presented;
  std::optional<bool> detailed_logging;
  std::optional<std::string> analyzer_preferences_json;
};

struct SettingsReceipt {
  std::uint64_t revision{};

  friend bool operator==(const SettingsReceipt&, const SettingsReceipt&) = default;
};

class SettingsStorePort {
 public:
  virtual ~SettingsStorePort() = default;
  [[nodiscard]] virtual Result<SettingsSnapshot, Error> load() = 0;
  [[nodiscard]] virtual Result<SettingsReceipt, Error> save(const SettingsPatch& patch) = 0;
};

struct LocalDateTime {
  std::int32_t year{};
  std::uint8_t month{};
  std::uint8_t day{};
  std::uint8_t hour{};
  std::uint8_t minute{};
  std::uint8_t second{};
  std::int16_t utc_offset_minutes{};

  friend bool operator==(const LocalDateTime&, const LocalDateTime&) = default;
};

class ClockPort {
 public:
  virtual ~ClockPort() = default;
  [[nodiscard]] virtual LocalDateTime now_local() const = 0;
};

struct HotkeyReceipt {
  std::string active_hotkey;

  friend bool operator==(const HotkeyReceipt&, const HotkeyReceipt&) = default;
};

using HotkeyTriggerHandler = std::function<void()>;

class GlobalHotkeyPort {
 public:
  virtual ~GlobalHotkeyPort() = default;
  virtual void set_trigger_handler(HotkeyTriggerHandler handler) = 0;
  [[nodiscard]] virtual Result<HotkeyReceipt, Error> register_hotkey(std::string hotkey) = 0;
  [[nodiscard]] virtual Result<HotkeyReceipt, Error> replace_hotkey(std::string hotkey) = 0;
  [[nodiscard]] virtual Result<HotkeyReceipt, Error> unregister_hotkey() = 0;
};

}  // namespace hdrshot
