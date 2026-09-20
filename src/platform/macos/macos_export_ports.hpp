#pragma once

#include "ports/export_ports.hpp"

namespace hdrshot {

class MacClipboardPort final : public ClipboardPort {
 public:
  // An empty pasteboard name selects the system clipboard. Named pasteboards
  // allow contract tests without replacing the user's clipboard.
  explicit MacClipboardPort(std::string cache_directory = {}, std::string pasteboard_name = {});
  Result<ClipboardReceipt, Error> write(const ClipboardWriteRequest& request) override;
 private:
  std::string cache_directory_;
  std::string pasteboard_name_;
};

class PosixFileStorePort final : public FileStorePort {
 public:
  Result<bool, Error> prepare_directory(const std::string& folder) override;
  Result<FileReceipt, Error> write(const WriteFileRequest& request) override;
  Result<std::unique_ptr<AtomicFileSink>, Error> open_atomic(
      const OpenAtomicFileRequest& request) override;
};

class MacFileDialogPort final : public FileDialogPort {
 public:
  Result<ChooseSavePathOutcome, Error> choose_save_path(
      const ChooseSavePathRequest& request) override;
};

class MacClockPort final : public ClockPort {
 public:
  LocalDateTime now_local() const override;
};

}  // namespace hdrshot
