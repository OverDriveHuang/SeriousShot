#include "platform/qt/qt_session_diagnostics_port.hpp"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QMutexLocker>
#include <QRegularExpression>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>
#include <utility>

namespace hdrshot {
namespace {
Error log_error(const char* reason) {
  return {ErrorCode::path_not_writable, "QtSessionDiagnosticsPort",
          Retryability::after_user_action, {{"reason", reason}}};
}

// Fixed-size, single-line tokens only: never persist paths, free-form context,
// pixel data or annotation text even if a caller accidentally supplies them.
QByteArray token(const std::string& value, int limit = 64) {
  auto text = QString::fromStdString(value).left(limit);
  text.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_.:+-]")),
               QStringLiteral("_"));
  return text.isEmpty() ? QByteArray("unknown") : text.toLatin1();
}
std::string field(const DiagnosticEvent& event, const char* key) {
  const auto it = event.safe_context.find(key);
  return it == event.safe_context.end() ? std::string{} : it->second;
}
QByteArray now() {
  return QDateTime::currentDateTime().toString(
      QStringLiteral("yyyy-MM-ddTHH:mm:ss.zzz")).toLatin1();
}
bool owned_log(const QFileInfo& info) {
  static const QRegularExpression pattern(
      QStringLiteral("^runtime-[0-9]{8}-[0-9]{6}-[0-9]{3}(?:-[0-9]+)?\\.log$"));
  return info.isFile() && !info.isSymLink() &&
      (info.fileName() == QStringLiteral("runtime.jsonl") ||
       pattern.match(info.fileName()).hasMatch());
}

QByteArray detailed_fields(const DiagnosticEvent& event) {
  QByteArray out;
  // Numeric allowlist: arbitrary context never becomes user text in a log.
  static const std::vector<std::string> numbers{
      "displayId", "displayCount", "width", "height", "x", "y",
      "logicalWidth", "logicalHeight", "scale", "currentEDR", "potentialEDR",
      "stride", "bitmapInfo", "bitsPerComponent", "bitsPerPixel", "elapsedMs",
      "diffuseWhite", "precision", "quality", "dimFactor", "uiWhite",
      "borderWidth", "maximumLinear", "maxCLL", "maxFALL", "nativeCode",
      "linearizationPasses", "sourceBytes", "contentBuilds", "contentCacheHits", "composedPixels", "cleanBytes",
      "clippedPixels", "clippedChannels", "encodeMs", "publishMs", "queuedMs"};
  static const std::vector<std::string> codes{
      "source", "api", "requestedSpace", "returnedSpace", "requestedFormat", "preset", "osVersion",
      "format", "range", "transfer", "surface", "reason", "opaque", "cursor", "sourceStorage"};
  for (const auto& key : numbers) {
    const auto it = event.safe_context.find(key);
    if (it == event.safe_context.end() || it->second.size() > 32) continue;
    bool ok = false;
    const auto v = QString::fromStdString(it->second).toDouble(&ok);
    if (ok && std::isfinite(v))
      out += ' ' + QByteArray::fromStdString(key) + '=' + token(it->second, 32);
  }
  for (const auto& key : codes) {
    const auto it = event.safe_context.find(key);
    if (it == event.safe_context.end()) continue;
    const auto& value = it->second;
    if (!value.empty() && value.size() <= 64 &&
        value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") ==
            std::string::npos)
      out += ' ' + QByteArray::fromStdString(key) + '=' + QByteArray::fromStdString(value);
  }
  if (event.error_origin) {
    const auto location = *event.error_origin;
    std::string file(location.file_name());
    std::replace(file.begin(), file.end(), '\\', '/'); // MSVC source locations.
    std::string relative;
    for (const auto* root : {"/src/", "/tools/", "/tests/"}) {
      const auto pos = file.rfind(root);
      if (pos != std::string::npos) { relative = file.substr(pos + 1); break; }
    }
    if (!relative.empty()) {
      out += " at=" + QByteArray::fromStdString(relative) + ':' +
          QByteArray::number(location.line()) + " function=" + token(location.function_name(), 240);
    }
  }
  return out;
}
}  // namespace

QtSessionDiagnosticsPort::QtSessionDiagnosticsPort(std::string directory)
    : directory_(std::move(directory)) {}

Result<DiagnosticReceipt, Error> QtSessionDiagnosticsPort::append(const QByteArray& line) {
  if (!file_.isOpen() || file_.write(line) != line.size() || !file_.flush()) {
    return Result<DiagnosticReceipt, Error>::failure(log_error("write_failed"));
  }
  return Result<DiagnosticReceipt, Error>::success({++sequence_});
}

