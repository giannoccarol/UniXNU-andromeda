#!/usr/bin/env bash
# Build andromedad for iOS 16 arm64 on macOS with Xcode (GitHub macOS runner),
# signed ad hoc with its entitlements, as ios-flame's phone/tools/build.sh
# does (Nick Wylynko, MIT; see CREDITS.md). Our kernel's AMFI accepts any
# cdhash (patch 0096), so no certificate is needed.
#
#   phone/andromedad/build.sh [OUT]          (or OUT=... in the environment)
#
# Writes OUT/andromedad/andromedad. Default OUT: <repo>/out/phone.
# Nothing from the iOS image is used: only the iphoneos SDK of Xcode.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${1:-${OUT:-$repo/out/phone}}
dst=$OUT/andromedad
mkdir -p "$dst"

sdk=$(xcrun --sdk iphoneos --show-sdk-path)
cc=$(xcrun --sdk iphoneos --find clang)
echo "andromedad: SDK $sdk"
"$cc" -arch arm64 -isysroot "$sdk" -miphoneos-version-min=16.0 \
	-std=gnu11 -O2 -Wall -Wextra -fobjc-arc \
	-I"$repo/xnu" \
	-framework Foundation \
	-o "$dst/andromedad" "$here/andromedad.m"
codesign -f -s - --entitlements "$here/andromedad.entitlements" "$dst/andromedad"
codesign -d --entitlements - "$dst/andromedad"
file "$dst/andromedad"
ls -l "$dst"
