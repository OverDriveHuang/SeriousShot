#pragma once

#include "ports/diagnostics_port.hpp"
#include <QFile>
#include <QMutex>
#include <string>

namespace hdrshot {

// Shared Mac/Windows disk executor. Hosts supply only their log directory,
// build identity and platform; no screenshot or native window logic lives here.
class QtSessionDiagnosticsPort final : public DiagnosticsPort {
 public:
  explicit QtSessionDiagnosticsPort(std::string directory);
  [[nodiscard]] Result<DiagnosticReceipt, Error> start(
      const std::string& build, const std::string& platform);
  [[nodiscard]] Result<DiagnosticReceipt, Error> record(
      const DiagnosticEvent& event) override;
  [[nodiscard]] std::string exact_path() const;
  void set_detailed_logging(bool enabled) override;
  [[nodiscard]] bool detailed_logging() const override;

 private:
  Result<DiagnosticReceipt, Error> append(const QByteArray& line);
  std::string directory_;
  QFile file_;
  std::uint64_t sequence_{};
  bool detailed_{};
  mutable QMutex mutex_;
};

}  // namespace hdrshot
