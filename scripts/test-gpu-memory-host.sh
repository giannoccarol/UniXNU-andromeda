#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
# Allow the exact mixed C/C++ production path to run under additional host
# sanitizers without changing default CI or suppressing reports. Quarantine
# scenarios deliberately keep backing until process exit; for ASan use
# ASAN_OPTIONS=detect_leaks=0 (not a leak-free claim).
if [[ ${CEPHEUS_GPU_HOST_ASAN:-0} == 1 ]]; then
    export CC="${CC:-clang}" CXX="${CXX:-clang++}"
    export CEPHEUS_GPU_SANITIZERS=address,undefined
fi
sanitizers=${CEPHEUS_GPU_SANITIZERS:-undefined}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
libs=()
if [[ $(uname -s) != Darwin ]]; then libs=(-lcrypto); fi
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize="$sanitizers" -I"$root/xnu" \
    -c "$root/xnu/cepheus_gpu_retire.c" -o "$tmp/retire.o"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize="$sanitizers" -I"$root/xnu" \
    -c "$root/xnu/cepheus_gmu_hfi_link.c" -o "$tmp/cepheus_gmu_hfi_link.c.o"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize="$sanitizers" -pthread \
    -DCEPHEUS_GPU_MEMORY_HOST_TEST=1 -I"$root/tests/gpu-memory-host" -I"$root/xnu" \
    "$root/xnu/cepheus_gpu_memory.cpp" "$root/tests/test_gpu_memory_host.cpp" "$tmp/retire.o" \
    -o "$tmp/test" "${libs[@]}"
"$tmp/test" "$@"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize="$sanitizers" -I"$root/xnu" \
    -c "$root/xnu/cepheus_gmu_workspace.c" -o "$tmp/workspace.o"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize="$sanitizers" -pthread \
    -DCEPHEUS_GPU_MEMORY_HOST_TEST=1 -I"$root/tests/gpu-memory-host" -I"$root/xnu" \
    "$root/xnu/cepheus_gpu_memory.cpp" "$root/tests/test_gmu_workspace.cpp" "$tmp/workspace.o" "$tmp/retire.o" \
    -o "$tmp/workspace" "${libs[@]}"
"$tmp/workspace"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize="$sanitizers" -pthread \
    -DCEPHEUS_GPU_MEMORY_HOST_TEST=1 -I"$root/tests/gpu-memory-host" -I"$root/xnu" \
    "$root/xnu/cepheus_gpu_memory.cpp" "$root/tests/test_gpu_terminal_race.cpp" "$tmp/workspace.o" "$tmp/retire.o" \
    -o "$tmp/terminal" "${libs[@]}"
"$tmp/terminal"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize="$sanitizers" -I"$root/xnu" \
    -c "$root/xnu/cepheus_cp_workspace.c" -o "$tmp/cp-workspace.o"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize="$sanitizers" -I"$root/xnu" \
    -c "$root/tests/test_cp_workspace_restore.c" -o "$tmp/cp-restore.o"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize="$sanitizers" -I"$root/xnu" \
    -c "$root/xnu/test_cp_preinit.c" -o "$tmp/cp-preinit.o"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -Wno-missing-field-initializers -fsanitize="$sanitizers" -pthread \
    -DCEPHEUS_GPU_MEMORY_HOST_TEST=1 -I"$root/tests/gpu-memory-host" -I"$root/xnu" \
    "$root/xnu/cepheus_gpu_memory.cpp" "$root/tests/test_cp_workspace.cpp" "$tmp/cp-workspace.o" "$tmp/cp-restore.o" "$tmp/cp-preinit.o" "$tmp/workspace.o" "$tmp/retire.o" \
    -o "$tmp/cp-workspace" "${libs[@]}"
if [[ $# -ge 2 ]]; then for mode in 0 1 2 3; do "$tmp/cp-workspace" "$2" "$mode"; done; else "$tmp/cp-workspace"; fi
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -Wno-missing-field-initializers -fsanitize="$sanitizers" -pthread \
    -DCEPHEUS_GPU_MEMORY_HOST_TEST=1 -I"$root/tests/gpu-memory-host" -I"$root/xnu" \
    "$root/xnu/cepheus_gpu_memory.cpp" "$root/tests/test_gmu_workspace_upload.cpp" "$tmp/workspace.o" "$tmp/retire.o" \
    -o "$tmp/upload" "${libs[@]}"
if [[ $# -ge 1 ]]; then "$tmp/upload" "$1"; else "$tmp/upload"; fi
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize="$sanitizers" -I"$root/xnu" \
    -c "$root/tests/test_gpu_retire_path.c" -o "$tmp/retire_test.o"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize="$sanitizers" -pthread \
    -DCEPHEUS_GPU_MEMORY_HOST_TEST=1 -I"$root/tests/gpu-memory-host" -I"$root/xnu" \
    "$root/xnu/cepheus_gpu_memory.cpp" "$root/tests/test_gpu_retire_main.cpp" \
    "$tmp/workspace.o" "$tmp/retire.o" "$tmp/retire_test.o" "$tmp/cepheus_gmu_hfi_link.c.o" -o "$tmp/retire_test" "${libs[@]}"
for mode in 0 1 2 3 4 5; do "$tmp/retire_test" "$mode"; done
for unit in xnu/cepheus_gmu_hfi_link.c tests/test_gmu_hfi_link.c; do
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize="$sanitizers" -I"$root/xnu" \
        -c "$root/$unit" -o "$tmp/$(basename "$unit").o"
done
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize="$sanitizers" -pthread \
    -DCEPHEUS_GPU_MEMORY_HOST_TEST=1 -I"$root/tests/gpu-memory-host" -I"$root/xnu" \
    "$root/xnu/cepheus_gpu_memory.cpp" "$root/tests/test_gmu_hfi_link_main.cpp" \
    "$tmp/workspace.o" "$tmp/retire.o" "$tmp/cepheus_gmu_hfi_link.c.o" "$tmp/test_gmu_hfi_link.c.o" \
    -o "$tmp/link" "${libs[@]}"
"$tmp/link"
for unit in xnu/cepheus_gmu_bringup.c xnu/cepheus_gpu_compile.c tests/test_gmu_bringup.c; do
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize="$sanitizers" -I"$root/xnu" \
        -c "$root/$unit" -o "$tmp/$(basename "$unit").o"
done
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize="$sanitizers" -pthread \
    -DCEPHEUS_GPU_MEMORY_HOST_TEST=1 -I"$root/tests/gpu-memory-host" -I"$root/xnu" \
    "$root/xnu/cepheus_gpu_memory.cpp" "$root/tests/test_gmu_bringup_main.cpp" \
    "$tmp/workspace.o" "$tmp/retire.o" "$tmp/cepheus_gmu_hfi_link.c.o" \
    "$tmp/cepheus_gmu_bringup.c.o" "$tmp/cepheus_gpu_compile.c.o" "$tmp/test_gmu_bringup.c.o" \
    -o "$tmp/bringup" "${libs[@]}"
if [[ $# -ge 1 ]]; then for mode in 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22; do "$tmp/bringup" "$1" "$mode"; done; else "$tmp/bringup"; fi
