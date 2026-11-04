#pragma once

#include "ports/export_ports.hpp"

#include <QString>
#include <QTranslator>

#include <functional>
#include <optional>

class QCoreApplication;

namespace hdrshot {

struct UiLanguageInitializationReceipt {
  UiLanguage requested_language;
  UiLanguage active_language;
  std::optional<Error> catalog_error;
};

struct UiLanguageCatalogPaths {
  QString english{QStringLiteral(":/seriousshot/i18n/seriousshot_en.qm")};
  QString simplified_chinese{QStringLiteral(":/seriousshot/i18n/seriousshot_zh_CN.qm")};
};

class QtUiLanguageRuntime {
 public:
  explicit QtUiLanguageRuntime(UiLanguageCatalogPaths paths = {});
  [[nodiscard]] Result<UiLanguageInitializationReceipt, Error> initialize(
      QCoreApplication& app, std::optional<UiLanguage> preference,
      const std::function<UiLanguage()>& detector);
  [[nodiscard]] UiLanguage active_language() const noexcept { return active_language_; }
  [[nodiscard]] QString restart_hint(UiLanguage target) const;
  [[nodiscard]] QString text(UiLanguage target, const char* id) const;
  [[nodiscard]] bool restart_available(UiLanguage target) const;

 private:
  UiLanguageCatalogPaths paths_;
  QTranslator english_;
  QTranslator chinese_;
  bool initialized_{};
  UiLanguage active_language_{UiLanguage::english};
};

}  // namespace hdrshot
