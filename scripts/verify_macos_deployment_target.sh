#!/bin/sh

set -eu

bundle_path=${1:?"usage: verify_macos_deployment_target.sh /path/to/SeriousShot.app [15.5]"}
required_target=${2:-15.5}
main_executable=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' \
  "$bundle_path/Contents/Info.plist")
main_binary="$bundle_path/Contents/MacOS/$main_executable"
plist_path="$bundle_path/Contents/Info.plist"

if [ ! -f "$main_binary" ]; then
  echo "missing main executable: $main_binary" >&2
  exit 2
fi

version_is_not_newer() {
  candidate=$1
  ceiling=$2
  awk -v candidate="$candidate" -v ceiling="$ceiling" 'BEGIN {
    split(candidate, c, "."); split(ceiling, m, ".");
    for (i = 1; i <= 3; ++i) {
      cv = (c[i] == "" ? 0 : c[i]) + 0;
      mv = (m[i] == "" ? 0 : m[i]) + 0;
      if (cv < mv) exit 0;
      if (cv > mv) exit 1;
    }
    exit 0;
  }'
}

read_minos_values() {
  otool -l "$1" | awk '
    $1 == "cmd" && ($2 == "LC_BUILD_VERSION" || $2 == "LC_VERSION_MIN_MACOSX") { in_version = 1; next }
    in_version && ($1 == "minos" || $1 == "version") { print $2; in_version = 0 }
  '
}

main_minos=$(read_minos_values "$main_binary" | head -n 1)
if [ "$main_minos" != "$required_target" ]; then
  echo "main executable minos is $main_minos; expected $required_target" >&2
  exit 1
fi

if [ ! -f "$plist_path" ]; then
  echo "missing Info.plist: $plist_path" >&2
  exit 2
fi

plist_target=$(/usr/libexec/PlistBuddy -c 'Print :LSMinimumSystemVersion' "$plist_path" 2>/dev/null || true)
if [ "$plist_target" != "$required_target" ]; then
  echo "Info.plist LSMinimumSystemVersion is ${plist_target:-missing}; expected $required_target" >&2
  exit 1
fi

find "$bundle_path/Contents" -type f -print0 | while IFS= read -r -d '' candidate; do
  if ! file -b "$candidate" | grep -q 'Mach-O'; then
    continue
  fi
  minos_values=$(read_minos_values "$candidate")
  if [ -z "$minos_values" ]; then
    echo "cannot read deployment target: $candidate" >&2
    exit 1
  fi
  for minos in $minos_values; do
    if ! version_is_not_newer "$minos" "$required_target"; then
      echo "$candidate declares minos $minos, newer than $required_target" >&2
      exit 1
    fi
  done
done

echo "verified macOS deployment target $required_target: $bundle_path"
