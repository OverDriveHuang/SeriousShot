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
#include <QFontDatabase>
#include <QPalette>
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
  std::atomic_bool *gate{};
  Reply(QNetworkRequest req, QByteArray body, int status, std::atomic<qint64> *largest, QObject *p,
        int delay = 0, std::atomic_bool *release_gate = nullptr)
      : QNetworkReply(p), bytes(std::move(body)), max_read(largest), gate(release_gate) {
    setRequest(req);
    setUrl(req.url());
    setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
    setRawHeader("ETag", "fixture-etag");
    open(QIODevice::ReadOnly);
    QTimer::singleShot(delay, this, [this] { finish_when_allowed(); });
  }
  void finish_when_allowed() {
      if (isFinished())
        return;
      if (gate && !gate->load()) {
        QTimer::singleShot(1, this, [this] { finish_when_allowed(); });
        return;
      }
      if (!bytes.isEmpty())
        emit readyRead();
      setFinished(true);
      emit finished();
  }
  void rate_limited() { setRawHeader("X-RateLimit-Remaining", "0"); }
  void redirect(const QUrl &url) { setAttribute(QNetworkRequest::RedirectionTargetAttribute, url); }
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
  bool rate_limit_header{}, abort_redirect{}, abort_catalog{};
  int redirects{}, reply_delay_ms{}, catalog_delay_ms{}, release_delay_ms{};
  std::atomic_bool *catalog_gate{};
};
struct Manager final : QNetworkAccessManager {
  Wire &wire;
  Manager(Wire &w, QObject *p) : QNetworkAccessManager(p), wire(w) {}
  QNetworkReply *createRequest(Operation op, const QNetworkRequest &req, QIODevice *) override {
    HDRSHOT_CHECK(op == GetOperation);
    if (req.url().host() == "raw.githubusercontent.com") {
      ++wire.checks;
      auto *reply = new Reply(req, wire.catalog, wire.catalog_status, nullptr, this,
                              wire.catalog_delay_ms, wire.catalog_gate);
      if (wire.rate_limit_header) reply->rate_limited();
      if (wire.abort_catalog) QTimer::singleShot(0,reply,&QNetworkReply::abort);
      return reply;
    }
    if (req.url().host() == "api.github.com") {
      ++wire.releases;
      return new Reply(req, wire.release, wire.release_status, nullptr, this,
                       wire.release_delay_ms);
    }
    ++wire.downloads;
    if (wire.downloads <= wire.redirects) {
      auto *r = new Reply(req, {}, 302, nullptr, this, wire.reply_delay_ms);
      r->setProperty("fixture_redirect", true);
      if (wire.abort_redirect) QTimer::singleShot(0,r,&QNetworkReply::abort);
      r->redirect(QUrl("https://github.com/OverDriveHuang/SeriousShot/releases/"
                       "download/v0.1.5/fixture.dmg?step=" +
                       QString::number(wire.downloads)));
      return r;
    }
    return new Reply(req, wire.archive, 200, &wire.max_read, this, wire.reply_delay_ms);
  }
};
struct Installer : NativeInstaller {
  QString install_identity() const override { return "test"; }
  bool manual() const override { return true; }
  Result<bool, Error> install(Slot &, const Candidate &, const QByteArray &,
                              std::function<bool()>) override {
    return Result<bool, Error>::success(true);
  }
  Result<bool, Error> cancel_manual(Slot &) override { return Result<bool, Error>::success(true); }
};
namespace hdrshot::update {
class RuntimeTestAccess {
public:
  static bool has_window(const Runtime &r) { return bool(r.update_window_); }
  static bool has_confirmation(const Runtime &r) { return bool(r.confirmation_); }
  static void quit_callback(Runtime &r, std::function<bool()> quit) { r.quit_ = std::move(quit); }
  static void busy_callback(Runtime &r, std::function<bool()> busy) { r.busy_ = std::move(busy); }
  static void idle(Runtime &r) {
    r.busy_ = [] { return false; };
  }
  static void metadata_budget(Runtime &r, int milliseconds) { r.metadata_timeout_ms_ = milliseconds; }
  static void download_budget(Runtime &r, int milliseconds) {
    r.download_timeout_ms_ = milliseconds;
  }
  static void replace_native(Runtime &r, std::unique_ptr<NativeInstaller> native) {
    r.native_ = std::move(native);
  }
  static void configure(Runtime &r, const Trust &trust, const QString &root, Wire &wire) {
    r.state_path_ = root + "/state.json";
    r.cache_path_ = root + "/cache.json";
    // A new instance must load the actual isolated persisted state, like the
    // production constructor, rather than retaining its default-path state.
    const auto state = read_json(r.state_path_);
    r.state_ = state ? state.value() : QJsonObject{};
    QMetaObject::invokeMethod(
        r.worker_,
        [&] {
          const auto cache = read_json(r.cache_path_);
          r.cache_ = cache ? cache.value() : QJsonObject{};
          r.envelope_ = QByteArray::fromBase64(r.cache_["envelope"].toString().toLatin1());
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
  void start() { runtime->start(); }
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
struct CapturingInstaller final : Installer {
  QByteArray &captured;
  explicit CapturingInstaller(QByteArray &out) : captured(out) {}
  Result<bool, Error> install(Slot &, const Candidate &, const QByteArray &envelope,
                              std::function<bool()>) override {
    captured = envelope;
    return Result<bool, Error>::failure(failure("fixture stop", ErrorCode::operation_cancelled));
  }
};
struct ExitingManualInstaller final : Installer {
  std::atomic_bool &entered;
  explicit ExitingManualInstaller(std::atomic_bool &value) : entered(value) {}
  Result<bool, Error> install(Slot &, const Candidate &, const QByteArray &,
                              std::function<bool()> quit) override {
    entered = true;
    QThread::msleep(80);
    return quit() ? Result<bool, Error>::success(true)
                  : Result<bool, Error>::failure(
                        failure("退出已取消", ErrorCode::operation_cancelled));
  }
};
struct DelayedInstaller final : Installer {
  std::atomic_bool &entered;
  explicit DelayedInstaller(std::atomic_bool &e) : entered(e) {}
  bool manual() const override { return false; }
  Result<bool, Error> install(Slot &, const Candidate &, const QByteArray &,
                              std::function<bool()> quit) override {
    entered = true;
    QThread::msleep(150);
    return quit()
               ? Result<bool, Error>::success(true)
               : Result<bool, Error>::failure(failure("cancelled", ErrorCode::operation_cancelled));
  }
};
QByteArray catalog_revision(const Fixture &f, int revision, bool altered = false,
                            std::function<void(QJsonObject &)> mutate = {}) {
  const auto envelope = QJsonDocument::fromJson(f.wire.catalog).object();
  auto payload = QJsonDocument::fromJson(
                     QByteArray::fromBase64(envelope["payload_base64"].toString().toLatin1()))
                     .object();
  payload["revision"] = revision;
  if (mutate) mutate(payload);
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
  if (argc >= 3 && (std::string_view(argv[1]) == "--ui-probe" || std::string_view(argv[1]) == "--manual-ui-probe")) {
    const QString out = QString::fromUtf8(argv[2]);
    QDir().mkpath(out);
    // The offscreen Windows plugin has no system font/theme integration.
    // Keep these explicit presentation fixtures confined to the UI probe.
#ifdef Q_OS_WIN
    const int font_id = QFontDatabase::addApplicationFont("C:/Windows/Fonts/msyh.ttc");
    if (font_id < 0 || QFontDatabase::applicationFontFamilies(font_id).isEmpty())
      return 2;
    app.setFont(QFont(QFontDatabase::applicationFontFamilies(font_id).front(), 10));
#endif
    app.setStyle("Fusion");
    const bool dark = argc >= 4 && std::string_view(argv[3]) == "dark";
    QPalette palette;
    palette.setColor(QPalette::Window, dark ? QColor(32, 32, 32) : QColor(245, 245, 245));
    palette.setColor(QPalette::WindowText, dark ? Qt::white : Qt::black);
    palette.setColor(QPalette::Base, dark ? QColor(24, 24, 24) : Qt::white);
    palette.setColor(QPalette::AlternateBase, dark ? QColor(40, 40, 40) : QColor(235, 235, 235));
    palette.setColor(QPalette::Text, dark ? Qt::white : Qt::black);
    palette.setColor(QPalette::Button, dark ? QColor(48, 48, 48) : QColor(235, 235, 235));
    palette.setColor(QPalette::ButtonText, dark ? Qt::white : Qt::black);
    palette.setColor(QPalette::Highlight, QColor(0, 120, 215));
    palette.setColor(QPalette::HighlightedText, Qt::white);
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor(128, 128, 128));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(128, 128, 128));
    app.setPalette(palette);
    UpdateWindow window;
    window.show();
    window.resize(argc >= 5 ? QString::fromUtf8(argv[4]).toInt() : 400, 320);
    auto step = std::make_shared<int>(std::string_view(argv[1])=="--manual-ui-probe"?4:0);
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
                              Phase::download_failed,Phase::deferred,Phase::deferred};
      v.phase = phases[static_cast<size_t>(*step % 6)];
      v.received = 42;
      v.total = 100;
      if (v.phase == Phase::check_failed || v.phase == Phase::download_failed)
        v.detail = "更新源拒绝访问，请稍后重试";
      if (v.phase == Phase::deferred) {
        v.error =
            failure("安装目录暂时不可写，请检查安装位置后重试。", ErrorCode::path_not_writable);
        v.detail = "安装目录暂时不可写，请检查安装位置后重试。";
      }
      if(*step==4) {
        v.error=failure("安装文件清单与当前程序不一致，自动更新未执行。当前版本仍可使用。");
        v.error->safe_context["manual_install_required"]="true";
        v.error->safe_context["install_directory"]="C:/Applications/SeriousShot/portable_installation_with_a_long_directory_name/long_directory_for_update_validation/long_directory_for_manual_replacement_validation/SeriousShot-0.1.4-windows-x64_2026-10-14_00-28-50";
        v.detail=v.error->safe_context.at("reason");
      }
      window.set_view(v);
      QApplication::processEvents();
      window.grab().save(out + QString("/update-window-%1.png").arg(*step));
      ++*step;
      if (*step == 6)
        app.quit();
    });
    timer->start(700);
    return app.exec();
  }
  return hdrshot::test::run({
      {"manual fallback opens package with selectable concrete instructions",[] {
        UpdateWindow w;View v;v.phase=Phase::deferred;Candidate c;c.version={0,1,5};v.candidate=c;
        v.error=failure("安装文件清单与当前程序不一致，当前版本仍可使用。");
        v.error->safe_context["manual_install_required"]="true";
        v.error->safe_context["install_directory"]="C:/Apps/SeriousShot";
        v.detail=v.error->safe_context.at("reason");int opened=0,installed=0;
        w.open_manual=[&]{++opened;};w.install=[&]{++installed;};w.set_view(v);
        auto* button=w.findChild<QPushButton*>("updateWindowPrimary");
        HDRSHOT_CHECK(button->text()==QStringLiteral("打开安装包"));button->click();
        HDRSHOT_CHECK(opened==1 && installed==0);
        auto* detail=w.findChild<QLabel*>("updateWindowDetail");
        HDRSHOT_CHECK(detail->text().contains("从托盘退出") && detail->text().contains("C:/Apps/SeriousShot"));
        HDRSHOT_CHECK(detail->textInteractionFlags() & Qt::TextSelectableByMouse);
      }},
      {"missing manual package offers redownload instead of install",[] {
        Fixture f;
        f.runtime->coordinator().manual_open_result(Result<bool,Error>::failure(
          failure("安装包已缺失，请重新下载。",ErrorCode::object_not_found)));
        const auto& view=f.runtime->coordinator().view();
        HDRSHOT_CHECK(view.phase==Phase::download_failed);
        auto& window=f.runtime->update_window();window.set_view(view);
        HDRSHOT_CHECK(window.findChild<QPushButton*>("updateWindowPrimary")->text()==QStringLiteral("重试"));
      }},
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
         HDRSHOT_CHECK(url.path() == "/OverDriveHuang/SeriousShot/releases");
         HDRSHOT_CHECK(w.findChild<QPushButton *>("openReleasesButton")->text() == QStringLiteral("前往官网下载 ↗"));
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
         std::atomic_bool release_new{false}, release_stale{false};
         Fixture f;
         f.identity.version = f.c.version;
         Wire stale;
         stale.catalog_gate = &release_stale;
         f.wire.catalog_gate = &release_new;
         stale.catalog = catalog_revision(f, 1);
         f.wire.catalog = catalog_revision(f, 2);
         auto b = std::make_unique<Runtime>(
             f.identity, std::make_unique<Installer>(), [] { return true; }, [] { return false; });
         RuntimeTestAccess::configure(*b, f.trust, f.dir.path(), stale);
         bool first = false, second = false, accepted = false;
         f.runtime->check(f.identity, [&](auto r) {
           HDRSHOT_CHECK(r);
           first = true;
         });
         // The first initial read lock is released before starting the second
         // request. Both HTTP replies remain in flight with the old cache.
         f.await([&] { return f.wire.checks == 1; });
         b->check(f.identity, [&](auto r) {
           accepted = bool(r);
           second = true;
         });
         f.await([&] { return stale.checks == 1; });
         release_new = true;
         f.await([&] { return first; });
         release_stale = true;
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
      {"clean completion automatically creates the install question without an "
       "opened window",
       [] {
         Fixture f;
         RuntimeTestAccess::idle(*f.runtime);
         f.start();
         f.await([&] { return f.runtime->update_window().isVisible(); });
         HDRSHOT_CHECK(f.runtime->coordinator().view().phase == Phase::ready &&
                       f.runtime->coordinator().view().prompted);
         auto &w = f.runtime->update_window();
         HDRSHOT_CHECK(w.findChild<QLabel *>("updateWindowStatus")->text() ==
                       QStringLiteral("新版本已下载，是否安装？"));
         w.close();
         for (int i = 0; i < 3; ++i)
           f.runtime->coordinator().tick(QDateTime::currentSecsSinceEpoch());
         HDRSHOT_CHECK(!w.isVisible());
       }},
      {"persisted manual transaction restores install actions and Later "
       "protects its slot",
       [] {
         Fixture f;
         RuntimeTestAccess::idle(*f.runtime);
         f.start();
         f.await([&] { return f.runtime->update_window().isVisible(); });
         f.runtime->update_window().close();
         f.runtime.reset();
         auto record = read_json(f.dir.path() + "/slot.json").value();
         record["phase"] = "manual_pending";
         record["generation"] = "3";
         record["mount"] = "/Volumes/old-ejected-update";
         HDRSHOT_CHECK(save_json(f.dir.path() + "/slot.json", record));
         f.runtime = std::make_unique<Runtime>(
             f.identity, std::make_unique<Installer>(), [] { return false; }, [] { return false; });
         RuntimeTestAccess::configure(*f.runtime, f.trust, f.dir.path(), f.wire);
         f.start();
         f.await([&] { return f.runtime->coordinator().view().phase == Phase::manual_pending; });
         auto &w = f.runtime->update_window();
         HDRSHOT_CHECK(w.isVisible() && f.runtime->coordinator().view().prompted);
         f.runtime->coordinator().tick(QDateTime::currentSecsSinceEpoch());
         HDRSHOT_CHECK(f.wire.checks == 1 && f.wire.downloads == 1);
         f.runtime->show_details();
         auto *button = w.findChild<QPushButton *>("updateWindowPrimary");
         HDRSHOT_CHECK(button->text() == QStringLiteral("安装") && button->isEnabled());
         HDRSHOT_CHECK(w.findChild<QPushButton *>("updateWindowSecondary")->text() ==
                       QStringLiteral("稍后"));
         w.close();
         HDRSHOT_CHECK(f.runtime->coordinator().view().phase == Phase::manual_pending);
         f.runtime->coordinator().trigger(QDateTime::currentSecsSinceEpoch(), false, true);
         HDRSHOT_CHECK(f.wire.checks == 1 && f.wire.downloads == 1);
         f.runtime->show_details();
         f.wire.release_delay_ms = 50;
         button->click();
         HDRSHOT_CHECK(f.runtime->coordinator().view().phase == Phase::preparing);
         f.await([&] { return f.runtime->coordinator().view().phase == Phase::manual_pending; });
         HDRSHOT_CHECK(f.wire.downloads == 1 &&
                       read_json(f.dir.path() + "/slot.json").value()["generation"] ==
                           record["generation"]);
       }},
      {"Later is reloaded across runtime restart and active install remains "
       "available",
       [] {
         Fixture f;
         RuntimeTestAccess::idle(*f.runtime);
         f.start();
         f.await([&] { return f.runtime->update_window().isVisible(); });
         f.runtime->update_window().close();
         f.runtime.reset();
         f.runtime = std::make_unique<Runtime>(
             f.identity, std::make_unique<Installer>(), [] { return false; }, [] { return false; });
         RuntimeTestAccess::configure(*f.runtime, f.trust, f.dir.path(), f.wire);
         f.start();
         f.await([&] { return f.runtime->update_window().isVisible(); });
         HDRSHOT_CHECK(f.runtime->coordinator().view().phase == Phase::ready &&
                       f.runtime->coordinator().view().prompted && f.wire.downloads == 1);
         f.runtime->coordinator().tick(QDateTime::currentSecsSinceEpoch());
         f.runtime->update_window().findChild<QPushButton *>("updateWindowPrimary")->click();
         f.await([&] { return f.runtime->coordinator().view().phase == Phase::manual_pending; });
         HDRSHOT_CHECK(f.wire.downloads == 1);
       }},
      {"single owned window close persists Later and stale confirmation is "
       "cleared",
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
      {"install failure appears in child window and retry needs a new explicit "
       "click",
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
      {"download presentation close keeps work and busy completion defers "
       "activation",
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
#ifdef Q_OS_MACOS
      {"manual handoff blocks new work, checks late busy, releases barrier on "
       "refusal",
       [] {
         Fixture f;
         std::atomic_bool entered{false}, busy{false};
         int quits = 0;
         RuntimeTestAccess::busy_callback(*f.runtime, [&] { return busy.load(); });
         RuntimeTestAccess::quit_callback(*f.runtime, [&] {
           ++quits;
           return false;
         });
         RuntimeTestAccess::replace_native(*f.runtime,
                                           std::make_unique<ExitingManualInstaller>(entered));
         f.start();
         f.await([&] { return f.runtime->coordinator().view().phase == Phase::ready; });
         f.wire.release_delay_ms = 100;
         f.runtime->coordinator().install();
         HDRSHOT_CHECK(f.runtime->barrier_active());
         f.await([&] { return entered.load(); });
         busy = true;
         f.await([&] { return f.runtime->coordinator().view().error.has_value(); });
         HDRSHOT_CHECK(quits == 0 && !f.runtime->barrier_active() &&
                       f.runtime->coordinator().view().phase == Phase::deferred);
         busy = false;
         entered = false;
         f.runtime->coordinator().install();
         HDRSHOT_CHECK(f.runtime->barrier_active());
         f.await([&] { return f.runtime->coordinator().view().error.has_value(); });
         HDRSHOT_CHECK(quits == 1 && !f.runtime->barrier_active());
       }},
      {"normal handoff owner destruction drains worker without dangling "
       "completion",
       [] {
         Fixture f;
         std::atomic_bool entered{false};
         int completions = 0;
         RuntimeTestAccess::idle(*f.runtime);
         RuntimeTestAccess::replace_native(*f.runtime,
                                           std::make_unique<ExitingManualInstaller>(entered));
         f.start();
         f.await([&] { return f.runtime->coordinator().view().phase == Phase::ready; });
         const auto state = read_json(f.dir.path() + "/slot.json");
         Receipt receipt{f.c.key(), state.value()["generation"].toString().toULongLong()};
         RuntimeTestAccess::quit_callback(*f.runtime, [&] {
           f.runtime.reset();
           return true;
         });
         QElapsedTimer deadline;
         deadline.start();
         f.runtime->install(f.c, receipt, [&](auto) { ++completions; });
         f.await([&] { return !f.runtime; });
         HDRSHOT_CHECK(deadline.elapsed() < 3000 && completions == 0 &&
                       QFileInfo::exists(f.dir.path() + "/payload.dmg"));
       }},
#endif
      {"aborted redirect never starts another download", [] {
         Fixture f; f.wire.redirects=1; f.wire.reply_delay_ms=100; f.wire.abort_redirect=true;
         f.start();
         f.await([&] { const auto p=f.runtime->coordinator().view().phase;
           return p==Phase::download_failed || p==Phase::ready; });
         HDRSHOT_CHECK(f.wire.downloads==1);
         HDRSHOT_CHECK(f.runtime->coordinator().view().phase==Phase::download_failed);
         HDRSHOT_CHECK(!QFileInfo::exists(f.dir.path()+"/payload.dmg"));
       }},
      {"aborted 304 never authenticates a successful check", [] {
         Fixture f;f.start();
         f.await([&]{return f.runtime->coordinator().view().phase==Phase::ready;});
         f.runtime->coordinator().later();
         f.wire.catalog_status=304;f.wire.catalog.clear();f.wire.abort_catalog=true;
         f.wire.catalog_delay_ms=100;
         f.runtime->coordinator().trigger(200,false,true);
         f.await([&]{auto p=f.runtime->coordinator().view().phase;
           return p==Phase::check_failed || p==Phase::deferred;});
         HDRSHOT_CHECK(f.runtime->coordinator().view().phase==Phase::check_failed);
       }},
      {"download total deadline survives redirects",
       [] {
         Fixture f;
         f.wire.redirects = 2;
         // Leave enough room for native durable state writes and Windows
         // timer scheduling, while the second reply still exceeds the one
         // total deadline. A reset-per-redirect implementation reaches GET3.
         f.wire.reply_delay_ms = 650;
         RuntimeTestAccess::download_budget(*f.runtime, 1000);
         f.start();
         f.await([&] { return f.runtime->coordinator().view().phase == Phase::download_failed; });
         HDRSHOT_CHECK(f.wire.downloads == 2 && !QFileInfo::exists(f.dir.path() + "/payload.dmg"));
       }},
      {"Qt TLS backend available",
       [] {
         HDRSHOT_CHECK(QSslSocket::supportsSsl());
         HDRSHOT_CHECK(!QSslSocket::availableBackends().isEmpty());
       }},
      {"signed catalog without Release API and bounded stream to single file",
       [] {
         Fixture f;
         f.start();
         f.await([&] { return f.runtime->coordinator().view().phase == Phase::ready; });
         HDRSHOT_CHECK(f.wire.checks == 1 && f.wire.releases == 0 && f.wire.downloads == 1);
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
      {"403 needs response header evidence before claiming rate limit", [] {
         for (const bool limited : {false, true}) {
           Fixture f; f.wire.catalog_status = 403; f.wire.rate_limit_header = limited;
           bool done = false; std::string reason;
           f.runtime->check(f.identity, [&](auto result) {
             HDRSHOT_CHECK(!result); reason = result.error().safe_context.at("reason"); done = true;
           });
           f.await([&] { return done; });
           HDRSHOT_CHECK(reason == (limited ? "更新源限流，请稍后重试" : "更新源拒绝访问，请稍后重试"));
         }
       }},
      {"API access denied is irrelevant to catalog download and install", [] {
         Fixture f; f.wire.release_status = 403;
         int calls = 0;
         RuntimeTestAccess::replace_native(*f.runtime, std::make_unique<FailedInstaller>(calls));
         f.start(); f.await([&] { return f.runtime->coordinator().view().phase == Phase::ready; });
         RuntimeTestAccess::idle(*f.runtime);
         f.runtime->coordinator().install();
         f.await([&] { return calls == 1 && f.runtime->coordinator().view().phase == Phase::deferred; });
         HDRSHOT_CHECK(f.wire.checks == 2 && f.wire.releases == 0 && f.wire.downloads == 1);
       }},
      {"unchanged candidate installation receives freshly signed revision", [] {
         Fixture f; QByteArray captured;
         RuntimeTestAccess::replace_native(*f.runtime, std::make_unique<CapturingInstaller>(captured));
         f.start(); f.await([&] { return f.runtime->coordinator().view().phase == Phase::ready; });
         f.wire.catalog = catalog_revision(f, 2);
         const auto expected = f.wire.catalog;
         RuntimeTestAccess::idle(*f.runtime); f.runtime->coordinator().install();
         f.await([&] { return f.runtime->coordinator().view().phase == Phase::deferred; });
         HDRSHOT_CHECK(captured == expected && !f.runtime->barrier_active());
       }},
      {"withdrawal leaves retry route and successful empty check clears error", [] {
         Fixture f;
         f.start(); f.await([&] { return f.runtime->coordinator().view().phase == Phase::ready; });
         f.wire.catalog = catalog_revision(f, 2, false, [](QJsonObject &p) { p["targets"] = QJsonArray{}; });
         RuntimeTestAccess::idle(*f.runtime); f.runtime->coordinator().install();
         f.await([&] { return f.runtime->coordinator().view().phase == Phase::check_failed; });
         f.runtime->coordinator().trigger(200, false, true);
         f.await([&] { return f.runtime->coordinator().view().phase == Phase::idle; });
         HDRSHOT_CHECK(!f.runtime->coordinator().view().error && !f.runtime->coordinator().view().candidate);
         HDRSHOT_CHECK(f.wire.downloads == 1 && f.wire.releases == 0);
       }},
      {"login startup stays silent across automatic ready and later ticks until user interaction", [] {
         Fixture f;
         f.runtime->set_interactive_presentation_enabled(false);
         RuntimeTestAccess::idle(*f.runtime);
         f.start();
         f.await([&] { return f.runtime->coordinator().view().phase == Phase::ready &&
                             RuntimeTestAccess::has_confirmation(*f.runtime); });
         for (int tick = 0; tick < 20; ++tick) {
           f.runtime->coordinator().tick(1000 + tick);
           QApplication::processEvents();
           HDRSHOT_CHECK(!RuntimeTestAccess::has_window(*f.runtime));
           HDRSHOT_CHECK(RuntimeTestAccess::has_confirmation(*f.runtime));
           HDRSHOT_CHECK(!f.runtime->busy());
         }
         f.runtime->allow_interactive_presentation();
         HDRSHOT_CHECK(RuntimeTestAccess::has_window(*f.runtime));
         HDRSHOT_CHECK(f.runtime->update_window().isVisible());
         HDRSHOT_CHECK(RuntimeTestAccess::has_confirmation(*f.runtime));
         f.runtime->update_window().hide();
         f.runtime->allow_interactive_presentation();
         HDRSHOT_CHECK(!f.runtime->update_window().isVisible());
         f.runtime->show_details();
         HDRSHOT_CHECK(f.runtime->update_window().isVisible());
         f.runtime->update_window().later();
         HDRSHOT_CHECK(!RuntimeTestAccess::has_confirmation(*f.runtime));
       }},
      {"login startup restores ready and manual pending without presentation on later ticks", [] {
         for (bool pending : {false, true}) {
           Fixture f;
           f.runtime->set_interactive_presentation_enabled(false);
           RuntimeTestAccess::idle(*f.runtime);
           f.start();
           f.await([&] { return f.runtime->coordinator().view().phase == Phase::ready; });
           f.runtime.reset();
           if (pending) {
             auto record = read_json(f.dir.path() + "/slot.json").value();
             record["phase"] = "manual_pending";
             record["mount"] = "/Volumes/isolated-unmounted-fixture";
             HDRSHOT_CHECK(save_json(f.dir.path() + "/slot.json", record));
           }
           f.runtime = std::make_unique<Runtime>(f.identity, std::make_unique<Installer>(),
               [] { return false; }, [] { return false; });
           f.runtime->set_interactive_presentation_enabled(false);
           RuntimeTestAccess::configure(*f.runtime, f.trust, f.dir.path(), f.wire);
           f.start();
           f.await([&] { return f.runtime->coordinator().view().phase ==
               (pending ? Phase::manual_pending : Phase::ready); });
           for(int tick=0;tick<20;++tick) {
             f.runtime->coordinator().tick(QDateTime::currentSecsSinceEpoch()+tick);
             QApplication::processEvents();
             HDRSHOT_CHECK(!RuntimeTestAccess::has_window(*f.runtime));
             HDRSHOT_CHECK(!f.runtime->busy());
           }
           HDRSHOT_CHECK(RuntimeTestAccess::has_confirmation(*f.runtime));
           HDRSHOT_CHECK(f.wire.downloads == 1);
           f.runtime->allow_interactive_presentation();
           HDRSHOT_CHECK(f.runtime->update_window().isVisible());
         }
       }},
      {"login startup errors never create a foreground window", [] {
         Fixture f; f.wire.catalog_status = 403;
         f.runtime->set_interactive_presentation_enabled(false);
         f.start();
         f.await([&] { return f.runtime->coordinator().view().phase == Phase::check_failed; });
         for(int tick=0;tick<10;++tick)f.runtime->coordinator().tick(200+tick);
         HDRSHOT_CHECK(!RuntimeTestAccess::has_window(*f.runtime));
         f.runtime->show_details();
         HDRSHOT_CHECK(f.runtime->update_window().isVisible());
       }},
      {"installation requires current authenticated catalog even with ready package", [] {
         for (int mode = 0; mode < 7; ++mode) {
           Fixture f; int calls = 0;
           RuntimeTestAccess::replace_native(*f.runtime, std::make_unique<FailedInstaller>(calls));
           f.start(); f.await([&] { return f.runtime->coordinator().view().phase == Phase::ready; });
           if (mode == 0) f.wire.catalog_status = 403;
           if (mode == 1) f.wire.catalog = "invalid signature";
           if (mode == 2) f.wire.catalog = catalog_revision(f, 0);
           if (mode == 3) f.wire.catalog = catalog_revision(f, 1, true);
           if (mode == 4) { f.wire.catalog_status = 304; f.wire.catalog.clear(); }
           if (mode == 5) { f.wire.catalog_status = 304; f.wire.catalog.clear();
                           HDRSHOT_CHECK(QFile::remove(f.dir.path() + "/cache.json")); }
           if (mode == 6) { RuntimeTestAccess::metadata_budget(*f.runtime, 30); f.wire.catalog_delay_ms = 100; }
           RuntimeTestAccess::idle(*f.runtime); f.runtime->coordinator().install();
           f.await([&] { return f.runtime->coordinator().view().phase ==
                             (mode == 4 ? Phase::deferred : Phase::check_failed); });
           HDRSHOT_CHECK(calls == (mode == 4 ? 1 : 0));
           HDRSHOT_CHECK(!f.runtime->barrier_active() && f.wire.releases == 0);
         }
       }},
      {"install rechecks every signed candidate field and releases barrier", [] {
         for (int mode = 0; mode < 11; ++mode) {
           Fixture f; int calls = 0;
           RuntimeTestAccess::replace_native(*f.runtime, std::make_unique<FailedInstaller>(calls));
           f.start(); f.await([&] { return f.runtime->coordinator().view().phase == Phase::ready; });
           f.wire.catalog = catalog_revision(f, 2, false, [mode](QJsonObject &p) {
             auto ts = p["targets"].toArray(); auto t = ts[0].toObject();
             switch (mode) {
             case 0: p["targets"] = QJsonArray{}; return;
             case 1: t["sha256"] = QString(64, 'a'); break;
             case 2: t["asset_size"] = t["asset_size"].toDouble() + 1; break;
             case 3: t["asset_url"] = t["asset_url"].toString() + "?changed=1"; break;
             case 4: t["prerelease"] = true; break;
             case 5: t["minimum_os"] = "99.0.0"; break;
             case 6: t["installer_protocol"] = 2; break;
             case 7: t["release_id"] = 999; break;
             case 8: t["asset_id"] = 999; break;
             case 9: t["version"] = "0.1.6"; t["release_tag"] = "v0.1.6"; break;
             case 10: t["asset_name"] = "changed.dmg";
                      t["asset_url"] = "https://github.com/OverDriveHuang/SeriousShot/releases/download/v0.1.5/changed.dmg"; break;
             }
             ts[0] = t; p["targets"] = ts;
           });
           RuntimeTestAccess::idle(*f.runtime); f.runtime->coordinator().install();
           f.await([&] { return f.runtime->coordinator().view().phase == Phase::check_failed; });
           HDRSHOT_CHECK(calls == 0 && !f.runtime->barrier_active() && f.wire.releases == 0);
           f.runtime->coordinator().tick(300);
           HDRSHOT_CHECK(calls == 0);
         }
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
