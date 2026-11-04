#include "platform/ui_language.hpp"
#include "test_support.hpp"
#include "ui/qt/ui_language_runtime.hpp"

#include <QCoreApplication>

namespace {

using hdrshot::QtUiLanguageRuntime;
using hdrshot::UiLanguage;
constexpr auto en = UiLanguage::english;
constexpr auto zh = UiLanguage::simplified_chinese;

QCoreApplication& app() {
  return *QCoreApplication::instance();
}

void native_language_mapping() {
  using hdrshot::ui_language_from_apple_language_tag;
  for (const auto tag : {"zh", "zh-Hans", "zh-Hant-TW", "zh-HK", "ZH_cn"}) {
    HDRSHOT_CHECK(ui_language_from_apple_language_tag(tag) == zh);
  }
  for (const auto tag : {"", "fr", "en-HK", "zhx", "x-zh", "123", "yue-Hant-HK"}) {
    HDRSHOT_CHECK(ui_language_from_apple_language_tag(tag) == en);
  }
#ifdef Q_OS_WIN
  using hdrshot::ui_language_from_windows_language_id;
  for (const auto id : {std::uint16_t{0x0404}, std::uint16_t{0x0804}, std::uint16_t{0x0c04},
                        std::uint16_t{0x1004}, std::uint16_t{0x1404}}) {
    HDRSHOT_CHECK(ui_language_from_windows_language_id(id) == zh);
  }
  for (const auto id : {std::uint16_t{0x0000}, std::uint16_t{0x0409}, std::uint16_t{0x0c0c}}) {
    HDRSHOT_CHECK(ui_language_from_windows_language_id(id) == en);
  }
#endif
}

void explicit_preference_bypasses_detector_and_stays_fixed() {
  QtUiLanguageRuntime runtime;
  int detector_calls = 0;
  const auto detector = [&] { ++detector_calls; return zh; };
  const auto initialized = runtime.initialize(app(), en, detector);
  HDRSHOT_CHECK(initialized && initialized.value().requested_language == en);
  HDRSHOT_CHECK(initialized.value().active_language == en && !initialized.value().catalog_error);
  HDRSHOT_CHECK(detector_calls == 0);
  HDRSHOT_CHECK(qtTrId("language.restart.en") == "Restart SeriousShot to apply English.");
  HDRSHOT_CHECK(runtime.restart_hint(zh) == QStringLiteral("重启 SeriousShot 后切换为简体中文。"));
  HDRSHOT_CHECK(runtime.active_language() == en);
  HDRSHOT_CHECK(qtTrId("language.restart.zh") == "Restart SeriousShot to apply Simplified Chinese.");
  HDRSHOT_CHECK(QCoreApplication::translate("QPlatformTheme", "Cancel") == "Cancel");
  HDRSHOT_CHECK(!runtime.initialize(app(), zh, detector));
  HDRSHOT_CHECK(detector_calls == 0 && runtime.active_language() == en);
}

void automatic_chinese_installs_chinese_and_qt_standard_catalog() {
  {
    QtUiLanguageRuntime runtime;
    int detector_calls = 0;
    const auto initialized = runtime.initialize(app(), std::nullopt, [&] {
      ++detector_calls;
      return zh;
    });
    HDRSHOT_CHECK(initialized && initialized.value().requested_language == zh);
    HDRSHOT_CHECK(initialized.value().active_language == zh && !initialized.value().catalog_error);
    HDRSHOT_CHECK(detector_calls == 1);
    HDRSHOT_CHECK(qtTrId("language.restart.zh") == QStringLiteral("重启 SeriousShot 后切换为简体中文。"));
    HDRSHOT_CHECK(QCoreApplication::translate("QPlatformTheme", "Cancel") == QStringLiteral("取消"));
    HDRSHOT_CHECK(runtime.restart_hint(en) == "Restart SeriousShot to apply English.");
    HDRSHOT_CHECK(runtime.active_language() == zh);
    HDRSHOT_CHECK(qtTrId("language.restart.en") == QStringLiteral("重启 SeriousShot 后切换为英文。"));
  }
  HDRSHOT_CHECK(qtTrId("language.restart.zh") == "language.restart.zh");
  HDRSHOT_CHECK(QCoreApplication::translate("QPlatformTheme", "Cancel") == "Cancel");
}

void missing_chinese_catalog_falls_back_wholly_to_english() {
  hdrshot::UiLanguageCatalogPaths paths;
  paths.simplified_chinese = QStringLiteral(":/missing-ui-catalog/zh.qm");
  QtUiLanguageRuntime runtime(paths);
  const auto initialized = runtime.initialize(app(), zh, [] { return en; });
  HDRSHOT_CHECK(initialized && initialized.value().requested_language == zh);
  HDRSHOT_CHECK(initialized.value().active_language == en && initialized.value().catalog_error);
  HDRSHOT_CHECK(initialized.value().catalog_error->safe_context.at("catalog") == "zh-CN");
  HDRSHOT_CHECK(qtTrId("language.restart.zh") == "Restart SeriousShot to apply Simplified Chinese.");
  HDRSHOT_CHECK(QCoreApplication::translate("QPlatformTheme", "Cancel") == "Cancel");
  HDRSHOT_CHECK(runtime.restart_hint(zh) == "Language resources unavailable.");
}

void missing_english_catalog_refuses_startup() {
  hdrshot::UiLanguageCatalogPaths paths;
  paths.english = QStringLiteral(":/missing-ui-catalog/en.qm");
  QtUiLanguageRuntime runtime(paths);
  const auto initialized = runtime.initialize(app(), zh, [] { return en; });
  HDRSHOT_CHECK(!initialized);
  HDRSHOT_CHECK(initialized.error().safe_context.at("catalog") == "en");
  HDRSHOT_CHECK(qtTrId("language.restart.zh") == "language.restart.zh");
}

}  // namespace

int main(int argc, char** argv) {
  QCoreApplication core_app(argc, argv);
  return hdrshot::test::run({
      {"native language tags and Windows LANGIDs ignore region-only lookalikes", native_language_mapping},
      {"explicit preference bypasses native detection and stays fixed", explicit_preference_bypasses_detector_and_stays_fixed},
      {"automatic Chinese installs application and Qt Chinese resources", automatic_chinese_installs_chinese_and_qt_standard_catalog},
      {"missing Chinese resources fall back wholly to English", missing_chinese_catalog_falls_back_wholly_to_english},
      {"missing English resources refuse startup", missing_english_catalog_refuses_startup},
  });
}
