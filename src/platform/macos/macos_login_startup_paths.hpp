#pragma once
#include <QString>
namespace hdrshot {
// Read-only, bounded evidence check. No build-machine paths are compiled in.
bool macos_login_target_is_project_staging(const QString& bundle_path);
} // namespace hdrshot
