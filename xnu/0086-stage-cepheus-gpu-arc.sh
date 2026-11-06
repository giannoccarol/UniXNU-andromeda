#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# DXB executes all stage scripts BEFORE applying patches. sweet_rpmh.c is
# created by the upstream patch series, so only the base XNU tree exists here.
test -f bsd/conf/files
test -d bsd/dev
install -m 0644 "$here/cepheus_arc.h" "$here/cepheus_arc_probe.h" "$here/cepheus_bcm.h" bsd/dev/
