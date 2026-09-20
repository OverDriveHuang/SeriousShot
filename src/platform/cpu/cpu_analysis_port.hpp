#pragma once
#include "domain/analysis/engine.hpp"
#include "ports/analysis_port.hpp"
namespace hdrshot {
std::shared_ptr<AnalysisPort> make_cpu_analysis_port();
Result<LinearSourceRef, Error>
make_cpu_analysis_source(PixelSize size, analysis::FloatImage pixels);
} // namespace hdrshot
