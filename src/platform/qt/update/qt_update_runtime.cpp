#include "platform/qt/update/qt_update_runtime.hpp"
#include "core/build_metadata.hpp"
#include "ui/qt/settings_window.hpp"
#include <QApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QOperatingSystemVersion>
#include <QStandardPaths>
#include <QSysInfo>
#include <QUuid>
namespace hdrshot::update {
Identity runtime_identity(const QString &install) {
  Identity i;
  auto v = Version::parse(product_version());
  if (v)
    i.version = v.value();
  i.install = install.toStdString();
  i.channel = compiled_trust().channel.toStdString();
  const auto os = QOperatingSystemVersion::current();
  i.os = {static_cast<std::uint32_t>(os.majorVersion()),
          static_cast<std::uint32_t>(std::max(0, os.minorVersion())),
          static_cast<std::uint32_t>(std::max(0, os.microVersion()))};
  i.platform = QSysInfo::productType() == "windows" ? "windows" : "macos";
  i.architecture = QSysInfo::buildCpuArchitecture() == "arm64" ? "arm64" : "x64";
  return i;
}
Runtime::Runtime(Identity i, std::unique_ptr<NativeInstaller> native, std::function<bool()> busy,
                 std::function<bool()> quit, QObject *parent)
    : QObject(parent), identity_(std::move(i)), native_(std::move(native)),
      trust_(compiled_trust()),
      slot_(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
                "/OverDrive/SeriousShot/updates",
            QString::fromStdString(identity_.install), trust_.channel),
      busy_(std::move(busy)), quit_(std::move(quit)), coordinator_(identity_, *this) {
  const auto partition =
      QString::fromLatin1(QCryptographicHash::hash(QByteArray::fromStdString(
                                                       identity_.install + "|" + identity_.channel),
                                                   QCryptographicHash::Sha256)
                              .toHex());
  state_path_ = slot_.root + "/installations/" + partition + "/state.json";
  cache_path_ = slot_.root + "/catalog-" + trust_.channel + ".json";
  const auto state = read_json(state_path_);
  if (state)
    state_ = state.value();
  worker_ = new QObject;
  worker_->moveToThread(&thread_);
  QObject::connect(&thread_, &QThread::finished, worker_, &QObject::deleteLater);
  thread_.start();
  work([this] {
    network_ = new QNetworkAccessManager(worker_);
    const auto cache = read_json(cache_path_);
    if (cache) {
      cache_ = cache.value();
      envelope_ = QByteArray::fromBase64(cache_["envelope"].toString().toLatin1());
    }
  });
  coordinator_.changed = [this](const View &v) { render(v); };
  timer_.setInterval(1000);
  QObject::connect(&timer_, &QTimer::timeout, this, [this] {
    coordinator_.tick(QDateTime::currentSecsSinceEpoch());
    if (QDateTime::currentSecsSinceEpoch() % 30 == 0)
      work([this] { (void)cleanup_managed(slot_.root); });
  });
}
Runtime::~Runtime() {
  stopped_ = true;
  if (settings_)
    settings_->set_update_opener({});
  timer_.stop();
  QMetaObject::invokeMethod(
      worker_,
      [this] {
        if (reply_)
          reply_->abort();
        writer_.reset();
        slot_.release();
      },
      Qt::BlockingQueuedConnection);
  thread_.quit();
  thread_.wait();
}
void Runtime::work(std::function<void()> f) {
  QMetaObject::invokeMethod(worker_, std::move(f), Qt::QueuedConnection);
}
void Runtime::ui(std::function<void()> f) {
  QMetaObject::invokeMethod(
      this,
      [this, f = std::move(f)] {
        if (!stopped_)
          f();
      },
      Qt::QueuedConnection);
}
void Runtime::start() {
  work([this] {
    (void)cleanup_managed(slot_.root);
    auto acquired = slot_.acquire();
    if (!acquired && slot_.lock && slot_.record.contains("journal"))
      acquired = native_->recover(slot_);
    if (acquired && slot_.record["owner"] == slot_.owner &&
        slot_.record["channel"] == slot_.channel) {
      auto prior = Version::parse(slot_.record["version"].toString().toStdString());
      if (prior && identity_.version >= prior.value() &&
          slot_.record["phase"] == "manual_pending") {
        auto detached = native_->cancel_manual(slot_);
        if (detached) {
          auto removed = slot_.remove_payload();
          if (removed) {
            slot_.record["phase"] = "empty";
            (void)slot_.save();
          }
        }
      }
    }
    std::optional<Candidate> recovered_candidate;
    std::optional<Receipt> recovered_receipt;
    const bool pending = slot_.record["phase"] == "manual_pending";
    if (acquired) {
      auto catalog = verify_catalog(envelope_, trust_);
      if (catalog) {
        for (const auto &c : catalog.value().targets) {
          if (c.key() == slot_.record["key"].toString().toStdString() &&
              c.version > identity_.version) {
            auto verified = slot_.verified(c);
            if (verified) {
              recovered_candidate = c;
              recovered_receipt = verified.value();
            }
            break;
          }
        }
      }
    }
    slot_.release();
    ui([this, recovered_candidate, recovered_receipt, pending] {
      if (recovered_candidate && recovered_receipt)
        coordinator_.restore(*recovered_candidate, *recovered_receipt, pending);
      timer_.start();
      coordinator_.trigger(QDateTime::currentSecsSinceEpoch(), true);
    });
  });
}
void Runtime::attach(SettingsWindow &w) {
  settings_ = &w;
  w.set_update_opener([this] { show_details(); });
  render(coordinator_.view());
}
void Runtime::delay(unsigned seconds, std::function<void()> f) {
  QTimer::singleShot(static_cast<int>(seconds * 1000), this, std::move(f));
}
bool Runtime::busy() const {
  return (busy_ && busy_()) || QApplication::activeModalWidget() != nullptr;
}
bool Runtime::manual_install() const { return native_->manual(); }
void Runtime::persist_prompt(const std::string &key, Done done) {
  state_["prompted"] = QString::fromStdString(key);
  const auto state = state_;
  work([this, state, done = std::move(done)] {
    auto saved = save_json(state_path_, state);
    ui([done, saved = std::move(saved)]() mutable { done(std::move(saved)); });
  });
}
bool Runtime::was_prompted(const std::string &key) const {
  return state_["prompted"].toString().toStdString() == key;
}
UpdateWindow &Runtime::update_window() {
  if (!update_window_) {
    update_window_ = std::make_unique<UpdateWindow>();
    update_window_->install = [this] {
      if (confirmation_) {
        auto done = std::move(confirmation_);
        done(true);
      } else
        coordinator_.install();
    };
    update_window_->later = [this] {
      if (confirmation_) {
        auto done = std::move(confirmation_);
        done(false);
      } else
        coordinator_.later();
    };
    update_window_->cancel = [this] { coordinator_.cancel(); };
    update_window_->retry = [this] {
      coordinator_.trigger(QDateTime::currentSecsSinceEpoch(), false, true);
    };
    update_window_->set_view(coordinator_.view());
  }
  return *update_window_;
}
void Runtime::show_details() {
  auto &window = update_window();
  window.set_view(coordinator_.view());
  window.present(settings_ ? settings_->window_activation_port() : nullptr);
}
void Runtime::ask_install(const Candidate &, std::function<void(bool)> done) {
  confirmation_generation_ = coordinator_.view().generation;
  confirmation_ = std::move(done);
  show_details();
}
void Runtime::get(const QUrl &url, const QByteArray &etag,
                  std::function<void(Result<QByteArray, Error>, QByteArray)> done, int redirects) {
  const bool is_catalog = url.host() == "raw.githubusercontent.com";
  const auto remaining = metadata_deadline_.remainingTime();
  if (remaining <= 0) {
    done(Result<QByteArray, Error>::failure(failure(
             "metadata_deadline", ErrorCode::precondition_failed, Retryability::same_input)),
         {});
    return;
  }
  if (!allowed_url(url, is_catalog) || redirects > 5) {
    done(Result<QByteArray, Error>::failure(failure("network_source")), {});
    return;
  }
  QNetworkRequest request(url);
  request.setRawHeader("User-Agent", "SeriousShot-updater/1");
  request.setRawHeader("Accept", "application/vnd.github+json");
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::ManualRedirectPolicy);
  request.setTransferTimeout(30000);
  if (!etag.isEmpty())
    request.setRawHeader("If-None-Match", etag);
  auto *r = network_->get(request);
  auto *deadline = new QTimer(r);
  deadline->setSingleShot(true);
  QObject::connect(deadline, &QTimer::timeout, r, &QNetworkReply::abort);
  deadline->start(static_cast<int>(std::min<qint64>(30000, remaining)));
  QObject::connect(r, &QNetworkReply::readyRead, r, [r] {
    if (r->bytesAvailable() > 512 * 1024)
      r->abort();
  });
  QObject::connect(
      r, &QNetworkReply::finished, worker_, [this, r, etag, done = std::move(done), redirects] {
        const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto location = r->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
        const auto bytes = r->readAll();
        const auto response_etag = r->rawHeader("ETag");
        const auto error = r->error();
        const auto original = r->url();
        r->deleteLater();
        if (status >= 300 && status < 400 && status != 304 && !location.isEmpty()) {
          get(original.resolved(location), etag, std::move(done), redirects + 1);
          return;
        }
        if (status == 304) {
          done(Result<QByteArray, Error>::success({}),
               response_etag.isEmpty() ? etag : response_etag);
          return;
        }
        if (error != QNetworkReply::NoError || status != 200 || bytes.isEmpty() ||
            bytes.size() > 512 * 1024) {
          // Rate limits end this round; the normal schedule/manual action can
          // start a later round.
          const bool transient = status == 0 || status >= 500;
          done(Result<QByteArray, Error>::failure(
                   failure(status == 404                    ? "更新清单不存在"
                           : status == 429 || status == 403 ? "更新源限流，请稍后重试"
                                                            : "更新网络请求失败",
                           ErrorCode::precondition_failed,
                           transient ? Retryability::same_input : Retryability::never)),
               {});
          return;
        }
        done(Result<QByteArray, Error>::success(bytes), response_etag);
      });
}
void Runtime::check(const Identity &identity, std::function<void(Result<Checked, Error>)> done) {
  state_["last_check"] = QString::number(QDateTime::currentSecsSinceEpoch());
  const auto attempted_state = state_;
  work([this, attempted_state] { (void)save_json(state_path_, attempted_state); });
  work([this, identity, done = std::move(done)] {
    check_worker(identity, [this, done](auto r) {
      ui([this, done, r = std::move(r)]() mutable {
        if (r) {
          state_["last_success"] = QString::number(QDateTime::currentSecsSinceEpoch());
          const auto saved_state = state_;
          work([this, saved_state] { (void)save_json(state_path_, saved_state); });
        }
        done(std::move(r));
      });
    });
  });
}
void Runtime::check_worker(const Identity &identity,
                           std::function<void(Result<Checked, Error>)> done) {
  metadata_deadline_ = QDeadlineTimer(30000);
  if (!trust_.configured()) {
    done(Result<Checked, Error>::failure(failure("正式更新签名源尚未配置")));
    return;
  }
  // Catalog acceptance is shared across installations, not a per-Runtime
  // snapshot.
  {
    QDir().mkpath(QFileInfo(cache_path_).absolutePath());
    QLockFile catalog_lock(cache_path_ + ".lock");
    catalog_lock.setStaleLockTime(0);
    if (!catalog_lock.tryLock(0)) {
      done(Result<Checked, Error>::failure(
          failure("catalog_busy", ErrorCode::precondition_failed, Retryability::same_input)));
      return;
    }
    const auto current = read_json(cache_path_);
    if (!current) {
      done(Result<Checked, Error>::failure(current.error()));
      return;
    }
    cache_ = current.value();
  }
  const auto cached = QByteArray::fromBase64(cache_["envelope"].toString().toLatin1());
  const auto prior = verify_catalog(cached, trust_);
  const auto etag = prior ? cache_["etag"].toString().toLatin1() : QByteArray{};
  get(trust_.catalog, etag,
      [this, identity, cached, prior, done = std::move(done)](auto body, auto response_etag) {
        if (!body) {
          done(Result<Checked, Error>::failure(body.error()));
          return;
        }
        const auto bytes = body.value().isEmpty() ? cached : body.value();
        const auto verified = verify_catalog(bytes, trust_);
        if (!verified) {
          done(Result<Checked, Error>::failure(verified.error()));
          return;
        }
        if (prior && (verified.value().revision < prior.value().revision ||
                      (verified.value().revision == prior.value().revision &&
                       verified.value().digest != prior.value().digest))) {
          done(Result<Checked, Error>::failure(failure("catalog_replay_conflict")));
          return;
        }
        auto selected = select(verified.value().targets, identity);
        if (!selected) {
          done(Result<Checked, Error>::failure(selected.error()));
          return;
        }
        QLockFile catalog_lock(cache_path_ + ".lock");
        catalog_lock.setStaleLockTime(0);
        if (!catalog_lock.tryLock(0)) {
          done(Result<Checked, Error>::failure(
              failure("catalog_busy", ErrorCode::precondition_failed, Retryability::same_input)));
          return;
        }
        const auto current = read_json(cache_path_);
        if (!current) {
          done(Result<Checked, Error>::failure(current.error()));
          return;
        }
        const auto disk = verify_catalog(
            QByteArray::fromBase64(current.value()["envelope"].toString().toLatin1()), trust_);
        if (disk && (verified.value().revision < disk.value().revision ||
                     (verified.value().revision == disk.value().revision &&
                      verified.value().digest != disk.value().digest))) {
          done(Result<Checked, Error>::failure(failure("catalog_replay_conflict")));
          return;
        }
        envelope_ = bytes;
        cache_ = {{"envelope", QString::fromLatin1(bytes.toBase64())},
                  {"etag", QString::fromLatin1(response_etag)}};
        const auto saved = save_json(cache_path_, cache_);
        if (!saved) {
          done(Result<Checked, Error>::failure(saved.error()));
          return;
        }
        catalog_lock.unlock();
        std::optional<Candidate> incompatible;
        for (const auto &c : verified.value().targets)
          if (c.product == "SeriousShot" && c.channel == identity.channel &&
              c.platform == identity.platform && c.architecture == identity.architecture &&
              !c.draft && !c.prerelease && c.minimum_os > identity.os &&
              (!incompatible || c.version > incompatible->version))
            incompatible = c;
        // Even same-version metadata requests are authenticated and recorded.
        if (!selected.value() || selected.value()->version <= identity.version) {
          done(Result<Checked, Error>::success({selected.value(), incompatible}));
          return;
        }
        const auto candidate = *selected.value();
        get(QUrl("https://api.github.com/repos/OverDriveHuang/SeriousShot/"
                 "releases/" +
                 QString::number(candidate.release_id)),
            {}, [candidate, done](auto release, auto) {
              if (!release) {
                done(Result<Checked, Error>::failure(release.error()));
                return;
              }
              const auto valid =
                  verify_release(QJsonDocument::fromJson(release.value()).object(), candidate);
              if (!valid) {
                done(Result<Checked, Error>::failure(valid.error()));
                return;
              }
              done(Result<Checked, Error>::success({candidate}));
            });
      });
}
void Runtime::acquire_and_download(const Candidate &candidate,
                                   std::function<void(std::uint64_t, std::uint64_t)> progress,
                                   std::function<void(Result<Receipt, Error>)> done) {
  work([this, candidate, progress = std::move(progress), done = std::move(done)] {
    auto acquired = slot_.acquire();
    if (!acquired) {
      slot_.release();
      ui([done, e = acquired.error()] { done(Result<Receipt, Error>::failure(e)); });
      return;
    }
    auto existing = slot_.verified(candidate);
    if (existing) {
      slot_.release();
      ui([done, r = std::move(existing)]() mutable { done(std::move(r)); });
      return;
    }
    auto reserved = slot_.reserve(candidate);
    if (!reserved) {
      slot_.release();
      ui([done, e = reserved.error()] { done(Result<Receipt, Error>::failure(e)); });
      return;
    }
    download_worker(candidate, std::move(progress), std::move(done));
  });
}
void Runtime::download_worker(Candidate c,
                              std::function<void(std::uint64_t, std::uint64_t)> progress,
                              std::function<void(Result<Receipt, Error>)> done) {
  writer_ = std::make_unique<QFile>(slot_.payload());
  received_ = 0;
  download_error_.reset();
  download_deadline_ = QDeadlineTimer(download_timeout_ms_);
  download_done_ = std::move(done);
  ++operation_;
  const auto op = operation_;
  if (!writer_->open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
    writer_.reset();
    slot_.release();
    ui([done = std::move(download_done_)] {
      done(
          Result<Receipt, Error>::failure(failure("payload_create", ErrorCode::path_not_writable)));
    });
    return;
  }
  auto finish =
      std::make_shared<std::function<void(Result<Receipt, Error>)>>([this, op](auto result) {
        if (op != operation_)
          return;
        writer_.reset();
        if (!result) {
          auto cleaned = slot_.remove_payload();
          slot_.record["phase"] = cleaned ? "empty" : "cleanup_blocked";
          (void)slot_.save();
          if (!cleaned)
            result = Result<Receipt, Error>::failure(cleaned.error());
        }
        slot_.release();
        auto callback = std::move(download_done_);
        ui([callback, result = std::move(result)]() mutable { callback(std::move(result)); });
      });
  auto start = std::make_shared<std::function<void(QUrl, int)>>();
  *start = [this, c, op, progress, finish, start](QUrl url, int redirects) {
    if (!allowed_url(url) || redirects > 5) {
      (*finish)(Result<Receipt, Error>::failure(failure("download_redirect")));
      *start = {};
      return;
    }
    if (download_deadline_.remainingTime() <= 0) {
      (*finish)(Result<Receipt, Error>::failure(failure("download_total_deadline")));
      *start = {};
      return;
    }
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(60000);
    auto *r = network_->get(request);
    r->setReadBufferSize(1024 * 1024);
    reply_ = r;
    auto *timeout = new QTimer(r);
    timeout->setSingleShot(true);
    timeout->start(static_cast<int>(std::max<qint64>(0, download_deadline_.remainingTime())));
    QObject::connect(timeout, &QTimer::timeout, r, &QNetworkReply::abort);
    QObject::connect(r, &QNetworkReply::readyRead, worker_, [this, r, c, op, progress] {
      if (op != operation_) {
        r->readAll();
        return;
      }
      const auto status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
      if (status != 200)
        return;
      while (r->bytesAvailable() > 0) {
        const auto chunk = r->read(1024 * 1024);
        received_ += static_cast<std::uint64_t>(chunk.size());
        if (received_ > c.asset_size || writer_->write(chunk) != chunk.size()) {
          download_error_ = failure("下载写入失败或大小超限", ErrorCode::storage_full);
          r->abort();
          return;
        }
        ui([progress, n = received_, total = c.asset_size] { progress(n, total); });
      }
    });
    QObject::connect(
        r, &QNetworkReply::finished, worker_, [this, r, c, op, finish, start, redirects] {
          const auto status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
          const auto redirect = r->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
          const auto url = r->url();
          const auto error = r->error();
          const auto remaining = r->readAll();
          r->deleteLater();
          reply_.clear();
          if (op != operation_) {
            *start = {};
            return;
          }
          if (status >= 300 && status < 400 && !redirect.isEmpty()) {
            (*start)(url.resolved(redirect), redirects + 1);
            return;
          }
          if (!remaining.isEmpty() && status == 200) {
            received_ += static_cast<std::uint64_t>(remaining.size());
            if (received_ > c.asset_size || writer_->write(remaining) != remaining.size())
              download_error_ = failure("download_write");
          }
          if (download_error_ || error != QNetworkReply::NoError || status != 200 ||
              received_ != c.asset_size || !writer_->flush() || !durable_flush(*writer_)) {
            writer_.reset();
            const auto cleanup = slot_.remove_payload();
            slot_.record["phase"] = cleanup ? "empty" : "cleanup_blocked";
            (void)slot_.save();
            (*finish)(Result<Receipt, Error>::failure(
                cleanup ? (download_error_ ? *download_error_ : failure("下载失败"))
                        : cleanup.error()));
            *start = {};
            return;
          }
          writer_.reset();
          const auto hash = hash_file(slot_.payload());
          if (!hash || hash.value().toStdString() != c.sha256) {
            auto removed = slot_.remove_payload();
            slot_.record["phase"] = removed ? "empty" : "cleanup_blocked";
            (void)slot_.save();
            (*finish)(Result<Receipt, Error>::failure(removed ? failure("安装包校验失败")
                                                              : removed.error()));
            *start = {};
            return;
          }
          const auto suffix = c.platform == "windows" ? ".zip" : ".dmg";
          if (!QFile::rename(slot_.payload(), slot_.root + "/payload" + suffix)) {
            (*finish)(Result<Receipt, Error>::failure(failure("payload_rename")));
            *start = {};
            return;
          }
          slot_.record["suffix"] = suffix;
          slot_.record["phase"] = "ready";
          const auto saved = slot_.save();
          (*finish)(saved ? slot_.verified(c) : Result<Receipt, Error>::failure(saved.error()));
          *start = {};
        });
  };
  (*start)(QUrl(QString::fromStdString(c.asset_url)), 0);
}
void Runtime::cancel_download(Done done) {
  work([this, done = std::move(done)] {
    ++operation_;
    if (reply_)
      reply_->abort();
    writer_.reset();
    download_done_ = {};
    auto result = slot_.acquire();
    if (result) {
      if (slot_.record["owner"] != slot_.owner || slot_.record["channel"] != slot_.channel)
        result = Result<bool, Error>::failure(failure("slot_foreign"));
      else {
        if (slot_.record["phase"] == "manual_pending")
          result = native_->cancel_manual(slot_);
        if (result)
          result = slot_.remove_payload();
        if (result) {
          slot_.record["phase"] = "empty";
          result = slot_.save();
        }
      }
    }
    slot_.release();
    ui([done, r = std::move(result)]() mutable { done(std::move(r)); });
  });
}
void Runtime::install(const Candidate &c, const Receipt &receipt, Done done) {
#ifdef Q_OS_MACOS
  // Opening a manual DMG now hands off to a normal exit. Prevent new work during
  // asynchronous Release verification, then recheck busy on the GUI thread.
  barrier_ = true;
#else
  barrier_ = !manual_install();
#endif
  work([this, c, receipt, done = std::move(done)] {
    metadata_deadline_ = QDeadlineTimer(30000);
    get(QUrl("https://api.github.com/repos/OverDriveHuang/SeriousShot/"
             "releases/" +
             QString::number(c.release_id)),
        {}, [this, c, receipt, done](auto release, auto) {
          auto result = release
                            ? verify_release(QJsonDocument::fromJson(release.value()).object(), c)
                            : Result<bool, Error>::failure(release.error());
          if (result) {
            result = slot_.acquire();
            if (result) {
              auto checked = slot_.verified(c);
              if (!slot_.matches(receipt) || !checked)
                result = Result<bool, Error>::failure(failure(
                    "安装包已缺失或损坏，请检查更新以自动补下载", ErrorCode::object_not_found));
              else
                result = native_->install(slot_, c, envelope_, [this] {
                  struct Decision {
                    std::atomic_bool done{false};
                    bool accepted{};
                  };
                  auto decision = std::make_shared<Decision>();
                  ui([this, decision] {
                    // A quit callback can destroy its owner synchronously. Copy
                    // the callable first and never touch Runtime after invoking it.
                    const auto quit = quit_;
#ifdef Q_OS_MACOS
                    const bool allowed = !busy();
#else
                    constexpr bool allowed = true;
#endif
                    decision->accepted = allowed && quit && quit();
                    decision->done.store(true, std::memory_order_release);
                  });
                  while (!decision->done.load(std::memory_order_acquire) && !stopped_.load())
                    QThread::msleep(20);
                  return decision->done.load(std::memory_order_acquire) && decision->accepted;
                });
            }
            slot_.release();
          }
          ui([this, done, r = std::move(result)]() mutable {
            if (!r)
              barrier_ = false;
            done(std::move(r));
          });
        });
  });
}
void Runtime::render(const View &view) {
  if (view.generation != confirmation_generation_ ||
      (view.phase != Phase::ready && view.phase != Phase::deferred &&
       view.phase != Phase::manual_pending))
    confirmation_ = {};
  if (update_window_)
    update_window_->set_view(view);
  if (settings_)
    settings_->set_update_view(view, manual_install());
}
} // namespace hdrshot::update
