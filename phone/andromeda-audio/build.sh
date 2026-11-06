#!/usr/bin/env bash
# Build AndromedaAudio.driver, the audio server plug-in of phase A0
# (AndromedaAudio.c), for iOS 16 arm64 on macOS with Xcode (GitHub macOS
# runner). Compiled as a CFPlugIn bundle the way HakiOS's userland/build.sh
# builds HakiosAudio.driver (clang -bundle, CoreFoundation only; HakiOS, MIT,
# see CREDITS.md), signed ad hoc like phone/andromedad/build.sh: our kernel
# has no AMFI enforcing signatures (patch 0096), so no certificate is needed.
#
#   phone/andromeda-audio/build.sh [OUT]          (or OUT=... in the environment)
#
# Writes OUT/andromeda-audio/AndromedaAudio.driver/{AndromedaAudio,Info.plist,
# _CodeSignature/}. Default OUT: <repo>/out/phone. Nothing from the iOS image
# is used: only the iphoneos SDK of Xcode.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${1:-${OUT:-$repo/out/phone}}
dst=$OUT/andromeda-audio
bundle=$dst/AndromedaAudio.driver
rm -rf "$bundle"
mkdir -p "$bundle"

sdk=$(xcrun --sdk iphoneos --show-sdk-path)
cc=$(xcrun --sdk iphoneos --find clang)
echo "andromeda-audio: SDK $sdk"
"$cc" -arch arm64 -isysroot "$sdk" -miphoneos-version-min=16.0 \
	-std=gnu11 -O2 -Wall -Wextra -Werror=implicit-function-declaration \
	-bundle -framework CoreFoundation \
	-o "$bundle/AndromedaAudio" "$here/AndromedaAudio.c"
# Binary, as Apple's own plug-ins have it; plutil checks the XML on the way.
plutil -convert binary1 -o "$bundle/Info.plist" "$here/Info.plist"
# The factory the Info.plist names must be exported.
nm -gU "$bundle/AndromedaAudio" | grep -q ' _AndromedaAudio_Create$' ||
	{ echo 'andromeda-audio: AndromedaAudio_Create is not exported' >&2; exit 1; }
codesign -f -s - "$bundle"
codesign -dv "$bundle" 2>&1
file "$bundle/AndromedaAudio"
ls -lR "$dst"
