#pragma once

#include "platform/windows/windows_display_state.hpp"
#include <QObject>
#include <cstdint>
#include <functional>
#include <memory>

namespace hdrshot {

// One UI-thread service per application. Consumers retain immutable snapshots;
// capture's independently frozen DisplaySnapshot is never invalidated here.
class WindowsDisplayStateService final : public QObject {
 public:
  using Token = std::uint64_t;
  using Update = Result<WindowsDisplayStateRef, Error>;
  using Callback = std::function<void(Update)>;
  using QueryResult = Result<WindowsDisplayInfo, Error>;
  using QueryCompletion = std::function<void(QueryResult)>;
  // Injectable asynchronous query. It must return promptly; completion may run
  // on any thread and may arrive after unsubscribe/shutdown. Native queries use
  // a bounded background executor, never the UI thread.
  using Query = std::function<void(std::uintptr_t, QueryCompletion)>;
  struct Options {
    Query query;
    bool native_notifications{true};
    int coalesce_ms{50};
    // Capability injection for older-system notification tests. Omitted in
    // production; it does not change capture/display-mode compatibility.
    std::function<std::uint32_t()> build_number;
  };

  explicit WindowsDisplayStateService(QObject* parent = nullptr,
      std::shared_ptr<DiagnosticsPort> diagnostics = {});
  WindowsDisplayStateService(Options options, QObject* parent = nullptr,
      std::shared_ptr<DiagnosticsPort> diagnostics = {});
  ~WindowsDisplayStateService() override;

  Token subscribe(std::uintptr_t monitor, Callback callback);
  void move(Token token, std::uintptr_t monitor);
  void unsubscribe(Token token);
  void shutdown();

  // Event entry points also serve deterministic tests. These only enqueue
  // work. No activation/resume events or periodic refresh are installed.
  void notify_monitor_changed(std::uintptr_t monitor);
  void notify_color_profiles_changed();
  void notify_topology_changed();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace hdrshot
