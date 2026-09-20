#!/bin/zsh

set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 /absolute/path/to/SeriousShot.app" >&2
  exit 2
fi

app_path=$1
identity_sha1=${HDRSHOT_CODESIGN_SHA1:-3ADD10D626DD6F1CD0705730CE09EC823F09205E}
expected_name=${HDRSHOT_CODESIGN_NAME:-HDRShot Local Development}
expected_bundle_id=dev.hdrshot.desktop

if [[ ! -d "$app_path" ]]; then
  echo "SeriousShot bundle does not exist: $app_path" >&2
  exit 1
fi

bundle_id=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' \
  "$app_path/Contents/Info.plist")
if [[ "$bundle_id" != "$expected_bundle_id" ]]; then
  echo "Unexpected bundle identifier: $bundle_id" >&2
  exit 1
fi

identities=$(/usr/bin/security find-identity -v -p codesigning)
if [[ "$identities" != *"$identity_sha1 \"$expected_name\""* ]]; then
  echo "Required code-signing identity is unavailable: $expected_name ($identity_sha1)" >&2
  exit 1
fi

# This development bundle loads the shared Qt SDK through build-tree rpaths.
# Do not enable Hardened Runtime here: Library Validation would reject those
# externally signed Qt frameworks because this local identity has no Team ID.
# A distributable bundle must first embed Qt, then sign nested code inside-out
# with its distribution identity and may enable Hardened Runtime there.
/usr/bin/codesign \
  --force \
  --deep \
  --timestamp=none \
  --identifier "$expected_bundle_id" \
  --sign "$identity_sha1" \
  "$app_path"

/usr/bin/codesign --verify --deep --strict --verbose=2 "$app_path"

details=$(/usr/bin/codesign -dv --verbose=4 "$app_path" 2>&1)
if [[ "$details" != *"Identifier=$expected_bundle_id"* ||
      "$details" != *"Authority=$expected_name"* ||
      "$details" == *"Signature=adhoc"* ||
      "$details" == *"(runtime)"* ]]; then
  echo "Signed bundle identity verification failed." >&2
  echo "$details" >&2
  exit 1
fi

requirement=$(/usr/bin/codesign -d -r- "$app_path" 2>&1)
if [[ "$requirement" != *"identifier \"$expected_bundle_id\""* ||
      "$requirement" == *"cdhash"* ]]; then
  echo "Unstable designated requirement: $requirement" >&2
  exit 1
fi

echo "codesignIdentity=$expected_name"
echo "codesignSha1=$identity_sha1"
echo "bundleIdentifier=$expected_bundle_id"
echo "$requirement"
