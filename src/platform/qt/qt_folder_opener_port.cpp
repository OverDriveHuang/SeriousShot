#include "platform/qt/qt_folder_opener_port.hpp"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QUrl>

#include <map>
#include <string>
#include <utility>

namespace hdrshot {
namespace {

Error open_error(
    const ErrorCode code,
    const Retryability retryability,
    std::map<std::string, std::string> context = {}) {
  return Error{code, "QtFolderOpenerPort", retryability, std::move(context)};
}

}  // namespace

Result<OpenFolderReceipt, Error> QtFolderOpenerPort::open_folder(
    const OpenFolderRequest& request) {
  if (request.exact_path.empty()) {
    return Result<OpenFolderReceipt, Error>::failure(open_error(
        ErrorCode::invalid_input,
        Retryability::never,
        {{"field", "exact_path"}}));
  }

  const auto path = QString::fromStdString(request.exact_path);
  if (!QFileInfo::exists(path)) {
    if (!request.create_if_missing || !QDir().mkpath(path)) {
      return Result<OpenFolderReceipt, Error>::failure(open_error(
          ErrorCode::permission_denied,
          Retryability::after_user_action,
          {{"reason", "folder_unavailable"}}));
    }
  }
  if (!QFileInfo(path).isDir()) {
    return Result<OpenFolderReceipt, Error>::failure(open_error(
        ErrorCode::invalid_input,
        Retryability::never,
        {{"reason", "path_is_not_folder"}}));
  }
  if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path))) {
    return Result<OpenFolderReceipt, Error>::failure(open_error(
        ErrorCode::dialog_failed,
        Retryability::after_user_action,
        {{"reason", "desktop_open_rejected"}}));
  }
  return Result<OpenFolderReceipt, Error>::success(OpenFolderReceipt{request.exact_path});
}

}  // namespace hdrshot
