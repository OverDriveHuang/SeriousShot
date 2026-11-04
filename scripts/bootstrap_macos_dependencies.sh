#!/bin/zsh

set -euo pipefail

project_dir=${0:A:h:h}
tool_root="$project_dir/.build-tools"
venv_dir="$tool_root/cmake-venv"
qt_root="$tool_root/Qt"
qt_sdk="$qt_root/6.11.2/macos"
qt_cmake="$qt_sdk/lib/cmake/Qt6/Qt6Config.cmake"

if ! xcrun --find clang >/dev/null 2>&1; then
  echo "Xcode Command Line Tools are required. Run: xcode-select --install" >&2
  exit 1
fi

if [[ ! -x "$venv_dir/bin/python" ]]; then
  python3 -m venv "$venv_dir"
fi

"$venv_dir/bin/python" -m pip install --upgrade pip
"$venv_dir/bin/python" -m pip install "cmake==4.4.2" "aqtinstall==3.3.0"

if [[ ! -f "$qt_cmake" ]]; then
  "$venv_dir/bin/aqt" install-qt mac desktop 6.11.2 clang_64 \
    --outputdir "$qt_root" --archives qtbase qtdeclarative qttools qttranslations qtsvg
elif [[ ! -e "$qt_sdk/lib/QtQml.framework" ]]; then
  # lrelease 6.11.2 links QtQml. Repair this one missing host tool dependency
  # from the official same-version archive, preserving framework symlinks.
  qt_tool_staging=$(mktemp -d "${TMPDIR:-/tmp}/seriousshot-qt-tooling.XXXXXX")
  trap 'rm -rf -- "$qt_tool_staging"' EXIT
  "$venv_dir/bin/aqt" install-qt mac desktop 6.11.2 clang_64 \
    --outputdir "$qt_tool_staging" --archives qtdeclarative
  /usr/bin/ditto "$qt_tool_staging/6.11.2/macos/lib/QtQml.framework" "$qt_sdk/lib/QtQml.framework"
fi

"$qt_sdk/bin/lupdate" -version
"$qt_sdk/bin/lrelease" -version
for locale in en zh_CN; do
  if [[ ! -f "$qt_sdk/translations/qtbase_${locale}.qm" ]]; then
    echo "Missing Qt Translations 6.11.2 catalog: qtbase_${locale}.qm" >&2
    exit 1
  fi
done

echo "Dependencies ready under $tool_root"
