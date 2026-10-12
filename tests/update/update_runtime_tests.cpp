#include "platform/qt/update/qt_update_runtime.hpp"
#include "test_support.hpp"
#include "ui/qt/update_window.hpp"
#include <QApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QSslSocket>
#include <QStyleHints>
#include <QTemporaryDir>
#include <atomic>
#include <cstring>
#include <sodium.h>
#undef HDRSHOT_CHECK
#define HDRSHOT_CHECK(...) ::hdrshot::test::check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__)
using namespace hdrshot;
using namespace hdrshot::update;
struct Reply final : QNetworkReply {
  QByteArray bytes;
  qint64 offset{};
  std::atomic<qint64> *max_read{};
  Reply(QNetworkRequest req, QByteArray body, int status, std::atomic<qint64> *largest, QObject *p,
        int delay = 0)
      : QNetworkReply(p), bytes(std::move(body)), max_read(largest) {
    setRequest(req);
    setUrl(req.url());
    setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
    setRawHeader("ETag", "fixture-etag");
    open(QIODevice::ReadOnly);
    QTimer::singleShot(delay, this, [this] {
      if (isFinished())
        return;
      if (!bytes.isEmpty())
        emit readyRead();
      setFinished(true);
      emit finished();
    });
  }
  void redirect(const QUrl &url) {
    setAttribute(QNetworkRequest::RedirectionTargetAttribute, url);
  }
  void abort() override {
    if (isFinished())
      return;
    setError(QNetworkReply::OperationCanceledError, "aborted");
    setFinished(true);
    emit finished();
  }
  qint64 bytesAvailable() const override {
    return bytes.size() - offset + QNetworkReply::bytesAvailable();
  }
  qint64 readData(char *out, qint64 limit) override {
    const auto n = std::min(limit, bytes.size() - offset);
    if (n <= 0)
      return -1;
    std::memcpy(out, bytes.constData() + offset, static_cast<size_t>(n));
    offset += n;
    if (max_read && n > max_read->load())
      max_read->store(n);
    return n;
  }
};
struct Wire {
  QByteArray catalog, release, archive;
  std::atomic<int> checks{}, releases{}, downloads{};
  std::atomic<qint64> max_read{};
  int catalog_status{200}, release_status{200};
  int redirects{}, reply_delay_ms{}, catalog_delay_ms{};
};
struct Manager final : QNetworkAccessManager {
  Wire &wire;
  Manager(Wire &w, QObject *p) : QNetworkAccessManager(p), wire(w) {}
  QNetworkReply *createRequest(Operation op, const QNetworkRequest &req, QIODevice *) override {
    HDRSHOT_CHECK(op == GetOperation);
    if (req.url().host() == "raw.githubusercontent.com") {
      ++wire.checks;
      return new Reply(req, wire.catalog, wire.catalog_status, nullptr, this,
                       wire.catalog_delay_ms);
    }
    if (req.url().host() == "api.github.com") {
      ++wire.releases;
      return new Reply(req, wire.release, wire.release_status, nullptr, this);
    }
    ++wire.downloads;
    if (wire.downloads <= wire.redirects) {
      auto *r = new Reply(req, {}, 302, nullptr, this, wire.reply_delay_ms);
      r->setProperty("fixture_redirect", true);
      r->redirect(QUrl("https://github.com/OverDriveHuang/SeriousShot/releases/"
                       "download/v0.1.5/fixture.dmg?step=" +
                       QString::number(wire.downloads)));
      return r;
    }
    return new Reply(req, wire.archive, 200, &wire.max_read, this, wire.reply_delay_ms);
  }
};
struct Installer : NativeInstaller {
  QString install_identity() const override {
    return "test";
  }
  bool manual() const override {
    return true;
  }
  Result<bool, Error> install(Slot &, const Candidate &, const QByteArray &,
                              std::function<bool()>) override {
    return Result<bool, Error>::success(true);
  }
  Result<bool, Error> cancel_manual(Slot &) override {
    return Result<bool, Error>::success(true);
  }
};
namespace hdrshot::update {
class RuntimeTestAccess {
public:
  static void busy_callback(Runtime &r, std::function<bool()> busy) {
    r.busy_ = std::move(busy);
  }
  static void idle(Runtime &r) {
    r.busy_ = [] { return false; };
  }
  static void download_budget(Runtime &r, int milliseconds) {
    r.download_timeout_ms_ = milliseconds;
  }
  static void replace_native(Runtime &r, std::unique_ptr<NativeInstaller> native) {
    r.native_ = std::move(native);
  }
  static void configure(Runtime &r, const Trust &trust, const QString &root, Wire &wire) {
    r.state_path_ = root + "/state.json";
    r.cache_path_ = root + "/cache.json";
    QMetaObject::invokeMethod(
        r.worker_,
        [&] {
          r.trust_ = trust;
          r.slot_.root = root;
          r.slot_.channel = "testing";
          delete r.network_;
          r.network_ = new Manager(wire, r.worker_);
        },
        Qt::BlockingQueuedConnection);
  }
};
} // namespace hdrshot::update
struct Fixture {
  QTemporaryDir dir;
  Wire wire;
  Trust trust;
  QByteArray secret_key;
  Identity identity;
  Candidate c;
  std::unique_ptr<Runtime> runtime;
  Fixture() {
    HDRSHOT_CHECK(sodium_init() >= 0);
    QByteArray pk(32, 0), sk(64, 0);
    crypto_sign_keypair(reinterpret_cast<unsigned char *>(pk.data()),
                        reinterpret_cast<unsigned char *>(sk.data()));
    secret_key = sk;
    trust = {pk, "fixture", "testing",
             QUrl("https://raw.githubusercontent.com/OverDriveHuang/"
                  "SeriousShot/main/updates/"
                  "testing.json"),
             true};
    identity = {{0, 1, 4}, {27, 0, 0}, "testing", "macos", "arm64", "runtime-test"};
    wire.archive = QByteArray(8 * 1024 * 1024, 'x');
    c.version = {0, 1, 5};
    c.minimum_os = {15, 5, 0};
    c.channel = "testing";
    c.platform = "macos";
    c.architecture = "arm64";
    c.asset_name = "fixture.dmg";
    c.release_tag = "v0.1.5";
    c.asset_id = 456;
    c.release_id = 123;
    c.asset_url = "https://github.com/OverDriveHuang/SeriousShot/releases/"
                  "download/v0.1.5/fixture.dmg";
    c.asset_size = static_cast<std::uint64_t>(wire.archive.size());
    c.sha256 =
        QCryptographicHash::hash(wire.archive, QCryptographicHash::Sha256).toHex().toStdString();
    auto t = QJsonObject{{"version", "0.1.5"},
                         {"minimum_os", "15.5.0"},
                         {"platform", "macos"},
                         {"architecture", "arm64"},
                         {"asset_name", "fixture.dmg"},
                         {"release_tag", "v0.1.5"},
                         {"asset_id", 456},
                         {"release_id", 123},
                         {"asset_url", QString::fromStdString(c.asset_url)},
                         {"asset_size", double(c.asset_size)},
                         {"sha256", QString::fromStdString(c.sha256)},
                         {"draft", false},
                         {"prerelease", false},
                         {"installer_protocol", 1}};
    const auto payload = QJsonDocument(QJsonObject{{"product", "SeriousShot"},
                                                   {"channel", "testing"},
                                                   {"revision", 1},
                                                   {"targets", QJsonArray{t}}})
                             .toJson(QJsonDocument::Compact),
               message = QByteArray("SeriousShot.UpdateCatalog.v1") + payload;
    QByteArray signature(64, 0);
    crypto_sign_detached(reinterpret_cast<unsigned char *>(signature.data()), nullptr,
                         reinterpret_cast<const unsigned char *>(message.constData()),
                         static_cast<unsigned long long>(message.size()),
                         reinterpret_cast<const unsigned char *>(sk.constData()));
    wire.catalog =
        QJsonDocument(QJsonObject{{"schema_version", 1},
                                  {"key_id", "fixture"},
                                  {"payload_base64", QString::fromLatin1(payload.toBase64())},
                                  {"signature_base64", QString::fromLatin1(signature.toBase64())}})
            .toJson();
    wire.release =
        QJsonDocument(
            QJsonObject{{"id", 123},
                        {"url", "https://api.github.com/repos/OverDriveHuang/"
                                "SeriousShot/releases/123"},
                        {"tag_name", "v0.1.5"},
                        {"draft", false},
                        {"prerelease", false},
                        {"assets", QJsonArray{QJsonObject{{"id", 456},
                                                          {"name", "fixture.dmg"},
                                                          {"size", double(c.asset_size)},
                                                          {"state", "uploaded"},
                                                          {"browser_download_url",
                                                           QString::fromStdString(c.asset_url)}}}}})
            .toJson();
    runtime = std::make_unique<Runtime>(
        identity, std::make_unique<Installer>(), [] { return true; }, [] { return false; });
    RuntimeTestAccess::configure(*runtime, trust, dir.path(), wire);
  }
  template <class F> void await(F f) {
    QElapsedTimer timer;
    timer.start();
    while (!f() && timer.elapsed() < 10000) {
      QApplication::processEvents();
      QThread::msleep(2);
    }
    HDRSHOT_CHECK(f());
  }
  void start() {
    runtime->start();
  }
};
struct FailedInstaller final : Installer {
  int &calls;
  explicit FailedInstaller(int &n) : calls(n) {}
  Result<bool, Error> install(Slot &, const Candidate &, const QByteArray &,
                              std::function<bool()>) override {
    ++calls;
    return Result<bool, Error>::failure(
        failure("隔离故障：安装目录不可写", ErrorCode::path_not_writable));
  }
};
struct DelayedInstaller final : Installer {
  std::atomic_bool &entered;
  explicit DelayedInstaller(std::atomic_bool &e) : entered(e) {}
  bool manual() const override {
    return false;
  }
  Result<bool, Error> install(Slot &, const Candidate &, const QByteArray &,
                              std::function<bool()> quit) override {
    entered = true;
    QThread::msleep(150);
    return quit()
               ? Result<bool, Error>::success(true)
               : Result<bool, Error>::failure(failure("cancelled", ErrorCode::operation_cancelled));
  }
};
QByteArray catalog_revision(const Fixture &f, int revision, bool altered = false) {
  const auto envelope = QJsonDocument::fromJson(f.wire.catalog).object();
  auto payload = QJsonDocument::fromJson(
                     QByteArray::fromBase64(envelope["payload_base64"].toString().toLatin1()))
                     .object();
  payload["revision"] = revision;
  if (altered)
    payload["altered_fixture"] = true;
  const auto bytes = QJsonDocument(payload).toJson(QJsonDocument::Compact),
             message = QByteArray("SeriousShot.UpdateCatalog.v1") + bytes;
  QByteArray signature(64, 0);
  crypto_sign_detached(reinterpret_cast<unsigned char *>(signature.data()), nullptr,
                       reinterpret_cast<const unsigned char *>(message.constData()),
                       static_cast<unsigned long long>(message.size()),
                       reinterpret_cast<const unsigned char *>(f.secret_key.constData()));
  auto result = envelope;
  result["payload_base64"] = QString::fromLatin1(bytes.toBase64());
  result["signature_base64"] = QString::fromLatin1(signature.toBase64());
  return QJsonDocument(result).toJson();
}
int main(int argc, char **argv) {
  if (!qEnvironmentVariableIsSet("HDRSHOT_UPDATE_NATIVE"))
    qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication app(argc, argv);
  if (argc >= 3 && std::string_view(argv[1]) == "--ui-probe") {
    const QString out = QString::fromUtf8(argv[2]);
    QDir().mkpath(out);
    app.styleHints()->setColorScheme(argc >= 4 && std::string_view(argv[3]) == "dark"
                                         ? Qt::ColorScheme::Dark
                                         : Qt::ColorScheme::Light);
    UpdateWindow window;
    window.show();
    window.resize(400, 300);
    auto step = std::make_shared<int>(0);
    auto *timer = new QTimer(&window);
    QObject::connect(timer, &QTimer::timeout, &window, [&] {
      View v;
      Candidate c;
      c.version = {0, 1, 5};
      c.release_tag = "v0.1.5";
      v.candidate = c;
      v.last_check = 1791810000;
      v.last_success = 1791809400;
      const std::array phases{Phase::downloading, Phase::ready, Phase::check_failed,
                              Phase::deferred};
      v.phase = phases[static_cast<size_t>(*step % 4)];
      v.received = 42;
      v.total = 100;
      if (v.phase == Phase::check_failed)
        v.detail = "更新源请求失败：网络暂时不可用或服务器限流。请稍后检查更新；已知的新版本与已下"
                   "载的安装包仍保留，可以在网络恢复后继续安装。";
      if (v.phase == Phase::deferred) {
        v.error =
            failure("安装目录暂时不可写，请检查安装位置后重试。", ErrorCode::path_not_writable);
        v.detail = "安装目录暂时不可写，请检查安装位置后重试。";
      }
      window.set_view(v);
      QApplication::processEvents();
      window.grab().save(out + QString("/update-window-%1.png").arg(*step));
      ++*step;
      if (*step == 4)
        app.quit();
    });
    timer->start(700);
    return app.exec();
  }
  return hdrshot::test::run({
      {"window close semantics native chrome errors wrapping and release tag",
       [] {
         UpdateWindow w;
         int later = 0, cancel = 0, install = 0;
         w.later = [&] { ++later; };
         w.cancel = [&] { ++cancel; };
         w.install = [&] { ++install; };
         View v;
         v.phase = Phase::downloading;
         v.received = 42;
         v.total = 100;
         w.set_view(v);
         w.show();
         w.close();
         HDRSHOT_CHECK(later == 0 && cancel == 0 && !w.isVisible());
         HDRSHOT_CHECK(!(w.windowFlags() & Qt::FramelessWindowHint));
         HDRSHOT_CHECK(w.findChild<QLabel *>("updateWindowDetail")->text() ==
                       QStringLiteral("关闭此窗口后下载不会停止。"));
         v.phase = Phase::deferred;
         Candidate c;
         c.version = {0, 1, 5};
         c.release_tag = "v0.1.5";
         v.candidate = c;
         v.error = failure("目录不可写", ErrorCode::path_not_writable);
         v.detail = "目录不可写";
         w.set_view(v);
         HDRSHOT_CHECK(w.findChild<QLabel *>("updateWindowDetail")->text().contains("目录不可写"));
         w.findChild<QPushButton *>("updateWindowPrimary")->click();
         HDRSHOT_CHECK(install == 1);
         QUrl url;
         w.set_release_page_opener([&](const QUrl &u) {
           url = u;
           return true;
         });
         w.findChild<QPushButton *>("openReleasesButton")->click();
         HDRSHOT_CHECK(url.path() == "/OverDriveHuang/SeriousShot/releases/tag/v0.1.5");
         w.show();
         w.close();
         HDRSHOT_CHECK(later == 1);
         v.error.reset();
         w.set_view(v);
         HDRSHOT_CHECK(w.findChild<QLabel *>("updateWindowStatus")->text() ==
                       QStringLiteral("新版本已下载，是否安装？"));
         v.phase = Phase::check_failed;
         v.detail = std::string(300, 'X');
         w.set_view(v);
         w.resize(400, 300);
         w.show();
         QApplication::processEvents();
         auto *label = w.findChild<QLabel *>("updateWindowStatus");
         HDRSHOT_CHECK(label->height() >= label->heightForWidth(label->width()));
       }},
      {"preparing shutdown never joins worker waiting synchronously for GUI",
       [] {
         Fixture f;
         f.start();
         f.await([&] { return f.runtime->coordinator().view().phase == Phase::ready; });
         std::atomic_bool entered{false};
         RuntimeTestAccess::replace_native(*f.runtime, std::make_unique<DelayedInstaller>(entered));
         auto state = read_json(f.dir.path() + "/slot.json");
         Receipt receipt{f.c.key(), state.value()["generation"].toString().toULongLong()};
         f.runtime->install(f.c, receipt, [](auto) {});
         f.await([&] { return entered.load(); });
         QElapsedTimer timer;
         timer.start();
         f.runtime.reset();
         HDRSHOT_CHECK(timer.elapsed() < 3000);
       }},
      {"shared signed cache cannot be downgraded by another Runtime",
       [] {
         Fixture f;
         f.identity.version = f.c.version;
         Wire stale;
         stale.catalog_delay_ms = 100;
         stale.catalog = catalog_revision(f, 1);
         f.wire.catalog = catalog_revision(f, 2);
         f.wire.catalog_delay_ms = 30;
         auto b = std::make_unique<Runtime>(
             f.identity, std::make_unique<Installer>(), [] { return true; }, [] { return false; });
         RuntimeTestAccess::configure(*b, f.trust, f.dir.path(), stale);
         bool first = false, second = false, accepted = false;
         f.runtime->check(f.identity, [&](auto r) {
           HDRSHOT_CHECK(r);
           first = true;
         });
         b->check(f.identity, [&](auto r) {
           accepted = bool(r);
           second = true;
         });
         f.await([&] { return first && second; });
         auto cache = read_json(f.dir.path() + "/cache.json");
         auto verified = verify_catalog(
             QByteArray::fromBase64(cache.value()["envelope"].toString().toLatin1()), f.trust);
         HDRSHOT_CHECK(!accepted && verified.value().revision == 2);
       }},
      {"same revision different signed bytes rejected",
       [] {
         Fixture f;
         f.identity.version = f.c.version;
         f.wire.catalog = catalog_revision(f, 2);
         bool finished = false;
         f.runtime->check(f.identity, [&](auto r) {
           HDRSHOT_CHECK(r);
           finished = true;
         });
         f.await([&] { return finished; });
         const auto original = read_json(f.dir.path() + "/cache.json").value();
         f.wire.catalog = catalog_revision(f, 2, true);
         finished = false;
         bool accepted = true;
         f.runtime->check(f.identity, [&](auto r) {
           accepted = bool(r);
           finished = true;
         });
         f.await([&] { return finished; });
         HDRSHOT_CHECK(!accepted && read_json(f.dir.path() + "/cache.json").value() == original);
       }},
      {"single owned window close persists Later and stale confirmation is cleared",
       [] {
         Fixture f;
         RuntimeTestAccess::idle(*f.runtime);
         auto &window = f.runtime->update_window();
         f.runtime->show_details();
         HDRSHOT_CHECK(&window == &f.runtime->update_window());
         f.start();
         f.await([&] {
           return f.runtime->coordinator().view().phase == Phase::ready &&
                  f.runtime->coordinator().view().prompted;
         });
         window.close();
         HDRSHOT_CHECK(f.runtime->coordinator().view().phase == Phase::deferred &&
                       !window.isVisible());
         f.runtime->coordinator().trigger(100, false, true);
         f.await([&] { return f.runtime->coordinator().view().phase == Phase::deferred; });
         window.findChild<QPushButton *>("updateWindowPrimary")->click();
         f.await([&] {
           auto v = f.runtime->coordinator().view();
           return v.phase == Phase::manual_pending;
         });
         HDRSHOT_CHECK(f.wire.downloads == 1);
       }},
      {"install failure appears in child window and retry needs a new explicit click",
       [] {
         Fixture f;
         RuntimeTestAccess::idle(*f.runtime);
         int calls = 0;
         RuntimeTestAccess::replace_native(*f.runtime, std::make_unique<FailedInstaller>(calls));
         f.start();
         f.await([&] {
           return f.runtime->coordinator().view().phase == Phase::ready &&
                  f.runtime->update_window().isVisible();
         });
         auto *button = f.runtime->update_window().findChild<QPushButton *>("updateWindowPrimary");
         button->click();
         f.await([&] { return f.runtime->coordinator().view().error.has_value(); });
         HDRSHOT_CHECK(calls == 1 && f.runtime->update_window()
                                         .findChild<QLabel *>("updateWindowDetail")
                                         ->text()
                                         .contains("安装目录不可写"));
         QApplication::processEvents();
         HDRSHOT_CHECK(calls == 1);
         button->click();
         f.await([&] { return calls == 2 && f.runtime->coordinator().view().error.has_value(); });
       }},
      {"download presentation close keeps work and busy completion defers activation",
       [] {
         Fixture f;
         f.wire.reply_delay_ms = 100;
         f.start();
         auto &w = f.runtime->update_window();
         w.show();
         w.close();
         f.await([&] { return f.runtime->coordinator().view().phase == Phase::ready; });
         HDRSHOT_CHECK(!w.isVisible() && f.wire.downloads == 1 &&
                       !f.runtime->coordinator().view().prompted);
         RuntimeTestAccess::idle(*f.runtime);
         f.runtime->coordinator().tick(QDateTime::currentSecsSinceEpoch());
         f.await([&] { return w.isVisible(); });
         HDRSHOT_CHECK(f.runtime->coordinator().view().prompted);
       }},
      {"visible ready nonmodal window follows busy transitions",
       [] {
         Fixture f;
         bool busy = false;
         RuntimeTestAccess::busy_callback(*f.runtime, [&] { return busy; });
         f.start();
         f.await([&] {
           return f.runtime->update_window().isVisible() &&
                  f.runtime->coordinator().view().phase == Phase::ready;
         });
         auto *button = f.runtime->update_window().findChild<QPushButton *>("updateWindowPrimary");
         HDRSHOT_CHECK(button->isEnabled());
         busy = true;
         f.runtime->coordinator().tick(QDateTime::currentSecsSinceEpoch());
         HDRSHOT_CHECK(!button->isEnabled());
         busy = false;
         f.runtime->coordinator().tick(QDateTime::currentSecsSinceEpoch());
         HDRSHOT_CHECK(button->isEnabled());
       }},
      {"download total deadline survives redirects",
       [] {
         Fixture f;
         f.wire.redirects = 2;
         f.wire.reply_delay_ms = 60;
         RuntimeTestAccess::download_budget(*f.runtime, 90);
         f.start();
         f.await([&] { return f.runtime->coordinator().view().phase == Phase::download_failed; });
         HDRSHOT_CHECK(f.wire.downloads == 2 && !QFileInfo::exists(f.dir.path() + "/payload.dmg"));
       }},
      {"Qt TLS backend available",
       [] {
         HDRSHOT_CHECK(QSslSocket::supportsSsl());
         HDRSHOT_CHECK(!QSslSocket::availableBackends().isEmpty());
       }},
      {"signed catalog Release and bounded stream to single file",
       [] {
         Fixture f;
         f.start();
         f.await([&] { return f.runtime->coordinator().view().phase == Phase::ready; });
         HDRSHOT_CHECK(f.wire.checks == 1 && f.wire.releases == 1 && f.wire.downloads == 1);
         HDRSHOT_CHECK(f.wire.max_read <= 1024 * 1024);
         auto state = read_json(f.dir.path() + "/slot.json");
         HDRSHOT_CHECK(state && state.value()["phase"] == "ready");
         HDRSHOT_CHECK(QFileInfo(f.dir.path() + "/payload.dmg").size() == 8 * 1024 * 1024);
         HDRSHOT_CHECK(!QFileInfo::exists(f.dir.path() + "/payload.part"));
       }},
      {"304 reuses only authenticated cache and records successful check",
       [] {
         Fixture f;
         f.start();
         f.await([&] { return f.runtime->coordinator().view().phase == Phase::ready; });
         f.runtime->coordinator().later();
         f.wire.catalog_status = 304;
         f.wire.catalog.clear();
         f.runtime->coordinator().trigger(200, false, true);
         f.await([&] { return f.runtime->coordinator().view().phase == Phase::deferred; });
         HDRSHOT_CHECK(f.wire.checks == 2 && f.wire.downloads == 1);
         auto state = read_json(f.dir.path() + "/state.json");
         HDRSHOT_CHECK(state && state.value().contains("last_check") &&
                       state.value().contains("last_success"));
       }},
      {"HTTP 200 empty body is failure, never cached success",
       [] {
         Fixture f;
         f.wire.catalog.clear();
         f.start();
         f.await([&] { return f.runtime->coordinator().view().phase == Phase::check_failed; });
         HDRSHOT_CHECK(f.wire.downloads == 0 && f.wire.checks == 1);
       }},
      {"HTTP 304 without authenticated cache fails",
       [] {
         Fixture f;
         f.wire.catalog_status = 304;
         f.start();
         f.await([&] { return f.runtime->coordinator().view().phase == Phase::check_failed; });
         HDRSHOT_CHECK(f.wire.downloads == 0);
       }},
      {"Release prerelease prevents archive GET",
       [] {
         Fixture f;
         auto release = QJsonDocument::fromJson(f.wire.release).object();
         release["prerelease"] = true;
         f.wire.release = QJsonDocument(release).toJson();
         f.start();
         f.await([&] { return f.runtime->coordinator().view().phase == Phase::check_failed; });
         HDRSHOT_CHECK(f.wire.checks == 1 && f.wire.releases == 1 && f.wire.downloads == 0);
       }},
      {"missing Ready package repaired on next normal trigger without reprompt",
       [] {
         Fixture f;
         f.start();
         f.await([&] { return f.runtime->coordinator().view().phase == Phase::ready; });
         f.runtime->coordinator().later();
         HDRSHOT_CHECK(QFile::remove(f.dir.path() + "/payload.dmg"));
         f.runtime->coordinator().trigger(100, false, true);
         f.await([&] { return f.runtime->coordinator().view().phase == Phase::deferred; });
         HDRSHOT_CHECK(f.wire.downloads == 2 && f.runtime->coordinator().view().prompted);
       }},
  });
}
