#include "platform/qt/qt_application_restart.hpp"
#include "platform/qt/qt_gui_instance_guard.hpp"
#include "ui/qt/ui_language_runtime.hpp"
#include "test_support.hpp"
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QMessageBox>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

namespace {
using namespace hdrshot;
class Activation final : public QtWindowActivationPort {
 public:
  std::function<void(QMessageBox&)> interact;
  int count{};
  void activate(QWidget& widget) override {
    ++count;
    auto* box = qobject_cast<QMessageBox*>(&widget);
    HDRSHOT_CHECK(box && box->testOption(QMessageBox::Option::DontUseNativeDialog));
    interact(*box);
  }
};

void strict_restart_arguments() {
  HDRSHOT_CHECK(!parse_application_restart_arguments({"--help"}).value());
  HDRSHOT_CHECK(parse_application_restart_arguments({"--language-restart", "en"}).value() == UiLanguage::english);
  HDRSHOT_CHECK(parse_application_restart_arguments({"--language-restart", "zh-CN"}).value() == UiLanguage::simplified_chinese);
  for (const auto& args : std::vector<std::vector<std::string_view>>{
      {"--language-restart"}, {"--language-restart=en"}, {"--language-restart", "zh-TW"},
      {"--login", "--language-restart", "en"}, {"--language-restart", "en", "--smoke"}})
    HDRSHOT_CHECK(!parse_application_restart_arguments(args));
}

void spawn_failure_requires_explicit_retry_and_drops_destroyed_context_callbacks() {
  QtUiLanguageRuntime runtime;
  HDRSHOT_CHECK(static_cast<bool>(runtime.initialize(*qApp, UiLanguage::english, {})));
  auto request = make_application_restart_request(UiLanguage::english, runtime);
  request.executable = QDir::tempPath() + "/SeriousShot-deliberately-missing/restart";
  bool old_callback_ran = false;
  {
    QObject old_shell_context;
    QTimer::singleShot(0, qApp, [&] {
      QTimer::singleShot(0, &old_shell_context, [&] { old_callback_ran = true; });
      qApp->exit(0);
    });
    HDRSHOT_CHECK(qApp->exec() == 0 && !old_callback_ran);
  }
  Activation activation;
  activation.interact = [&](QMessageBox& box) {
    HDRSHOT_CHECK(box.objectName() == "applicationRestartFailure");
    HDRSHOT_CHECK(box.text() == request.failure);
    HDRSHOT_CHECK(!old_callback_ran);
    if (activation.count == 1)
      box.findChild<QPushButton*>("applicationRestartRetryButton")->click();
    else box.reject();
  };
  HDRSHOT_CHECK(relaunch_application(request, activation) == 1);
  HDRSHOT_CHECK(activation.count == 2 && !old_callback_ran);
}

void early_failure_is_target_language_close_only() {
  for (const auto language : {UiLanguage::english, UiLanguage::simplified_chinese}) {
    for (const bool collision : {false, true}) {
      Activation activation;
      activation.interact = [&](QMessageBox& box) {
        HDRSHOT_CHECK(box.objectName() == "applicationRestartStartupFailure");
        HDRSHOT_CHECK(box.buttons().size() == 1);
        HDRSHOT_CHECK(box.buttons().front()->text() ==
            (language == UiLanguage::english ? QStringLiteral("Close") : QStringLiteral("关闭")));
        HDRSHOT_CHECK(box.text().contains(language == UiLanguage::english
            ? (collision ? QStringLiteral("already running") : QStringLiteral("could not finish starting"))
            : (collision ? QStringLiteral("已在运行") : QStringLiteral("未能完成启动"))));
        box.reject();
      };
      show_application_restart_failure(language, collision, activation);
      HDRSHOT_CHECK(activation.count == 1);
    }
  }
}

void detached_successor_uses_same_executable_directory_environment_and_released_lock() {
  QTemporaryDir temporary;
  HDRSHOT_CHECK(temporary.isValid());
  const auto lock_path = temporary.filePath("instance.lock");
  const auto receipt = temporary.filePath("successor.txt");
  {
    QtGuiInstanceGuard old_instance(lock_path);
    HDRSHOT_CHECK(old_instance.status() == GuiInstanceStatus::owned);
  }
  qputenv("HDRSHOT_RESTART_TEST_DIRECTORY", temporary.path().toUtf8());
  QtUiLanguageRuntime runtime;
  HDRSHOT_CHECK(static_cast<bool>(runtime.initialize(*qApp, UiLanguage::english, {})));
  auto request = make_application_restart_request(UiLanguage::english, runtime);
  request.working_directory = temporary.path();
  Activation activation;
  activation.interact = [](QMessageBox& box) { box.reject(); };
  const auto result = relaunch_application(request, activation);
  qunsetenv("HDRSHOT_RESTART_TEST_DIRECTORY");
  HDRSHOT_CHECK(result == 0 && activation.count == 0);
  QElapsedTimer wait;
  wait.start();
  while (!QFile::exists(receipt) && wait.elapsed() < 5000) QThread::msleep(5);
  QFile output(receipt);
  HDRSHOT_CHECK(output.open(QIODevice::ReadOnly));
  const auto fields = output.readAll().split('\n');
  HDRSHOT_CHECK(fields.size() == 4);
  HDRSHOT_CHECK(fields[0] == QCoreApplication::applicationFilePath().toUtf8());
  const auto actual_directory = QString::fromUtf8(fields[1]);
  std::cout << "successorCwd actual=[" << actual_directory.toStdString()
            << "] requested=[" << temporary.path().toStdString()
            << "] canonical=[" << QFileInfo(actual_directory).canonicalFilePath().toStdString() << "]\n";
  HDRSHOT_CHECK(!QFileInfo(actual_directory).canonicalFilePath().isEmpty());
  HDRSHOT_CHECK(QFileInfo(actual_directory).canonicalFilePath() ==
      QFileInfo(temporary.path()).canonicalFilePath());
  HDRSHOT_CHECK(fields[2] == "en" && fields[3] == "owned");
}
} // namespace

int main(int argc, char** argv) {
  if (argc == 3 && std::string_view(argv[1]) == "--language-restart") {
    QCoreApplication child(argc, argv);
    const auto root = qEnvironmentVariable("HDRSHOT_RESTART_TEST_DIRECTORY");
    if (root.isEmpty()) return 2;
    QtGuiInstanceGuard lock(QDir(root).filePath("instance.lock"));
    QSaveFile receipt(QDir(root).filePath("successor.txt"));
    if (!receipt.open(QIODevice::WriteOnly)) return 3;
    const auto record = QCoreApplication::applicationFilePath().toUtf8() + '\n' +
        QDir::currentPath().toUtf8() + '\n' + argv[2] + '\n' +
        (lock.status() == GuiInstanceStatus::owned ? "owned" : "blocked");
    if (receipt.write(record) != record.size()) return 4;
    return receipt.commit() ? 0 : 5;
  }
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication app(argc, argv);
  app.setQuitOnLastWindowClosed(false);
  return test::run({
      {"restart arguments cannot mix with other launch modes", strict_restart_arguments},
      {"spawn failure has explicit Retry/Close and cannot revive deleted callbacks", spawn_failure_requires_explicit_retry_and_drops_destroyed_context_callbacks},
      {"child startup failures use selected language and only available Close action", early_failure_is_target_language_close_only},
      {"successor retains executable cwd environment and can acquire released lock", detached_successor_uses_same_executable_directory_environment_and_released_lock}});
}
