#include "platform/macos/macos_export_ports.hpp"

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace hdrshot {
namespace {

Error export_error(
    const ErrorCode code,
    const char* module,
    const Retryability retryability,
    std::map<std::string, std::string> context = {},
    std::source_location origin = std::source_location::current()) {
  return Error{code, module, retryability, std::move(context), origin};
}

Error file_error(const int code,
    std::source_location origin = std::source_location::current()) {
  const auto mapped = code == EEXIST ? ErrorCode::path_already_exists :
      code == ENOSPC ? ErrorCode::storage_full : ErrorCode::path_not_writable;
  return export_error(mapped, "PosixFileStorePort",
      code == EEXIST ? Retryability::same_input : Retryability::after_user_action,
      {{"nativeCode", std::to_string(code)}, {"reason", "posix_file_operation_failed"}}, origin);
}

class PosixAtomicFileSink final : public AtomicFileSink {
 public:
  PosixAtomicFileSink(
      const int descriptor,
      std::string pending_path,
      std::string exact_path,
      const bool overwrite)
      : descriptor_(descriptor),
        pending_path_(std::move(pending_path)),
        exact_path_(std::move(exact_path)),
        overwrite_(overwrite) {}

  ~PosixAtomicFileSink() override { abort(); }

  Result<std::size_t, Error> write(
      const std::span<const std::uint8_t> bytes) override {
    if (descriptor_ < 0 || committed_) {
      return Result<std::size_t, Error>::failure(export_error(
          ErrorCode::state_inconsistent,
          "PosixAtomicFileSink",
          Retryability::never,
          {{"reason", "sink_not_writable"}}));
    }
    if (bytes.empty()) {
      return Result<std::size_t, Error>::success(0U);
    }
    const auto count = ::write(descriptor_, bytes.data(), bytes.size());
    if (count <= 0) {
      return Result<std::size_t, Error>::failure(file_error(errno));
    }
    const auto accepted = static_cast<std::size_t>(count);
    bytes_written_ += accepted;
    return Result<std::size_t, Error>::success(accepted);
  }

  Result<FileReceipt, Error> commit() override {
    if (descriptor_ < 0 || committed_ || bytes_written_ == 0U) {
      return Result<FileReceipt, Error>::failure(export_error(
          ErrorCode::state_inconsistent,
          "PosixAtomicFileSink",
          Retryability::never,
          {{"reason", "sink_not_committable"}}));
    }
    if (::close(descriptor_) != 0) {
      const auto code = errno;
      descriptor_ = -1;
      ::unlink(pending_path_.c_str());
      pending_path_.clear();
      return Result<FileReceipt, Error>::failure(file_error(code));
    }
    descriptor_ = -1;
    int result = 0;
    if (overwrite_) {
      result = ::rename(pending_path_.c_str(), exact_path_.c_str());
    } else {
#if defined(__APPLE__)
      result = ::renamex_np(pending_path_.c_str(), exact_path_.c_str(), RENAME_EXCL);
#else
      result = ::link(pending_path_.c_str(), exact_path_.c_str());
      if (result == 0) {
        result = ::unlink(pending_path_.c_str());
      }
#endif
    }
    if (result != 0) {
      const auto code = errno;
      ::unlink(pending_path_.c_str());
      pending_path_.clear();
      return Result<FileReceipt, Error>::failure(file_error(code));
    }
    pending_path_.clear();
    committed_ = true;
    return Result<FileReceipt, Error>::success(
        FileReceipt{exact_path_, bytes_written_});
  }

  void abort() noexcept override {
    if (descriptor_ >= 0) {
      ::close(descriptor_);
      descriptor_ = -1;
    }
    if (!committed_ && !pending_path_.empty()) {
      ::unlink(pending_path_.c_str());
      pending_path_.clear();
    }
  }

 private:
  int descriptor_{-1};
  std::string pending_path_;
  std::string exact_path_;
  bool overwrite_{};
  bool committed_{};
  std::size_t bytes_written_{};
};

NSScreen* screen_for_display(const DisplayId display_id) {
  if (display_id.value == 0U) {
    return nil;
  }
  for (NSScreen* screen in NSScreen.screens) {
    NSNumber* number = screen.deviceDescription[NSDeviceDescriptionKey(@"NSScreenNumber")];
    if (number != nil && number.unsignedIntValue == display_id.value) {
      return screen;
    }
  }
  return nil;
}

}  // namespace

MacClipboardPort::MacClipboardPort(std::string cache_directory, std::string pasteboard_name)
    : cache_directory_(std::move(cache_directory)), pasteboard_name_(std::move(pasteboard_name)) {}

Result<ClipboardReceipt, Error> MacClipboardPort::write(
    const ClipboardWriteRequest& request) {
  const bool is_png = std::find(
      request.mime_types.begin(), request.mime_types.end(), "image/png") !=
      request.mime_types.end();
  const bool is_jpeg = std::find(
      request.mime_types.begin(), request.mime_types.end(), "image/jpeg") !=
      request.mime_types.end();
  if (request.bytes.empty() || is_png == is_jpeg) {
    return Result<ClipboardReceipt, Error>::failure(export_error(
        ErrorCode::unsupported_encoding, "MacClipboardPort", Retryability::never));
  }
  // File-aware consumers must be able to paste the encoded original, not an
  // NSImage/TIFF/8-bit reconstruction. Keep every published file immutable and
  // alive across subsequent copies and process exit (clipboard history).
  NSString* root = cache_directory_.empty()
      ? [NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES)
             .firstObject stringByAppendingPathComponent:@"Overdrive/SeriousShot/clipboard"]
      : [NSString stringWithUTF8String:cache_directory_.c_str()];
  const auto rejected = [](const char* reason) {
    return Result<ClipboardReceipt, Error>::failure(export_error(
        ErrorCode::clipboard_rejected, "MacClipboardPort", Retryability::same_input,
        {{"reason", reason}}));
  };
  if (root == nil || ![NSFileManager.defaultManager createDirectoryAtPath:root
          withIntermediateDirectories:YES attributes:@{NSFilePosixPermissions: @0700}
          error:nil]) return rejected("cache_directory_failed");
  NSString* name = [NSString stringWithFormat:@"SeriousShot_%@.%@",
      NSUUID.UUID.UUIDString, is_png ? @"png" : @"jpg"];
  NSURL* url = [NSURL fileURLWithPath:[root stringByAppendingPathComponent:name]];
  NSData* data = [NSData dataWithBytesNoCopy:const_cast<std::uint8_t*>(request.bytes.data())
      length:request.bytes.size() freeWhenDone:NO];
  if (![data writeToURL:url options:NSDataWritingAtomic error:nil])
    return rejected("cache_write_failed");
  const NSPasteboardType pasteboard_type = is_png
      ? NSPasteboardTypePNG
      : UTTypeJPEG.identifier;
  NSPasteboardItem* item = [[NSPasteboardItem alloc] init];
  if (![item setString:url.absoluteString forType:NSPasteboardTypeFileURL] ||
      ![item setData:data forType:pasteboard_type]) {
    [NSFileManager.defaultManager removeItemAtURL:url error:nil];
    return rejected("prepare_item_failed");
  }
  NSPasteboard* pasteboard = pasteboard_name_.empty() ? NSPasteboard.generalPasteboard
      : [NSPasteboard pasteboardWithName:
          [NSString stringWithUTF8String:pasteboard_name_.c_str()]];
  [pasteboard clearContents];
  if (![pasteboard writeObjects:@[item]]) {
    // Publication may have partially exposed the URL. Do not delete its target.
    return rejected("publish_failed");
  }
  // Verify the actual board, not only our local item. Check publication and
  // ownership without downloading a second full 5K payload on the UI thread.
  const auto published_change = pasteboard.changeCount;
  NSArray<NSPasteboardItem*>* published = pasteboard.pasteboardItems;
  const bool visible = published.count == 1 &&
      [published.firstObject.types containsObject:pasteboard_type] &&
      [[published.firstObject stringForType:NSPasteboardTypeFileURL]
          isEqualToString:url.absoluteString];
  if (!visible || pasteboard.changeCount != published_change)
    return rejected("publication_not_visible");
  return Result<ClipboardReceipt, Error>::success(ClipboardReceipt{
      request.bytes.size(), {is_png ? "image/png" : "image/jpeg", "text/uri-list"}});
}

