#!/usr/bin/env bash
# Build Turnip, Mesa's Vulkan driver for Adreno, for iOS 16 arm64 with the
# "darwin" kernel backend, plus ios-flame's two smoke tests (phase F5 of
# docs/research/gpu.md). Runs on macOS with Xcode (the gpu-userland-andromeda
# workflow); nothing from an iOS image is used, only the iphoneos SDK.
#
#   ANDROMEDA_GPU_USERLAND=1 phone/turnip/build.sh [OUT]   (or OUT=... in the environment)
#
# Without ANDROMEDA_GPU_USERLAND=1 it only prints a note and exits 0: the
# generic userland-andromeda workflow runs every phone/*/build.sh, and this one
# takes far longer than the others, so it has a workflow of its own.
#
# Writes OUT/turnip/ (default OUT: <repo>/out/phone):
#   libvulkan_freedreno.dylib  Turnip, darwin kernel backend, minimum iOS 16.0,
#                              signed ad hoc (installed as
#                              /private/var/local/lib/libvulkan_freedreno.dylib)
#   vktest                     Turnip on the GPU: device, fill+copy, compute
#                              shader, triangle, read back (ios-flame)
#   v2test                     the kernel's NativeAdreno ABI v1 user client
#                              without Turnip (ios-flame)
#   NOTICE.txt, licenses/, patches/, BUILDINFO.txt
#                              what is inside, under which licence, from which
#                              commits, with which tools
#
# Sources, fetched at build time from their public repositories at fixed
# commits (nothing of them is copied into this repository):
#   - Mesa (https://gitlab.freedesktop.org/mesa/mesa) 85fda3b3, MIT: the pin
#     of UniXNU (scripts/data/adreno-compiler-inputs.json) and ios-flame;
#   - UniXNU (https://github.com/UniXNU/UniXNU) b6ae99c1, BSD-2-Clause-Patent:
#     the 8 patches of patches/mesa in the order of that JSON file (the darwin
#     kernel backend tu_knl_darwin.cc, MIT, and the iOS fixes), and the Python
#     build pins scripts/requirements-adreno-compiler.txt;
#   - ios-flame (Nick Wylynko, https://github.com/NWylynko/ios-flame) 745c4341,
#     MIT: phone/turnip (NativeAdrenoABI.h, a copy of UniXNU NativeAdreno's,
#     MIT; vktest.c, v2test.cpp, shaders/, patches/0100) and the build recipe
#     of tools/build-turnip-ios.sh, which follows UniXNU's
#     scripts/build-turnip-darwin.py. See CREDITS.md.
#
# Licences and how we comply: Mesa is MIT (Khronos headers and registry
# Apache-2.0 OR MIT), the UniXNU patches BSD-2-Clause-Patent (files they add:
# MIT), ios-flame MIT. All are permissive: the binaries go out with the
# copyright notices and licence texts (licenses/), the exact patches
# (patches/) and the commits (NOTICE.txt). No GPL/LGPL code is in the Turnip
# library (Mesa's GPL-licensed headers belong to the msm/kgsl backends, which
# are not built).
#
# Environment: GPU_WORK (work tree, default <repo>/.cache/gpu-userland),
# GPU_JOBS (ninja jobs, default: ninja's own). Needs git, python3, ninja,
# glslangValidator (>= 12.2), bison (>= 3.7) and flex: Homebrew's
# `brew install ninja glslang bison flex` (bison/flex are found in their keg).
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${1:-${OUT:-$repo/out/phone}}
name=turnip

if [[ ${ANDROMEDA_GPU_USERLAND:-0} != 1 ]]; then
	echo "$name: skipped (long build: run the gpu-userland-andromeda workflow, or set ANDROMEDA_GPU_USERLAND=1)"
	exit 0
fi

