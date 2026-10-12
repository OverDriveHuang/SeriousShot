#pragma once
#include "domain/update/update_policy.hpp"
#include <QByteArray>
#include <QJsonObject>
#include <QUrl>
namespace hdrshot::update {
struct Trust {
  QByteArray public_key;
  QString key_id, channel{"stable"};
  QUrl catalog;
  bool testing{};
  bool configured() const;
};
struct Catalog {
  std::vector<Candidate> targets;
  std::uint64_t revision{};
  QByteArray digest;
};
Result<Catalog, Error> verify_catalog(const QByteArray &, const Trust &);
Result<bool, Error> verify_release(const QJsonObject &, const Candidate &);
bool allowed_url(const QUrl &, bool catalog = false);
Trust compiled_trust();
} // namespace hdrshot::update
