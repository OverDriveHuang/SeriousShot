#pragma once
#include "platform/qt/update/update_catalog.hpp"
namespace hdrshot::update {
struct ApplicationPaths { QString data, config, pictures; };
inline bool isolated_testing(const Trust &trust) {
  return trust.testing && trust.channel == "testing";
}
inline QString testing_namespace(const QString &base, const Trust &trust) {
  return isolated_testing(trust) ? base + "/testing" : base;
}
// Versions share a single isolated testing installation identity and settings
// root, so A->B upgrades preserve test settings without accessing daily data.
inline ApplicationPaths application_paths(const QString &data, const QString &config,
                                         const QString &pictures, const Trust &trust) {
  if (isolated_testing(trust)) {
    const auto root = data + "/testing";
    return {root, root + "/settings.ini", root + "/Pictures"};
  }
  return {data, config, pictures};
}
} // namespace hdrshot::update
