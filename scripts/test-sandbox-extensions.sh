#!/usr/bin/env bash
# Host test of patch 0116 (sandbox extension issue/consume in the SWEET
# "Sandbox" policy): cuts the functions out of a patched bsd/kern/sweet_amfi.c
# and runs xnu/test_sandbox_ext.c against them.
#
#   scripts/test-sandbox-extensions.sh PATH/TO/sweet_amfi.c
set -euo pipefail
SRC=${1:?usage: test-sandbox-extensions.sh PATH/TO/sweet_amfi.c}
here=$(cd "$(dirname "$0")/.." && pwd)
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
# body.c: from the first #define of the patch to the line before
# "static int" + "sweet_sandbox_syscall(" (the harness adds "static int").
awk '/^#define SWEET_SB_EXT_TOKEN_MAX/{on=1} /^sweet_sandbox_syscall\(/{exit} on{print}' "$SRC" | sed '$d' > "$W/body.c"
awk '/^sweet_sandbox_syscall\(/{on=1} on{print} on && /^}/{exit}' "$SRC" > "$W/sys.c"
grep -q 'sweet_sandbox_ext_issue' "$W/body.c" || { echo "no patch 0116 code in $SRC" >&2; exit 1; }
cc -std=gnu11 -Wall -Wextra -Werror -fsanitize=undefined -I"$W" "$here/xnu/test_sandbox_ext.c" -o "$W/test"
"$W/test"
