#!/usr/bin/env bash
# Build SweetMetal2.bundle, the Metal driver of ios-flame for the Adreno 640:
# UniXNU's Metal frontend over the metal2vulkan runtime (Metal on Vulkan) over
# Turnip (phone/turnip), for iOS 16 arm64 (phase F5 of docs/research/gpu.md,
# installed for F6). Runs on macOS with Xcode and Rust (the
# gpu-userland-andromeda workflow); nothing from an iOS image is used, only
# the iphoneos SDK.
#
#   ANDROMEDA_GPU_USERLAND=1 phone/metal2/build.sh [OUT]   (or OUT=... in the environment)
#
# Without ANDROMEDA_GPU_USERLAND=1 it only prints a note and exits 0 (see
# phone/turnip/build.sh). Needs OUT/turnip/libvulkan_freedreno.dylib; when it
# is missing, phone/turnip/build.sh runs first.
#
# Writes OUT/metal2/ (default OUT: <repo>/out/phone):
#   SweetMetal2.bundle/        SweetMetal2 (frontend + bridge + metal2vulkan,
#                              static), Info.plist (NSPrincipalClass
#                              SweetMTL2Device), libvulkan_freedreno.dylib
#                              (Turnip, stripped), _CodeSignature (ad hoc);
#                              installed in /System/Library/Extensions
#   m2vtest                    the bridge's self-test on the GPU (ios-flame)
#   debug/SweetMetal2.dSYM     symbols for crash logs (not installed)
#   NOTICE.txt, BUILDINFO.txt, licenses/ (with licenses/crates/)
#   source/SweetMetal2-corresponding-source.tar.xz
#                              the Complete Corresponding Source (see below)
#
# Sources, fetched at build time at fixed commits (none copied into this
# repository; phone/metal2/Cargo.lock is ours, resolved with cargo 1.98.0):
#   - ios-flame (Nick Wylynko, https://github.com/NWylynko/ios-flame) 745c4341,
#     MIT: phone/metal2 = SweetMetal2.mm, Info.plist, m2vtest.c (ios-flame's
#     own); NativeBridgeMetalFrontend.mm/.h (UniXNU NativeMetal, MIT, adapted
#     by ios-flame to iOS 16); bridge/NativeMetalBridge.rs, NativeMetalObjects.*
#     (UniXNU NativeMetal, MIT, bridge/LICENSE); bridge/patches 0001-0006
#     (UniXNU's, to metal2vulkan) and 0100-0101 (ios-flame's). The build
#     follows ios-flame's phone/metal2/build.sh and tools/build-m2v-bridge.sh,
#     which follow UniXNU's scripts/build-native-metal-bridge.py.
#   - metal2vulkan (steelbrain / Anees Iqbal,
#     https://github.com/steelbrain/metal2vulkan) 720ef1ce, UniXNU's and
#     ios-flame's pin: GPL-2.0-or-later OR LGPL-3.0-or-later. Only the crates
#     the bridge builds (metal2vulkan, -air, -runtime) and the two fixtures it
#     embeds are checked out.
#   - crates.io dependencies at the versions of phone/metal2/Cargo.lock (MIT,
#     MIT OR Apache-2.0, ISC, Unlicense OR MIT; listed in
#     licenses/crates/THIRD-PARTY-CRATES.txt with their licence files).
#
# Licences and how we comply. The bundle links metal2vulkan statically, so it
# is a combined work with a GPL-2.0-or-later OR LGPL-3.0-or-later part; the
# rest is MIT (ios-flame, UniXNU NativeMetal), permissive crates and Turnip
# (MIT, a separate dylib loaded at run time). Each binary output therefore
# travels with: the licence texts (licenses/metal2vulkan/: GPL-2.0, GPL-3.0,
# LGPL-3.0), a notice naming the library and its licence (NOTICE.txt), and in
# the same artifact the Complete Corresponding Source: the exact tree that was
# compiled (metal2vulkan subset with the patches applied, the patches
# themselves, the frontend and bridge, the vendored crates, Cargo.toml,
# Cargo.lock and these scripts), from which the bundle can be rebuilt and
# relinked with a modified metal2vulkan. That meets GPL-2.0 section 3(a) and
# LGPL-3.0 section 4(d)(0) alike, whichever licence a recipient picks. Apple's
# frameworks are system libraries. Our scripts are distributed in source too.
#
# Environment: GPU_WORK (work tree, default <repo>/.cache/gpu-userland),
# LLVM_AS (llvm-as of LLVM >= 21; default Homebrew's `brew install llvm`).
# Needs rustup (toolchain 1.98.0 and target aarch64-apple-ios are installed
# on demand), git, python3.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${1:-${OUT:-$repo/out/phone}}
name=metal2

