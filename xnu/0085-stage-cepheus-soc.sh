#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
test -f bsd/conf/files
install -m 0644 "$here/cepheus_soc.c" "$here/cepheus_soc.h" bsd/dev/
install -m 0644 "$here/cepheus_cpu_policy.h" bsd/dev/
install -m 0644 "$here/cepheus_gpu_power.h" bsd/dev/
