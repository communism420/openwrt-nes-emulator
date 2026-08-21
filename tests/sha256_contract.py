#!/usr/bin/env python3
"""Check the small bundled SHA-256 implementation against Python hashlib."""

from __future__ import annotations

import ctypes
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "package" / "nes-emulator" / "src" / "sha256.c"
INCLUDE = SOURCE.parent


def main() -> None:
	compiler = os.environ.get("CC", "cc")
	with tempfile.TemporaryDirectory(prefix="nes-sha256-test-") as directory:
		library_path = Path(directory) / "libnes_sha256.so"
		subprocess.run(
			[
				compiler,
				"-std=c11",
				"-Wall",
				"-Wextra",
				"-Werror",
				"-fPIC",
				"-shared",
				f"-I{INCLUDE}",
				str(SOURCE),
				"-o",
				str(library_path),
			],
			check=True,
		)

		library = ctypes.CDLL(str(library_path))
		digest = library.nes_sha256_digest
		digest.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p]
		digest.restype = None

		vectors = [
			b"",
			b"abc",
			bytes(range(256)),
			bytes((index * 37 + 11) & 0xFF for index in range(55)),
			bytes((index * 37 + 11) & 0xFF for index in range(56)),
			bytes((index * 37 + 11) & 0xFF for index in range(63)),
			bytes((index * 37 + 11) & 0xFF for index in range(64)),
			bytes((index * 37 + 11) & 0xFF for index in range(65)),
			bytes((index * 131 + 17) & 0xFF for index in range(10000)),
		]

		for payload in vectors:
			output = (ctypes.c_ubyte * 32)()
			if payload:
				input_buffer = ctypes.create_string_buffer(payload, len(payload))
				input_pointer = ctypes.cast(input_buffer, ctypes.c_void_p)
			else:
				input_pointer = None
			digest(input_pointer, len(payload), output)
			actual = bytes(output)
			expected = hashlib.sha256(payload).digest()
			if actual != expected:
				raise AssertionError(
					f"SHA-256 mismatch for a {len(payload)}-byte test vector"
				)

	print("sha256 contract: all vectors passed")


if __name__ == "__main__":
	main()
