#include "application/update_coordinator.hpp"
#include "platform/qt/update/update_catalog.hpp"
#include "platform/qt/update/update_package.hpp"
#include "platform/qt/update/application_paths.hpp"
#include "test_support.hpp"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <archive.h>
#include <archive_entry.h>
#include <sodium.h>
#undef HDRSHOT_CHECK
#define HDRSHOT_CHECK(...) ::hdrshot::test::check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__)
using namespace hdrshot;
using namespace hdrshot::update;
Candidate candidate() {
  Candidate c;
  c.channel = "stable";
  c.platform = "macos";
  c.architecture = "arm64";
  c.version = {0, 1, 5};
  c.minimum_os = {15, 5, 0};
  c.asset_size = 1;
  c.release_id = 123;
  c.asset_id = 456;
  c.release_tag = "v0.1.5";
  c.asset_name = "SeriousShot.dmg";
  c.asset_url = "https://github.com/OverDriveHuang/SeriousShot/releases/"
                "download/v0.1.5/SeriousShot.dmg";
  c.sha256 = QCryptographicHash::hash("x", QCryptographicHash::Sha256).toHex().toStdString();
  return c;
}
Identity identity() {
  Identity i;
  i.version = {0, 1, 4};
  i.os = {27, 0, 0};
  i.platform = "macos";
  i.architecture = "arm64";
  i.install = "test-only";
  return i;
}
struct Fake final : Effects {
  std::vector<std::function<void(Result<Checked, Error>)>> checks;
  std::vector<std::function<void()>> delays;
  std::function<void(Result<Receipt, Error>)> downloaded;
  std::function<void(bool)> question;
  Done installed, pending_prompt;
  bool hold_prompt{};
  std::string prompted;
  bool busy_flag{}, fail_prompt{};
  std::optional<Error> prior_install;
  std::optional<Error> previous_install_error(const Candidate &) const override {return prior_install;}
  int downloads{}, questions{}, installs{};
  void check(const Identity &, std::function<void(Result<Checked, Error>)> d) override {
    checks.push_back(d);
  }
  void delay(unsigned, std::function<void()> d) override { delays.push_back(d); }
  void acquire_and_download(const Candidate &, std::function<void(std::uint64_t, std::uint64_t)>,
                            std::function<void(Result<Receipt, Error>)> d) override {
    ++downloads;
    downloaded = d;
  }
  void cancel_download(Done d) override { d(Result<bool, Error>::success(true)); }
  void persist_prompt(const std::string &k, Done done) override {
    if (fail_prompt) {
      done(Result<bool, Error>::failure(failure("disk_full")));
      return;
    }
    prompted = k;
    if (hold_prompt) {
      pending_prompt = std::move(done);
      return;
    }
    done(Result<bool, Error>::success(true));
  }
  bool was_prompted(const std::string &k) const override { return prompted == k; }
  bool busy() const override { return busy_flag; }
  void ask_install(const Candidate &, std::function<void(bool)> d) override {
    ++questions;
    question = d;
  }
  void install(const Candidate &, const Receipt &, Done d) override {
    ++installs;
    installed = d;
  }
  bool manual_install() const override { return true; }
  void checked() { checks.back()(Result<Checked, Error>::success({candidate()})); }
  void ready() { downloaded(Result<Receipt, Error>::success({candidate().key(), 1})); }
};
QByteArray sign_payload(const QJsonObject &payload, const QByteArray &key) {
  const auto bytes = QJsonDocument(payload).toJson(QJsonDocument::Compact),
             message = QByteArray("SeriousShot.UpdateCatalog.v1") + bytes;
  QByteArray sig(crypto_sign_BYTES, 0);
  crypto_sign_detached(reinterpret_cast<unsigned char *>(sig.data()), nullptr,
                       reinterpret_cast<const unsigned char *>(message.constData()),
                       static_cast<unsigned long long>(message.size()),
                       reinterpret_cast<const unsigned char *>(key.constData()));
  return QJsonDocument(QJsonObject{{"schema_version", 1},
                                   {"key_id", "test"},
                                   {"payload_base64", QString::fromLatin1(bytes.toBase64())},
                                   {"signature_base64", QString::fromLatin1(sig.toBase64())}})
      .toJson();
}
QJsonObject target(const Candidate &c) {
  return {{"version", QString::fromStdString(c.version.text())},
          {"minimum_os", "15.5.0"},
          {"platform", QString::fromStdString(c.platform)},
          {"architecture", QString::fromStdString(c.architecture)},
          {"asset_url", QString::fromStdString(c.asset_url)},
          {"asset_name", QString::fromStdString(c.asset_name)},
          {"sha256", QString::fromStdString(c.sha256)},
          {"release_tag", QString::fromStdString(c.release_tag)},
          {"asset_size", double(c.asset_size)},
          {"release_id", double(c.release_id)},
          {"asset_id", double(c.asset_id)},
          {"draft", false},
          {"prerelease", false},
          {"installer_protocol", 1}};
}
void zip(const QString &path, const QList<QPair<QString, QByteArray>> &entries,
         bool symlink = false) {
  auto *w = archive_write_new();
  archive_write_set_format_zip(w);
#ifdef Q_OS_WIN
  HDRSHOT_CHECK(archive_write_open_filename_w(w, reinterpret_cast<const wchar_t *>(path.utf16())) ==
                ARCHIVE_OK);
#else
  HDRSHOT_CHECK(archive_write_open_filename(w, QFile::encodeName(path).constData()) == ARCHIVE_OK);
#endif
  for (const auto &f : entries) {
    auto *e = archive_entry_new();
    archive_entry_set_pathname(e, f.first.toUtf8().constData());
    archive_entry_set_size(e, f.second.size());
    archive_entry_set_filetype(e, symlink ? AE_IFLNK : AE_IFREG);
    archive_entry_set_perm(e, 0644);
    if (symlink)
      archive_entry_set_symlink(e, "../outside");
    HDRSHOT_CHECK(archive_write_header(w, e) == ARCHIVE_OK);
    if (!symlink)
      HDRSHOT_CHECK(archive_write_data(w, f.second.constData(),
                                       static_cast<size_t>(f.second.size())) == f.second.size());
    archive_entry_free(e);
  }
  archive_write_close(w);
  archive_write_free(w);
}
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  HDRSHOT_CHECK(sodium_init() >= 0);
  using hdrshot::test::run;
  return run({
      {"schema one clients at 0.1.5 and 0.1.6 accept future and skipped versions", [] {
         QByteArray pk(32, 0), sk(64, 0);
         crypto_sign_keypair(reinterpret_cast<unsigned char *>(pk.data()), reinterpret_cast<unsigned char *>(sk.data()));
         Trust trust{pk, "test", "stable", QUrl("https://raw.githubusercontent.com/OverDriveHuang/SeriousShot/main/updates/stable.json"), false};
         for (const auto installed : {Version{0,1,5}, Version{0,1,6}}) {
           for (const auto future : {Version{0,1,7}, Version{0,1,8}}) {
             auto c = candidate(); c.version = future; c.release_tag = "v" + future.text();
             c.asset_url = "https://github.com/OverDriveHuang/SeriousShot/releases/download/" + c.release_tag + "/" + c.asset_name;
             const auto signed_catalog = sign_payload({{"product", "SeriousShot"}, {"channel", "stable"}, {"revision", 4}, {"targets", QJsonArray{target(c)}}}, sk);
             const auto verified = verify_catalog(signed_catalog, trust);
             HDRSHOT_CHECK(verified);
             auto i = identity(); i.version = installed;
             const auto selected = select(verified.value().targets, i);
             HDRSHOT_CHECK(selected && selected.value() && selected.value()->version == future && selected.value()->version > installed);
           }
         }
       }},
      {"same version different full metadata conflicts even with identical key", [] {
         auto a = candidate(), b = a; ++b.asset_id;
         HDRSHOT_CHECK(a.key() == b.key() && a != b);
         HDRSHOT_CHECK(!select({a,b}, identity()));
       }},
      {"rolled-back result survives cleanup and cannot cross owner or candidate",
       [] {
         QTemporaryDir tmp;auto c=candidate();Slot slot(tmp.path(),"install-a","stable");
         QJsonObject j{{"owner",slot.owner},{"channel",slot.channel},
           {"candidate",QString::fromStdString(c.key())},{"phase","rolled_back"},
           {"old_manifest",QJsonObject{{"version","0.1.4"}}},{"failure_reason","replace_move"},
           {"failure_context",QJsonObject{{"native_error","3"}}}};
         HDRSHOT_CHECK(slot.finish_install_result(j));
         HDRSHOT_CHECK(cleanup_managed(tmp.path()));
         auto result=read_json(install_result_path(tmp.path(),slot.owner,slot.channel));HDRSHOT_CHECK(result);
         auto error=install_result_error(result.value(),slot.owner,slot.channel,c);
         HDRSHOT_CHECK(error && error->safe_context.at("reason").find("0.1.4")!=std::string::npos);
         HDRSHOT_CHECK(error->safe_context.at("native_error")=="3");
         HDRSHOT_CHECK(!install_result_error(result.value(),"install-b",slot.channel,c));
         auto other=c;other.sha256=std::string(64,'0');
         HDRSHOT_CHECK(!install_result_error(result.value(),slot.owner,slot.channel,other));
         j["phase"]="complete";j["candidate"]=QString::fromStdString(other.key());
         HDRSHOT_CHECK(slot.finish_install_result(j) && QFileInfo::exists(install_result_path(tmp.path(),slot.owner,slot.channel)));
         j["candidate"]=QString::fromStdString(c.key());
         HDRSHOT_CHECK(slot.finish_install_result(j) && !QFileInfo::exists(install_result_path(tmp.path(),slot.owner,slot.channel)));
       }},
      {"rollback explanation persists through restart and seven-day reminders",
       [] {
         Fake f;f.prior_install=failure("更新失败，已恢复 0.1.4。无法替换应用文件");
         Coordinator c(identity(),f);c.restore(candidate(),{candidate().key(),1},false);
         HDRSHOT_CHECK(c.view().error && c.view().detail.find("已恢复 0.1.4")!=std::string::npos);
         c.trigger(100,true);HDRSHOT_CHECK(f.questions==1 && c.view().error);
         f.question(false);c.tick(100+604799);HDRSHOT_CHECK(f.questions==1);
         // Complete the ordinary scheduled metadata check before the due tick.
         if(!f.checks.empty()){f.checked();f.ready();}
         c.tick(100+604800);HDRSHOT_CHECK(f.questions==2 && c.view().error);
         f.question(false);c.trigger(100+604801,false,true);f.checked();f.ready();
         HDRSHOT_CHECK(c.view().error && c.view().detail.find("已恢复 0.1.4")!=std::string::npos);
       }},
      {"testing paths isolate daily settings and remain stable across versions",
       [] {
         Trust trust; trust.testing=true; trust.channel="testing";
         const auto a=application_paths("app-data","config/settings.ini","Pictures/SeriousShot",trust);
         // The path policy accepts trust and base locations only; product
         // version is not an input, so upgrades use the same namespace.
         const auto b=application_paths("app-data","config/settings.ini","Pictures/SeriousShot",trust);
         HDRSHOT_CHECK(a.data==b.data && a.config==b.config && a.pictures==b.pictures);
         HDRSHOT_CHECK(a.config=="app-data/testing/settings.ini" && a.pictures=="app-data/testing/Pictures");
         HDRSHOT_CHECK(testing_namespace("updates",trust)=="updates/testing");
         trust.testing=false;trust.channel="stable";
         const auto normal=application_paths("app-data","config/settings.ini","Pictures/SeriousShot",trust);
         HDRSHOT_CHECK(normal.data=="app-data" && normal.config=="config/settings.ini" && normal.pictures=="Pictures/SeriousShot");
         HDRSHOT_CHECK(testing_namespace("updates",trust)=="updates");
       }},
      {"strict version and lexicographic comparison",
       [] {
         for (const auto s : {"v0.1.5", "0.1", "0.01.5", "0.1.05", "-1.1.1", "0.1.5-beta",
                              "4294967296.0.0", "1.2.3.4", "1.2.3\n"})
           HDRSHOT_CHECK(!Version::parse(s));
         HDRSHOT_CHECK(Version::parse("0.1.9").value() < Version::parse("0.2.0").value());
         HDRSHOT_CHECK(Version::parse("0.10.0").value() > Version::parse("0.2.9").value());
       }},
      {"platform stable highest compatible candidate",
       [] {
         auto a = candidate(), b = a;
         b.version = {0, 9, 0};
         b.release_tag = "v0.9.0";
         b.prerelease = true;
         auto result = select({a, b}, identity());
         HDRSHOT_CHECK(result && result.value()->version == a.version);
         b.prerelease = false;
         b.minimum_os = {99, 0, 0};
         result = select({a, b}, identity());
         HDRSHOT_CHECK(result.value()->version == a.version);
         b.minimum_os = {0, 0, 0};
         b.architecture = "x64";
         HDRSHOT_CHECK(select({a, b}, identity()).value()->version == a.version);
       }},
      {"one shared round, three attempts, no fourth on tick",
       [] {
         Fake f;
         Coordinator c(identity(), f);
         c.trigger(100, true);
         c.trigger(100, false, true);
         HDRSHOT_CHECK(f.checks.size() == 1);
         for (unsigned n = 0; n < 3; ++n) {
           f.checks.back()(Result<Checked, Error>::failure(
               failure("offline", ErrorCode::precondition_failed, Retryability::same_input)));
           if (n < 2)
             f.delays.back()();
         }
         HDRSHOT_CHECK(f.checks.size() == 3);
         HDRSHOT_CHECK(c.view().phase == Phase::check_failed);
         c.tick(101);
         HDRSHOT_CHECK(f.checks.size() == 3);
       }},
      {"second attempt success stops retry",
       [] {
         Fake f;
         Coordinator c(identity(), f);
         c.trigger(100, true);
         f.checks.back()(Result<Checked, Error>::failure(
             failure("stage2", ErrorCode::precondition_failed, Retryability::same_input)));
         f.delays.back()();
         f.checked();
         f.ready();
         HDRSHOT_CHECK(f.checks.size() == 2 && f.downloads == 1 && f.questions == 1);
       }},
      {"busy, new-process reminder, missing install package auto-redownload",
       [] {
         Fake f;
         f.busy_flag = true;
         Coordinator c(identity(), f);
         c.trigger(100, true);
         f.checked();
         f.ready();
         HDRSHOT_CHECK(f.questions == 0);
         f.busy_flag = false;
         c.tick(101);
         HDRSHOT_CHECK(f.questions == 1);
         f.question(false);
         HDRSHOT_CHECK(c.view().phase == Phase::deferred);
         Coordinator restarted(identity(), f);
         restarted.trigger(200, true);
         f.checked();
         f.ready();
         HDRSHOT_CHECK(f.questions == 2);
         restarted.install();
         HDRSHOT_CHECK(f.installs == 1);
         f.installed(Result<bool, Error>::failure(failure("missing", ErrorCode::object_not_found)));
         HDRSHOT_CHECK(f.downloads == 3);
         f.ready();
         HDRSHOT_CHECK(f.installs == 1 && f.questions == 2 &&
                       restarted.view().phase == Phase::deferred);
       }},
      {"cancellation rejects stale metadata and lifetime callbacks",
       [] {
         Fake f;
         {
           Coordinator c(identity(), f);
           c.trigger(1, true);
           c.cancel();
           f.checked();
           HDRSHOT_CHECK(f.downloads == 0);
         }
         f.checked();
         HDRSHOT_CHECK(f.downloads == 0);
       }},
      {"higher incompatible version never shows current or install",
       [] {
         Fake f;
         Coordinator c(identity(), f);
         c.trigger(100, true);
         auto incompatible = candidate();
         incompatible.minimum_os = {99, 0, 0};
         f.checks.back()(Result<Checked, Error>::success({{}, incompatible}));
         HDRSHOT_CHECK(c.view().detail.find("0.1.5") != std::string::npos);
         HDRSHOT_CHECK(c.view().last_check == 100 && c.view().last_success == 100);
         HDRSHOT_CHECK(f.downloads == 0);
         c.install();
         HDRSHOT_CHECK(f.installs == 0);
       }},
      {"cleanup independent from slot generation and survives refused path",
       [] {
         QTemporaryDir tmp;
         const auto root = tmp.path() + "/updates", target = tmp.path() + "/install",
                    tx = QString("transaction-a");
         QDir().mkpath(target);
         const auto runtime = root + "/helpers/" + tx,
                    backup = target + "/.seriousshot-backup-" + tx,
                    stage = target + "/.seriousshot-stage-" + tx;
         QDir().mkpath(runtime);
         QDir().mkpath(backup);
         QDir().mkpath(stage);
         QJsonObject j{{"transaction", tx},
                       {"target", target},
                       {"backup", backup},
                       {"staging", stage},
                       {"phase", "complete"}};
         HDRSHOT_CHECK(save_json(runtime + "/transaction.json", j));
         HDRSHOT_CHECK(register_cleanup(root, j, QCoreApplication::applicationPid()));
         HDRSHOT_CHECK(cleanup_managed(root));
         HDRSHOT_CHECK(QFileInfo::exists(runtime));
         HDRSHOT_CHECK(register_cleanup(root, j, 99999999));
         Slot next(root, "next-install", "testing");
         auto c = candidate();
         HDRSHOT_CHECK(next.reserve(c));
         next.release();
         HDRSHOT_CHECK(cleanup_managed(root));
         HDRSHOT_CHECK(!QFileInfo::exists(runtime) && !QFileInfo::exists(backup) &&
                       !QFileInfo::exists(stage));
         HDRSHOT_CHECK(QDir(root + "/cleanup").entryList({"*.json"}, QDir::Files).isEmpty());
       }},
      {"legacy prompt storage failure cannot suppress a process reminder",
       [] {
         Fake f;
         f.fail_prompt = true;
         Coordinator c(identity(), f);
         c.trigger(1, true);
         f.checked();
         f.ready();
         HDRSHOT_CHECK(f.questions == 1 && f.installs == 0 && c.view().phase == Phase::ready);
       }},
      {"no TTL and bounded seven-day schedule",
       [] {
         Schedule s;
         HDRSHOT_CHECK(s.due(100, true));
         s.begin(100);
         HDRSHOT_CHECK(!s.due(100 + 604799, false));
         HDRSHOT_CHECK(s.due(100 + 604800, false));
         HDRSHOT_CHECK(s.due(99, false));
         s.begin(99);
         HDRSHOT_CHECK(!s.due(99, false));
       }},
      {"Ed25519 protocol domain, wrong key, tamper, unconfigured fail closed",
       [] {
         QByteArray pk(32, 0), sk(64, 0);
         crypto_sign_keypair(reinterpret_cast<unsigned char *>(pk.data()),
                             reinterpret_cast<unsigned char *>(sk.data()));
         Trust trust{pk, "test", "stable",
                     QUrl("https://raw.githubusercontent.com/OverDriveHuang/"
                          "SeriousShot/main/"
                          "updates/stable.json"),
                     true};
         auto c = candidate();
         QJsonObject payload{{"product", "SeriousShot"},
                             {"channel", "stable"},
                             {"revision", 1},
                             {"targets", QJsonArray{target(c)}}};
         auto bytes = sign_payload(payload, sk);
         auto catalog = verify_catalog(bytes, trust);
         HDRSHOT_CHECK(catalog && catalog.value().targets.size() == 1);
         auto bad = QJsonDocument::fromJson(bytes).object();
         bad["payload_base64"] = QString::fromLatin1(QByteArray("{}").toBase64());
         HDRSHOT_CHECK(!verify_catalog(QJsonDocument(bad).toJson(), trust));
         trust.public_key[0] = char(trust.public_key[0] ^ 1);
         HDRSHOT_CHECK(!verify_catalog(bytes, trust));
         HDRSHOT_CHECK(!verify_catalog(bytes, compiled_trust()));
       }},
      {"Release truth rejects prerelease draft wrong asset",
       [] {
         auto c = candidate();
         auto asset = QJsonObject{{"id", 456},
                                  {"name", QString::fromStdString(c.asset_name)},
                                  {"size", 1},
                                  {"state", "uploaded"},
                                  {"browser_download_url", QString::fromStdString(c.asset_url)},
                                  {"digest", "sha256:" + QString::fromStdString(c.sha256)}};
         QJsonObject release{{"id", 123},
                             {"url", "https://api.github.com/repos/"
                                     "OverDriveHuang/SeriousShot/releases/123"},
                             {"tag_name", "v0.1.5"},
                             {"draft", false},
                             {"prerelease", false},
                             {"assets", QJsonArray{asset}}};
         HDRSHOT_CHECK(verify_release(release, c));
         release["prerelease"] = true;
         HDRSHOT_CHECK(!verify_release(release, c));
         release["prerelease"] = false;
         release["draft"] = true;
         HDRSHOT_CHECK(!verify_release(release, c));
       }},
      {"slot generation, no foreign receipt, deferred replacement, stale "
       "writer recovery",
       [] {
         QTemporaryDir tmp;
         Slot a(tmp.path(), "install-a", "stable"), b(tmp.path(), "install-b", "testing");
         auto c = candidate();
         HDRSHOT_CHECK(a.reserve(c));
         QFile file(a.payload());
         HDRSHOT_CHECK(file.open(QIODevice::WriteOnly | QIODevice::NewOnly));
         HDRSHOT_CHECK(file.write("x") == 1);
         file.close();
         HDRSHOT_CHECK(QFile::rename(a.payload(), tmp.path() + "/payload.dmg"));
         a.record["suffix"] = ".dmg";
         a.record["phase"] = "ready";
         HDRSHOT_CHECK(a.save());
         auto receipt = a.verified(c);
         HDRSHOT_CHECK(receipt);
         a.release();
         HDRSHOT_CHECK(b.reserve(c));
         HDRSHOT_CHECK(b.record["generation"].toString().toULongLong() == 2);
         HDRSHOT_CHECK(!b.matches(receipt.value()));
         HDRSHOT_CHECK(!QFileInfo::exists(tmp.path() + "/payload.dmg"));
         b.release();
         HDRSHOT_CHECK(a.acquire());
         HDRSHOT_CHECK(a.record["phase"] == "empty");
       }},
      {"slot reservation parent-helper both-dead recovery and live-owner "
       "barrier",
       [] {
         QTemporaryDir tmp;
         Slot a(tmp.path(), "install", "stable");
         auto c = candidate();
         HDRSHOT_CHECK(a.reserve(c));
         a.record["phase"] = "reserved";
         a.record["parent_pid"] = QString::number(QCoreApplication::applicationPid());
         a.record["parent_creation"] = process_identity(QCoreApplication::applicationPid());
         HDRSHOT_CHECK(a.save());
         a.release();
         HDRSHOT_CHECK(!a.acquire());
         a.record["parent_pid"] = "99999999";
         a.record["helper_pid"] = "99999998";
         a.record["parent_creation"] = "dead";
         a.record["helper_creation"] = "dead";
         HDRSHOT_CHECK(a.save());
         a.release();
         HDRSHOT_CHECK(!a.acquire());
         HDRSHOT_CHECK(a.record["phase"] == "reserved");
         const auto journal=tmp.path()+"/transaction.json";
         HDRSHOT_CHECK(save_json(journal,{{"phase","complete"}}));
         a.record["journal"]=journal;
         HDRSHOT_CHECK(a.save());a.release();
         HDRSHOT_CHECK(!a.acquire());
         HDRSHOT_CHECK(a.record["phase"]=="reserved");
       }},
      {"manual mounted slot blocks another payload",
       [] {
         QTemporaryDir tmp;
         Slot a(tmp.path(), "install", "stable");
         HDRSHOT_CHECK(a.reserve(candidate()));
         a.record["phase"] = "manual_pending";
         HDRSHOT_CHECK(a.save());
         a.release();
         Slot b(tmp.path(), "other", "testing");
         HDRSHOT_CHECK(!b.reserve(candidate()));
         HDRSHOT_CHECK(!QFileInfo::exists(tmp.path() + "/payload.part"));
       }},
      {"ready plus orphan part converges, conflicting complete packages fail "
       "closed",
       [] {
         QTemporaryDir tmp;
         Slot slot(tmp.path(), "test", "stable");
         auto c = candidate();
         HDRSHOT_CHECK(slot.reserve(c));
         QFile complete(tmp.path() + "/payload.dmg");
         HDRSHOT_CHECK(complete.open(QIODevice::WriteOnly));
         complete.write("x");
         complete.close();
         slot.record["phase"] = "ready";
         slot.record["suffix"] = ".dmg";
         HDRSHOT_CHECK(slot.save());
         slot.release();
         QFile part(tmp.path() + "/payload.part");
         HDRSHOT_CHECK(part.open(QIODevice::WriteOnly));
         part.write("orphan");
         part.close();
         HDRSHOT_CHECK(!slot.verified(c));
         HDRSHOT_CHECK(slot.acquire() && slot.verified(c));
         HDRSHOT_CHECK(!QFileInfo::exists(tmp.path() + "/payload.part"));
         slot.release();
         QFile extra(tmp.path() + "/payload.zip");
         HDRSHOT_CHECK(extra.open(QIODevice::WriteOnly));
         extra.write("other");
         extra.close();
         HDRSHOT_CHECK(!slot.acquire());
       }},
      {"manual source preserved; platform link semantics and blocked deletion fail closed",
       [] {
         QTemporaryDir tmp;
         Slot s(tmp.path(), "test", "stable");
         auto c = candidate();
         HDRSHOT_CHECK(s.reserve(c));
         QFile f(tmp.path() + "/payload.dmg");
         HDRSHOT_CHECK(f.open(QIODevice::WriteOnly));
         f.write("x");
         f.close();
         s.record["suffix"] = ".dmg";
         s.record["phase"] = "manual_pending";
         s.record["mount"] = "keep-me";
         HDRSHOT_CHECK(s.save());
         s.release();
         QFile part(tmp.path() + "/payload.part");
         HDRSHOT_CHECK(part.open(QIODevice::WriteOnly));
         part.write("old");
         part.close();
         HDRSHOT_CHECK(s.acquire());
         HDRSHOT_CHECK(QFileInfo::exists(s.payload()) && s.record["mount"] == "keep-me" &&
                       !QFileInfo::exists(part.fileName()));
         s.release();
#ifdef Q_OS_WIN
         // QFile::link requires an explicit .lnk extension on Windows;
         // a Shell shortcut is not a filesystem link at payload.part.
         const auto shortcut = part.fileName() + ".lnk";
         HDRSHOT_CHECK(QFile::link(s.payload(), shortcut));
         HDRSHOT_CHECK(QFileInfo::exists(shortcut) && !QFileInfo::exists(part.fileName()));
         HDRSHOT_CHECK(s.acquire() && QFileInfo::exists(s.payload()));
         HDRSHOT_CHECK(QFile::remove(shortcut));
#else
         HDRSHOT_CHECK(QFile::link(s.payload(), part.fileName()));
         HDRSHOT_CHECK(!s.acquire());
         HDRSHOT_CHECK(QFile::remove(part.fileName()));
#endif
         s.release();
         HDRSHOT_CHECK(QDir().mkpath(part.fileName() + "/child"));
         HDRSHOT_CHECK(!s.acquire());
         HDRSHOT_CHECK(QFileInfo::exists(s.payload()));
       }},
      {"empty orphan payloads reclaimed before new writer",
       [] {
         QTemporaryDir tmp;
         Slot s(tmp.path(), "test", "stable");
         HDRSHOT_CHECK(s.reserve(candidate()));
         s.record["phase"] = "empty";
         HDRSHOT_CHECK(s.save());
         s.release();
         for (const auto suffix : {".part", ".zip", ".dmg"}) {
           QFile f(tmp.path() + "/payload" + suffix);
           HDRSHOT_CHECK(f.open(QIODevice::WriteOnly));
           f.write("old");
         }
         HDRSHOT_CHECK(s.reserve(candidate()));
         for (const auto suffix : {".part", ".zip", ".dmg"})
           HDRSHOT_CHECK(!QFileInfo::exists(tmp.path() + "/payload" + suffix));
       }},
      {"busy postpones reminder consumption and publishes idle before asking",
       [] {
         Fake f;
         f.busy_flag = true;
         Coordinator c(identity(), f);
         c.trigger(100, true);
         f.checked();
         f.ready();
         HDRSHOT_CHECK(f.questions == 0 && !c.view().prompted);
         f.busy_flag = false;
         c.tick(101);
         HDRSHOT_CHECK(!c.view().busy && f.questions == 1);
         f.busy_flag = true;
         c.tick(102);
         HDRSHOT_CHECK(c.view().busy);
         f.busy_flag = false;
         c.tick(103);
         HDRSHOT_CHECK(!c.view().busy && f.questions == 1);
       }},
      {"legacy manual pending asks at cold-start clock without check or download",
       [] {
         Fake f;
         f.prompted = candidate().key();
         Coordinator c(identity(), f);
         c.restore(candidate(), {candidate().key(), 3}, true);
         HDRSHOT_CHECK(f.questions == 0);
         c.trigger(1791821000, true);
         HDRSHOT_CHECK(f.questions == 1 && f.checks.empty() && f.downloads == 0);
         c.tick(1791821001);
         HDRSHOT_CHECK(f.questions == 1 && c.view().phase == Phase::manual_pending);
         f.question(false);
         for (int i = 0; i < 10; ++i)
           c.tick(1791821002 + i);
         c.trigger(1791822000, false, true);
         HDRSHOT_CHECK(f.questions == 1 && f.checks.empty());
       }},
      {"restored package reminder defers one startup round and failed ticks never add HTTP",
       [] {
         Fake f;
         Coordinator c(identity(), f);
         c.restore(candidate(), {candidate().key(), 3}, false);
         c.trigger(100, true);
         HDRSHOT_CHECK(f.questions == 1 && f.checks.empty());
         f.question(false);
         c.tick(101);
         HDRSHOT_CHECK(f.checks.size() == 1);
         for (int i = 0; i < 20; ++i)
           c.tick(102 + i);
         HDRSHOT_CHECK(f.checks.size() == 1);
         for (int n = 0; n < 3; ++n) {
           f.checks.back()(Result<Checked, Error>::failure(
               failure("offline", ErrorCode::precondition_failed, Retryability::same_input)));
           if (n < 2)
             f.delays.back()();
         }
         for (int i = 0; i < 20; ++i)
           c.tick(200 + i);
         HDRSHOT_CHECK(f.checks.size() == 3 && f.questions == 1);
       }},
      {"seven-day reminder clock survives manual checks and resets from actual question",
       [] {
         Fake f;
         Coordinator c(identity(), f);
         c.trigger(100, true);
         f.checked();
         f.ready();
         f.question(false);
         c.trigger(100 + 3 * 86400, false, true);
         f.checked();
         f.ready();
         HDRSHOT_CHECK(f.questions == 1);
         c.tick(100 + 604799);
         HDRSHOT_CHECK(f.questions == 1);
         c.tick(100 + 604800);
         HDRSHOT_CHECK(f.questions == 2 && f.checks.size() == 2);
         // An already-open question never opens again, even after another week.
         c.tick(100 + 2 * 604800);
         HDRSHOT_CHECK(f.questions == 2);
         f.question(false);
         c.tick(100 + 2 * 604800 + 1);
         HDRSHOT_CHECK(f.questions == 2);
       }},
      {"failed manual metadata check cannot postpone a verified package reminder",
       [] {
         Fake f;
         Coordinator c(identity(), f);
         c.trigger(100, true);
         f.checked();
         f.ready();
         f.question(false);
         c.trigger(100 + 3 * 86400, false, true);
         f.checks.back()(Result<Checked, Error>::failure(failure("metadata unavailable")));
         HDRSHOT_CHECK(c.view().phase == Phase::check_failed);
         c.tick(100 + 604800);
         HDRSHOT_CHECK(f.questions == 2 && c.view().phase == Phase::ready && f.downloads == 1);
       }},
      {"incompatible candidate cannot borrow a prior verified package reminder or install",
       [] {
         Fake f;
         Coordinator c(identity(), f);
         c.trigger(100, true);
         f.checked();
         f.ready();
         f.question(false);
         auto incompatible = candidate();
         incompatible.version = {0, 1, 6};
         incompatible.release_tag = "v0.1.6";
         incompatible.minimum_os = {99, 0, 0};
         c.trigger(101, false, true);
         f.checks.back()(Result<Checked, Error>::success({std::nullopt, incompatible}));
         c.install();
         HDRSHOT_CHECK(f.installs == 0);
         c.trigger(102, false, true);
         f.checks.back()(Result<Checked, Error>::failure(failure("metadata unavailable")));
         c.tick(100 + 604800);
         c.trigger(100 + 604801, true);
         HDRSHOT_CHECK(f.questions == 1 && f.downloads == 1 && f.installs == 0);
       }},
      {"overdue busy reminder asks once when idle without catch-up or network dependency",
       [] {
         Fake f;
         Coordinator c(identity(), f);
         c.restore(candidate(), {candidate().key(), 3}, true);
         c.trigger(100, true);
         f.question(false);
         f.busy_flag = true;
         c.tick(100 + 604800);
         c.tick(100 + 3 * 604800);
         HDRSHOT_CHECK(f.questions == 1);
         f.busy_flag = false;
         c.tick(100 + 3 * 604800 + 5);
         HDRSHOT_CHECK(f.questions == 2 && f.checks.empty() && f.downloads == 0);
         f.question(false);
         c.tick(100 + 4 * 604800 + 4);
         HDRSHOT_CHECK(f.questions == 2);
         c.tick(100 + 4 * 604800 + 5);
         HDRSHOT_CHECK(f.questions == 3);
       }},
      {"a new candidate has an immediate reminder despite Later for the old one",
       [] {
         Fake f;
         Coordinator c(identity(), f);
         c.trigger(100, true);
         f.checked();
         f.ready();
         f.question(false);
         auto newer = candidate();
         newer.version = {0, 1, 6};
         newer.release_tag = "v0.1.6";
         c.trigger(101, false, true);
         f.checks.back()(Result<Checked, Error>::success({newer}));
         f.downloaded(Result<Receipt, Error>::success({newer.key(), 4}));
         HDRSHOT_CHECK(f.questions == 2 && c.view().candidate->version == newer.version);
       }},
      {"safe Windows paths",
       [] {
         for (const auto p : {"../x", "/x", "x\\y", "a/../b", "a:stream", "CON.txt", "aux",
                              "com1.exe", "lpt9", "file.", "file ", "a//b"})
           HDRSHOT_CHECK(!safe_relative(p));
         HDRSHOT_CHECK(safe_relative("plugins/platforms/qwindows.dll"));
         HDRSHOT_CHECK(safe_relative(QString::fromUtf8("字体/正常.txt")));
       }},
      {"libarchive ZIP manifest extraction and adversarial names",
       [] {
         QTemporaryDir tmp;
         auto c = candidate();
         c.platform = "windows";
         c.architecture = "x64";
         const QByteArray data = "new exe";
         const auto digest = QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
         QJsonObject manifest{
             {"product", "SeriousShot"},
             {"version", "0.1.5"},
             {"platform", "windows"},
             {"architecture", "x64"},
             {"installer_protocol", 1},
             {"entrypoint", "SeriousShot.exe"},
             {"unpacked_size", data.size()},
             {"files", QJsonArray{QJsonObject{{"path", "SeriousShot.exe"},
                                              {"size", data.size()},
                                              {"sha256", QString::fromLatin1(digest)}}}}};
         const auto mb = QJsonDocument(manifest).toJson();
         zip(tmp.path() + "/good.zip", {{"package_manifest.json", mb}, {"SeriousShot.exe", data}});
         HDRSHOT_CHECK(extract_zip(tmp.path() + "/good.zip", tmp.path() + "/good", c));
         for (const auto p : {"../escape", "CON.txt", "file.", "a:stream"}) {
           zip(tmp.path() + "/bad.zip", {{"package_manifest.json", mb}, {p, data}});
           HDRSHOT_CHECK(!extract_zip(tmp.path() + "/bad.zip",
                                      tmp.path() + "/bad-" + QString::number(qHash(p)), c));
         }
         zip(tmp.path() + "/duplicate.zip",
             {{"package_manifest.json", mb}, {"SeriousShot.exe", data}, {"seriousshot.EXE", data}});
         HDRSHOT_CHECK(!extract_zip(tmp.path() + "/duplicate.zip", tmp.path() + "/duplicate", c));
         zip(tmp.path() + "/symlink.zip", {{"link", data}}, true);
         HDRSHOT_CHECK(!extract_zip(tmp.path() + "/symlink.zip", tmp.path() + "/symlink", c));
       }},
  });
}
