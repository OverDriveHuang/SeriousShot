#include "platform/qt/update/update_catalog.hpp"
#include "platform/qt/update/update_storage.hpp"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <limits>
#include <sodium.h>
#ifndef HDRSHOT_UPDATE_PUBLIC_KEY
#define HDRSHOT_UPDATE_PUBLIC_KEY ""
#endif
#ifndef HDRSHOT_UPDATE_KEY_ID
#define HDRSHOT_UPDATE_KEY_ID ""
#endif
#ifndef HDRSHOT_UPDATE_CATALOG
#define HDRSHOT_UPDATE_CATALOG                                                                     \
  "https://raw.githubusercontent.com/OverDriveHuang/SeriousShot/main/updates/stable.json"
#endif
#ifndef HDRSHOT_UPDATE_CHANNEL
#define HDRSHOT_UPDATE_CHANNEL "stable"
#endif
#ifndef HDRSHOT_UPDATE_TESTING
#define HDRSHOT_UPDATE_TESTING 0
#endif
namespace hdrshot::update {
Trust compiled_trust() {
  return {QByteArray::fromHex(HDRSHOT_UPDATE_PUBLIC_KEY), HDRSHOT_UPDATE_KEY_ID,
          HDRSHOT_UPDATE_CHANNEL, QUrl(HDRSHOT_UPDATE_CATALOG), HDRSHOT_UPDATE_TESTING != 0};
}
bool Trust::configured() const {
  const auto expected =
      QString("https://raw.githubusercontent.com/OverDriveHuang/SeriousShot/main/updates/") +
      channel + ".json";
  return public_key.size() == crypto_sign_PUBLICKEYBYTES && !key_id.isEmpty() &&
         (channel == "stable" || testing) && catalog.toString() == expected;
}
bool allowed_url(const QUrl &url, bool catalog) {
  if (url.scheme() != "https" || !url.userInfo().isEmpty() || url.port(443) != 443)
    return false;
  const auto host = url.host().toLower();
  if (catalog)
    return host == "raw.githubusercontent.com" &&
           url.path().startsWith("/OverDriveHuang/SeriousShot/");
  return host == "github.com" || host == "api.github.com" ||
         host == "release-assets.githubusercontent.com" || host == "objects.githubusercontent.com";
}
static std::optional<std::uint64_t> number(const QJsonValue &value) {
  if (!value.isDouble())
    return {};
  const auto n = value.toDouble(-1);
  if (n < 0 || n > 9007199254740991.0 || n != static_cast<double>(static_cast<std::uint64_t>(n)))
    return {};
  return static_cast<std::uint64_t>(n);
}
Result<Catalog, Error> verify_catalog(const QByteArray &bytes, const Trust &trust) {
  using R = Result<Catalog, Error>;
  if (!trust.configured())
    return R::failure(failure("正式更新签名源尚未配置"));
  if (bytes.size() > 512 * 1024)
    return R::failure(failure("catalog_size"));
  QJsonParseError parse;
  auto doc = QJsonDocument::fromJson(bytes, &parse);
  if (parse.error != QJsonParseError::NoError || !doc.isObject())
    return R::failure(failure("catalog_json"));
  const auto envelope = doc.object();
  if (envelope["schema_version"] != 1 || envelope["key_id"] != trust.key_id)
    return R::failure(failure("catalog_schema_key"));
  const auto payloadResult = QByteArray::fromBase64Encoding(
      envelope["payload_base64"].toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
  const auto signatureResult = QByteArray::fromBase64Encoding(
      envelope["signature_base64"].toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
  if (!payloadResult || !signatureResult || payloadResult.decoded.size() > 256 * 1024 ||
      signatureResult.decoded.size() != crypto_sign_BYTES || sodium_init() < 0)
    return R::failure(failure("catalog_envelope"));
  const auto signed_bytes = QByteArray("SeriousShot.UpdateCatalog.v1") + payloadResult.decoded;
  if (crypto_sign_verify_detached(
          reinterpret_cast<const unsigned char *>(signatureResult.decoded.constData()),
          reinterpret_cast<const unsigned char *>(signed_bytes.constData()),
          static_cast<unsigned long long>(signed_bytes.size()),
          reinterpret_cast<const unsigned char *>(trust.public_key.constData())) != 0)
    return R::failure(failure("catalog_signature"));
  doc = QJsonDocument::fromJson(payloadResult.decoded, &parse);
  if (parse.error != QJsonParseError::NoError || !doc.isObject())
    return R::failure(failure("catalog_payload"));
  const auto o = doc.object();
  auto rev = number(o["revision"]);
  if (o["product"] != "SeriousShot" || o["channel"] != trust.channel || !rev || *rev == 0 ||
      !o["targets"].isArray() || o["targets"].toArray().size() > 64)
    return R::failure(failure("catalog_contract"));
  Catalog catalog;
  catalog.revision = *rev;
  catalog.digest =
      QCryptographicHash::hash(payloadResult.decoded, QCryptographicHash::Sha256).toHex();
  for (const auto v : o["targets"].toArray()) {
    if (!v.isObject())
      return R::failure(failure("candidate_object"));
    const auto t = v.toObject();
    Candidate c;
    auto version = Version::parse(t["version"].toString().toStdString());
    auto os = Version::parse(t["minimum_os"].toString().toStdString());
    auto size = number(t["asset_size"]), rid = number(t["release_id"]), aid = number(t["asset_id"]);
    if (!version || !os || !size || !rid || !aid || !t["draft"].isBool() ||
        !t["prerelease"].isBool())
      return R::failure(failure("candidate_fields"));
    c.version = version.value();
    c.minimum_os = os.value();
    c.channel = trust.channel.toStdString();
    c.platform = t["platform"].toString().toStdString();
    c.architecture = t["architecture"].toString().toStdString();
    c.sha256 = t["sha256"].toString().toStdString();
    c.asset_url = t["asset_url"].toString().toStdString();
    c.asset_name = t["asset_name"].toString().toStdString();
    c.release_tag = t["release_tag"].toString().toStdString();
    c.asset_size = *size;
    c.release_id = *rid;
    c.asset_id = *aid;
    c.draft = t["draft"].toBool();
    c.prerelease = t["prerelease"].toBool();
    c.installer_protocol = t["installer_protocol"].toInt(-1);
    const auto url = QUrl(QString::fromStdString(c.asset_url));
    const auto expected = "/OverDriveHuang/SeriousShot/releases/download/" +
                          QString::fromStdString(c.release_tag) + "/" +
                          QString::fromStdString(c.asset_name);
    if (!allowed_url(url) || url.host() != "github.com" || url.path() != expected ||
        !url.query().isEmpty() || !url.fragment().isEmpty())
      return R::failure(failure("asset_source"));
    catalog.targets.push_back(std::move(c));
  }
  return R::success(catalog);
}
Result<bool, Error> verify_release(const QJsonObject &release, const Candidate &c) {
  using R = Result<bool, Error>;
  if (number(release["id"]) != c.release_id || !release["draft"].isBool() ||
      !release["prerelease"].isBool() || release["draft"].toBool() ||
      release["prerelease"].toBool() ||
      release["tag_name"].toString().toStdString() != c.release_tag ||
      release["url"].toString() !=
          QString("https://api.github.com/repos/OverDriveHuang/SeriousShot/releases/") +
              QString::number(c.release_id))
    return R::failure(failure("Release 不是匹配的正式发布"));
  for (const auto asset : release["assets"].toArray()) {
    const auto a = asset.toObject();
    if (number(a["id"]) != c.asset_id)
      continue;
    if (a["name"].toString().toStdString() != c.asset_name || number(a["size"]) != c.asset_size ||
        a["state"] != "uploaded" ||
        a["browser_download_url"].toString().toStdString() != c.asset_url)
      return R::failure(failure("release_asset_mismatch"));
    const auto digest = a["digest"].toString();
    if (!digest.isEmpty() && digest != "sha256:" + QString::fromStdString(c.sha256))
      return R::failure(failure("release_digest"));
    return R::success(true);
  }
  return R::failure(failure("release_asset_missing"));
}
} // namespace hdrshot::update
