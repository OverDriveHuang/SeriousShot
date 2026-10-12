#pragma once
#include "platform/qt/update/update_package.hpp"
namespace hdrshot::update {
QString windows_executable();
bool windows_install_quiet(const QString &target, const QString &backup = {});
bool windows_move(const QString &from, const QString &to);
Result<bool, Error> windows_replace_transaction(const QString &journal, bool recover = false);
void windows_write_start_receipt();
} // namespace hdrshot::update
