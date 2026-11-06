#!/usr/bin/env bash
# Runs inside build/Dockerfile.firmware (see build-uefi.sh). Builds, for the
# device in board.toml, firmware-only boot images (GUACESP payload, no kernel):
#   boot-<name>-uefi-plain.img   plain Aloha UEFI (test 1: does the UEFI start?)
#   boot-<name>-uefi-unixnu.img  Aloha + UniXNU/DarwinBoot (test 2: UniXNU menu)
# Every step is idempotent: re-running skips what is already done.
set -euo pipefail
cd /work
NAME="$(python3 -c 'import tomllib; print(tomllib.load(open("board.toml","rb"))["board"]["name"])')"
export JOBS="${JOBS:-16}" FW_TARGET="${FW_TARGET:-RELEASE}"
OUT="out/$NAME"
mkdir -p "$OUT/fv" "$OUT/boot"

apply_once() {  # apply_once <repo> <patch path relative to repo> [git apply flags]
  local repo="$1" patch="$2"; shift 2
  if git -C "$repo" apply --reverse --check "$patch" 2>/dev/null; then
    echo "patch: $(basename "$patch") already applied in $repo"
  else
    git -C "$repo" apply "$@" "$patch" && echo "patch: applied $(basename "$patch") in $repo"
  fi
}

echo "== patches"
apply_once unixnu ../patches/unixnu-cepheus.patch
apply_once unixnu/drivers/DarwinBoot ../../../patches/darwinboot-cepheus.patch
python3 scripts/patch-darwinboot-diag.py unixnu/drivers/DarwinBoot/DarwinBootPkg/Driver/DarwinBoot.c
apply_once uefi ../patches/aloha-cepheus.patch --reject --whitespace=nowarn \
  || echo "NOTE: some aloha hunks were rejected; fix_aloha_tree.py re-does them"
python3 scripts/fix_aloha_tree.py uefi

echo "== device tree"
bash scripts/get-dtb.sh

echo "== Mu BaseCrypto prebuilt driver (microsoft/mu_crypto_release v1.1.3)"
CRYPTO=uefi/MU_BASECORE/CryptoPkg/Binaries/edk2-basecrypto-driver-bin_extdep
if [ ! -f "$CRYPTO/Driver/Bin/CryptoDriver.inc.dsc" ]; then
  mkdir -p "$CRYPTO"
  curl -fsSL -o /tmp/cryptobin.zip \
    https://github.com/microsoft/mu_crypto_release/releases/download/v1.1.3/Mu_CryptoBin_v1_1_3.zip
  unzip -q -o /tmp/cryptobin.zip -d "$CRYPTO"
fi
ls -l "$CRYPTO/Driver/Bin/CryptoDriver.inc.dsc"

echo "== EDK II host tools"
if [ ! -f uefi/MU_BASECORE/BaseTools/Source/C/bin/GenFw ]; then
  # The old PCCTS sources only build with a pre-C23 standard.
  WRAP=/tmp/legacycc; mkdir -p "$WRAP"
  for c in cc gcc; do
    printf '#!/bin/sh\nexec /usr/bin/%s -std=gnu17 "$@"\n' "$c" > "$WRAP/$c"; chmod +x "$WRAP/$c"
  done
  PATH="$WRAP:$PATH" make -C uefi/MU_BASECORE/BaseTools/Source/C -j"$JOBS" \
    EXTRA_OPTFLAGS="-Wno-error -std=gnu17" > "$OUT/basetools-host.log" 2>&1 \
    || { tail -40 "$OUT/basetools-host.log"; exit 1; }
fi

echo "== EDK II Conf with relaxed clang flags"
CONF="$OUT/edk2-conf"; TPL=uefi/MU_BASECORE/BaseTools/Conf
mkdir -p "$CONF"
for n in target tools_def build_rule; do
  [ -f "$CONF/$n.txt" ] || cp "$TPL/$n.template" "$CONF/$n.txt"
done
python3 scripts/relax_clang.py "$CONF/tools_def.txt"

echo "== test payload (GUACESP FAT, no kernel)"
PAYLOAD="${PAYLOAD:-$OUT/payload/empty-payload.img}"
[ -f "$PAYLOAD" ] || bash scripts/make-empty-payload.sh "$PAYLOAD"

FD="uefi/Build/SurfaceDuo1Pkg/${FW_TARGET}_CLANGPDB/FV/SM8150_EFI.fd"
# VARIANTS: space-separated name=modules (modules: sheng,unixnu,unixnudxe,darwinboot;
# empty = plain Aloha UEFI). Default: the plain UEFI and the full UniXNU one.
VARIANTS="${VARIANTS:-plain= unixnu=sheng,unixnu,unixnudxe,darwinboot}"
for SPEC in $VARIANTS; do
  VARIANT="${SPEC%%=*}"; MODS="${SPEC#*=}"
  echo "== firmware: $VARIANT (${MODS:-plain})"
  unset NO_DARWINBOOT
  if [ -z "$MODS" ]; then export NO_DARWINBOOT=1; else export UNIXNU_MODULES="$MODS"; fi
  EXTRA_OPTFLAGS="-Wno-error" python3 scripts/build-firmware.py
  cp "$FD" "$OUT/fv/SM8150_EFI-$VARIANT.fd"
  # Always pack a GUACESP FAT payload: with an empty ramdisk ShengBootMemoryDxe
  # scans Reserved memory too and hangs on the Hypervisor region (see ANDROMEDA.md).
  python3 scripts/build-bootimg.py --fd "$OUT/fv/SM8150_EFI-$VARIANT.fd" \
    --ramdisk "$PAYLOAD" -o "$OUT/boot/boot-$NAME-uefi-$VARIANT.img"
  mv "$OUT/boot/boot.json" "$OUT/boot/boot-$NAME-uefi-$VARIANT.json"
done

echo "== done"
ls -l "$OUT/fv" "$OUT/boot"
sha256sum "$OUT"/boot/*.img
