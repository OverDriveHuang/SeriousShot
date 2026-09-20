#pragma once

#include "ports/diagnostics_port.hpp"

#include <QMutex>

#include <cstdint>
#include <string>

namespace hdrshot {

class QtJsonLinesDiagnosticsPort final : public DiagnosticsPort {
 public:
  explicit QtJsonLinesDiagnosticsPort(std::string exact_path);

  [[nodiscard]] Result<DiagnosticReceipt, Error> record(
      const DiagnosticEvent& event) override;

  [[nodiscard]] const std::string& exact_path() const noexcept { return exact_path_; }

 private:
  std::string exact_path_;
  std::uint64_t sequence_{};
  QMutex mutex_;
};

}  // namespace hdrshot
