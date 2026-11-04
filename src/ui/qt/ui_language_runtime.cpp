#include "ui/qt/ui_language_runtime.hpp"

#include <QCoreApplication>

#include <utility>

namespace hdrshot {
namespace {

//% "重启 SeriousShot 后切换为简体中文。"
constexpr auto restart_zh = QT_TRID_NOOP("language.restart.zh");
//% "重启 SeriousShot 后切换为英文。"
constexpr auto restart_en = QT_TRID_NOOP("language.restart.en");
//% "语言资源不可用。"
constexpr auto unavailable = QT_TRID_NOOP("language.catalog.unavailable");

Error catalog_unavailable(const char* language) {
  return {ErrorCode::precondition_failed, "QtUiLanguageRuntime", Retryability::after_recreate,
          {{"reason", "language_catalog_unavailable"}, {"catalog", language}}};
}

bool load_catalog(QTranslator& translator, const QString& path) {
  // A Qt-only or wrong-file resource must not count as an application catalog.
  return translator.load(path) && !translator.translate(nullptr, restart_en).isEmpty() &&
      !translator.translate(nullptr, restart_zh).isEmpty() &&
      !translator.translate(nullptr, unavailable).isEmpty();
}

}  // namespace

QtUiLanguageRuntime::QtUiLanguageRuntime(UiLanguageCatalogPaths paths) : paths_(std::move(paths)) {}

Result<UiLanguageInitializationReceipt, Error> QtUiLanguageRuntime::initialize(
    QCoreApplication& app, const std::optional<UiLanguage> preference,
    const std::function<UiLanguage()>& detector) {
  using Initialization = Result<UiLanguageInitializationReceipt, Error>;
  if (initialized_) {
    return Initialization::failure(
        {ErrorCode::state_inconsistent, "QtUiLanguageRuntime", Retryability::never,
         {{"reason", "language_already_initialized"}}});
  }
  const auto requested = preference ? *preference : detector();
  if (!load_catalog(english_, paths_.english) || !app.installTranslator(&english_)) {
    return Initialization::failure(catalog_unavailable("en"));
  }

  std::optional<Error> fault;
  if (requested == UiLanguage::simplified_chinese) {
    if (load_catalog(chinese_, paths_.simplified_chinese) && app.installTranslator(&chinese_)) {
      active_language_ = UiLanguage::simplified_chinese;
    } else {
      fault = catalog_unavailable("zh-CN");
    }
  }
  initialized_ = true;
  return Initialization::success({requested, active_language_, std::move(fault)});
}

QString QtUiLanguageRuntime::restart_hint(const UiLanguage target) const {
  const auto hint = text(target, target == UiLanguage::simplified_chinese ? restart_zh : restart_en);
  if (!hint.isEmpty() && restart_available(target)) return hint;
  const auto fault = english_.translate(nullptr, unavailable);
  return fault.isEmpty() ? QStringLiteral("Language resources unavailable.") : fault;
}

QString QtUiLanguageRuntime::text(const UiLanguage target, const char* id) const {
  const bool chinese = target == UiLanguage::simplified_chinese;
  if (initialized_) {
    const auto& active_catalog = chinese ? chinese_ : english_;
    if (target == active_language_ || !chinese) {
      const auto hint = active_catalog.translate(nullptr, id);
      if (!hint.isEmpty()) return hint;
    } else {
      QTranslator target_catalog;
      if (load_catalog(target_catalog, paths_.simplified_chinese)) {
        return target_catalog.translate(nullptr, id);
      }
    }
  }
  return {};
}

bool QtUiLanguageRuntime::restart_available(const UiLanguage target) const {
  for (const auto* id : {"language.restart.title", "language.restart.confirm_zh",
                        "language.restart.confirm_en", "language.restart.action",
                        "language.restart.cancel", "language.restart.busy"}) {
    if (text(target, id).isEmpty()) return false;
  }
  return true;
}

}  // namespace hdrshot
