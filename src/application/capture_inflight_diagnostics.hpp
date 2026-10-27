#pragma once
#include "ports/capture_progress_port.hpp"
#include "ports/diagnostics_port.hpp"
#include <memory>
#include <atomic>
#include <vector>
#include <mutex>
#include <set>
namespace hdrshot {
// Small, platform-neutral in-flight facts. No images, UI owners or native handles.
class CaptureInFlightDiagnostics {
 public:
  CaptureInFlightDiagnostics(CaptureOperationKey key, std::shared_ptr<DiagnosticsPort> sink,
      std::vector<DisplayId> targets = {});
  void stage(DisplayId display, const char* api, const char* stage) noexcept;
  void image_received(DisplayId display) noexcept;
  void failure(DisplayId display, const Error& error, const char* stage) noexcept;
  void snapshot() noexcept;
  bool write_failed() const noexcept { return write_failed_.load(); }
 private:
  struct Entry { std::string api, stage; CaptureSteadyTime since; };
  void write(DiagnosticEvent event) noexcept;
  CaptureOperationKey key_; std::shared_ptr<DiagnosticsPort> sink_;
  CaptureSteadyTime started_; mutable std::mutex mutex_;
  std::map<DisplayId, Entry> entries_; std::set<DisplayId> targets_, received_;
  bool cancelled_{};
  std::atomic_bool write_failed_{};
};
}
