#pragma once
#include "application/update_coordinator.hpp"
#include "platform/qt/update/native_update_backend.hpp"
#include "platform/qt/update/update_catalog.hpp"
#include "ui/qt/update_window.hpp"
#include <QDeadlineTimer>
#include <QObject>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <atomic>
#include <memory>
class QNetworkAccessManager;
class QNetworkReply;
namespace hdrshot {
class SettingsWindow;
}
namespace hdrshot::update {
// Long-lived application service. Settings is an observer, never the download owner.
class Runtime final : public QObject, public Effects {
public:
  Runtime(Identity, std::unique_ptr<NativeInstaller>, std::function<bool()> busy,
          std::function<bool()> quit, QObject *parent = nullptr);
  ~Runtime() override;
  void start();
  void attach(SettingsWindow &);
  void show_details();
  UpdateWindow &update_window();
  Coordinator &coordinator() {
    return coordinator_;
  }
  bool barrier_active() const {
    return barrier_;
  }
  void check(const Identity &, std::function<void(Result<Checked, Error>)>) override;
  void delay(unsigned, std::function<void()>) override;
  void acquire_and_download(const Candidate &, std::function<void(std::uint64_t, std::uint64_t)>,
                            std::function<void(Result<Receipt, Error>)>) override;
  void cancel_download(Done) override;
  void persist_prompt(const std::string &, Done) override;
  bool was_prompted(const std::string &) const override;
  bool busy() const override;
  void ask_install(const Candidate &, std::function<void(bool)>) override;
  void install(const Candidate &, const Receipt &, Done) override;
  bool manual_install() const override;

private:
  friend class RuntimeTestAccess;
  void get(const QUrl &, const QByteArray &,
           std::function<void(Result<QByteArray, Error>, QByteArray)>, int redirects = 0);
  void check_worker(const Identity &, std::function<void(Result<Checked, Error>)>);
  void download_worker(Candidate, std::function<void(std::uint64_t, std::uint64_t)>,
                       std::function<void(Result<Receipt, Error>)>);
  void ui(std::function<void()>);
  void work(std::function<void()>);
  void render(const View &);
  Identity identity_;
  std::unique_ptr<NativeInstaller> native_;
  Trust trust_;
  Slot slot_;
  std::function<bool()> busy_, quit_;
  Coordinator coordinator_;
  QThread thread_;
  QObject *worker_{};
  QNetworkAccessManager *network_{};
  QPointer<QNetworkReply> reply_;
  QPointer<SettingsWindow> settings_;
  std::unique_ptr<UpdateWindow> update_window_;
  std::function<void(bool)> confirmation_;
  std::uint64_t confirmation_generation_{};
  QJsonObject state_, cache_;
  QString state_path_, cache_path_;
  QByteArray envelope_;
  QTimer timer_;
  std::function<void(Result<Receipt, Error>)> download_done_;
  std::unique_ptr<QFile> writer_;
  std::uint64_t received_{};
  std::optional<Error> download_error_;
  std::uint64_t operation_{};
  std::atomic_bool stopped_{false};
  QDeadlineTimer metadata_deadline_;
  QDeadlineTimer download_deadline_;
  int download_timeout_ms_{2 * 60 * 60 * 1000};
  bool barrier_{};
};
Identity runtime_identity(const QString &install);
} // namespace hdrshot::update
