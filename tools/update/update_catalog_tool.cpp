#include "platform/qt/update/update_catalog.hpp"
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>
#include <sodium.h>
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  auto args = app.arguments();
  if (sodium_init() < 0)
    return 1;
  if (args.size() == 6 && args[1] == "verify") {
    QFile envelope(args[2]), publicfile(args[3]);
    if (!envelope.open(QIODevice::ReadOnly) || !publicfile.open(QIODevice::ReadOnly))
      return 2;
    hdrshot::update::Trust trust{
        QByteArray::fromHex(publicfile.readAll().trimmed()), args[4], args[5],
        QUrl("https://raw.githubusercontent.com/OverDriveHuang/SeriousShot/main/updates/" +
             args[5] + ".json"),
        args[5] == "testing"};
    const auto valid = hdrshot::update::verify_catalog(envelope.readAll(), trust);
    if (!valid)
      return 3;
    std::printf("signature/schema verified; %zu targets; local verification only\n",
                valid.value().targets.size());
    return 0;
  }
  if (args.size() == 3 && args[1] == "generate-test-key") {
    QByteArray pk(32, 0), sk(64, 0);
    if (crypto_sign_keypair(reinterpret_cast<unsigned char *>(pk.data()),
                            reinterpret_cast<unsigned char *>(sk.data())) != 0)
      return 2;
    QFile secret(args[2] + ".secret");
    if (!secret.open(QIODevice::WriteOnly | QIODevice::NewOnly))
      return 3;
    secret.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    if (secret.write(sk) != sk.size())
      return 4;
    secret.close();
    QFile publickey(args[2] + ".public");
    if (!publickey.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
        publickey.write(pk.toHex()) != 64)
      return 5;
    sodium_memzero(sk.data(), static_cast<size_t>(sk.size()));
    std::printf("Isolated testing key created. Never trust this key in a stable build.\n");
    return 0;
  }
  if (args.size() != 6 || args[1] != "sign") {
    std::fprintf(stderr, "generate-test-key PREFIX | sign PAYLOAD_JSON SECRET_KEY KEY_ID OUTPUT\n");
    return 2;
  }
  QFile p(args[2]), key(args[3]), out(args[5]);
  if (!p.open(QIODevice::ReadOnly) || p.size() > 256 * 1024 || !key.open(QIODevice::ReadOnly) ||
      key.size() != 64)
    return 3;
  auto payload = p.readAll(), sk = key.readAll();
  const auto json = QJsonDocument::fromJson(payload);
  if (!json.isObject() || json.object()["product"] != "SeriousShot")
    return 4;
  const auto message = QByteArray("SeriousShot.UpdateCatalog.v1") + payload;
  QByteArray signature(64, 0);
  const int signed_ok =
      crypto_sign_detached(reinterpret_cast<unsigned char *>(signature.data()), nullptr,
                           reinterpret_cast<const unsigned char *>(message.constData()),
                           static_cast<unsigned long long>(message.size()),
                           reinterpret_cast<const unsigned char *>(sk.constData()));
  sodium_memzero(sk.data(), static_cast<size_t>(sk.size()));
  if (signed_ok != 0)
    return 5;
  const auto bytes =
      QJsonDocument(QJsonObject{{"schema_version", 1},
                                {"key_id", args[4]},
                                {"payload_base64", QString::fromLatin1(payload.toBase64())},
                                {"signature_base64", QString::fromLatin1(signature.toBase64())}})
          .toJson(QJsonDocument::Indented);
  if (!out.open(QIODevice::WriteOnly | QIODevice::NewOnly) || out.write(bytes) != bytes.size())
    return 6;
  return 0;
}
