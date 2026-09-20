#pragma once
#include "application/export_workflow.hpp"
#include "ports/analysis_port.hpp"
#include "ports/source_range_probe_port.hpp"

namespace hdrshot {
struct PreparedAnalysis {
  analysis::Input input;
  ExportSnapshot original; // independent ROI, never retains unrelated displays
};
class AnalysisWorkflow {
public:
  static Result<PreparedAnalysis, Error>
  prepare(const ExportSnapshot &snapshot, AnalysisPort &executor,
          SourceRangeProbePort *range_probe = nullptr);
  // Reuse the existing encoding/destination workflow for a composed report.
  static Result<ExportSnapshot, Error>
  report_snapshot(const ExportSnapshot &original, LinearSourceRef composed);
};
} // namespace hdrshot
