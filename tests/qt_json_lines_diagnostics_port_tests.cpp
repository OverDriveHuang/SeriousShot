#include "platform/qt/qt_json_lines_diagnostics_port.hpp"
#include "test_support.hpp"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <vector>

namespace {

using namespace hdrshot;

void writes_stable_safe_json_lines() {
  QTemporaryDir directory;
  HDRSHOT_CHECK(directory.isValid());
  const auto path = directory.filePath(QStringLiteral("logs/runtime.jsonl"));
  QtJsonLinesDiagnosticsPort diagnostics(path.toStdString());
  const auto receipt = diagnostics.record(DiagnosticEvent{
      SessionId{7},
      OperationId{11},
      "export",
      "save_default",
      "publish",
      "failure",
      "path_not_writable",
      {{"errno", "13"}},
      "example.png",
      0,
  });
  HDRSHOT_CHECK(receipt.has_value());
  HDRSHOT_CHECK(receipt.value().sequence == 1U);

  QFile file(path);
  HDRSHOT_CHECK(file.open(QIODevice::ReadOnly | QIODevice::Text));
  const auto document = QJsonDocument::fromJson(file.readLine());
  HDRSHOT_CHECK(document.isObject());
  const auto object = document.object();
  HDRSHOT_CHECK(object.value("sessionId").toInteger() == 7);
  HDRSHOT_CHECK(object.value("operationId").toInteger() == 11);
  HDRSHOT_CHECK(object.value("stage").toString() == QStringLiteral("publish"));
  HDRSHOT_CHECK(object.value("errorCode").toString() ==
                QStringLiteral("path_not_writable"));
  HDRSHOT_CHECK(object.value("safeContext").toObject().value("errno").toString() ==
                QStringLiteral("13"));
  HDRSHOT_CHECK(!object.contains("pixels"));
  HDRSHOT_CHECK(!object.contains("text"));
  HDRSHOT_CHECK(!object.contains("destinationPath"));
  HDRSHOT_CHECK(object.value("destinationName").toString() ==
                QStringLiteral("example.png"));
}

}  // namespace

int main() {
  using hdrshot::test::TestCase;
  return hdrshot::test::run(std::vector<TestCase>{
      {"P10 diagnostics writes safe JSONL", writes_stable_safe_json_lines},
  });
}
