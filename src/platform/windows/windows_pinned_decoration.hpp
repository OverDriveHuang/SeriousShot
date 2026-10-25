#pragma once
#include "ports/diagnostics_port.hpp"
#include <memory>
class QWidget;
namespace hdrshot {
// Window-only decoration; owns no Source input or presentation scheduler.
void configure_windows_pinned_decoration(QWidget &,std::shared_ptr<DiagnosticsPort>,bool preview=false);
}
