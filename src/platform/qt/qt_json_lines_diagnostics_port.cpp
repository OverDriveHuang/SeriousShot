#include "platform/qt/qt_json_lines_diagnostics_port.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>

#include <map>
#include <string>
#include <utility>

namespace hdrshot {
namespace {

Error diagnostic_error(const std::string& path, const char* reason) {
  return Error{
      ErrorCode::path_not_writable,
      "QtJsonLinesDiagnosticsPort",
      Retryability::after_user_action,
      {{"path", path}, {"reason", reason}},
  };
}

}  // namespace

QtJsonLinesDiagnosticsPort::QtJsonLinesDiagnosticsPort(std::string exact_path)
    : exact_path_(std::move(exact_path)) {}

Result<DiagnosticReceipt, Error> QtJsonLinesDiagnosticsPort::record(
    const DiagnosticEvent& event) {
  const QMutexLocker lock(&mutex_);
  if (exact_path_.empty()) {
    return Result<DiagnosticReceipt, Error>::failure(
        diagnostic_error(exact_path_, "empty_path"));
  }
  const QFileInfo info(QString::fromStdString(exact_path_));
  if (!QDir().mkpath(info.absolutePath())) {
    return Result<DiagnosticReceipt, Error>::failure(
        diagnostic_error(exact_path_, "parent_directory_unavailable"));
  }
  QFile file(QString::fromStdString(exact_path_));
  if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
    return Result<DiagnosticReceipt, Error>::failure(
        diagnostic_error(exact_path_, "open_failed"));
  }
  QJsonObject context;
  for (const auto& [key, value] : event.safe_context) {
    context.insert(QString::fromStdString(key), QString::fromStdString(value));
  }
  const auto next_sequence = sequence_ + 1U;
  QJsonObject object{
      {"timestamp", QDateTime::currentDateTime().toString(Qt::ISODateWithMs)},
      {"sequence", static_cast<qint64>(next_sequence)},
      {"sessionId", static_cast<qint64>(event.session_id.value)},
      {"operationId", static_cast<qint64>(event.operation_id.value)},
      {"subsystem", QString::fromStdString(event.subsystem)},
      {"command", QString::fromStdString(event.command)},
      {"stage", QString::fromStdString(event.stage)},
      {"outcome", QString::fromStdString(event.outcome)},
      {"errorCode", QString::fromStdString(event.error_code)},
      {"safeContext", context},
      {"destinationName", QString::fromStdString(event.destination_name)},
      {"bytes", static_cast<qint64>(event.bytes)},
  };
  const auto line = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
  if (file.write(line) != line.size() || !file.flush()) {
    return Result<DiagnosticReceipt, Error>::failure(
        diagnostic_error(exact_path_, "write_failed"));
  }
  sequence_ = next_sequence;
  return Result<DiagnosticReceipt, Error>::success(DiagnosticReceipt{sequence_});
}

}  // namespace hdrshot
