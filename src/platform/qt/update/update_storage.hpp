#pragma once
#include "ports/update/update_ports.hpp"
#include <QFile>
#include <QJsonObject>
#include <QLockFile>
#include <QString>
#include <memory>
#if defined(Q_OS_WIN) && defined(HDRSHOT_UPDATE_STORAGE_TESTING)
#include <functional>
#endif
namespace hdrshot::update {
Error failure(const char *reason, ErrorCode code = ErrorCode::state_inconsistent,
              Retryability retry = Retryability::never);
Result<QJsonObject, Error> read_json(const QString &);
Result<bool, Error> save_json(const QString &, const QJsonObject &);
#ifdef Q_OS_WIN
// Confirm an already committed record without installing another snapshot.
Result<bool, Error> confirm_json_durability(const QString &);
#ifdef HDRSHOT_UPDATE_STORAGE_TESTING
namespace storage_test {
struct Hooks {
  std::function<void(unsigned, bool)> before_attempt;
  std::function<void(unsigned)> before_retry;
  std::function<void()> after_commit;
  std::function<std::optional<unsigned long>()> flush_error;
};
void set_hooks(Hooks *);
} // namespace storage_test
#endif
#endif
Result<QByteArray, Error> hash_file(const QString &);
QString process_identity(qint64 pid);
bool process_alive(qint64 pid, const QString &creation);
bool durable_flush(QFile &);
bool durable_directory(const QString &);
QString install_result_path(const QString &root, const QString &owner, const QString &channel);
std::optional<Error> install_result_error(const QJsonObject &, const QString &owner,
                                         const QString &channel, const Candidate &);
struct Slot {
  QString root, owner, channel;
  QJsonObject record;
  std::unique_ptr<QLockFile> lock;
  explicit Slot(QString root_path, QString install, QString ch);
  Result<bool, Error> acquire();
  Result<bool, Error> recover();
  Result<bool, Error> ensure_single_payload(bool repair = false);
  Result<bool, Error> reserve(const Candidate &);
  Result<bool, Error> remove_payload();
  Result<bool, Error> save();
  bool matches(const Receipt &) const;
  Result<Receipt, Error> verified(const Candidate &);
  QString payload() const;
  void release();
  Result<bool, Error> finish_install_result(const QJsonObject &journal);
};
} // namespace hdrshot::update
namespace hdrshot::update {
Result<bool, Error> register_cleanup(const QString &root, const QJsonObject &transaction,
                                     qint64 helper_pid = 0);
Result<bool, Error> cleanup_managed(const QString &root);
} // namespace hdrshot::update
