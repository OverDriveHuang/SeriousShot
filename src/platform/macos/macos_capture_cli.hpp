#pragma once

#include "application/headless_capture.hpp"

class QApplication;

namespace hdrshot {
// Composition only: native main display / run loop, settings, existing executors.
int run_macos_capture_cli(QApplication& app, const CaptureCommand& command);
}  // namespace hdrshot