Result<DiagnosticReceipt, Error> QtSessionDiagnosticsPort::start(
    const std::string& build, const std::string& platform) {
  const QMutexLocker lock(&mutex_);
  if (file_.isOpen() || directory_.empty()) {
    return Result<DiagnosticReceipt, Error>::failure(log_error("invalid_start"));
  }
  const QDir directory(QString::fromStdString(directory_));
  if (!QDir().mkpath(directory.absolutePath())) {
    return Result<DiagnosticReceipt, Error>::failure(log_error("directory_unavailable"));
  }
  const auto stem = QStringLiteral("runtime-") + QDateTime::currentDateTime().toString(
      QStringLiteral("yyyyMMdd-HHmmss-zzz"));
  static std::atomic_uint64_t launch_sequence{};
  for (int collision = 0; collision < 1000; ++collision) {
    const auto suffix = QStringLiteral("-%1").arg(
        static_cast<qulonglong>(launch_sequence.fetch_add(1)), 6, 10, QLatin1Char('0'));
    file_.setFileName(directory.filePath(stem + suffix + QStringLiteral(".log")));
    if (file_.open(QIODevice::WriteOnly | QIODevice::NewOnly)) break;
    if (!QFileInfo::exists(file_.fileName())) {
      return Result<DiagnosticReceipt, Error>::failure(log_error("create_failed"));
    }
  }
  if (!file_.isOpen()) {
    return Result<DiagnosticReceipt, Error>::failure(log_error("name_exhausted"));
  }
  const auto started = append(now() + " start SeriousShot build=" + token(build) +
                              " platform=" + token(platform) + '\n');
  if (!started) {
    file_.close();
    (void)file_.remove();  // Only the failed new file; all previous logs stay.
    return started;
  }

  // Pin the current file even if the wall clock moved backwards. Of the
  // previous owned logs keep the most recently written one. Never follow links.
  bool kept_previous = false;
  auto previous = directory.entryInfoList(QDir::Files | QDir::NoSymLinks);
  std::sort(previous.begin(), previous.end(), [](const QFileInfo& a, const QFileInfo& b) {
    const bool legacy_a = a.fileName() == QStringLiteral("runtime.jsonl");
    const bool legacy_b = b.fileName() == QStringLiteral("runtime.jsonl");
    if (legacy_a != legacy_b) return !legacy_a;
    return a.lastModified() == b.lastModified() ? a.fileName() > b.fileName() :
        a.lastModified() > b.lastModified();
  });
  for (const auto& info : previous) {
    if (!owned_log(info) || info.absoluteFilePath() == QFileInfo(file_).absoluteFilePath()) continue;
    if (!kept_previous) { kept_previous = true; continue; }
    if (!QFile::remove(info.absoluteFilePath())) {
      return Result<DiagnosticReceipt, Error>::failure(log_error("cleanup_failed"));
    }
  }
  return started;
}

Result<DiagnosticReceipt, Error> QtSessionDiagnosticsPort::record(const DiagnosticEvent& event) {
  const QMutexLocker lock(&mutex_);
  const bool export_result = event.subsystem == "export" && event.stage == "complete" &&
      (event.outcome == "success" || event.outcome == "failure" || event.outcome == "cancelled");
  if (!detailed_ && !export_result && event.outcome != "failure") {
    return Result<DiagnosticReceipt, Error>::success({sequence_});
  }
  QByteArray line = now() + ' ';
  if (detailed_) {
    line += "session=" + QByteArray::number(event.session_id.value) +
        " op=" + QByteArray::number(event.operation_id.value) + ' ' +
        token(event.subsystem) + '.' + token(event.stage) + ' ' +
        token(event.outcome) + " command=" + token(event.command, 24) +
        " bytes=" + QByteArray::number(event.bytes);
    if (!event.error_code.empty()) line += " code=" + token(event.error_code);
    return append(line + detailed_fields(event) + '\n');
  }
  if (export_result) {
    const auto format = field(event, "format");
    const auto range = field(event, "range");
    bool valid_ms = false;
    const auto ms = QString::fromStdString(field(event, "elapsedMs")).toULongLong(&valid_ms);
    line += token(event.command, 16) + ' ' +
        (format == "PNG" || format == "JPEG" ? QByteArray::fromStdString(format) : QByteArray("unknown")) + ' ' +
        (range == "HDR" || range == "SDR" ? QByteArray::fromStdString(range) : QByteArray("unknown")) + ' ' +
        (valid_ms ? QByteArray::number(ms) + "ms" : QByteArray("unknown")) + ' ';
  } else {
    line += token(event.subsystem, 24) + ' ';
  }
  line += event.outcome == "success" ? QByteArray("ok") : token(event.outcome, 12);
  if (event.outcome == "failure") {
    line += " code=" + token(event.error_code);
    const auto reason = field(event, "reason");
    // Stable reason-code tokens only, not OS error descriptions or user text.
    if (!reason.empty() && reason.size() <= 64 &&
        reason.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") == std::string::npos)
      line += " reason=" + QByteArray::fromStdString(reason);
  }
  return append(line + '\n');
}

std::string QtSessionDiagnosticsPort::exact_path() const {
  const QMutexLocker lock(&mutex_);
  return file_.fileName().toStdString();
}

void QtSessionDiagnosticsPort::set_detailed_logging(bool enabled) {
  const QMutexLocker lock(&mutex_);
  if (detailed_ == enabled) return;
  detailed_ = enabled;
  if (file_.isOpen())
    (void)append(now() + " logging mode=" + (enabled ? "detailed\n" : "simple\n"));
}

bool QtSessionDiagnosticsPort::detailed_logging() const {
  const QMutexLocker lock(&mutex_);
  return detailed_;
}
}  // namespace hdrshot