if [[ ${ANDROMEDA_GPU_USERLAND:-0} != 1 ]]; then
	echo "$name: skipped (long build: run the gpu-userland-andromeda workflow, or set ANDROMEDA_GPU_USERLAND=1)"
	exit 0
fi

# --- Pins -------------------------------------------------------------------
FLAME_COMMIT=745c4341af86089efb0df851fa8a27fb8face624
FLAME_URLS=(https://github.com/NWylynko/ios-flame.git)
M2V_COMMIT=720ef1ce9ae2a2068999a8675717418b9416283e
M2V_URLS=(https://github.com/steelbrain/metal2vulkan.git)
# What the bridge compiles (and what the corresponding source contains): the
# workspace manifest, the licences, three crates and the two embedded fixtures.
M2V_PATHS=(
	/Cargo.toml /LICENSE /LICENSE-GPL-2.0 /LICENSE-GPL-3.0 /LICENSE-LGPL-3.0 /README.md /CHANGELOG.md
	/crates/metal2vulkan/ /crates/metal2vulkan-air/ /crates/metal2vulkan-runtime/
	/crates/metal2vulkan-validation/fixtures/public/examples/runtime_examples.ll
	/crates/metal2vulkan-validation/fixtures/public/kernel_function_constant_gate_fold.ll
)
# ios-flame phone/metal2/bridge/patches, applied to metal2vulkan in this order.
M2V_PATCHES=(
	0001-optional-constant-output-members.patch:4873084b07b44f7dd3dc3419fb8d758e275fd630435a3c7de3939219a41f5846
	0002-resource-wait-storage-dependencies.patch:fc1d13dc6e8fbdb0f7bc5e7e5a8144e4e1a4a148586a89384d4ed7645b7c2712
	0003-cache-device-entry-point-priming.patch:f734b0811904ff096df4f7f0b44e410f0ad046af1bda8ee2a4b52363b3cd92ba
	0004-fold-constant-global-field-loads.patch:1b00fe99626deffb1343b676d12c6929c4eafe9710f175ce97f13ffcc788a5c1
	0005-keep-module-decodes-and-profile-fingerprints.patch:03e9968db60fead33992d941a18f37291afc18b9fc981f388f844450c3ec35b4
	0006-skip-draws-without-a-pipeline.patch:3ee976e3e134c1f0fc1758c910fe75768a1338286ecf55970537fa783f3298cf
	0100-sweet-8mb-memory-blocks.patch:9a9fdc8ef1050142c196c50fe22880db657f8c6c4c41b96706468024b6c94a00
	0101-sweet-padded-writers-alias.patch:3b031122cfc8f82e86ece7f98b95d64c16a272527dc226d35f4eaf42e5bb273f
)
BRIDGE_LICENSE_SHA256=c51a32dfc2140d3d56f796a3a7010e574f2e3649b97c42e010303ad57f848265
RUST_TOOLCHAIN=1.98.0
RUST_TARGET=aarch64-apple-ios
LLVM_MIN=21
IOS_MIN=16.0
BUNDLE=SweetMetal2
CLASS=SweetMTL2Device

WORK=${GPU_WORK:-$repo/.cache/gpu-userland}
src=$WORK/src
M=$WORK/metal2
dst=$OUT/$name
mkdir -p "$src" "$M"

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

# fetch_git DIR COMMIT URL... [-- PATH...]: as in phone/turnip/build.sh.
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

check_macho() {
	local f=$1 archs build
	archs=$(lipo -archs "$f")
	[[ $archs == arm64 ]] || die "$f: architectures '$archs', expected arm64"
	build=$(otool -l "$f" | awk '$2 == "LC_BUILD_VERSION" {b = 1} b && $1 == "platform" {p = $2} b && $1 == "minos" {print p, $2; exit}')
	[[ $build == "2 $IOS_MIN" || $build == "IOS $IOS_MIN" ]] ||
		die "$f: LC_BUILD_VERSION '$build', expected iOS (platform 2) minos $IOS_MIN"
}

list_weak() {
	local weak
	weak=$(nm -m "$1" | grep "weak external" | grep -v "dynamically looked up" || true)
	if [[ -n $weak ]]; then
		echo "$name: weak imports in ${1##*/} (missing on iOS $IOS_MIN if newer than it):"
		printf '%s\n' "$weak"
	fi
}

# run LOG CMD...: CMD with its output in LOG; on failure the log's tail.
run() {
	local log=$1
	shift
	if ! "$@" >"$log" 2>&1; then
		grep -E "^(error|warning: unused)|error:" -A6 "$log" | head -80 >&2 || true
		tail -30 "$log" >&2
		die "command failed (${1##*/} ...), log $log"
	fi
}

toml_str() {
	python3 -I -c 'import json, sys; print(json.dumps(sys.argv[1]))' "$1"
}

# --- Tools -------------------------------------------------------------------
need xcrun "Xcode"
need git "Xcode command line tools"
need python3 "Xcode command line tools or Homebrew"
need rustup "https://rustup.rs (preinstalled on GitHub's macOS runners)"
llvm_as=${LLVM_AS:-}
if [[ -z $llvm_as ]] && command -v brew >/dev/null 2>&1; then
	llvm_as=$(brew --prefix llvm 2>/dev/null || true)/bin/llvm-as
fi
[[ -x $llvm_as ]] || llvm_as=$(command -v llvm-as || true)
[[ -n $llvm_as && -x $llvm_as ]] || die "llvm-as not found (brew install llvm, or LLVM_AS=...)"
llvm_major=$("$llvm_as" --version | grep -Eo 'version [0-9]+' | head -1 | awk '{print $2}' || true)
[[ -n $llvm_major && $llvm_major -ge $LLVM_MIN ]] ||
	die "$llvm_as is LLVM '${llvm_major:-?}': the fixtures need LLVM >= $LLVM_MIN"

sdk=$(xcrun --sdk iphoneos --show-sdk-path)
cc=$(xcrun --sdk iphoneos --find clang)
ios_flags=(-arch arm64 -isysroot "$sdk" "-miphoneos-version-min=$IOS_MIN")
echo "$name: SDK $sdk ($(xcrun --sdk iphoneos --show-sdk-version)), $llvm_as (LLVM $llvm_major)"

rustup toolchain install "$RUST_TOOLCHAIN" --profile minimal --no-self-update
rustup target add --toolchain "$RUST_TOOLCHAIN" "$RUST_TARGET"
cargo=(rustup run "$RUST_TOOLCHAIN" cargo)

# --- Turnip --------------------------------------------------------------------
turnip=$OUT/turnip/libvulkan_freedreno.dylib
if [[ ! -f $turnip ]]; then
	echo "$name: no $turnip: building Turnip first"
	bash "$repo/phone/turnip/build.sh" "$OUT"
fi
[[ -f $turnip ]] || die "no $turnip after phone/turnip/build.sh"

# --- Sources -----------------------------------------------------------------
flame=$src/ios-flame
m2v=$src/metal2vulkan
fm=$flame/phone/metal2
fetch_git "$flame" "$FLAME_COMMIT" "${FLAME_URLS[@]}" -- \
	/LICENSE /CREDITS.md /phone/turnip/ /phone/metal2/
fetch_git "$m2v" "$M2V_COMMIT" "${M2V_URLS[@]}" -- "${M2V_PATHS[@]}"
check_sha256 "$fm/bridge/LICENSE" "$BRIDGE_LICENSE_SHA256"
for license in LICENSE LICENSE-GPL-2.0 LICENSE-GPL-3.0 LICENSE-LGPL-3.0; do
	[[ -s $m2v/$license ]] || die "metal2vulkan $M2V_COMMIT has no $license: the binary cannot be distributed without it"
done
patch_files=()
for entry in "${M2V_PATCHES[@]}"; do
	file=$fm/bridge/patches/${entry%%:*}
	check_sha256 "$file" "${entry#*:}"
	patch_files+=("$file")
done
for file in "${patch_files[@]}"; do
	echo "$name: applying ${file#"$src"/}"
	git -C "$m2v" apply --whitespace=nowarn "$file" || die "patch ${file##*/} does not apply to metal2vulkan $M2V_COMMIT"
done

# --- Bridge: static library for aarch64-apple-ios ------------------------------
# The runtime's example library and its function-constant fixture, assembled to
# AIR and embedded in the bridge for its self-test (NMB_FIXTURE_AIR, NMB_FC_AIR).
air=$M/air
mkdir -p "$air"
fixtures=$m2v/crates/metal2vulkan-validation/fixtures/public
"$llvm_as" -o "$air/runtime_examples.air" "$fixtures/examples/runtime_examples.ll"
"$llvm_as" -o "$air/function_constant.air" "$fixtures/kernel_function_constant_gate_fold.ll"

bridge=$M/bridge
rm -rf "$bridge"
mkdir -p "$bridge"
# write_manifest FILE LIB_PATH M2V_DIR: the bridge package, as ios-flame's
# tools/build-m2v-bridge.sh writes it.
write_manifest() {
	cat >"$1" <<EOF
[package]
name = "native-metal-bridge"
version = "0.1.0"
edition = "2024"
rust-version = "1.98"
[workspace]
[lib]
crate-type = ["staticlib"]
path = $(toml_str "$2")
[dependencies]
metal2vulkan = { path = $(toml_str "$3/crates/metal2vulkan"), features = ["ash"] }
metal2vulkan-runtime = { path = $(toml_str "$3/crates/metal2vulkan-runtime") }
libc = "0.2"
sha2 = { version = "0.10", features = ["asm"] }
[profile.release]
panic = "unwind"
debug = 1
EOF
}
write_manifest "$bridge/Cargo.toml" "$fm/bridge/NativeMetalBridge.rs" "$m2v"
cp "$here/Cargo.lock" "$bridge/Cargo.lock"
echo "$name: cargo build $RUST_TARGET (log $M/cargo-build.log)"
run "$M/cargo-build.log" env NMB_FIXTURE_AIR="$air/runtime_examples.air" NMB_FC_AIR="$air/function_constant.air" \
	IPHONEOS_DEPLOYMENT_TARGET="$IOS_MIN" MACOSX_DEPLOYMENT_TARGET=14.0 \
	"${cargo[@]}" build --release --locked --manifest-path "$bridge/Cargo.toml" --target "$RUST_TARGET" \
	--target-dir "$M/target"
bridge_lib=$M/target/$RUST_TARGET/release/libnative_metal_bridge.a
[[ -f $bridge_lib ]] || die "no $bridge_lib"

# --- Bundle and m2vtest (ios-flame phone/metal2/build.sh) ---------------------
rm -rf "$dst"
mkdir -p "$dst/debug" "$dst/source" "$dst/licenses"
B=$dst/$BUNDLE.bundle
mkdir -p "$B"
# shellcheck disable=SC2054 # -Wl,-dead_strip is one argument
bundle_cmd=("$cc" "${ios_flags[@]}" -x objective-c++ -std=c++17 -fobjc-arc -O2 -g -Wall
	-Wno-deprecated-declarations -Wno-unused-function -bundle -I "$fm"
	-o "$B/$BUNDLE" "$fm/SweetMetal2.mm" "$fm/NativeBridgeMetalFrontend.mm" -x none "$bridge_lib"
	-Wl,-dead_strip -lc++ -framework Foundation -framework Metal -framework IOSurface -framework IOKit
	-framework CoreFoundation)
# shellcheck disable=SC2054
m2vtest_cmd=("$cc" "${ios_flags[@]}" -O2 -Wall -I "$fm" -o "$dst/m2vtest" "$fm/m2vtest.c" "$bridge_lib"
	-Wl,-dead_strip)
run "$M/link-bundle.log" "${bundle_cmd[@]}"
grep -v "was built for newer 'iOS' version" "$M/link-bundle.log" || true
run "$M/link-m2vtest.log" "${m2vtest_cmd[@]}"
grep -v "was built for newer 'iOS' version" "$M/link-m2vtest.log" || true

# The class Metal.framework instantiates must be the one the Info.plist names.
principal=$(plutil -extract NSPrincipalClass raw -o - "$fm/Info.plist")
[[ $principal == "$CLASS" ]] || die "$fm/Info.plist: NSPrincipalClass '$principal', expected $CLASS"
classes=$(nm "$B/$BUNDLE")
grep -qF "_OBJC_CLASS_\$_$CLASS" <<<"$classes" || die "$B/$BUNDLE does not define the class $CLASS"

if [[ -d $B/$BUNDLE.dSYM ]]; then
	mv "$B/$BUNDLE.dSYM" "$dst/debug/"
else
	dsymutil "$B/$BUNDLE" -o "$dst/debug/$BUNDLE.dSYM" || echo "$name: warning: no dSYM" >&2
fi
strip -x "$B/$BUNDLE"
cp "$fm/Info.plist" "$B/Info.plist"
cp "$turnip" "$B/libvulkan_freedreno.dylib"
strip -x "$B/libvulkan_freedreno.dylib"
codesign -f -s - "$B/libvulkan_freedreno.dylib"
codesign -f -s - "$B"
codesign -v "$B"
strip -x "$dst/m2vtest"
codesign -f -s - "$dst/m2vtest"
for f in "$B/$BUNDLE" "$B/libvulkan_freedreno.dylib" "$dst/m2vtest"; do
	check_macho "$f"
	list_weak "$f"
done

# --- Licences ----------------------------------------------------------------
mkdir -p "$dst/licenses/metal2vulkan" "$dst/licenses/ios-flame" "$dst/licenses/unixnu-nativemetal" \
	"$dst/licenses/crates"
cp "$m2v/LICENSE" "$m2v/LICENSE-GPL-2.0" "$m2v/LICENSE-GPL-3.0" "$m2v/LICENSE-LGPL-3.0" "$dst/licenses/metal2vulkan/"
cp "$flame/LICENSE" "$dst/licenses/ios-flame/LICENSE"
cp "$fm/bridge/LICENSE" "$dst/licenses/unixnu-nativemetal/LICENSE"
if [[ -d $OUT/turnip/licenses ]]; then
	cp -R "$OUT/turnip/licenses" "$dst/licenses/turnip"
	[[ -f $OUT/turnip/NOTICE.txt ]] && cp "$OUT/turnip/NOTICE.txt" "$dst/licenses/turnip/NOTICE.txt"
else
	echo "$name: warning: no $OUT/turnip/licenses (Turnip's notices)" >&2
fi
# Every crates.io package of the lock file: name, version, SPDX licence, where
# it comes from, whether it is linked into the target, and its licence files.
"${cargo[@]}" metadata --format-version 1 --locked --manifest-path "$bridge/Cargo.toml" \
	--filter-platform "$RUST_TARGET" >"$M/metadata.json"
python3 -I - "$M/metadata.json" "$dst/licenses/crates" <<'PY'
import json, os, shutil, sys

meta = json.load(open(sys.argv[1]))
out = sys.argv[2]
packages = {p['id']: p for p in meta['packages']}
nodes = {n['id']: n for n in meta['resolve']['nodes']}
# Linked into the static library: reachable from the root through normal
# dependencies, proc-macro crates excluded (they run in the compiler only).
linked, stack = set(), [meta['resolve']['root']]
while stack:
    pid = stack.pop()
    if pid in linked:
        continue
    if any('proc-macro' in t['kind'] for t in packages[pid]['targets']):
        continue
    linked.add(pid)
    for dep in nodes[pid]['deps']:
        if any(k.get('kind') is None for k in dep['dep_kinds']):
            stack.append(dep['pkg'])
lines, missing = [], []
for p in sorted(packages.values(), key=lambda p: (p['name'], p['version'])):
    if not p.get('source'):  # path packages: the bridge and metal2vulkan, covered by NOTICE.txt
        continue
    root = os.path.dirname(p['manifest_path'])
    names = sorted(f for f in os.listdir(root)
                   if f.upper().startswith(('LICENSE', 'LICENCE', 'COPYING', 'NOTICE', 'UNLICENSE', 'COPYRIGHT')))
    if p.get('license_file'):
        names.append(os.path.relpath(os.path.join(root, p['license_file']), root))
    target = os.path.join(out, '%s-%s' % (p['name'], p['version']))
    os.makedirs(target, exist_ok=True)
    for f in dict.fromkeys(names):
        path = os.path.join(root, f)
        if os.path.isfile(path):
            shutil.copyfile(path, os.path.join(target, os.path.basename(f)))
    if not names:
        missing.append(p['name'])
    lines.append('%-22s %-18s %-32s %-6s %s' % (
        p['name'], p['version'], p.get('license') or '?', 'linked' if p['id'] in linked else 'build',
        p.get('repository') or ''))
with open(os.path.join(out, 'THIRD-PARTY-CRATES.txt'), 'w') as f:
    f.write('Rust crates from crates.io in the build of the bridge (phone/metal2/Cargo.lock).\n'
            '"linked": inside SweetMetal2 and m2vtest; "build": only used while compiling.\n'
            'Licence files: the directory of the same name next to this file.\n\n')
    f.write('\n'.join(lines) + '\n')
for name in missing:
    print('metal2: warning: crate %s ships no licence file; its SPDX expression is listed' % name, file=sys.stderr)
print('metal2: %d crates listed, %d linked' % (len(lines), sum(1 for p in linked if packages[p].get('source'))))
PY

# --- Complete Corresponding Source -------------------------------------------
# The exact tree compiled above, rebuildable offline: metal2vulkan (subset,
# patched), its patches, ios-flame's phone/metal2 (frontend, bridge, plist,
# test), the bridge manifest with relative paths, Cargo.lock, the vendored
# crates, Turnip's patches and notices, and our scripts.
stage=$M/source-stage
top=$stage/$BUNDLE-source
rm -rf "$stage"
mkdir -p "$top/metal2vulkan" "$top/patches/metal2vulkan" "$top/ios-flame/phone" "$top/bridge/.cargo" \
	"$top/andromeda/phone/metal2" "$top/andromeda/phone/turnip"
(cd "$m2v" && COPYFILE_DISABLE=1 tar --exclude .git -cf - .) | tar -xf - -C "$top/metal2vulkan"
for file in "${patch_files[@]}"; do
	cp "$file" "$top/patches/metal2vulkan/"
	echo "${file##*/}" >>"$top/patches/metal2vulkan/series"
done
cp "$flame/LICENSE" "$flame/CREDITS.md" "$top/ios-flame/"
cp -R "$fm" "$top/ios-flame/phone/metal2"
write_manifest "$top/bridge/Cargo.toml" ../ios-flame/phone/metal2/bridge/NativeMetalBridge.rs ../metal2vulkan
cp "$here/Cargo.lock" "$top/bridge/Cargo.lock"
echo "$name: cargo vendor (log $M/cargo-vendor.log)"
run "$M/cargo-vendor.log" "${cargo[@]}" vendor --locked --versioned-dirs --manifest-path "$top/bridge/Cargo.toml" \
	"$top/vendor"
cat >"$top/bridge/.cargo/config.toml" <<'EOF'
[source.crates-io]
replace-with = "vendored-sources"

[source.vendored-sources]
directory = "../vendor"
EOF
cp "$here/build.sh" "$here/install-root.sh" "$here/Cargo.lock" "$top/andromeda/phone/metal2/"
cp "$repo/phone/turnip/build.sh" "$repo/phone/turnip/install-root.sh" "$top/andromeda/phone/turnip/"
if [[ -f $repo/.github/workflows/gpu-userland-andromeda.yml ]]; then
	mkdir -p "$top/andromeda/.github/workflows"
	cp "$repo/.github/workflows/gpu-userland-andromeda.yml" "$top/andromeda/.github/workflows/"
fi
if [[ -d $OUT/turnip ]]; then
	mkdir -p "$top/turnip"
	for item in NOTICE.txt BUILDINFO.txt patches licenses; do
		[[ -e $OUT/turnip/$item ]] && cp -R "$OUT/turnip/$item" "$top/turnip/"
	done
fi
cat >"$top/REBUILD.txt" <<EOF
Complete Corresponding Source of $BUNDLE.bundle and m2vtest
==========================================================

$BUNDLE links metal2vulkan (https://github.com/steelbrain/metal2vulkan,
commit $M2V_COMMIT, GPL-2.0-or-later OR LGPL-3.0-or-later) statically. This
archive is everything that was compiled into it, so you can rebuild it, and
relink it with a modified metal2vulkan:

  metal2vulkan/        the crates the bridge builds, at $M2V_COMMIT, with
                       patches/metal2vulkan applied (order: series); the
                       licence texts are in metal2vulkan/LICENSE*
  ios-flame/           ios-flame (Nick Wylynko) commit $FLAME_COMMIT, MIT:
                       phone/metal2 = SweetMetal2.mm, Info.plist, m2vtest.c,
                       NativeBridgeMetalFrontend.* and bridge/ (UniXNU
                       NativeMetal, MIT, bridge/LICENSE)
  bridge/              Cargo.toml + Cargo.lock of the static library; vendored
                       crates through bridge/.cargo/config.toml
  vendor/              the crates.io sources at the locked versions
  turnip/              how the Turnip dylib inside the bundle was built (Mesa
                       commit, patches, notices); Mesa is MIT, its source is
                       at https://gitlab.freedesktop.org/mesa/mesa
  andromeda/           the UniXNU-andromeda scripts that ran the build

On macOS with Xcode, Rust $RUST_TOOLCHAIN (rustup target add aarch64-apple-ios)
and LLVM >= $LLVM_MIN (llvm-as):

  F=metal2vulkan/crates/metal2vulkan-validation/fixtures/public
  llvm-as -o runtime_examples.air  \$F/examples/runtime_examples.ll
  llvm-as -o function_constant.air \$F/kernel_function_constant_gate_fold.ll
  cd bridge
  NMB_FIXTURE_AIR=\$PWD/../runtime_examples.air NMB_FC_AIR=\$PWD/../function_constant.air \\
    IPHONEOS_DEPLOYMENT_TARGET=$IOS_MIN cargo +$RUST_TOOLCHAIN build --release --offline --locked \\
    --target $RUST_TARGET
  cd ..
  A=bridge/target/$RUST_TARGET/release/libnative_metal_bridge.a
  SDK=\$(xcrun --sdk iphoneos --show-sdk-path)
  M=ios-flame/phone/metal2
  mkdir -p $BUNDLE.bundle
  xcrun --sdk iphoneos clang -arch arm64 -isysroot "\$SDK" -miphoneos-version-min=$IOS_MIN \\
    -x objective-c++ -std=c++17 -fobjc-arc -O2 -g -Wall -bundle -I \$M -o $BUNDLE.bundle/$BUNDLE \\
    \$M/SweetMetal2.mm \$M/NativeBridgeMetalFrontend.mm -x none \$A -Wl,-dead_strip -lc++ \\
    -framework Foundation -framework Metal -framework IOSurface -framework IOKit -framework CoreFoundation
  xcrun --sdk iphoneos clang -arch arm64 -isysroot "\$SDK" -miphoneos-version-min=$IOS_MIN \\
    -O2 -Wall -I \$M -o m2vtest \$M/m2vtest.c \$A -Wl,-dead_strip
  cp \$M/Info.plist $BUNDLE.bundle/
  cp libvulkan_freedreno.dylib $BUNDLE.bundle/      # Turnip (phone/turnip/build.sh)
  codesign -f -s - $BUNDLE.bundle/libvulkan_freedreno.dylib $BUNDLE.bundle m2vtest

andromeda/phone/metal2/build.sh does the same with checks (the exact command
lines it ran are in BUILDINFO.txt next to the binaries).
EOF
COPYFILE_DISABLE=1 tar -C "$stage" -cJf "$dst/source/$BUNDLE-corresponding-source.tar.xz" "$BUNDLE-source"

# --- Notice and build information ----------------------------------------------
device_commit=$(git -C "$repo" rev-parse HEAD 2>/dev/null || echo unknown)
cat >"$dst/NOTICE.txt" <<EOF
$BUNDLE.bundle and m2vtest for the Xiaomi Mi Mix 3 5G (andromeda), iOS $IOS_MIN arm64
=================================================================================

Built by phone/metal2/build.sh of UniXNU-andromeda (commit $device_commit),
from public sources at fixed commits; nothing here comes from an iOS image.

What is inside, and under which licence
- metal2vulkan (Anees Iqbal / steelbrain, https://github.com/steelbrain/metal2vulkan,
  commit $M2V_COMMIT): the Metal API on Vulkan, AIR -> SPIR-V. LINKED
  STATICALLY into $BUNDLE and m2vtest. Licence: GPL-2.0-or-later OR
  LGPL-3.0-or-later, at your choice (licenses/metal2vulkan/: LICENSE,
  LICENSE-GPL-2.0, LICENSE-GPL-3.0, LICENSE-LGPL-3.0). It is used with the
  patches of ios-flame phone/metal2/bridge/patches: 0001-0006 by UniXNU,
  0100-0101 by ios-flame; as modifications of metal2vulkan they are under
  its licence.
- The bridge (NativeMetalBridge.rs, NativeMetalObjects.rs/.h) and the Metal
  frontend (NativeBridgeMetalFrontend.mm/.h): UniXNU NativeMetal
  (https://github.com/UniXNU), MIT (licenses/unixnu-nativemetal/LICENSE); the
  frontend as adapted to iOS 16 by ios-flame.
- SweetMetal2.mm, Info.plist, m2vtest.c: ios-flame, Nick Wylynko
  (https://github.com/NWylynko/ios-flame, commit $FLAME_COMMIT), MIT
  (licenses/ios-flame/LICENSE).
- Rust crates from crates.io (MIT, MIT OR Apache-2.0, ISC, Unlicense OR MIT):
  licenses/crates/THIRD-PARTY-CRATES.txt and one directory per crate with its
  licence files.
- libvulkan_freedreno.dylib inside the bundle: Turnip (Mesa, MIT, with UniXNU's
  BSD-2-Clause-Patent patches and ios-flame's MIT patch), a separate library
  loaded at run time: licenses/turnip/ (its NOTICE.txt names the commits).

Source (GPL-2.0 section 3(a), LGPL-3.0 section 4(d)(0))
  source/$BUNDLE-corresponding-source.tar.xz, shipped with these binaries, is
  the Complete Corresponding Source: every file compiled into $BUNDLE and
  m2vtest (metal2vulkan with its patches, frontend, bridge, vendored crates,
  Cargo.toml, Cargo.lock) and the scripts that build and install them, with
  REBUILD.txt. With it you can modify metal2vulkan and rebuild and relink the
  bundle. If you received the binaries without it, the same files come from
  the commits above and from https://github.com/giannoccarol/UniXNU-cepheus
  (branch andromeda: phone/metal2, phone/turnip).

There is no warranty for this program (see the licence texts).
EOF
{
	echo "Built $(date -u +%Y-%m-%dT%H:%M:%SZ) from UniXNU-andromeda $device_commit (phone/metal2/build.sh)"
	echo "ios-flame $FLAME_COMMIT from ${FLAME_URLS[0]}"
	echo "metal2vulkan $M2V_COMMIT from ${M2V_URLS[0]}"
	echo "Turnip: $turnip"
	echo "minimum iOS $IOS_MIN"
	echo
	xcodebuild -version 2>/dev/null || true
	echo "iphoneos SDK $(xcrun --sdk iphoneos --show-sdk-version)"
	"$cc" --version 2>/dev/null | head -1 || true
	rustup run "$RUST_TOOLCHAIN" rustc --version
	"${cargo[@]}" --version
	"$llvm_as" --version 2>/dev/null | grep -i version | head -1 || true
	echo
	echo "commands:"
	printf '  %q' "${bundle_cmd[@]}"
	echo
	printf '  %q' "${m2vtest_cmd[@]}"
	echo
	echo
	echo "sha256:"
	(cd "$dst" && find "$BUNDLE.bundle" m2vtest source -type f -print0 | xargs -0 shasum -a 256 | sed 's/^/  /')
} >"$dst/BUILDINFO.txt"

ls -l "$dst" "$B" "$dst/source"
echo "$name: done"
