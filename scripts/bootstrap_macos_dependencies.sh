#!/bin/zsh

set -euo pipefail

project_dir=${0:A:h:h}
tool_root="$project_dir/.build-tools"
venv_dir="$tool_root/cmake-venv"
qt_root="$tool_root/Qt"
qt_cmake="$qt_root/6.11.1/macos/lib/cmake/Qt6/Qt6Config.cmake"

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
  "$venv_dir/bin/aqt" install-qt mac desktop 6.11.1 clang_64 \
    --outputdir "$qt_root"
fi

echo "Dependencies ready under $tool_root"
