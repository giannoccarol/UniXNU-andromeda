#!/usr/bin/env python3
"""Offline transport rewrite regressions (not a network availability test)."""
import importlib.util
from pathlib import Path

spec = importlib.util.spec_from_file_location("source", Path(__file__).with_name("prepare-bearssl-source.py"))
source = importlib.util.module_from_spec(spec)
spec.loader.exec_module(source)
original = f'bearssl_sha={source.SHA}\ngit fetch {source.ORIGIN} "$bearssl_sha"\n{source.CHECK}\n'
updated = source.prepare(original)
assert source.MIRROR in updated and source.ORIGIN not in updated
assert source.CHECK in updated and source.TREE_CHECK in updated
assert source.prepare(updated) == updated
for bad in (original.replace(source.SHA, "0" * 40), original.replace(source.CHECK, "true"),
            original + source.ORIGIN, original.replace(source.ORIGIN, "https://invalid.example"),
            updated.replace(source.TREE, "0" * 40)):
    try:
        source.prepare(bad)
    except ValueError:
        pass
    else:
        raise AssertionError("accepted changed pin/check/transport")
print("BearSSL transport: original commit + tree locked, drift refused, idempotence PASS")
