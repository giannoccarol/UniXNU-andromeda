#!/usr/bin/env python3
"""Change transport, never the pinned BearSSL object or crypto sources."""
from pathlib import Path
import sys

SHA = "7bea48e5e850ab4cafbe68d3765cdaba13a86d6f"
TREE = "a8f933a27b14eaf3a348fe9b294bf4262dbb216c"
ORIGIN = "https://www.bearssl.org/git/BearSSL"
MIRROR = "https://github.com/AmanoTeam/BearSSL"
CHECK = '[[ "$(git -C "$bearssl_tmp" rev-parse HEAD)" == "$bearssl_sha" ]]'
TREE_CHECK = f'[[ "$(git -C "$bearssl_tmp" rev-parse HEAD^{{tree}})" == "{TREE}" ]]'


def prepare(text: str) -> str:
    if text.count(f"bearssl_sha={SHA}\n") != 1 or text.count(CHECK) != 1:
        raise ValueError("upstream BearSSL pin/check changed; review required")
    if ORIGIN not in text and text.count(MIRROR) == 1 and text.count(TREE_CHECK) == 1:
        return text
    if text.count(ORIGIN) != 1 or MIRROR in text or TREE_CHECK in text:
        raise ValueError("unexpected BearSSL fetch layout; review required")
    return text.replace(ORIGIN, MIRROR).replace(CHECK, CHECK + "\n" + TREE_CHECK)


if __name__ == "__main__":
    path = Path(sys.argv[1]) / "0020-stage-bearssl.sh"
    text = path.read_text()
    result = prepare(text)
    if result != text:
        path.write_text(result)
    print(f"BearSSL transport: {MIRROR}; unchanged commit {SHA}; tree {TREE}")
