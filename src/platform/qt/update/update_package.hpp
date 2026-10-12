#pragma once
#include "platform/qt/update/update_storage.hpp"
#include <QMap>
namespace hdrshot::update {
struct ManagedFile {
  QString path;
  QByteArray sha256;
  qint64 size{};
};
struct Package {
  QString entrypoint;
  QVector<ManagedFile> files;
  QJsonObject raw;
};
bool safe_relative(const QString &);
Result<Package, Error> package_manifest(const QString &root, const Candidate &,
                                        const QString &manifest_path = {});
Result<Package, Error> extract_zip(const QString &archive, const QString &staging,
                                   const Candidate &);
Result<bool, Error> verify_tree(const QString &, const Package &);
} // namespace hdrshot::update
