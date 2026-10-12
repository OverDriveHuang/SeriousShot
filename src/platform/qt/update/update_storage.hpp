#pragma once
#include "ports/update/update_ports.hpp"
#include <QFile>
#include <QJsonObject>
#include <QLockFile>
#include <QString>
#include <memory>
namespace hdrshot::update {
Error failure(const char *reason, ErrorCode code = ErrorCode::state_inconsistent,
              Retryability retry = Retryability::never);
Result<QJsonObject, Error> read_json(const QString &);
Result<bool, Error> save_json(const QString &, const QJsonObject &);
Result<QByteArray, Error> hash_file(const QString &);
QString process_identity(qint64 pid);
bool process_alive(qint64 pid, const QString &creation);
bool durable_flush(QFile &);
bool durable_directory(const QString &);
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
};
} // namespace hdrshot::update
namespace hdrshot::update {
Result<bool, Error> register_cleanup(const QString &root, const QJsonObject &transaction,
                                     qint64 helper_pid = 0);
Result<bool, Error> cleanup_managed(const QString &root);
} // namespace hdrshot::update