# --- Pins -------------------------------------------------------------------
MESA_COMMIT=85fda3b36405bcaea8bffc64aa4f2651dc90770d
MESA_URLS=(https://gitlab.freedesktop.org/mesa/mesa.git
	https://chromium.googlesource.com/external/gitlab.freedesktop.org/mesa/mesa)
UNIXNU_COMMIT=b6ae99c17194e6a18fd819d3148e69fe405e91fc
UNIXNU_URLS=(https://github.com/UniXNU/UniXNU.git)
FLAME_COMMIT=745c4341af86089efb0df851fa8a27fb8face624
FLAME_URLS=(https://github.com/NWylynko/ios-flame.git)
# Applied to Mesa in this order: UniXNU's (patches/mesa, order of
# scripts/data/adreno-compiler-inputs.json), then ios-flame's (phone/turnip/patches).
UNIXNU_PATCHES=(
	freedreno-tools-nir.patch:77e966b6e9a725892d10889a4d8deeefb03a32ae3a007904868132e56350191f
	ir3-darwin-build-id.patch:be3ca7840a16cb7696d8d7e67195c8d097cf0e38a3b5011edc674a5aed9c6975
	ir3-portable-asm-roundtrip.patch:bfc5552bc2bd6e1668d75773cce7bf62fd19065e6281bf7da49f9ddfc2e79339
	offline-adreno-compiler-probe.patch:b7d7a113beb5ef99edea24858d906194d4b4755c987520545ad2f7f3136bb4fb
	turnip-darwin-kmd.patch:f3bc6d470f8e0bde1b1d0d370175ec105ee45fe50075e43279581e61b1455c2b
	turnip-graphics-cache-feedback.patch:07f397e38d9acfb0085db4c19567c84fd8a3bcd63d6fd05192857abfbde5aed2
	turnip-empty-scissor.patch:dec657626dcd8e8d3f1447e44574f31e4afb8c90b6ebf7d589ba10ee0f8b6461
	turnip-builtin-shader-pack.patch:3c606dc99643c3ca3e4736c5783348cd85cbff3fbc55161e53226ab459f34473
)
FLAME_PATCHES=(
	0100-sweet-fetch-gmem-min-draws.patch:ab5bb6c7af74d7091560176e478e0ee9c1ab4f0e9740cb520251f21b1a55644f
)
UNIXNU_REQUIREMENTS_SHA256=493c8cfda8dc52729876b8ba8726fe589c14521caf092b7c92504b9ec0244b15
ABI_HEADER_SHA256=b5574e34ee3a368616f722f9fa40ea2c87bbc5c03989a299d9d4f954c9f8be70
IOS_MIN=16.0

WORK=${GPU_WORK:-$repo/.cache/gpu-userland}
src=$WORK/src
T=$WORK/turnip
dst=$OUT/$name
mkdir -p "$src" "$T"

die() {
	echo "$name: $*" >&2
	exit 1
}

need() {
	command -v "$1" >/dev/null 2>&1 || die "$1 not found ($2)"
}

sha256() {
	shasum -a 256 "$1" | awk '{print $1}'
}

check_sha256() { # FILE EXPECTED
	local got
	got=$(sha256 "$1")
	[[ $got == "$2" ]] || die "$1: sha256 $got, expected $2 (pin changed? review the file, then update the pin)"
}

# fetch_git DIR COMMIT URL... [-- PATH...]: DIR becomes a depth-1 clone at
# exactly COMMIT (each URL is tried in turn, three rounds). With PATHs, a
# blobless sparse checkout of just those paths. The tree is reset to the
# commit every time, so patches always start from the pinned source.
fetch_git() {
	local dir=$1 commit=$2
	shift 2
	local urls paths fetched round url filter
	urls=()
	paths=()
	while (($#)); do
		if [[ $1 == -- ]]; then
			shift
			paths=("$@")
			break
		fi
		urls+=("$1")
		shift
	done
	if [[ ! -d $dir/.git ]]; then
		rm -rf "$dir"
		git init -q "$dir"
		git -C "$dir" remote add origin "${urls[0]}"
	fi
	if [[ $(git -C "$dir" rev-parse -q --verify HEAD 2>/dev/null || true) != "$commit" ]]; then
		fetched=0
		filter=(--depth 1)
		((${#paths[@]})) && filter+=(--filter=blob:none)
		for round in 1 2 3; do
			for url in "${urls[@]}"; do
				git -C "$dir" remote set-url origin "$url"
				echo "$name: fetching $url $commit"
				if git -C "$dir" fetch -q "${filter[@]}" origin "$commit"; then
					fetched=1
					break 2
				fi
			done
			sleep $((round * 15))
		done
		((fetched)) || die "cannot fetch $commit from ${urls[*]}"
	fi
	if ((${#paths[@]})); then
		git -C "$dir" sparse-checkout set --no-cone "${paths[@]}"
	fi
	git -C "$dir" -c advice.detachedHead=false checkout -q --detach -f "$commit"
	git -C "$dir" reset -q --hard "$commit"
	git -C "$dir" clean -q -fdx
	[[ $(git -C "$dir" rev-parse HEAD) == "$commit" ]] || die "$dir is not at $commit"
}

# check_macho FILE: arm64 Mach-O for iOS with minimum version IOS_MIN.
check_macho() {
	local f=$1 archs build
	archs=$(lipo -archs "$f")
	[[ $archs == arm64 ]] || die "$f: architectures '$archs', expected arm64"
	build=$(otool -l "$f" | awk '$2 == "LC_BUILD_VERSION" {b = 1} b && $1 == "platform" {p = $2} b && $1 == "minos" {print p, $2; exit}')
	[[ $build == "2 $IOS_MIN" || $build == "IOS $IOS_MIN" ]] ||
		die "$f: LC_BUILD_VERSION '$build', expected iOS (platform 2) minos $IOS_MIN"
}

# Weak imports resolve to NULL on iOS 16 when the SDK is newer than the phone
# (ios-flame's strchrnul lesson): list them so a crash at PC 0 has a suspect.
list_weak() {
	local weak
	weak=$(nm -m "$1" | grep "weak external" | grep -v "dynamically looked up" || true)
	if [[ -n $weak ]]; then
		echo "$name: weak imports in ${1##*/} (missing on iOS $IOS_MIN if newer than it):"
		printf '%s\n' "$weak"
	fi
}

# --- Tools -------------------------------------------------------------------
need xcrun "Xcode"
need git "Xcode command line tools"
need python3 "Xcode command line tools or Homebrew"
need ninja "brew install ninja"
need glslangValidator "brew install glslang"
if command -v brew >/dev/null 2>&1; then
	for keg in bison flex; do
		prefix=$(brew --prefix "$keg" 2>/dev/null || true)
		[[ -n $prefix && -x $prefix/bin/$keg ]] && PATH=$prefix/bin:$PATH
	done
	export PATH
fi
need bison "brew install bison"
need flex "brew install flex"
bison_version=$(bison --version | awk 'NR == 1 {print $NF}')
[[ $bison_version =~ ^(3\.([7-9]|[1-9][0-9])|[4-9]|[1-9][0-9])(\.|$) ]] ||
	die "bison $bison_version is too old: Mesa's ir3 grammar needs >= 3.7 (brew install bison)"

sdk=$(xcrun --sdk iphoneos --show-sdk-path)
cc=$(xcrun --sdk iphoneos --find clang)
cxx=$(xcrun --sdk iphoneos --find clang++)
ios_flags=(-arch arm64 -isysroot "$sdk" "-miphoneos-version-min=$IOS_MIN")
echo "$name: SDK $sdk ($(xcrun --sdk iphoneos --show-sdk-version))"

# --- Sources -----------------------------------------------------------------
mesa=$src/mesa
unixnu=$src/unixnu
flame=$src/ios-flame
fetch_git "$unixnu" "$UNIXNU_COMMIT" "${UNIXNU_URLS[@]}" -- \
	/LICENSE /patches/mesa/ /scripts/requirements-adreno-compiler.txt /scripts/data/adreno-compiler-inputs.json
fetch_git "$flame" "$FLAME_COMMIT" "${FLAME_URLS[@]}" -- \
	/LICENSE /CREDITS.md /phone/turnip/ /phone/metal2/
fetch_git "$mesa" "$MESA_COMMIT" "${MESA_URLS[@]}"

patch_files=()
for entry in "${UNIXNU_PATCHES[@]}"; do
	file=$unixnu/patches/mesa/${entry%%:*}
	check_sha256 "$file" "${entry#*:}"
	patch_files+=("$file")
done
for entry in "${FLAME_PATCHES[@]}"; do
	file=$flame/phone/turnip/patches/${entry%%:*}
	check_sha256 "$file" "${entry#*:}"
	patch_files+=("$file")
done
check_sha256 "$unixnu/scripts/requirements-adreno-compiler.txt" "$UNIXNU_REQUIREMENTS_SHA256"
abi_dir=$flame/phone/turnip
check_sha256 "$abi_dir/NativeAdrenoABI.h" "$ABI_HEADER_SHA256"
for file in "${patch_files[@]}"; do
	echo "$name: applying ${file#"$src"/}"
	git -C "$mesa" apply --whitespace=nowarn "$file" || die "patch ${file##*/} does not apply to Mesa $MESA_COMMIT"
done

# --- Python build tools (UniXNU's pins: meson, Mako, PyYAML, packaging, ply) --
venv=$T/venv
stamp=$venv/.requirements-$UNIXNU_REQUIREMENTS_SHA256
if [[ ! -f $stamp ]]; then
	rm -rf "$venv"
	python3 -m venv "$venv"
	"$venv/bin/python" -m pip install -q --disable-pip-version-check \
		-r "$unixnu/scripts/requirements-adreno-compiler.txt"
	: >"$stamp"
fi
PATH=$venv/bin:$PATH
export PATH

# --- Configure and build (options of ios-flame tools/build-turnip-ios.sh) ----
build=$T/build-ios
cross=$T/ios$IOS_MIN.cross
flags="'-target', 'arm64-apple-ios$IOS_MIN', '-isysroot', '$sdk', '-fstack-protector-strong'"
cat >"$cross" <<EOF
[binaries]
c = '$cc'
cpp = '$cxx'
objc = '$cc'
ar = '$(xcrun --sdk iphoneos --find ar)'
strip = '$(xcrun --sdk iphoneos --find strip)'
pkg-config = '/usr/bin/false'
[host_machine]
system = 'darwin'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'
[properties]
needs_exe_wrapper = true
[built-in options]
c_args = [$flags]
cpp_args = [$flags]
objc_args = [$flags]
c_link_args = [$flags]
cpp_link_args = [$flags]
objc_link_args = [$flags]
EOF
meson_options=(
	-Dgallium-drivers= -Dvulkan-drivers=freedreno -Dfreedreno-kmds=darwin "-Dtu-darwin-abi-dir=$abi_dir"
	-Dplatforms=macos -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false -Dgles1=disabled
	-Dgles2=disabled -Dllvm=disabled -Dshared-glapi=disabled -Dxmlconfig=disabled -Dshader-cache=disabled
	-Dtools= -Dbuild-tests=false -Dvideo-codecs= -Dvulkan-layers= -Dlibunwind=disabled -Dlmsensors=disabled
	-Dvalgrind=disabled -Dexpat=disabled -Dzstd=disabled -Dbuildtype=release -Db_ndebug=true
)
# A build directory is reused only for the very same sources, patches,
# options, SDK and compiler; otherwise it starts clean.
config_id=$({
	echo "$MESA_COMMIT"
	for file in "${patch_files[@]}"; do sha256 "$file"; done
	printf '%s\n' "${meson_options[@]}"
	cat "$cross"
	"$cxx" --version
} | shasum -a 256 | awk '{print $1}')
if [[ ! -f $build/.config-$config_id || ! -f $build/build.ninja ]]; then
	rm -rf "$build"
	unset CFLAGS CXXFLAGS CPPFLAGS LDFLAGS OBJCFLAGS
	echo "$name: meson setup (log $T/configure-ios.log)"
	CC=$cc CXX=$cxx meson setup "$build" "$mesa" --wrap-mode=nodownload --cross-file "$cross" \
		"${meson_options[@]}" >"$T/configure-ios.log" 2>&1 || {
		tail -40 "$T/configure-ios.log" >&2
		die "meson setup failed"
	}
	: >"$build/.config-$config_id"
fi
ninja_args=(-C "$build")
[[ -n ${GPU_JOBS:-} ]] && ninja_args+=(-j "$GPU_JOBS")
target=src/freedreno/vulkan/libvulkan_freedreno.dylib
echo "$name: ninja $target (log $T/build-ios.log)"
ninja "${ninja_args[@]}" "$target" >"$T/build-ios.log" 2>&1 || {
	grep -E "error:|FAILED" "$T/build-ios.log" | head -40 >&2 || true
	tail -20 "$T/build-ios.log" >&2
	die "ninja failed"
}
lib=$build/$target
exports=$(nm -gU "$lib")
grep -q " T _vkGetInstanceProcAddr$" <<<"$exports" ||
	die "$lib does not export vkGetInstanceProcAddr (iOS has no Vulkan loader; the bridge and vktest dlsym it)"
grep -q " T _vk_icdGetInstanceProcAddr$" <<<"$exports" ||
	echo "$name: note: no vk_icdGetInstanceProcAddr export (UniXNU's build checks it; ours only needs vkGetInstanceProcAddr)"
check_macho "$lib"
list_weak "$lib"

# --- Outputs -----------------------------------------------------------------
rm -rf "$dst"
mkdir -p "$dst/licenses/mesa" "$dst/licenses/unixnu" "$dst/licenses/ios-flame" "$dst/patches"
cp "$lib" "$dst/libvulkan_freedreno.dylib"
codesign -f -s - "$dst/libvulkan_freedreno.dylib"

# vktest: SPIR-V of ios-flame's shaders as C arrays (shaders.h), then the test.
gen=$T/vktest-gen
rm -rf "$gen"
mkdir -p "$gen"
: >"$gen/shaders.h"
for shader in fill.comp:fill_comp tri.vert:tri_vert tri.frag:tri_frag; do
	glslangValidator -V --target-env vulkan1.2 --vn "${shader#*:}" -o "$gen/${shader#*:}.h" \
		"$flame/phone/turnip/shaders/${shader%%:*}" >/dev/null
	cat "$gen/${shader#*:}.h" >>"$gen/shaders.h"
done
"$cc" "${ios_flags[@]}" -O2 -Wall -I "$gen" -I "$mesa/include" \
	-o "$dst/vktest" "$flame/phone/turnip/vktest.c"
"$cxx" "${ios_flags[@]}" -std=c++17 -O2 -Wall -I "$abi_dir" \
	-o "$dst/v2test" "$flame/phone/turnip/v2test.cpp" -framework IOKit -framework CoreFoundation
for bin in vktest v2test; do
	codesign -f -s - "$dst/$bin"
	check_macho "$dst/$bin"
	list_weak "$dst/$bin"
done

# Licences and the exact patches that went into the library.
cp "$mesa/docs/license.rst" "$dst/licenses/mesa/"
cp -R "$mesa/licenses" "$dst/licenses/mesa/licenses"
cp "$unixnu/LICENSE" "$dst/licenses/unixnu/LICENSE"
cp "$flame/LICENSE" "$dst/licenses/ios-flame/LICENSE"
series=()
i=0
for file in "${patch_files[@]}"; do
	i=$((i + 1))
	numbered=$(printf '%02d-%s' "$i" "${file##*/}")
	cp "$file" "$dst/patches/$numbered"
	series+=("$numbered")
done
printf '%s\n' "${series[@]}" >"$dst/patches/series"

device_commit=$(git -C "$repo" rev-parse HEAD 2>/dev/null || echo unknown)
{
	echo "Built $(date -u +%Y-%m-%dT%H:%M:%SZ) from UniXNU-andromeda $device_commit (phone/turnip/build.sh)"
	echo "Mesa $MESA_COMMIT ($(cat "$mesa/VERSION" 2>/dev/null || echo '?')) from ${MESA_URLS[0]}"
	echo "UniXNU $UNIXNU_COMMIT from ${UNIXNU_URLS[0]}"
	echo "ios-flame $FLAME_COMMIT from ${FLAME_URLS[0]}"
	echo "minimum iOS $IOS_MIN"
	echo
	xcodebuild -version 2>/dev/null || true
	echo "iphoneos SDK $(xcrun --sdk iphoneos --show-sdk-version)"
	"$cxx" --version 2>/dev/null | head -1 || true
	echo "meson $(meson --version), ninja $(ninja --version)"
	glslangValidator --version 2>/dev/null | head -1 || true
	bison --version 2>/dev/null | head -1 || true
	flex --version 2>/dev/null | head -1 || true
	"$venv/bin/python" --version
	echo
	echo "meson options:"
	printf '  %s\n' "${meson_options[@]}"
	echo
	echo "sha256:"
	(cd "$dst" && shasum -a 256 libvulkan_freedreno.dylib vktest v2test | sed 's/^/  /')
} >"$dst/BUILDINFO.txt"

{
	cat <<EOF
Turnip for the Xiaomi Mi Mix 3 5G (andromeda), iOS $IOS_MIN arm64, phase F5
===========================================================================

Built by phone/turnip/build.sh of UniXNU-andromeda (commit $device_commit).
Built from public sources at fixed commits; nothing here comes from an iOS
image. Details of the tools: BUILDINFO.txt.

libvulkan_freedreno.dylib
  Turnip, the Vulkan driver of Mesa (freedreno, ir3, NIR, Vulkan runtime and
  util), https://gitlab.freedesktop.org/mesa/mesa commit $MESA_COMMIT.
  Licence: MIT for most of Mesa; files carry their own SPDX identifiers (the
  Khronos Vulkan headers and registry are Apache-2.0 OR MIT). Mesa's summary
  and licence texts: licenses/mesa/. No GPL or LGPL code is linked: Mesa's
  GPL-licensed headers belong to the msm and kgsl kernel backends, which are
  not built (-Dfreedreno-kmds=darwin).
  Patched with, in this order (exact copies in patches/, order in
  patches/series):
    - UniXNU, https://github.com/UniXNU/UniXNU commit $UNIXNU_COMMIT
      (patches/mesa), BSD-2-Clause-Patent (licenses/unixnu/LICENSE); the files
      the patches add (tu_knl_darwin.cc, tu_builtin_cache.*) are MIT. They add
      the "darwin" kernel backend, which drives the kernel's IOKit user client
      through NativeAdreno ABI v1, an exported vkGetInstanceProcAddr (iOS has
      no Vulkan loader) and fixes for Darwin.
    - ios-flame, Nick Wylynko, https://github.com/NWylynko/ios-flame commit
      $FLAME_COMMIT (phone/turnip/patches), MIT
      (licenses/ios-flame/LICENSE).
  NativeAdrenoABI.h, the ABI header the darwin backend includes: UniXNU
  NativeAdreno, MIT, as copied in ios-flame phone/turnip/.

vktest, v2test
  ios-flame phone/turnip (vktest.c, v2test.cpp, shaders/), Nick Wylynko, MIT,
  unchanged. vktest embeds SPIR-V compiled from ios-flame's shaders with
  glslangValidator.

Corresponding source: the commits above (public), the patches in patches/ and
phone/turnip/build.sh rebuild everything here.
EOF
} >"$dst/NOTICE.txt"

ls -lR "$dst"
echo "$name: done"