Result<FileReceipt, Error> PosixFileStorePort::write(const WriteFileRequest& request) {
  if (request.bytes.empty() || request.exact_path.empty()) {
    return Result<FileReceipt, Error>::failure(export_error(
        ErrorCode::invalid_input, "PosixFileStorePort", Retryability::never));
  }
  auto opened = open_atomic(OpenAtomicFileRequest{request.exact_path, request.overwrite});
  if (!opened) {
    return Result<FileReceipt, Error>::failure(opened.error());
  }
  std::size_t written = 0;
  while (written < request.bytes.size()) {
    auto result = opened.value()->write(request.bytes.subspan(written));
    if (!result) {
      opened.value()->abort();
      return Result<FileReceipt, Error>::failure(result.error());
    }
    written += result.value();
  }
  return opened.value()->commit();
}

Result<bool, Error> PosixFileStorePort::prepare_directory(const std::string& folder) {
  std::error_code error;
  if (!folder.empty() && folder.find('\0') == std::string::npos) {
    std::filesystem::create_directories(folder, error);
    if (!error && ::access(folder.c_str(), W_OK | X_OK) == 0)
      return Result<bool, Error>::success(true);
  }
  return Result<bool, Error>::failure(export_error(
      ErrorCode::path_not_writable, "PosixFileStorePort",
      Retryability::after_user_action, {{"reason", "directory_unavailable"}}));
}

