#!/usr/bin/env python3
"""Generate the original, redistributable NROM used in project screenshots."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import struct
import tempfile


GLYPHS = {
    " ": ("00000",) * 7,
    "A": ("01110", "10001", "10001", "11111", "10001", "10001", "10001"),
    "B": ("11110", "10001", "10001", "11110", "10001", "10001", "11110"),
    "C": ("01111", "10000", "10000", "10000", "10000", "10000", "01111"),
    "D": ("11110", "10001", "10001", "10001", "10001", "10001", "11110"),
    "E": ("11111", "10000", "10000", "11110", "10000", "10000", "11111"),
    "F": ("11111", "10000", "10000", "11110", "10000", "10000", "10000"),
    "G": ("01111", "10000", "10000", "10111", "10001", "10001", "01111"),
    "H": ("10001", "10001", "10001", "11111", "10001", "10001", "10001"),
    "I": ("11111", "00100", "00100", "00100", "00100", "00100", "11111"),
    "J": ("00111", "00010", "00010", "00010", "10010", "10010", "01100"),
    "K": ("10001", "10010", "10100", "11000", "10100", "10010", "10001"),
    "L": ("10000", "10000", "10000", "10000", "10000", "10000", "11111"),
    "M": ("10001", "11011", "10101", "10101", "10001", "10001", "10001"),
    "N": ("10001", "11001", "10101", "10011", "10001", "10001", "10001"),
    "O": ("01110", "10001", "10001", "10001", "10001", "10001", "01110"),
    "P": ("11110", "10001", "10001", "11110", "10000", "10000", "10000"),
    "Q": ("01110", "10001", "10001", "10001", "10101", "10010", "01101"),
    "R": ("11110", "10001", "10001", "11110", "10100", "10010", "10001"),
    "S": ("01111", "10000", "10000", "01110", "00001", "00001", "11110"),
    "T": ("11111", "00100", "00100", "00100", "00100", "00100", "00100"),
    "U": ("10001", "10001", "10001", "10001", "10001", "10001", "01110"),
    "V": ("10001", "10001", "10001", "10001", "10001", "01010", "00100"),
    "W": ("10001", "10001", "10001", "10101", "10101", "10101", "01010"),
    "X": ("10001", "10001", "01010", "00100", "01010", "10001", "10001"),
    "Y": ("10001", "10001", "01010", "00100", "00100", "00100", "00100"),
    "Z": ("11111", "00001", "00010", "00100", "01000", "10000", "11111"),
    "0": ("01110", "10001", "10011", "10101", "11001", "10001", "01110"),
    "1": ("00100", "01100", "00100", "00100", "00100", "00100", "01110"),
    "2": ("01110", "10001", "00001", "00010", "00100", "01000", "11111"),
    "3": ("11110", "00001", "00001", "01110", "00001", "00001", "11110"),
    "4": ("00010", "00110", "01010", "10010", "11111", "00010", "00010"),
    "5": ("11111", "10000", "10000", "11110", "00001", "00001", "11110"),
    "6": ("01110", "10000", "10000", "11110", "10001", "10001", "01110"),
    "7": ("11111", "00001", "00010", "00100", "01000", "01000", "01000"),
    "8": ("01110", "10001", "10001", "01110", "10001", "10001", "01110"),
    "9": ("01110", "10001", "10001", "01111", "00001", "00001", "01110"),
    "+": ("00000", "00100", "00100", "11111", "00100", "00100", "00000"),
    "-": ("00000", "00000", "00000", "11111", "00000", "00000", "00000"),
    "=": ("00000", "11111", "00000", "11111", "00000", "00000", "00000"),
    "|": ("00100",) * 7,
    "[": ("01110", "01000", "01000", "01000", "01000", "01000", "01110"),
    "]": ("01110", "00010", "00010", "00010", "00010", "00010", "01110"),
    "~": ("00000", "00000", "01001", "10110", "00000", "00000", "00000"),
    ".": ("00000", "00000", "00000", "00000", "00000", "01100", "01100"),
}


def make_chr() -> bytes:
    result = bytearray(8192)
    blue = {"+", "-", "|", "[", "]"}
    orange = {"=", "~"}
    for character, rows in GLYPHS.items():
        tile = ord(character) * 16
        for row_index, row in enumerate(rows):
            bits = int(row, 2) << 2
            if character in orange:
                result[tile + row_index] = bits
                result[tile + 8 + row_index] = bits
            elif character in blue:
                result[tile + 8 + row_index] = bits
            else:
                result[tile + row_index] = bits
    return bytes(result)


def make_name_table() -> bytes:
    table = bytearray(b" " * 1024)

    for row in range(2, 28):
        table[row * 32] = ord("|")
        table[row * 32 + 31] = ord("|")
    for row in (2, 27):
        table[row * 32 : row * 32 + 32] = b"+" + b"-" * 30 + b"+"

    def centered(row: int, text: str) -> None:
        encoded = text.encode("ascii")
        if len(encoded) > 30:
            raise ValueError(f"demo text is wider than the frame: {text}")
        column = (32 - len(encoded)) // 2
        table[row * 32 + column : row * 32 + column + len(encoded)] = encoded

    centered(5, "OPENWRT NES EMULATOR")
    centered(8, "ROUTER-SIDE EMULATION")
    centered(10, "FCEUMM + SOFTWARE RENDER")
    centered(13, "[==================]")
    centered(14, "[    ROUTER CPU    ]")
    centered(15, "[==================]")
    centered(18, "~~~~ WI-FI STREAM ~~~~")
    centered(21, "BROWSER = THIN CLIENT")
    centered(24, "NO COMMERCIAL GAME ASSETS")
    centered(26, "PROJECT DEMO ROM")
    return bytes(table)


def make_rom() -> bytes:
    palette_address = 0x8100
    name_table_address = 0x8200
    program = bytearray((
        0x78,                         # SEI
        0xD8,                         # CLD
        0xA2, 0xFF,                   # LDX #$ff
        0x9A,                         # TXS
        0xE8,                         # INX -> 0
        0x8E, 0x00, 0x20,             # STX $2000
        0x8E, 0x01, 0x20,             # STX $2001
        0x2C, 0x02, 0x20, 0x10, 0xFB, # wait for vblank
        0x2C, 0x02, 0x20, 0x10, 0xFB, # wait for a second vblank
        0xA9, 0x3F, 0x8D, 0x06, 0x20,
        0xA9, 0x00, 0x8D, 0x06, 0x20,
        0xA2, 0x00,
        0xBD, palette_address & 0xFF, palette_address >> 8,
        0x8D, 0x07, 0x20, 0xE8, 0xE0, 0x20, 0xD0, 0xF5,
        0xA9, 0x20, 0x8D, 0x06, 0x20,
        0xA9, 0x00, 0x8D, 0x06, 0x20,
    ))
    for page in range(4):
        address = name_table_address + page * 0x100
        program.extend((
            0xA2, 0x00,
            0xBD, address & 0xFF, address >> 8,
            0x8D, 0x07, 0x20,
            0xE8,
            0xD0, 0xF7,
        ))
    program.extend((
        0xA9, 0x00,
        0x8D, 0x05, 0x20,
        0x8D, 0x05, 0x20,
        0x8D, 0x00, 0x20,
        0xA9, 0x08,
        0x8D, 0x01, 0x20,
        0x4C, 0x73, 0x80,             # idle forever at $8073
    ))
    if len(program) != 0x76:
        raise AssertionError(f"unexpected demo program size: {len(program)}")

    palette = bytes((
        0x0F, 0x30, 0x21, 0x27,
        0x0F, 0x30, 0x21, 0x27,
        0x0F, 0x30, 0x21, 0x27,
        0x0F, 0x30, 0x21, 0x27,
    ) * 2)
    prg = bytearray(b"\xEA" * 0x4000)
    prg[: len(program)] = program
    prg[0x100 : 0x100 + len(palette)] = palette
    prg[0x200 : 0x600] = make_name_table()
    prg[0x3FFA : 0x4000] = struct.pack("<HHH", 0x8000, 0x8000, 0x8000)

    header = b"NES\x1a" + bytes((1, 1, 0, 0)) + bytes(8)
    return header + bytes(prg) + make_chr()


def write_atomic(path: Path, data: bytes, force: bool) -> None:
    path = path.expanduser()
    if path.exists() and not force:
        raise FileExistsError(f"refusing to replace {path}; pass --force")
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary_name: str | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="wb", prefix=f".{path.name}.", suffix=".tmp",
            dir=path.parent, delete=False,
        ) as temporary:
            temporary_name = temporary.name
            temporary.write(data)
            temporary.flush()
            os.fsync(temporary.fileno())
        os.chmod(temporary_name, 0o644)
        os.replace(temporary_name, path)
        temporary_name = None
    finally:
        if temporary_name is not None:
            try:
                os.unlink(temporary_name)
            except FileNotFoundError:
                pass


def main() -> int:
    parser = argparse.ArgumentParser(
        description="generate the original OpenWrt NES Emulator demo ROM"
    )
    parser.add_argument("output", type=Path, help="destination .nes file")
    parser.add_argument(
        "--force", action="store_true", help="replace an existing output file"
    )
    args = parser.parse_args()
    if args.output.suffix.lower() != ".nes":
        parser.error("the output filename must end in .nes")
    rom = make_rom()
    write_atomic(args.output, rom, args.force)
    print(f"wrote {args.output} ({len(rom)} bytes)")
    print(f"sha256 {hashlib.sha256(rom).hexdigest()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
