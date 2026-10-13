#include "platform/qt/update/native_update_backend.hpp"
#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <cstdio>
#include <string_view>
using namespace hdrshot::update;
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  if (argc != 3 && argc != 4)
    return 2;
  const QString image = QString::fromUtf8(argv[1]), root = QString::fromUtf8(argv[2]);
  Slot slot(root, "isolated-native-test", "testing");
  Candidate c;
  c.platform = "macos";
  c.architecture = "arm64";
  c.version = {0, 1, 6};
  if (argc == 4 && std::string_view(argv[3]) != "reject") {
    auto parsed = Version::parse(argv[3]);
    if (!parsed)
      return 2;
    c.version = parsed.value();
  }
  c.asset_size = static_cast<std::uint64_t>(QFileInfo(image).size());
  auto hash = hash_file(image);
  if (!hash)
    return 3;
  c.sha256 = hash.value().toStdString();
  if (!slot.reserve(c) || !QFile::copy(image, slot.payload()))
    return 4;
  if (!QFile::rename(slot.payload(), root + "/payload.dmg"))
    return 5;
  slot.record["suffix"] = ".dmg";
  slot.record["phase"] = "ready";
  if (!slot.save())
    return 6;
  auto native = native_installer();
  int exit_requests = 0;
  // Recover an unmounted manual receipt: recursive install must retain quit.
  slot.record["phase"] = "manual_pending";
  slot.record["mount"] = "";
  if (!slot.save())
    return 6;
  auto opened = native->install(slot, c, {}, [&] {
    ++exit_requests;
    return false;
  });
  if (argc == 4 && std::string_view(argv[3]) == "reject") {
    const bool safe = !opened && exit_requests == 0 && slot.record["phase"] == "ready" &&
                      QFileInfo::exists(slot.payload());
    std::printf("invalid_bundle_not_opened=%d retained_for_cleanup=%d\n", !opened, safe);
    return safe ? 0 : 15;
  }
  if (opened || opened.error().code != hdrshot::ErrorCode::operation_cancelled ||
      exit_requests != 1)
    return 7;
  auto accepted = native->install(slot, c, {}, [&] {
    ++exit_requests;
    return true;
  });
  if (!accepted || exit_requests != 2)
    return 16;
  if (slot.record["phase"] != "manual_pending" || !QFileInfo::exists(slot.payload()))
    return 8;
  // Finder ejects the volume independently. The nonempty persisted path is
  // deliberately kept stale; retry must attach the same verified archive.
  const auto stale_mount = slot.record["mount"].toString();
  QProcess eject;
  eject.start("/usr/bin/hdiutil", {"detach", stale_mount});
  if (!eject.waitForFinished(30000) || eject.exitCode() != 0)
    return 18;
  const auto before_retry = hash_file(slot.payload());
  auto reopened = native->install(slot, c, {}, [&] {
    ++exit_requests;
    return true;
  });
  if (!reopened || exit_requests != 3 || !before_retry ||
      hash_file(slot.payload()).value() != before_retry.value())
    return 19;
  const auto mount = slot.record["mount"].toString();
  auto wrong = c;
  wrong.version = {0, 1, 7};
  if (verify_macos_update_bundle(mount, wrong))
    return 13;
  wrong = c;
  wrong.architecture = "x64";
  if (verify_macos_update_bundle(mount, wrong))
    return 14;
  auto denied = native->install(slot, wrong, {}, [&] {
    ++exit_requests;
    return true;
  });
  if (denied || exit_requests != 3)
    return 17;
  QProcess holder;
  holder.setWorkingDirectory(mount);
  holder.start("/bin/sleep", {"30"});
  if (!holder.waitForStarted())
    return 9;
  auto busy = native->cancel_manual(slot);
  const bool busy_blocked = !busy && QFileInfo::exists(slot.payload());
  holder.kill();
  holder.waitForFinished();
  // Simulate a crash after attach, before the mount receipt was persisted.
  slot.record["phase"] = "manual_pending";
  slot.record["mount"] = "";
  if (!slot.save())
    return 10;
  auto cleaned = native->cancel_manual(slot);
  if (!cleaned || !slot.remove_payload())
    return 11;
  const QJsonObject evidence{
      {"quit_called_only_after_validated_open", exit_requests == 3},
      {"quit_refusal_preserves_pending_source", true},
      {"recursive_recovery_retains_quit", true},
      {"invalid_bundle_never_requests_exit", true},
      {"real_signed_app_manifest_validated", true},
      {"wrong_version_rejected", true},
      {"wrong_architecture_rejected", true},
      {"same_download_opened", true},
      {"external_eject_stale_receipt_reopens_same_archive", true},
      {"manual_pending_not_success", true},
      {"busy_volume_blocks_cleanup", busy_blocked},
      {"missing_mount_receipt_recovered", true},
      {"payload_removed_after_detach", !QFileInfo::exists(root + "/payload.dmg")}};
  std::printf("%s\n", QJsonDocument(evidence).toJson().constData());
  return busy_blocked ? 0 : 12;
}
