#!/usr/bin/env bash
# Build andromeda-apps (the LaunchServices helper for the apps staged by
# scripts/add-ipa.py) for iOS 16 arm64 on macOS with Xcode (GitHub macOS
# runner), signed ad hoc with its entitlements like phone/andromedad/build.sh
# (ios-flame's phone/tools/build.sh, Nick Wylynko, MIT; see CREDITS.md). Our
# kernel's AMFI accepts any cdhash (patch 0096), so no certificate is needed.
#
#   phone/andromeda-apps/build.sh [OUT]          (or OUT=... in the environment)
#
# Writes OUT/andromeda-apps/andromeda-apps. Default OUT: <repo>/out/phone.
# Only the program is built here: the apps themselves never leave the PC
# (scripts/add-ipa.py stages them locally, install-root.sh copies them).
# Nothing from the iOS image is used: only the iphoneos SDK of Xcode.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${1:-${OUT:-$repo/out/phone}}
dst=$OUT/andromeda-apps
mkdir -p "$dst"

sdk=$(xcrun --sdk iphoneos --show-sdk-path)
cc=$(xcrun --sdk iphoneos --find clang)
echo "andromeda-apps: SDK $sdk"
"$cc" -arch arm64 -isysroot "$sdk" -miphoneos-version-min=16.0 \
	-std=gnu11 -O2 -Wall -Wextra -fobjc-arc \
	-framework Foundation \
	-o "$dst/andromeda-apps" "$here/andromeda-apps.m"
codesign -f -s - --entitlements "$here/andromeda-apps.entitlements" "$dst/andromeda-apps"
codesign -d --entitlements - "$dst/andromeda-apps"
file "$dst/andromeda-apps"
ls -l "$dst"
