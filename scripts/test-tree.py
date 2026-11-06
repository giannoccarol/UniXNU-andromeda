#!/usr/bin/env python3
"""Regression for stale same-size scripts during upstream synchronization."""
import tempfile
from pathlib import Path
from tree import assemble

with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    upstream, device, work = (root / name for name in ("upstream", "device", "work"))
    for source in (upstream, device):
        (source / "scripts").mkdir(parents=True)
    (upstream / "package").mkdir()
    script = device / "scripts/probe.py"
    (upstream / "scripts/probe.py").write_text("upstream")
    script.write_text("old code")
    tree = assemble(work, upstream, device)
    target = tree / "scripts/probe.py"
    assert target.read_text() == "old code"
    script.write_text("new code")
    assemble(work, upstream, device)
    assert target.read_text() == "new code"
    outside = root / "outside"
    outside.write_text("keep me!")
    target.unlink()
    target.symlink_to(outside)
    assemble(work, upstream, device)
    assert not target.is_symlink() and target.read_text() == "new code"
    assert outside.read_text() == "keep me!"
print("Build tree refresh: same-size upstream/device overrides, external symlink preserved PASS")