Result<std::unique_ptr<AtomicFileSink>, Error> PosixFileStorePort::open_atomic(
    const OpenAtomicFileRequest& request) {
  if (request.exact_path.empty()) {
    return Result<std::unique_ptr<AtomicFileSink>, Error>::failure(export_error(
        ErrorCode::invalid_input, "PosixFileStorePort", Retryability::never));
  }
  if (!request.overwrite && ::access(request.exact_path.c_str(), F_OK) == 0) {
    return Result<std::unique_ptr<AtomicFileSink>, Error>::failure(file_error(EEXIST));
  }
  std::vector<char> template_path(request.exact_path.begin(), request.exact_path.end());
  constexpr char suffix[] = ".hdrshot.XXXXXX";
  template_path.insert(template_path.end(), std::begin(suffix), std::end(suffix));
  const int descriptor = ::mkstemp(template_path.data());
  if (descriptor < 0) {
    return Result<std::unique_ptr<AtomicFileSink>, Error>::failure(file_error(errno));
  }
  ::fchmod(descriptor, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
  return Result<std::unique_ptr<AtomicFileSink>, Error>::success(
      std::make_unique<PosixAtomicFileSink>(
          descriptor, template_path.data(), request.exact_path, request.overwrite));
}

Result<ChooseSavePathOutcome, Error> MacFileDialogPort::choose_save_path(
    const ChooseSavePathRequest& request) {
  NSSavePanel* panel = [NSSavePanel savePanel];
  panel.level = NSModalPanelWindowLevel;
  panel.collectionBehavior = NSWindowCollectionBehaviorMoveToActiveSpace;
  panel.allowedContentTypes = @[[UTType typeWithIdentifier:
      request.save_format == SaveFormat::ultra_hdr_jpeg
          ? @"public.jpeg"
          : @"public.png"]];
  panel.nameFieldStringValue = [NSString stringWithUTF8String:request.suggested_name.c_str()];
  if (!request.initial_folder.empty()) {
    NSString* folder = [NSString stringWithUTF8String:request.initial_folder.c_str()];
    panel.directoryURL = [NSURL fileURLWithPath:folder isDirectory:YES];
  }
  [NSApp activateIgnoringOtherApps:YES];
  NSScreen* target_screen = screen_for_display(request.owner_display_id);
  if (target_screen != nil) {
    const NSRect visible = target_screen.visibleFrame;
    NSRect frame = panel.frame;
    frame.origin.x = NSMidX(visible) - NSWidth(frame) / 2.0;
    frame.origin.y = NSMidY(visible) - NSHeight(frame) / 2.0;
    [panel setFrameOrigin:frame.origin];
  } else {
    [panel center];
  }
  [panel makeKeyAndOrderFront:nil];
  const auto response = [panel runModal];
  if (response == NSModalResponseCancel) {
    return Result<ChooseSavePathOutcome, Error>::success(UserCancelled{});
  }
  if (response != NSModalResponseOK || panel.URL == nil) {
    return Result<ChooseSavePathOutcome, Error>::failure(export_error(
        ErrorCode::dialog_failed, "MacFileDialogPort", Retryability::after_user_action));
  }
  return Result<ChooseSavePathOutcome, Error>::success(ChosenPath{
      panel.URL.path.UTF8String});
}

LocalDateTime MacClockPort::now_local() const {
  NSCalendar* calendar = NSCalendar.currentCalendar;
  NSDateComponents* components = [calendar components:
      NSCalendarUnitYear | NSCalendarUnitMonth | NSCalendarUnitDay |
      NSCalendarUnitHour | NSCalendarUnitMinute | NSCalendarUnitSecond |
      NSCalendarUnitTimeZone fromDate:[NSDate date]];
  const auto offset_seconds = [components.timeZone secondsFromGMT];
  return LocalDateTime{
      static_cast<int>(components.year),
      static_cast<std::uint8_t>(components.month),
      static_cast<std::uint8_t>(components.day),
      static_cast<std::uint8_t>(components.hour),
      static_cast<std::uint8_t>(components.minute),
      static_cast<std::uint8_t>(components.second),
      static_cast<std::int16_t>(offset_seconds / 60),
  };
}

}  // namespace hdrshot
