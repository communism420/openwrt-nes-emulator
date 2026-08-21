#!/usr/bin/env python3
"""Deterministic and safety checks for the project-authored demo ROM."""

from __future__ import annotations

import hashlib
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "make-demo-rom.py"
EXPECTED_SHA256 = "b55a4dc9b8a8e0b4b78922bc8a2ec473379a1c56bab2807d417c70991ce97767"


def load_generator():
    spec = importlib.util.spec_from_file_location("make_demo_rom", SCRIPT)
    if spec is None or spec.loader is None:
        raise AssertionError("could not load demo ROM generator")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main() -> int:
    module = load_generator()
    rom = module.make_rom()
    assert len(rom) == 16 + 0x4000 + 0x2000
    assert rom[:16] == b"NES\x1a\x01\x01\x00\x00" + bytes(8)
    assert rom[16 + 0x3FFA : 16 + 0x4000] == b"\x00\x80" * 3
    assert any(rom[16 + 0x4000 :]), "CHR data is unexpectedly blank"
    assert hashlib.sha256(rom).hexdigest() == EXPECTED_SHA256
    lowered = rom.lower()
    for forbidden in (b"nintendo", b"mario", b"zelda", b"metroid"):
        assert forbidden not in lowered

    with tempfile.TemporaryDirectory(prefix="openwrt-nes-demo-") as temporary:
        destination = Path(temporary) / "project-demo.nes"
        subprocess.run(
            [sys.executable, str(SCRIPT), str(destination)],
            check=True,
            stdout=subprocess.DEVNULL,
        )
        assert destination.read_bytes() == rom
        duplicate = subprocess.run(
            [sys.executable, str(SCRIPT), str(destination)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        assert duplicate.returncode != 0, "generator overwrote output without --force"
        subprocess.run(
            [sys.executable, str(SCRIPT), str(destination), "--force"],
            check=True,
            stdout=subprocess.DEVNULL,
        )
        assert destination.read_bytes() == rom

    print("demo ROM contract: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
