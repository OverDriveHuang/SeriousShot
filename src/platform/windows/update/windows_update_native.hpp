#pragma once
#include "platform/qt/update/update_package.hpp"
namespace hdrshot::update {
QString windows_executable();
bool windows_install_quiet(const QString &target, const QString &backup = {});
QString windows_extended_path(const QString &path);
Result<bool, Error> windows_move(const QString &from, const QString &to);
Result<bool, Error> windows_replace_transaction(const QString &journal, bool recover = false);
void windows_write_start_receipt();
// True means the startup caller must return normally before initializing its
// shell; the independent armed helper owns recovery and relaunch.
Result<bool, Error> windows_begin_startup_recovery(const QString &isolated_root = {});
} // namespace hdrshot::update
