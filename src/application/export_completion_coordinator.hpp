#pragma once
#include "application/export_workflow.hpp"
#include "application/input_mapper.hpp"
#include "ports/export_task_executor_port.hpp"
#include "ports/diagnostics_port.hpp"
#include <functional>

namespace hdrshot {
struct ExportCompletionPorts {
  FileStorePort& files;
  FileDialogPort& dialog;
  ClipboardPort& clipboard;
  const ClockPort& clock;
  ExportTaskExecutorPort& executor;
  TextRasterizerPort* text{};
  ExportPixelProcessorPort* pixels{};
  UltraHdrInputRendererPort* ultra_hdr_input{};
  UltraHdrEncoderPort* ultra_hdr_encoder{};
  SourceRangeProbePort* range{};
};

// Ports/coordinator must outlive accepted work; the host drains the executor
// before destroying them. Product decisions never live in the native shell.
class ExportCompletionCoordinator {
 public:
  using Outcome = Result<ExportReceipt, Error>;
  using Completed = std::function<void(Outcome)>;
  using DialogVisibility = std::function<void(bool, DisplayId)>;
  using Event = std::function<void(const ExportSnapshot&, UiCommand,
                                  const char*, const Outcome*)>;
  explicit ExportCompletionCoordinator(ExportCompletionPorts ports,
      DialogVisibility dialog_visibility = {}, Event event = {},
      DiagnosticsPort* diagnostics = nullptr);
  // Called on UI thread. false: cancel/error before acceptance, keep editor.
  // true: immutable job queued; editor can close before encoding starts.
  bool submit(UiCommand command, ExportSnapshot snapshot, Completed completed);
 private:
  ExportCompletionPorts ports_;
  DialogVisibility dialog_visibility_;
  Event event_;
  DiagnosticsPort* diagnostics_{};
};
}  // namespace hdrshot
