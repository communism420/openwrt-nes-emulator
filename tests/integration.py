#!/usr/bin/env python3
"""Black-box regression tests for a native nesd binary."""

from __future__ import annotations

import argparse
import base64
import json
import os
from pathlib import Path
import resource
import signal
import socket
import struct
import subprocess
import tempfile
import time


TOKEN = "0123456789abcdef" * 4
MAX_UPLOAD_BYTES = 16 * 1024 * 1024
STD_LUMA_QUANTIZATION = (
    16, 11, 10, 16, 24, 40, 51, 61,
    12, 12, 14, 19, 26, 58, 60, 55,
    14, 13, 16, 24, 40, 57, 69, 56,
    14, 17, 22, 29, 51, 87, 80, 62,
    18, 22, 37, 56, 68, 109, 103, 77,
    24, 35, 55, 64, 81, 104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101,
    72, 92, 95, 98, 112, 100, 103, 99,
)
NATURAL_TO_ZIGZAG = (
    0, 1, 5, 6, 14, 15, 27, 28,
    2, 4, 7, 13, 16, 26, 29, 42,
    3, 8, 12, 17, 25, 30, 41, 43,
    9, 11, 18, 24, 31, 40, 44, 53,
    10, 19, 23, 32, 39, 45, 52, 54,
    20, 22, 33, 38, 46, 51, 55, 60,
    21, 34, 37, 47, 50, 56, 59, 61,
    35, 36, 48, 49, 57, 58, 62, 63,
)


class BufferedSocket:
    """Socket wrapper that preserves bytes read past an HTTP upgrade."""

    def __init__(self, sock: socket.socket, pending: bytes = b"") -> None:
        self.sock = sock
        self.pending = bytearray(pending)

    def recv(self, length: int) -> bytes:
        if self.pending:
            count = min(length, len(self.pending))
            result = bytes(self.pending[:count])
            del self.pending[:count]
            return result
        return self.sock.recv(length)

    def sendall(self, data: bytes) -> None:
        self.sock.sendall(data)

    def close(self) -> None:
        self.sock.close()


def make_test_rom() -> bytes:
    """Return a tiny, redistributable battery-backed NROM image."""
    header = bytearray(b"NES\x1a")
    header.extend((1, 1, 0x02, 0x00))
    header.extend(b"\0" * 8)
    prg = bytearray(b"\xea" * 0x4000)
    # SEI; LDA #$42; STA $6000; JMP $8006
    prg[:9] = bytes((0x78, 0xA9, 0x42, 0x8D, 0x00, 0x60, 0x4C, 0x06, 0x80))
    prg[0x3FFA:0x4000] = struct.pack("<HHH", 0x8000, 0x8000, 0x8000)
    return bytes(header + prg + bytearray(0x2000))


def make_pal_test_rom() -> bytes:
    """Return the test NROM image as NES 2.0 with an explicit PAL region."""
    rom = bytearray(make_test_rom())
    rom[7] = 0x08
    rom[12] = 1
    return bytes(rom)


def make_input_probe_rom() -> bytes:
    """Return a battery NROM which mirrors controller A into SRAM byte 0."""
    rom = bytearray(make_test_rom())
    prg = memoryview(rom)[16 : 16 + 0x4000]
    # SEI; repeatedly strobe port 1, read A, and store its low bit at $6000.
    program = bytes((
        0x78,
        0xA9, 0x01,
        0x8D, 0x16, 0x40,
        0xA9, 0x00,
        0x8D, 0x16, 0x40,
        0xAD, 0x16, 0x40,
        0x29, 0x01,
        0x8D, 0x00, 0x60,
        0x4C, 0x01, 0x80,
    ))
    prg[: len(program)] = program
    return bytes(rom)


def make_counter_rom() -> bytes:
    """Return an NROM image with changing volatile RAM, SRAM and video."""
    header = bytearray(b"NES\x1a")
    header.extend((1, 1, 0x02, 0x00))
    header.extend(b"\0" * 8)
    prg = bytearray(b"\xea" * 0x4000)
    # SEI; loop: INC $00; select palette $3f00; mirror $00 to color/SRAM.
    prg[:24] = bytes((
        0x78,
        0xE6, 0x00,
        0xA9, 0x3F,
        0x8D, 0x06, 0x20,
        0xA9, 0x00,
        0x8D, 0x06, 0x20,
        0xA5, 0x00,
        0x8D, 0x07, 0x20,
        0x8D, 0x00, 0x60,
        0x4C, 0x01, 0x80,
    ))
    prg[0x3FFA:0x4000] = struct.pack("<HHH", 0x8000, 0x8000, 0x8000)
    return bytes(header + prg + bytearray(0x2000))


def parse_state_file(path: Path) -> tuple[bytes, bytes]:
    """Return the FCEUmm payload and portable RGB565 screenshot."""
    data = path.read_bytes()
    assert data.startswith(b"NESDST2\0"), data[:16]
    assert len(data) >= 320
    assert struct.unpack_from("<I", data, 8)[0] == 2
    assert struct.unpack_from("<I", data, 12)[0] == 320
    payload_size = struct.unpack_from("<Q", data, 32)[0]
    frame_size = struct.unpack_from("<Q", data, 256)[0]
    assert 320 + payload_size + frame_size == len(data)
    return (
        data[320:320 + payload_size],
        data[320 + payload_size:],
    )


def fcs_field(payload: bytes, field_name: bytes) -> bytes:
    """Extract one named four-byte FCEUmm savestate field."""
    assert len(field_name) == 4
    assert payload[:3] == b"FCS"
    offset = 16
    matches: list[bytes] = []
    while offset < len(payload):
        assert len(payload) - offset >= 5
        chunk_size = struct.unpack_from("<I", payload, offset + 1)[0]
        offset += 5
        chunk_end = offset + chunk_size
        assert chunk_end <= len(payload)
        while offset < chunk_end:
            name = payload[offset:offset + 4]
            field_size = struct.unpack_from("<I", payload, offset + 4)[0]
            offset += 8
            field_end = offset + field_size
            assert field_end <= chunk_end
            if name == field_name:
                matches.append(payload[offset:field_end])
            offset = field_end
        assert offset == chunk_end
    assert offset == len(payload)
    assert len(matches) == 1, (field_name, len(matches))
    return matches[0]


def free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def voluntary_context_switches(pid: int) -> int | None:
    """Return Linux main-thread voluntary switches, when procfs is present."""
    try:
        status = Path(f"/proc/{pid}/status").read_text(encoding="ascii")
    except (OSError, UnicodeError):
        return None
    for line in status.splitlines():
        if line.startswith("voluntary_ctxt_switches:"):
            return int(line.split(":", 1)[1].strip())
    return None


def receive_all(sock: socket.socket) -> bytes:
    chunks: list[bytes] = []
    while True:
        try:
            chunk = sock.recv(65536)
        except socket.timeout:
            break
        if not chunk:
            break
        chunks.append(chunk)
    return b"".join(chunks)


def raw_http(port: int, request: bytes, split: int | None = None) -> tuple[int, dict[str, str], bytes]:
    with socket.create_connection(("127.0.0.1", port), timeout=3) as sock:
        sock.settimeout(5)
        if split is None:
            sock.sendall(request)
        else:
            for offset in range(0, len(request), split):
                sock.sendall(request[offset : offset + split])
                time.sleep(0.003)
        response = receive_all(sock)
    head, separator, body = response.partition(b"\r\n\r\n")
    assert separator, f"malformed HTTP response: {response[:200]!r}"
    lines = head.decode("iso-8859-1").split("\r\n")
    status = int(lines[0].split()[1])
    headers = {
        name.strip().lower(): value.strip()
        for line in lines[1:]
        for name, value in (line.split(":", 1),)
    }
    if "content-length" in headers:
        assert len(body) == int(headers["content-length"])
    return status, headers, body


def request(
    port: int,
    method: str,
    target: str,
    body: bytes = b"",
    *,
    authorized: bool = True,
    origin: str | None = None,
    extra_headers: dict[str, str] | None = None,
    split: int | None = None,
) -> tuple[int, dict[str, str], bytes]:
    headers = {
        "Host": f"127.0.0.1:{port}",
        "Connection": "close",
    }
    if authorized:
        headers["Authorization"] = f"Bearer {TOKEN}"
    if origin is not None:
        headers["Origin"] = origin
    if body or method == "POST":
        headers["Content-Length"] = str(len(body))
    if body:
        headers["Content-Type"] = "application/json"
    if extra_headers:
        headers.update(extra_headers)
    wire = (
        f"{method} {target} HTTP/1.1\r\n".encode()
        + b"".join(f"{name}: {value}\r\n".encode() for name, value in headers.items())
        + b"\r\n"
        + body
    )
    return raw_http(port, wire, split)


def json_request(*args, **kwargs) -> tuple[int, dict]:
    status, _headers, body = request(*args, **kwargs)
    return status, json.loads(body)


def wait_ready(process: subprocess.Popen, port: int) -> None:
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise AssertionError(f"nesd exited early with {process.returncode}")
        try:
            status, reply = json_request(port, "GET", "/api/status")
            if status == 200 and "running" in reply:
                return
        except (OSError, ValueError, AssertionError):
            pass
        time.sleep(0.05)
    raise AssertionError("nesd did not become ready")


def test_client_visibility_options(binary: Path) -> None:
    cases = (
        ((), True, True),
        (("--show-fps",), True, True),
        (("--hide-fps",), False, True),
        (("--show-touch-controls",), True, True),
        (("--hide-touch-controls",), True, False),
        (("--hide-fps", "--hide-touch-controls"), False, False),
    )
    with tempfile.TemporaryDirectory(prefix="nesd-client-visibility-") as temporary:
        root = Path(temporary)
        directories = [root / name for name in ("roms", "saves", "system")]
        for directory in directories:
            directory.mkdir(mode=0o750)
        for options, expected_fps, expected_touch_controls in cases:
            port = free_port()
            process = subprocess.Popen(
                [
                    str(binary),
                    "--bind", "127.0.0.1",
                    "--port", str(port),
                    "--rom-dir", str(directories[0]),
                    "--save", str(directories[1]),
                    "--system", str(directories[2]),
                    "--auth-token", TOKEN,
                    "--raw-video",
                    *options,
                ],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            try:
                wait_ready(process, port)
                status, reply = json_request(port, "GET", "/api/status")
                assert status == 200, reply
                assert reply.get("show_fps") is expected_fps, (options, reply)
                assert (
                    reply.get("show_touch_controls")
                    is expected_touch_controls
                ), (options, reply)
                websocket = websocket_connect(port)
                try:
                    send_ws(websocket, 0x1, b'{"t":"hello"}')
                    websocket.sock.settimeout(3)
                    websocket_reply = None
                    for _ in range(6):
                        opcode, frame = receive_ws(websocket)
                        if opcode == 0x9:
                            send_ws(websocket, 0xA, frame)
                        elif opcode == 0x1:
                            candidate = json.loads(frame)
                            if candidate.get("t") == "status":
                                websocket_reply = candidate
                                break
                    assert websocket_reply is not None, options
                    assert websocket_reply.get("show_fps") is expected_fps, (
                        options,
                        websocket_reply,
                    )
                    assert (
                        websocket_reply.get("show_touch_controls")
                        is expected_touch_controls
                    ), (options, websocket_reply)
                finally:
                    websocket.close()
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    try:
                        process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
            assert process.returncode == 0, (options, process.returncode)


def run_rpc_cli(
    binary: Path,
    port: int,
    token_file: Path,
    path: str,
    method: str,
    body: dict | None = None,
) -> subprocess.CompletedProcess[bytes]:
    command = [
        str(binary),
        "--rpc-client",
        "--rpc-host", "127.0.0.1",
        "--rpc-port", str(port),
        "--rpc-path", path,
        "--rpc-method", method,
        "--rpc-token-file", str(token_file),
        "--rpc-timeout-ms", "3000",
    ]
    if body is not None:
        command.extend(("--rpc-body", json.dumps(body)))
    return subprocess.run(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=5,
        check=False,
    )


def websocket_upgrade_request(port: int) -> bytes:
    key = base64.b64encode(os.urandom(16)).decode()
    return (
        f"GET /ws?token={TOKEN} HTTP/1.1\r\n"
        f"Host: 127.0.0.1:{port}\r\n"
        "Connection: Upgrade\r\n"
        "Upgrade: websocket\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        f"Origin: http://127.0.0.1:{port}\r\n"
        "\r\n"
    ).encode()


def websocket_connect(
    port: int, *, receive_buffer: int | None = None
) -> BufferedSocket:
    request_bytes = websocket_upgrade_request(port)
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    if receive_buffer is not None:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, receive_buffer)
    sock.settimeout(3)
    sock.connect(("127.0.0.1", port))
    sock.settimeout(5)
    sock.sendall(request_bytes)
    response = b""
    while b"\r\n\r\n" not in response:
        response += sock.recv(4096)
    head, separator, pending = response.partition(b"\r\n\r\n")
    try:
        assert separator and head.startswith(b"HTTP/1.1 101 "), head[:200]
    except Exception:
        sock.close()
        raise
    return BufferedSocket(sock, pending)


def send_ws(
    sock: BufferedSocket, opcode: int, payload: bytes, *, final: bool = True
) -> None:
    first = opcode | (0x80 if final else 0)
    mask = os.urandom(4)
    length = len(payload)
    if length < 126:
        header = bytes((first, 0x80 | length))
    elif length <= 0xFFFF:
        header = bytes((first, 0xFE)) + struct.pack(">H", length)
    else:
        header = bytes((first, 0xFF)) + struct.pack(">Q", length)
    masked = bytes(value ^ mask[index % 4] for index, value in enumerate(payload))
    sock.sendall(header + mask + masked)


def receive_ws(sock: BufferedSocket) -> tuple[int, bytes]:
    head = recv_exact(sock, 2)
    opcode = head[0] & 0x0F
    assert head[0] & 0x80, "server fragment was unexpected"
    assert not head[1] & 0x80, "server frames must not be masked"
    length = head[1] & 0x7F
    if length == 126:
        length = struct.unpack(">H", recv_exact(sock, 2))[0]
    elif length == 127:
        length = struct.unpack(">Q", recv_exact(sock, 8))[0]
    return opcode, recv_exact(sock, length)


def receive_raw_after_event(
    sock: BufferedSocket, expected_event: str, timeout: float = 5.0
) -> bytes:
    """Return the first raw frame sent after a named status event."""
    deadline = time.monotonic() + timeout
    event_seen = False
    while time.monotonic() < deadline:
        sock.sock.settimeout(max(0.1, deadline - time.monotonic()))
        opcode, frame = receive_ws(sock)
        if opcode == 0x9:
            send_ws(sock, 0xA, frame)
        elif opcode == 0x1:
            message = json.loads(frame)
            if message.get("t") == "status" and message.get("event") == expected_event:
                event_seen = True
        elif opcode == 0x2 and event_seen and frame and frame[0] == 1:
            width, height = struct.unpack_from("<HH", frame, 2)
            assert len(frame) == 12 + width * height * 2
            return frame
        elif opcode == 0x8:
            raise AssertionError(f"unexpected WebSocket close: {frame!r}")
    raise AssertionError(f"no raw frame followed WebSocket event {expected_event!r}")


def expect_ws_close(sock: BufferedSocket, code: int) -> None:
    while True:
        opcode, frame = receive_ws(sock)
        if opcode == 0x9:
            send_ws(sock, 0xA, frame)
        elif opcode == 0x8:
            assert len(frame) >= 2, frame
            assert struct.unpack_from(">H", frame)[0] == code, frame
            return


def recv_exact(sock: BufferedSocket, length: int) -> bytes:
    chunks = bytearray()
    while len(chunks) < length:
        chunk = sock.recv(length - len(chunks))
        if not chunk:
            raise AssertionError("unexpected WebSocket EOF")
        chunks.extend(chunk)
    return bytes(chunks)


def test_daemon(binary: Path) -> None:
    rejected_fps = subprocess.run(
        [str(binary), "--stream-fps", "61"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        timeout=5,
        check=False,
    )
    assert rejected_fps.returncode == 2, rejected_fps.stderr
    assert b"invalid value for option" in rejected_fps.stderr

    rom = make_test_rom()
    port = free_port()
    same_origin = f"http://127.0.0.1:{port}"
    allowed_origin = "https://client.example"
    with tempfile.TemporaryDirectory(prefix="nesd-integration-") as temporary:
        root = Path(temporary)
        rom_dir = root / "roms"
        save_dir = root / "saves"
        system_dir = root / "system"
        for directory in (rom_dir, save_dir, system_dir):
            directory.mkdir(mode=0o750)
        primary = rom_dir / "battery.nes"
        primary.write_bytes(rom)
        outside_rom = root / "outside-root.nes"
        outside_rom.write_bytes(rom)
        outside_link = rom_dir / "outside-link.nes"
        outside_link.symlink_to(outside_rom)
        quoted = rom_dir / 'quote"name.nes'
        quoted.write_bytes(rom)
        unicode_rom = rom_dir / "игра.nes"
        unicode_rom.write_bytes(rom)
        unreadable_rom: Path | None = None
        if os.geteuid() != 0:
            unreadable_rom = rom_dir / "root-only.nes"
            unreadable_rom.write_bytes(rom)
            unreadable_rom.chmod(0)
        abandoned_upload = rom_dir / ".nes-upload-2147483646-abc-0.tmp"
        abandoned_upload.write_bytes(b"partial upload")
        unrelated_hidden = rom_dir / ".keep-this-file"
        unrelated_hidden.write_bytes(b"keep")
        upload_symlink = rom_dir / ".nes-upload-2147483646-def-1.tmp"
        upload_symlink.symlink_to(unrelated_hidden.name)
        hardlink_target = rom_dir / ".keep-this-hardlink"
        hardlink_target.write_bytes(b"keep-hardlink")
        upload_hardlink = rom_dir / ".nes-upload-2147483646-feed-2.tmp"
        os.link(hardlink_target, upload_hardlink)
        deep_directory = rom_dir
        for index in range(6):
            deep_directory /= f"{index}-" + "d" * 78
            deep_directory.mkdir()
        deep_rom = deep_directory / "deep.nes"
        deep_rom.write_bytes(rom)
        assert len(str(deep_rom).encode()) > 512
        log_path = root / "nesd.log"
        with log_path.open("wb") as log:
            daemon_arguments = [
                str(binary),
                "--bind", "127.0.0.1",
                "--port", str(port),
                "--rom-dir", str(rom_dir),
                "--save", str(save_dir),
                "--system", str(system_dir),
                "--auth-token", TOKEN,
                "--allowed-origin", allowed_origin,
                "--raw-video",
                "--stream-fps", "60",
            ]
            # exec preserves the wrapper PID, reproducing PID reuse after a
            # reboot before nesd has had a chance to accept a new upload.
            process = subprocess.Popen(
                [
                    "/bin/sh",
                    "-c",
                    ': > "$1/.nes-upload-$$-cafe-0.tmp"; '
                    'shift; exec "$@"',
                    "nesd-integration-wrapper",
                    str(rom_dir),
                    *daemon_arguments,
                ],
                stdout=log,
                stderr=subprocess.STDOUT,
            )
            same_pid_abandoned_upload = (
                rom_dir / f".nes-upload-{process.pid}-cafe-0.tmp"
            )
            try:
                wait_ready(process, port)

                status, idle = json_request(port, "GET", "/api/status")
                assert status == 200, idle
                assert idle.get("running") is False, idle
                assert idle.get("game_loaded") is False, idle
                assert idle.get("core_loaded") is False, idle
                assert idle.get("demo") is False, idle
                assert idle.get("viewers") == 0, idle
                assert idle.get("stream_fps") == 60, idle
                assert idle.get("show_fps") is True, idle
                assert idle.get("show_touch_controls") is True, idle

                status, empty_states = json_request(port, "GET", "/api/states")
                assert status == 200, empty_states
                assert empty_states.get("game_loaded") is False, empty_states
                assert len(empty_states.get("slots", [])) == 10, empty_states
                assert all(
                    not item.get("exists") for item in empty_states["slots"]
                ), empty_states
                for state_action in ("save", "load", "delete"):
                    status, state_error = json_request(
                        port,
                        "POST",
                        f"/api/state/{state_action}",
                        b'{"slot":1}',
                    )
                    assert status == 409 and state_error.get("error"), (
                        state_action,
                        state_error,
                    )
                for invalid_state_body in (
                    b"{}",
                    b'{"slot":0}',
                    b'{"slot":11}',
                    b'{"slot":"1"}',
                    b'{"slot":1,"slot":2}',
                ):
                    status, _headers, rejected_body = request(
                        port, "POST", "/api/state/save", invalid_state_body
                    )
                    assert status == 400, (invalid_state_body, rejected_body)
                assert not abandoned_upload.exists()
                assert not same_pid_abandoned_upload.exists()
                assert unrelated_hidden.read_bytes() == b"keep"
                assert upload_symlink.is_symlink()
                assert upload_hardlink.read_bytes() == b"keep-hardlink"
                assert hardlink_target.read_bytes() == b"keep-hardlink"
                upload_symlink.unlink()
                upload_hardlink.unlink()

                status, refused_start = json_request(
                    port, "POST", "/api/start", b"{}"
                )
                assert status == 409 and refused_start.get("error"), refused_start

                idle_websocket = websocket_connect(port)
                try:
                    status, _headers, second_body = raw_http(
                        port, websocket_upgrade_request(port)
                    )
                    assert status == 503, (status, second_body)

                    send_ws(idle_websocket, 0x1, b'{"t":"hello"}')
                    idle_websocket.sock.settimeout(0.2)
                    media_received = False
                    deadline = time.monotonic() + 0.8
                    while time.monotonic() < deadline:
                        try:
                            opcode, frame = receive_ws(idle_websocket)
                        except socket.timeout:
                            continue
                        if opcode == 0x9:
                            send_ws(idle_websocket, 0xA, frame)
                        elif opcode == 0x2:
                            media_received = True
                            break
                    assert not media_received, "no-ROM WebSocket emitted A/V"
                finally:
                    idle_websocket.close()

                status, _headers, rejected_body = request(
                    port,
                    "POST",
                    "/api/upload?name=unauthorized.nes",
                    rom,
                    authorized=False,
                    extra_headers={"Content-Type": "application/octet-stream"},
                    split=1024,
                )
                assert status == 401, status
                assert json.loads(rejected_body).get("error")

                status, _headers, rejected_body = request(
                    port,
                    "POST",
                    "/api/upload?name=one.nes&name=two.nes",
                    rom,
                    extra_headers={"Content-Type": "application/octet-stream"},
                    split=1024,
                )
                assert status == 400, status
                assert json.loads(rejected_body).get("error")

                oversized_header = (
                    f"POST /api/upload?name=too-large.nes HTTP/1.1\r\n"
                    f"Host: 127.0.0.1:{port}\r\n"
                    f"Authorization: Bearer {TOKEN}\r\n"
                    "Content-Type: application/octet-stream\r\n"
                    f"Content-Length: {MAX_UPLOAD_BYTES + 1}\r\n"
                    "Connection: close\r\n\r\n"
                ).encode()
                status, _headers, rejected_body = raw_http(
                    port, oversized_header + b"x" * 65536, split=4096
                )
                assert status == 413, status
                assert json.loads(rejected_body).get("error")

                status, _headers, rejected_body = request(
                    port,
                    "POST",
                    "/api/load",
                    b"x" * (64 * 1024 + 1),
                    split=4096,
                )
                assert status == 413, status
                assert json.loads(rejected_body).get("error")

                status, _headers, _body = request(
                    port, "GET", "/api/status", authorized=False
                )
                assert status == 401, status

                status, headers, _body = request(
                    port,
                    "GET",
                    "/api/status",
                    authorized=False,
                    origin=allowed_origin,
                )
                assert status == 401, status
                assert headers.get("access-control-allow-origin") == allowed_origin

                status, _headers, page = request(
                    port, "GET", "/play", authorized=False
                )
                assert status == 200
                assert TOKEN.encode() not in page
                assert b"nes-auth-token" in page

                status, _headers, _body = request(
                    port, "GET", "/api/status", origin="http://evil.invalid"
                )
                assert status == 403, status

                status, _headers, _body = request(
                    port,
                    "GET",
                    "/api/status",
                    origin=f"https://127.0.0.1:{port}",
                )
                assert status == 403, status

                status, _headers, _body = request(
                    port, "GET", "/api/status", origin=same_origin
                )
                assert status == 200, status

                status, headers, _body = request(
                    port,
                    "OPTIONS",
                    "/api/status",
                    authorized=False,
                    origin=allowed_origin,
                    extra_headers={
                        "Access-Control-Request-Method": "GET",
                        "Access-Control-Request-Headers": "Authorization",
                    },
                )
                assert status in (200, 204), status
                assert headers.get("access-control-allow-origin") == allowed_origin

                status, roms = json_request(port, "GET", "/api/roms")
                assert status == 200
                assert any(item.get("name") == 'quote"name.nes' for item in roms["roms"])
                assert any(item.get("name") == "игра.nes" for item in roms["roms"])
                assert any(item.get("path") == str(deep_rom) for item in roms["roms"])
                assert all(
                    item.get("path") not in (str(outside_rom), str(outside_link))
                    for item in roms["roms"]
                )
                assert all(
                    item.get("readable") is True
                    for item in roms["roms"]
                    if item.get("name") != "root-only.nes"
                )
                if unreadable_rom is not None:
                    locked = next(
                        item for item in roms["roms"]
                        if item.get("path") == str(unreadable_rom)
                    )
                    assert locked.get("readable") is False, locked
                    assert "mode 0640" in locked.get("error", ""), locked
                    status, locked_reply = json_request(
                        port,
                        "POST",
                        "/api/load",
                        json.dumps({"path": str(unreadable_rom)}).encode(),
                    )
                    assert status == 403, (status, locked_reply)
                    assert "mode 0640" in locked_reply.get("error", ""), locked_reply

                # The loopback RPC client deliberately forwards an absolute
                # path, so nesd must keep the final canonical-root boundary.
                # Reject both a valid ROM outside the configured root and a
                # symlink inside the root which resolves to that ROM.
                for forbidden_path in (outside_rom, outside_link):
                    status, rejected_load = json_request(
                        port,
                        "POST",
                        "/api/load",
                        json.dumps({"path": str(forbidden_path)}).encode(),
                    )
                    assert status == 400, (forbidden_path, rejected_load)
                    assert "not allowed" in rejected_load.get("error", ""), (
                        forbidden_path,
                        rejected_load,
                    )

                # ensure_ascii=True deliberately exercises \uXXXX decoding.
                payload = json.dumps({"path": str(unicode_rom)}).encode()
                status, loaded = json_request(
                    port, "POST", "/api/load", payload, split=1
                )
                assert status == 200 and loaded.get("ok") is True, loaded

                status, deep_loaded = json_request(
                    port,
                    "POST",
                    "/api/load",
                    json.dumps({"path": str(deep_rom)}).encode(),
                )
                assert status == 200 and deep_loaded.get("ok") is True, deep_loaded

                # FCEUmm stores the content path in 2048 bytes.  Build this
                # fixture after ROM enumeration so its deliberately excessive
                # nesting cannot trip the scanner's independent depth limit.
                too_long_directory = rom_dir
                index = 0
                while len(
                    str(too_long_directory / "too-long (Europe).nes").encode()
                ) < 2048:
                    too_long_directory /= f"long-{index:02d}-" + "p" * 192
                    too_long_directory.mkdir()
                    index += 1
                too_long_rom = too_long_directory / "too-long (Europe).nes"
                too_long_rom.write_bytes(rom)
                assert 2048 <= len(str(too_long_rom).encode()) < 4096
                try:
                    status, path_too_long = json_request(
                        port,
                        "POST",
                        "/api/load",
                        json.dumps({"path": str(too_long_rom)}).encode(),
                    )
                    assert status == 400, path_too_long
                    assert "too long" in path_too_long.get("error", ""), (
                        path_too_long
                    )
                    status, still_loaded = json_request(port, "GET", "/api/status")
                    assert still_loaded.get("rom_path") == str(deep_rom), still_loaded
                finally:
                    too_long_rom.unlink(missing_ok=True)
                    while too_long_directory != rom_dir:
                        parent = too_long_directory.parent
                        too_long_directory.rmdir()
                        too_long_directory = parent

                status, _headers, _body = request(
                    port,
                    "POST",
                    "/api/input",
                    b'{"mask":1}',
                )
                assert status == 405, status
                for malformed in (
                    b'{"path":"battery.nes","bad":}',
                    b'{"path":"battery.nes"}junk',
                    b'{"path":"battery.nes","path":"battery.nes"}',
                ):
                    status, _headers, _body = request(
                        port,
                        "POST",
                        "/api/load",
                        malformed,
                    )
                    assert status == 400, (status, malformed)
                status, _headers, _body = request(
                    port,
                    "POST",
                    "/api/load",
                    b'{"path":"broken"junk}',
                )
                assert status == 400, status

                status, current = json_request(port, "GET", "/api/status")
                assert status == 200
                assert 59.0 < float(current["fps"]) < 61.0, current
                assert current.get("running") is True, current
                assert current.get("core_loaded") is True, current
                assert current.get("game_loaded") is True, current
                assert current.get("stream_fps") == 60, current

                status, states = json_request(port, "GET", "/api/states")
                assert status == 200, states
                assert states.get("game_loaded") is True, states
                assert all(not item.get("exists") for item in states["slots"]), states

                state_label = "Перед замком"
                status, saved_state = json_request(
                    port,
                    "POST",
                    "/api/state/save",
                    json.dumps({"slot": 1, "label": state_label}).encode(),
                )
                assert status == 200 and saved_state.get("ok") is True, saved_state
                assert saved_state.get("durable") is True, saved_state

                state_directory = save_dir / "states"
                state_one = next(state_directory.glob("*slot-01.nss"))
                assert state_one.stat().st_mode & 0o777 == 0o600
                state_bytes = state_one.read_bytes()
                assert state_bytes.startswith(b"NESDST2\0")
                assert len(state_bytes) > 320
                assert struct.unpack_from("<I", state_bytes, 12)[0] == 320
                assert struct.unpack_from("<Q", state_bytes, 256)[0] <= 512 * 480 * 2

                status, states = json_request(port, "GET", "/api/states")
                slot_one = states["slots"][0]
                assert status == 200 and slot_one.get("loadable") is True, states
                assert slot_one.get("label") == state_label, slot_one
                assert slot_one.get("size") == len(state_bytes), slot_one

                status, _headers, rejected_body = request(
                    port,
                    "POST",
                    "/api/state/save",
                    json.dumps({"slot": 2, "label": "bad\nlabel"}).encode(),
                )
                assert status == 400, rejected_body

                status, paused = json_request(port, "POST", "/api/pause", b"{}")
                assert status == 200 and paused.get("ok") is True, paused
                status, loaded_state = json_request(
                    port, "POST", "/api/state/load", b'{"slot":1}'
                )
                assert status == 200 and loaded_state.get("ok") is True, loaded_state
                status, after_state_load = json_request(port, "GET", "/api/status")
                assert after_state_load.get("running") is True, after_state_load
                assert after_state_load.get("paused") is True, after_state_load
                status, resumed = json_request(port, "POST", "/api/pause", b"{}")
                assert status == 200 and resumed.get("ok") is True, resumed

                status, saved_three = json_request(
                    port, "POST", "/api/state/save", b'{"slot":3}'
                )
                assert status == 200 and saved_three.get("ok") is True, saved_three
                state_three = next(state_directory.glob("*slot-03.nss"))
                corrupt = bytearray(state_three.read_bytes())
                corrupt[-1] ^= 0x80
                state_three.write_bytes(corrupt)
                state_three.chmod(0o600)
                status, states = json_request(port, "GET", "/api/states")
                assert status == 200, states
                assert states["slots"][2].get("exists") is True, states
                # Listing reads only the bounded header so it cannot stall the
                # emulation thread by hashing up to 16 MiB of state payloads.
                assert states["slots"][2].get("loadable") is True, states
                status, corrupt_load = json_request(
                    port, "POST", "/api/state/load", b'{"slot":3}'
                )
                assert status == 422 and corrupt_load.get("error"), corrupt_load
                corrupt[304] = 1
                state_three.write_bytes(corrupt)
                state_three.chmod(0o600)
                status, states = json_request(port, "GET", "/api/states")
                assert status == 200, states
                assert states["slots"][2].get("loadable") is False, states
                assert states["slots"][2].get("error"), states
                status, deleted = json_request(
                    port, "POST", "/api/state/delete", b'{"slot":3}'
                )
                assert status == 200 and deleted.get("ok") is True, deleted
                status, missing_delete = json_request(
                    port, "POST", "/api/state/delete", b'{"slot":3}'
                )
                assert status == 404 and missing_delete.get("error"), missing_delete

                status, overwritten = json_request(
                    port,
                    "POST",
                    "/api/state/save",
                    json.dumps({"slot": 1, "label": "Overwritten"}).encode(),
                )
                assert status == 200 and overwritten.get("ok") is True, overwritten
                assert len(list(state_directory.glob("*slot-01.nss"))) == 1
                assert state_one.stat().st_mode & 0o777 == 0o600

                websocket = websocket_connect(port)
                try:
                    status, _headers, second_body = raw_http(
                        port, websocket_upgrade_request(port)
                    )
                    assert status == 503, (status, second_body)

                    # The application heartbeat renews the complete input
                    # state and is acknowledged even while media is flowing.
                    # It has a separate bounded slot, so it cannot overwrite
                    # an application state event queued behind active video.
                    reset_status, reset_reply = json_request(
                        port, "POST", "/api/reset", b"{}"
                    )
                    assert reset_status == 200 and reset_reply.get("ok") is True
                    send_ws(
                        websocket,
                        0x1,
                        b'{"t":"heartbeat","mask":1,"seq":1}',
                    )
                    send_ws(
                        websocket,
                        0x1,
                        b'{"t":"heartbeat","mask":0,"seq":2}',
                    )
                    heartbeat_sequences = []
                    latest_heartbeat_seen = False
                    reset_event_seen = False
                    heartbeat_deadline = time.monotonic() + 2.0
                    while (
                        time.monotonic() < heartbeat_deadline
                        and (not latest_heartbeat_seen or not reset_event_seen)
                    ):
                        opcode, frame = receive_ws(websocket)
                        if opcode == 0x9:
                            send_ws(websocket, 0xA, frame)
                        elif opcode == 0x1:
                            candidate = json.loads(frame)
                            if candidate.get("t") == "heartbeat":
                                heartbeat_sequences.append(candidate.get("seq"))
                                if candidate == {"t": "heartbeat", "seq": 2}:
                                    latest_heartbeat_seen = True
                            elif (
                                candidate.get("t") == "status"
                                and candidate.get("event") == "reset"
                            ):
                                reset_event_seen = True
                        elif opcode == 0x8:
                            raise AssertionError(
                                f"unexpected WebSocket close: {frame!r}"
                            )
                    assert latest_heartbeat_seen, heartbeat_sequences
                    assert heartbeat_sequences[-1] == 2, heartbeat_sequences
                    assert reset_event_seen, "heartbeat overwrote the reset event"

                    # A word such as "mask" in an unrelated JSON string must
                    # not be mistaken for a legacy input command.
                    send_ws(
                        websocket,
                        0x1,
                        b'{"t":"noop","note":"mask"}',
                    )
                    send_ws(websocket, 0x1, b'{"t":"inp', final=False)
                    send_ws(websocket, 0x0, b'ut","mask":1}', final=True)
                    raw_frame = None
                    deadline = time.monotonic() + 8
                    while time.monotonic() < deadline:
                        opcode, frame = receive_ws(websocket)
                        if opcode == 0x9:
                            send_ws(websocket, 0xA, frame)
                        elif opcode == 0x2 and frame and frame[0] == 1:
                            raw_frame = frame
                            break
                        elif opcode == 0x8:
                            raise AssertionError(f"unexpected WebSocket close: {frame!r}")
                    assert raw_frame is not None, "no raw video frame received"
                    assert len(raw_frame) > 65535
                    width, height = struct.unpack_from("<HH", raw_frame, 2)
                    assert len(raw_frame) == 12 + width * height * 2
                    assert width > 0 and height > 0

                    first_frame_id = struct.unpack_from("<I", raw_frame, 6)[0]
                    last_frame_id = first_frame_id
                    video_packets = 0
                    video_bytes = 0
                    video_arrivals = []
                    received_audio_frames = 0
                    received_audio_packets = 0
                    pacing_started = time.monotonic()
                    while time.monotonic() - pacing_started < 2.0:
                        opcode, frame = receive_ws(websocket)
                        if opcode == 0x9:
                            send_ws(websocket, 0xA, frame)
                        elif opcode == 0x2 and frame and frame[0] == 1:
                            last_frame_id = struct.unpack_from("<I", frame, 6)[0]
                            video_packets += 1
                            video_bytes += len(frame)
                            video_arrivals.append(time.monotonic())
                        elif opcode == 0x2 and len(frame) >= 12 and frame[0] == 2:
                            channels = frame[1]
                            packet_frames = struct.unpack_from("<I", frame, 8)[0]
                            assert channels in (1, 2)
                            assert len(frame) == 12 + packet_frames * channels * 2
                            received_audio_frames += packet_frames
                            received_audio_packets += 1
                        elif opcode == 0x8:
                            raise AssertionError(
                                f"unexpected WebSocket close: {frame!r}"
                            )
                    pacing_elapsed = time.monotonic() - pacing_started
                    emulated_frames = (
                        last_frame_id - first_frame_id
                    ) & 0xFFFFFFFF
                    measured_fps = emulated_frames / pacing_elapsed
                    assert 45.0 < measured_fps < 75.0, (
                        "emulation pacing ignored native FPS: "
                        f"{measured_fps:.1f}"
                    )
                    assert video_packets >= int(pacing_elapsed * 40), (
                        "60 FPS stream did not rise above the former 30 FPS limit: "
                        f"{video_packets} packets in {pacing_elapsed:.2f}s"
                    )
                    assert video_packets <= int(
                        pacing_elapsed * current["stream_fps"]
                    ) + 2, (
                        "stream FPS limiter allowed excessive video traffic: "
                        f"{video_packets} packets/{video_bytes} bytes in "
                        f"{pacing_elapsed:.2f}s"
                    )
                    video_intervals = [
                        later - earlier
                        for earlier, later in zip(
                            video_arrivals, video_arrivals[1:]
                        )
                    ]
                    short_video_intervals = sum(
                        interval < 0.013 for interval in video_intervals
                    )
                    assert len(video_intervals) >= 60, video_intervals
                    assert short_video_intervals <= len(video_intervals) // 4, (
                        "60 FPS stream fell back to a bursty 10/20 ms cadence: "
                        f"{short_video_intervals}/{len(video_intervals)} "
                        "intervals were shorter than 13 ms"
                    )
                    assert received_audio_frames > 0.94 * 48000 * pacing_elapsed, (
                        "audio was drained but dropped behind video: "
                        f"{received_audio_frames} frames in "
                        f"{pacing_elapsed:.2f}s"
                    )
                    assert received_audio_packets <= int(30 * pacing_elapsed) + 2, (
                        "PCM packet cadence regressed into browser-GC churn: "
                        f"{received_audio_packets} packets in {pacing_elapsed:.2f}s"
                    )

                    # A client which temporarily stops reading must not make
                    # the single-threaded HTTP control plane or emulation loop
                    # wait behind its TCP send buffer.
                    websocket.sock.setsockopt(
                        socket.SOL_SOCKET, socket.SO_RCVBUF, 4096
                    )
                    slow_http = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                    try:
                        slow_http.settimeout(2)
                        slow_http.setsockopt(
                            socket.SOL_SOCKET, socket.SO_RCVBUF, 4096
                        )
                        slow_http.connect(("127.0.0.1", port))
                        slow_http.sendall(
                            (
                                f"GET /play HTTP/1.1\r\n"
                                f"Host: 127.0.0.1:{port}\r\n"
                                "Connection: close\r\n\r\n"
                            ).encode()
                        )
                        time.sleep(0.35)
                        control_started = time.monotonic()
                        for _ in range(6):
                            control_status, control_reply = json_request(
                                port, "GET", "/api/status"
                            )
                            assert (
                                control_status == 200 and "running" in control_reply
                            )
                        control_elapsed = time.monotonic() - control_started
                        assert control_elapsed < 1.5, (
                            "slow HTTP/WebSocket output stalled the control plane: "
                            f"{control_elapsed:.2f}s"
                        )
                    finally:
                        slow_http.close()
                finally:
                    websocket.close()

                time.sleep(0.05)
                websocket = websocket_connect(port)
                try:
                    send_ws(
                        websocket,
                        0x1,
                        b'{"t":"input","id":0,"pressed":truejunk}',
                    )
                    expect_ws_close(websocket, 1007)
                finally:
                    websocket.close()

                websocket = websocket_connect(port)
                try:
                    send_ws(
                        websocket,
                        0x1,
                        b'{"t":"heartbeat","mask":65536,"seq":1}',
                    )
                    expect_ws_close(websocket, 1007)
                finally:
                    websocket.close()

                websocket = websocket_connect(port)
                try:
                    send_ws(websocket, 0x1, b'{"t":"heartbeat","mask":0}')
                    expect_ws_close(websocket, 1007)
                finally:
                    websocket.close()

                websocket = websocket_connect(port)
                try:
                    send_ws(
                        websocket,
                        0x1,
                        b'{"t":"heartbeat","mask":0,"seq":9007199254740992}',
                    )
                    expect_ws_close(websocket, 1007)
                finally:
                    websocket.close()

                websocket = websocket_connect(port)
                try:
                    send_ws(
                        websocket,
                        0x1,
                        b'{"t":"heartbeat","mask":0,"seq":2}',
                    )
                    heartbeat_deadline = time.monotonic() + 2.0
                    while True:
                        assert time.monotonic() < heartbeat_deadline
                        opcode, frame = receive_ws(websocket)
                        if opcode == 0x9:
                            send_ws(websocket, 0xA, frame)
                        elif opcode == 0x1 and json.loads(frame) == {
                            "t": "heartbeat",
                            "seq": 2,
                        }:
                            break
                    send_ws(
                        websocket,
                        0x1,
                        b'{"t":"heartbeat","mask":0,"seq":2}',
                    )
                    expect_ws_close(websocket, 1007)
                finally:
                    websocket.close()

                websocket = websocket_connect(port)
                try:
                    send_ws(
                        websocket,
                        0x1,
                        b'{"t":"hello","t":"input","mask":1}',
                    )
                    expect_ws_close(websocket, 1007)
                finally:
                    websocket.close()

                original = primary.read_bytes()
                partial_header = (
                    f"POST /api/upload?name=battery.nes HTTP/1.1\r\n"
                    f"Host: 127.0.0.1:{port}\r\n"
                    f"Authorization: Bearer {TOKEN}\r\n"
                    "Content-Type: application/octet-stream\r\n"
                    f"Content-Length: {len(rom)}\r\n\r\n"
                ).encode()
                with socket.create_connection(("127.0.0.1", port), timeout=3) as sock:
                    sock.sendall(partial_header + rom[:100])
                    time.sleep(0.1)
                    status, _headers, _body = request(
                        port,
                        "POST",
                        "/api/upload?name=concurrent.nes",
                        rom,
                        extra_headers={
                            "Content-Type": "application/octet-stream"
                        },
                    )
                    assert status == 409, status
                time.sleep(0.2)
                assert primary.read_bytes() == original
                assert not list(rom_dir.glob(".nes-upload-*.tmp"))

                status, _headers, uploaded_body = request(
                    port,
                    "POST",
                    "/api/upload?name=uploaded.nes",
                    rom,
                    extra_headers={"Content-Type": "application/octet-stream"},
                )
                uploaded = json.loads(uploaded_body)
                assert status == 200 and uploaded.get("ok") is True, uploaded
                assert (rom_dir / "uploaded.nes").read_bytes() == rom

                slow_clients = []
                try:
                    for _ in range(8):
                        client = socket.create_connection(("127.0.0.1", port), timeout=2)
                        client.sendall(b"GET /api/status HTTP/1.1\r\nHost: x\r\n")
                        slow_clients.append(client)
                    time.sleep(0.1)
                    with socket.create_connection(("127.0.0.1", port), timeout=2) as extra:
                        extra.settimeout(2)
                        busy = receive_all(extra)
                    assert b" 503 " in busy

                    # Header timeout and the bounded error-drain phase take
                    # 5 s + 2 s.  Poll instead of sleeping for only the first
                    # phase: while every slot is still occupied a best-effort
                    # busy response may be observed as either HTTP 503 or a
                    # TCP reset when Linux closes with the complete GET still
                    # unread.  Capacity must nevertheless recover promptly.
                    recovery_started = time.monotonic()
                    recovery_deadline = recovery_started + 8.0
                    recovery_last = "no recovery attempt"
                    recovered = False
                    while time.monotonic() < recovery_deadline:
                        try:
                            recovery_status, _headers, recovery_body = request(
                                port, "GET", "/api/status"
                            )
                        except OSError as error:
                            recovery_last = f"{type(error).__name__}: {error}"
                        else:
                            if recovery_status == 200:
                                recovery_reply = json.loads(recovery_body)
                                assert "running" in recovery_reply
                                recovered = True
                                break
                            assert recovery_status == 503, recovery_status
                            recovery_last = f"HTTP {recovery_status}"
                        time.sleep(0.05)
                    recovery_elapsed = time.monotonic() - recovery_started
                    assert recovered, (
                        "HTTP client capacity did not recover within the "
                        "8-second header/error-drain bound; last result: "
                        f"{recovery_last}"
                    )
                    assert recovery_elapsed < 8.0, recovery_elapsed
                finally:
                    for client in slow_clients:
                        client.close()

                status, stopped = json_request(port, "POST", "/api/stop", b"{}")
                assert status == 200 and stopped.get("ok") is True
                saves = list(save_dir.glob("*.srm"))
                assert saves, "SRAM was not flushed"
                assert any(path.read_bytes()[:1] == b"\x42" for path in saves)
            except Exception:
                process.send_signal(signal.SIGTERM)
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
                log.flush()
                print(log_path.read_text(encoding="utf-8", errors="replace"))
                raise
            else:
                process.send_signal(signal.SIGTERM)
                process.wait(timeout=10)
                assert process.returncode == 0, process.returncode


def test_savestate_roundtrip(binary: Path) -> None:
    """A full state restores changing core RAM and survives a restart."""
    port = free_port()
    with tempfile.TemporaryDirectory(prefix="nesd-savestate-") as temporary:
        root = Path(temporary)
        rom_dir = root / "roms"
        save_dir = root / "saves"
        system_dir = root / "system"
        for directory in (rom_dir, save_dir, system_dir):
            directory.mkdir(mode=0o750)
        original_rom = rom_dir / "counter.nes"
        original_rom.write_bytes(make_counter_rom())
        log_path = root / "nesd.log"

        def start_daemon(active_port: int) -> subprocess.Popen:
            return subprocess.Popen(
                [
                    str(binary),
                    "--bind", "127.0.0.1",
                    "--port", str(active_port),
                    "--rom-dir", str(rom_dir),
                    "--save", str(save_dir),
                    "--system", str(system_dir),
                    "--auth-token", TOKEN,
                    "--raw-video",
                    "--stream-fps", "60",
                ],
                stdout=log_handle,
                stderr=subprocess.STDOUT,
            )

        with log_path.open("wb") as log_handle:
            process = start_daemon(port)
            websocket: BufferedSocket | None = None
            try:
                wait_ready(process, port)
                status, loaded = json_request(
                    port,
                    "POST",
                    "/api/load",
                    json.dumps({"path": str(original_rom)}).encode(),
                )
                assert status == 200 and loaded.get("ok") is True, loaded
                websocket = websocket_connect(port)
                time.sleep(0.12)
                status, paused = json_request(port, "POST", "/api/pause", b"{}")
                assert status == 200 and paused.get("ok") is True, paused

                status, saved = json_request(
                    port,
                    "POST",
                    "/api/state/save",
                    json.dumps({"slot": 1, "label": "Counter checkpoint"}).encode(),
                )
                assert status == 200 and saved.get("durable") is True, saved
                state_directory = save_dir / "states"
                state_one = next(state_directory.glob("*slot-01.nss"))
                saved_payload, saved_frame = parse_state_file(state_one)
                assert saved_frame, "save state did not include the paused display frame"
                saved_volatile_ram = fcs_field(saved_payload, b"RAM\0")
                sram_file = next(save_dir.glob("*.srm"))
                saved_sram = sram_file.read_bytes()
                assert saved_sram, "counter ROM exposed no battery RAM"

                changed_sram = saved_sram
                for _ in range(4):
                    status, resumed = json_request(
                        port, "POST", "/api/pause", b"{}"
                    )
                    assert status == 200 and resumed.get("ok") is True, resumed
                    time.sleep(0.08)
                    status, paused = json_request(
                        port, "POST", "/api/pause", b"{}"
                    )
                    assert status == 200 and paused.get("ok") is True, paused
                    changed_sram = sram_file.read_bytes()
                    if changed_sram != saved_sram:
                        break
                assert changed_sram != saved_sram, "test ROM state did not advance"

                status, advanced_state = json_request(
                    port, "POST", "/api/state/save", b'{"slot":2,"label":"Advanced"}'
                )
                assert status == 200 and advanced_state.get("ok") is True, advanced_state
                state_two = next(state_directory.glob("*slot-02.nss"))
                advanced_payload, advanced_frame = parse_state_file(state_two)
                assert fcs_field(advanced_payload, b"RAM\0") != saved_volatile_ram, (
                    "volatile CPU RAM did not advance between save states"
                )
                assert advanced_frame != saved_frame, (
                    "test ROM did not produce distinguishable paused frames"
                )

                status, restored = json_request(
                    port, "POST", "/api/state/load", b'{"slot":1}'
                )
                assert status == 200 and restored.get("ok") is True, restored
                restored_frame = receive_raw_after_event(websocket, "state-loaded")
                assert restored_frame[12:] == saved_frame, (
                    "paused save-state load did not restore its display frame"
                )
                assert sram_file.read_bytes() == saved_sram, (
                    "loading the full state did not restore battery RAM"
                )
                status, current = json_request(port, "GET", "/api/status")
                assert current.get("running") is True, current
                assert current.get("paused") is True, current
                status, resaved = json_request(
                    port, "POST", "/api/state/save", b'{"slot":3,"label":"Restored"}'
                )
                assert status == 200 and resaved.get("ok") is True, resaved
                state_three = next(state_directory.glob("*slot-03.nss"))
                restored_payload, restored_saved_frame = parse_state_file(state_three)
                assert fcs_field(restored_payload, b"RAM\0") == saved_volatile_ram, (
                    "loading did not restore volatile CPU RAM"
                )
                assert restored_saved_frame == saved_frame
            except Exception:
                process.send_signal(signal.SIGTERM)
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
                log_handle.flush()
                print(log_path.read_text(encoding="utf-8", errors="replace"))
                raise
            else:
                if websocket is not None:
                    websocket.close()
                process.send_signal(signal.SIGTERM)
                process.wait(timeout=10)
                assert process.returncode == 0, process.returncode

            renamed_rom = rom_dir / "renamed-counter.nes"
            original_rom.rename(renamed_rom)
            port = free_port()
            process = start_daemon(port)
            try:
                wait_ready(process, port)
                status, loaded = json_request(
                    port,
                    "POST",
                    "/api/load",
                    json.dumps({"path": str(renamed_rom)}).encode(),
                )
                assert status == 200 and loaded.get("ok") is True, loaded
                status, states = json_request(port, "GET", "/api/states")
                assert status == 200, states
                assert states["slots"][0].get("loadable") is True, states
                assert states["slots"][0].get("label") == "Counter checkpoint"
                restart_sram_before = set(save_dir.glob("*.srm"))
                status, restarted_restore = json_request(
                    port, "POST", "/api/state/load", b'{"slot":1}'
                )
                assert status == 200 and restarted_restore.get("ok") is True, (
                    restarted_restore
                )
                restart_sram_after = set(save_dir.glob("*.srm"))
                restored_sram_files = restart_sram_after - restart_sram_before
                assert len(restored_sram_files) == 1
                assert restored_sram_files.pop().read_bytes() == saved_sram, (
                    "save state did not remain loadable after daemon restart"
                )

                european_rom = rom_dir / "renamed-counter (Europe).nes"
                renamed_rom.rename(european_rom)
                status, loaded = json_request(
                    port,
                    "POST",
                    "/api/load",
                    json.dumps({"path": str(european_rom)}).encode(),
                )
                assert status == 200 and loaded.get("ok") is True, loaded
                status, european_status = json_request(port, "GET", "/api/status")
                assert 49.0 < float(european_status["fps"]) < 51.0, european_status
                status, states = json_request(port, "GET", "/api/states")
                assert status == 200, states
                assert all(not item.get("exists") for item in states["slots"]), states

                different_rom = rom_dir / "different.nes"
                different = bytearray(make_counter_rom())
                different[-1] ^= 1
                different_rom.write_bytes(different)
                status, loaded = json_request(
                    port,
                    "POST",
                    "/api/load",
                    json.dumps({"path": str(different_rom)}).encode(),
                )
                assert status == 200 and loaded.get("ok") is True, loaded
                status, states = json_request(port, "GET", "/api/states")
                assert status == 200, states
                assert all(not item.get("exists") for item in states["slots"]), states
            finally:
                process.send_signal(signal.SIGTERM)
                process.wait(timeout=10)
                assert process.returncode == 0, process.returncode


def test_explicit_demo_mode(binary: Path) -> None:
    port = free_port()
    with tempfile.TemporaryDirectory(prefix="nesd-demo-") as temporary:
        root = Path(temporary)
        directories = [root / name for name in ("roms", "saves", "system")]
        for directory in directories:
            directory.mkdir(mode=0o750)
        process = subprocess.Popen(
            [
                str(binary),
                "--bind", "127.0.0.1",
                "--port", str(port),
                "--rom-dir", str(directories[0]),
                "--save", str(directories[1]),
                "--system", str(directories[2]),
                "--auth-token", TOKEN,
                "--allow-demo",
                "--raw-video",
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        try:
            wait_ready(process, port)
            status, demo = json_request(port, "GET", "/api/status")
            assert status == 200, demo
            assert demo.get("running") is True, demo
            assert demo.get("game_loaded") is False, demo
            assert demo.get("demo") is True, demo
        finally:
            process.send_signal(signal.SIGTERM)
            process.wait(timeout=10)
            assert process.returncode == 0, process.returncode


def test_startup_diagnostics_and_small_stack(binary: Path) -> None:
    """Startup failures stay diagnosable and do not require a large main stack."""
    with tempfile.TemporaryDirectory(prefix="nesd-startup-") as temporary:
        root = Path(temporary)
        directories = [root / name for name in ("roms", "saves", "system")]
        for directory in directories:
            directory.mkdir(mode=0o750)
        rom_path = directories[0] / "small-stack.nes"
        rom_path.write_bytes(make_test_rom())

        base_arguments = [
            str(binary),
            "--bind", "127.0.0.1",
            "--rom-dir", str(directories[0]),
            "--save", str(directories[1]),
            "--system", str(directories[2]),
            "--auth-token", TOKEN,
            "--raw-video",
        ]

        # A port collision is a common router failure mode (notably with local
        # proxy controllers). It must have a stable exit status for rpcd.
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen(1)
            occupied_port = int(listener.getsockname()[1])
            collision = subprocess.run(
                [*base_arguments, "--port", str(occupied_port)],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
                timeout=10,
                check=False,
            )
        assert collision.returncode == 67, collision.stderr.decode(
            errors="replace"
        )

        missing_token = subprocess.run(
            [
                str(binary),
                "--bind", "127.0.0.1",
                "--port", str(free_port()),
                "--rom-dir", str(directories[0]),
                "--save", str(directories[1]),
                "--system", str(directories[2]),
                "--auth-token-file", str(root / "missing-token"),
                "--raw-video",
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            timeout=10,
            check=False,
        )
        assert missing_token.returncode == 64, missing_token.stderr.decode(
            errors="replace"
        )

        def limit_main_stack() -> None:
            resource.setrlimit(
                resource.RLIMIT_STACK,
                (512 * 1024, 512 * 1024),
            )

        port = free_port()
        process = subprocess.Popen(
            [*base_arguments, "--port", str(port)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            preexec_fn=limit_main_stack,
        )
        try:
            wait_ready(process, port)
            status, loaded = json_request(
                port,
                "POST",
                "/api/load",
                json.dumps({"path": str(rom_path)}).encode(),
            )
            assert status == 200 and loaded.get("ok") is True, loaded
            status, current = json_request(port, "GET", "/api/status")
            assert status == 200 and current.get("running") is True, current
        except Exception:
            if process.poll() is None:
                process.send_signal(signal.SIGTERM)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            print(process.stderr.read().decode(errors="replace"))
            raise
        else:
            process.send_signal(signal.SIGTERM)
            process.wait(timeout=10)
            assert process.returncode == 0, process.stderr.read()


def test_invalid_startup_rom_recovers(binary: Path) -> None:
    """A stale autoload selection must not make the control API unavailable."""
    port = free_port()
    with tempfile.TemporaryDirectory(prefix="nesd-stale-autoload-") as temporary:
        root = Path(temporary)
        directories = [root / name for name in ("roms", "saves", "system")]
        for directory in directories:
            directory.mkdir(mode=0o750)
        invalid_rom = directories[0] / "stale.nes"
        invalid_rom.write_bytes(b"NES\x1a" + b"\0" * 12)
        replacement_rom = directories[0] / "replacement.nes"
        replacement_rom.write_bytes(make_test_rom())
        log_path = root / "nesd.log"
        with log_path.open("wb") as log:
            process = subprocess.Popen(
                [
                    str(binary),
                    "--bind", "127.0.0.1",
                    "--port", str(port),
                    "--rom-dir", str(directories[0]),
                    "--save", str(directories[1]),
                    "--system", str(directories[2]),
                    "--auth-token", TOKEN,
                    "--rom", str(invalid_rom),
                    "--raw-video",
                ],
                stdout=log,
                stderr=subprocess.STDOUT,
            )
            try:
                wait_ready(process, port)
                status, idle = json_request(port, "GET", "/api/status")
                assert status == 200, idle
                assert idle.get("running") is False, idle
                assert idle.get("game_loaded") is False, idle

                status, loaded = json_request(
                    port,
                    "POST",
                    "/api/load",
                    json.dumps({"path": str(replacement_rom)}).encode(),
                )
                assert status == 200 and loaded.get("ok") is True, loaded
            except Exception:
                process.send_signal(signal.SIGTERM)
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
                log.flush()
                print(log_path.read_text(encoding="utf-8", errors="replace"))
                raise
            else:
                process.send_signal(signal.SIGTERM)
                process.wait(timeout=10)
                assert process.returncode == 0, process.returncode


def test_idle_exit(binary: Path) -> None:
    """On-demand daemons exit only when truly unused and no game is loaded."""
    with tempfile.TemporaryDirectory(prefix="nesd-idle-exit-") as temporary:
        root = Path(temporary)
        directories = [root / name for name in ("roms", "saves", "system")]
        for directory in directories:
            directory.mkdir(mode=0o750)
        rom_path = directories[0] / "idle-test.nes"
        rom_path.write_bytes(make_test_rom())

        def launch(*extra: str) -> tuple[subprocess.Popen[bytes], int]:
            port = free_port()
            process = subprocess.Popen(
                [
                    str(binary),
                    "--bind", "127.0.0.1",
                    "--port", str(port),
                    "--rom-dir", str(directories[0]),
                    "--save", str(directories[1]),
                    "--system", str(directories[2]),
                    "--auth-token", TOKEN,
                    "--raw-video",
                    *extra,
                ],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
            )
            return process, port

        def stop(process: subprocess.Popen[bytes]) -> None:
            if process.poll() is None:
                process.send_signal(signal.SIGTERM)
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()

        # The disabled-by-default setting must keep a normal daemon alive.
        process, port = launch()
        try:
            wait_ready(process, port)
            time.sleep(1.35)
            assert process.poll() is None, "default idle exit was not disabled"
            status, current = json_request(port, "GET", "/api/status")
            assert status == 200 and current.get("game_loaded") is False, current
        finally:
            stop(process)
        assert process.returncode == 0, process.stderr.read()

        # The upper documented bound is accepted as a server option.
        process, port = launch("--idle-exit-seconds", "86400")
        try:
            wait_ready(process, port)
            assert process.poll() is None
        finally:
            stop(process)
        assert process.returncode == 0, process.stderr.read()

        # With no connection at all, the short on-demand lifetime expires.
        started = time.monotonic()
        process, _port = launch("--idle-exit-seconds", "1")
        try:
            process.wait(timeout=4)
        except subprocess.TimeoutExpired as error:
            stop(process)
            raise AssertionError("empty on-demand daemon did not exit") from error
        elapsed = time.monotonic() - started
        assert process.returncode == 0, process.stderr.read()
        assert elapsed >= 0.8, f"idle daemon exited too early after {elapsed:.3f}s"

        # New activity resets the timer, and a connected partial request keeps
        # the daemon alive even after that timer would otherwise have elapsed.
        process, port = launch("--idle-exit-seconds", "1")
        slow_client: socket.socket | None = None
        try:
            wait_ready(process, port)
            time.sleep(0.65)
            status, _current = json_request(port, "GET", "/api/status")
            assert status == 200
            time.sleep(0.65)
            assert process.poll() is None, "recent HTTP activity was ignored"

            slow_client = socket.create_connection(
                ("127.0.0.1", port), timeout=3
            )
            slow_client.sendall(b"GET /api/status HTTP/1.1\r\nHost: x\r\n")
            time.sleep(1.25)
            assert process.poll() is None, "active HTTP client did not defer exit"
            disconnected_at = time.monotonic()
            slow_client.close()
            slow_client = None
            time.sleep(0.55)
            assert process.poll() is None, (
                "time spent connected was incorrectly charged to idle timeout"
            )
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired as error:
                raise AssertionError(
                    "idle daemon stayed alive after its final client left"
                ) from error
            disconnected_for = time.monotonic() - disconnected_at
            assert process.returncode == 0, process.stderr.read()
            assert disconnected_for >= 0.8, (
                "daemon did not grant a full idle period after disconnect: "
                f"{disconnected_for:.3f}s"
            )
        finally:
            if slow_client is not None:
                slow_client.close()
            stop(process)

        # Loading a game is persistent state: it must never be auto-unloaded or
        # used as an excuse to terminate the server.
        process, port = launch("--idle-exit-seconds", "1")
        try:
            wait_ready(process, port)
            status, loaded = json_request(
                port,
                "POST",
                "/api/load",
                json.dumps({"path": str(rom_path)}).encode(),
            )
            assert status == 200 and loaded.get("ok") is True, loaded
            time.sleep(1.35)
            assert process.poll() is None, "loaded game was terminated as idle"
            status, current = json_request(port, "GET", "/api/status")
            assert status == 200 and current.get("game_loaded") is True, current
        finally:
            stop(process)
        assert process.returncode == 0, process.stderr.read()

        # Zero is the disabled internal default, but explicit CLI values must
        # stay inside 1..86400.
        for invalid in ("0", "86401"):
            process, _port = launch("--idle-exit-seconds", invalid)
            try:
                process.wait(timeout=5)
            finally:
                stop(process)
            error = process.stderr.read()
            assert process.returncode == 2, (invalid, error)
            assert b"invalid value for option" in error, (invalid, error)


def test_rpc_cli(binary: Path) -> None:
    """The packaged transport must preserve authenticated non-2xx JSON bodies."""
    if os.geteuid() != 0:
        print("integration: RPC CLI token-file test skipped (requires root ownership)")
        return
    port = free_port()
    with tempfile.TemporaryDirectory(prefix="nesd-rpc-cli-") as temporary:
        root = Path(temporary)
        directories = [root / name for name in ("roms", "saves", "system")]
        for directory in directories:
            directory.mkdir(mode=0o750)
        rom_path = directories[0] / "rpc-test.nes"
        rom_path.write_bytes(make_test_rom())
        token_file = root / "auth.token"
        token_file.write_text(TOKEN + "\n", encoding="ascii")
        token_file.chmod(0o640)
        bad_token_file = root / "bad-auth.token"
        bad_token_file.write_text(("f" * 64) + "\n", encoding="ascii")
        bad_token_file.chmod(0o640)
        hardlinked_token_file = root / "hardlinked-auth.token"
        hardlinked_token_file.write_text(TOKEN + "\n", encoding="ascii")
        hardlinked_token_file.chmod(0o640)
        os.link(hardlinked_token_file, root / "hardlinked-auth.alias")
        rejected_server = subprocess.run(
            [
                str(binary),
                "--bind", "127.0.0.1",
                "--port", str(free_port()),
                "--rom-dir", str(directories[0]),
                "--save", str(directories[1]),
                "--system", str(directories[2]),
                "--auth-token-file", str(hardlinked_token_file),
                "--raw-video",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=5,
            check=False,
        )
        assert rejected_server.returncode == 64, rejected_server.stderr
        assert b"cannot read secure auth token file" in rejected_server.stderr
        process = subprocess.Popen(
            [
                str(binary),
                "--bind", "127.0.0.1",
                "--port", str(port),
                "--rom-dir", str(directories[0]),
                "--save", str(directories[1]),
                "--system", str(directories[2]),
                "--auth-token", TOKEN,
                "--raw-video",
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        try:
            wait_ready(process, port)

            status = run_rpc_cli(
                binary, port, token_file, "/api/status", "GET"
            )
            assert status.returncode == 0, status.stderr
            status_json = json.loads(status.stdout)
            assert status_json.get("t") == "status", status_json
            assert (
                status_json.get("architecture") == "router-cpu-thin-client"
            ), status_json

            unauthorized = run_rpc_cli(
                binary, port, bad_token_file, "/api/status", "GET"
            )
            assert unauthorized.returncode == 22, unauthorized.stderr
            unauthorized_json = json.loads(unauthorized.stdout)
            assert unauthorized_json.get("error"), unauthorized_json

            no_game = run_rpc_cli(
                binary, port, token_file, "/api/start", "POST", {}
            )
            assert no_game.returncode == 22, no_game.stderr
            no_game_json = json.loads(no_game.stdout)
            assert no_game_json.get("error"), no_game_json

            missing = run_rpc_cli(
                binary,
                port,
                token_file,
                "/api/load",
                "POST",
                {"path": str(directories[0] / "missing.nes")},
            )
            assert missing.returncode == 22, missing.stderr
            missing_json = json.loads(missing.stdout)
            assert missing_json.get("error"), missing_json

            loaded = run_rpc_cli(
                binary,
                port,
                token_file,
                "/api/load",
                "POST",
                {"path": str(rom_path)},
            )
            assert loaded.returncode == 0, loaded.stderr
            loaded_json = json.loads(loaded.stdout)
            assert loaded_json.get("ok") is True, loaded_json

            rpc_hardlink = run_rpc_cli(
                binary,
                port,
                hardlinked_token_file,
                "/api/status",
                "GET",
            )
            assert rpc_hardlink.returncode == 1, rpc_hardlink.stderr
            assert not rpc_hardlink.stdout, rpc_hardlink.stdout
            assert b"cannot read secure RPC token file" in rpc_hardlink.stderr
        finally:
            process.send_signal(signal.SIGTERM)
            process.wait(timeout=10)
            assert process.returncode == 0, process.returncode


def assert_jpeg_quantization(jpeg: bytes, quality: int) -> None:
    assert jpeg.startswith(b"\xff\xd8") and jpeg.endswith(b"\xff\xd9")
    marker = jpeg.find(b"\xff\xdb")
    assert marker >= 0, "JPEG has no DQT marker"
    assert jpeg[marker + 2 : marker + 4] == b"\x00\x43"
    assert jpeg[marker + 4] == 0, "first DQT is not 8-bit luminance table 0"
    actual = jpeg[marker + 5 : marker + 69]
    assert len(actual) == 64

    scale = 5000 // quality if quality < 50 else 200 - quality * 2
    expected = bytearray(64)
    for natural_index, base_value in enumerate(STD_LUMA_QUANTIZATION):
        scaled = min(255, max(1, (base_value * scale + 50) // 100))
        expected[NATURAL_TO_ZIGZAG[natural_index]] = scaled
    assert actual == expected, "luminance DQT is not serialized in zigzag order"


def test_input_lease(binary: Path) -> None:
    """Heartbeats renew held buttons and silence releases them automatically."""
    port = free_port()
    origin = f"http://127.0.0.1:{port}"
    with tempfile.TemporaryDirectory(prefix="nesd-input-lease-") as temporary:
        root = Path(temporary)
        rom_dir = root / "roms"
        save_dir = root / "saves"
        system_dir = root / "system"
        for directory in (rom_dir, save_dir, system_dir):
            directory.mkdir(mode=0o750)
        rom_path = rom_dir / "input-probe.nes"
        rom_path.write_bytes(make_input_probe_rom())
        log_path = root / "nesd.log"
        with log_path.open("wb") as log:
            process = subprocess.Popen(
                [
                    str(binary),
                    "--bind", "127.0.0.1",
                    "--port", str(port),
                    "--rom-dir", str(rom_dir),
                    "--save", str(save_dir),
                    "--system", str(system_dir),
                    "--auth-token", TOKEN,
                    "--allowed-origin", origin,
                    "--raw-video",
                    "--stream-fps", "2",
                ],
                stdout=log,
                stderr=subprocess.STDOUT,
            )
            websocket = None
            try:
                wait_ready(process, port)
                status, loaded = json_request(
                    port,
                    "POST",
                    "/api/load",
                    json.dumps({"path": str(rom_path)}).encode(),
                )
                assert status == 200 and loaded.get("ok") is True, loaded
                websocket = websocket_connect(port)

                def drain_for(seconds: float) -> None:
                    deadline = time.monotonic() + seconds

                    while time.monotonic() < deadline:
                        opcode, frame = receive_ws(websocket)
                        if opcode == 0x9:
                            send_ws(websocket, 0xA, frame)
                        elif opcode == 0x8:
                            raise AssertionError(
                                f"unexpected WebSocket close: {frame!r}"
                            )

                # The second heartbeat lands after half the lease. The A
                # button must still be held after more than one full lease
                # has elapsed since the first message.
                send_ws(
                    websocket,
                    0x1,
                    b'{"t":"heartbeat","mask":256,"seq":1}',
                )
                drain_for(2.0)
                send_ws(
                    websocket,
                    0x1,
                    b'{"t":"heartbeat","mask":256,"seq":2}',
                )
                drain_for(2.0)
                status, paused = json_request(port, "POST", "/api/pause", b"{}")
                assert status == 200 and paused.get("ok") is True, paused
                status, saved = json_request(
                    port, "POST", "/api/state/save", b'{"slot":1}'
                )
                assert status == 200 and saved.get("ok") is True, saved
                sram_path = next(save_dir.glob("*.srm"))
                assert sram_path.read_bytes()[0] == 1, (
                    "heartbeat did not renew the held input lease"
                )

                status, resumed = json_request(port, "POST", "/api/pause", b"{}")
                assert status == 200 and resumed.get("ok") is True, resumed
                send_ws(
                    websocket,
                    0x1,
                    b'{"t":"heartbeat","mask":256,"seq":3}',
                )
                drain_for(3.9)
                status, paused = json_request(port, "POST", "/api/pause", b"{}")
                assert status == 200 and paused.get("ok") is True, paused
                status, saved = json_request(
                    port, "POST", "/api/state/save", b'{"slot":1}'
                )
                assert status == 200 and saved.get("ok") is True, saved
                assert sram_path.read_bytes()[0] == 0, (
                    "expired input lease left the A button stuck"
                )

                # Once paused there is neither fresh video nor PCM. Allow
                # the no-frame retry to reach its cap, then verify that a
                # low-FPS static stream sleeps instead of polling at ~60 Hz.
                time.sleep(1.2)
                switches_before = voluntary_context_switches(process.pid)
                time.sleep(2.2)
                switches_after = voluntary_context_switches(process.pid)
                if switches_before is not None and switches_after is not None:
                    assert switches_after - switches_before <= 30, (
                        "paused low-FPS stream busy-spun: "
                        f"{switches_after - switches_before} voluntary wakeups"
                    )
            except Exception:
                process.send_signal(signal.SIGTERM)
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
                log.flush()
                print(log_path.read_text(encoding="utf-8", errors="replace"))
                raise
            else:
                if websocket is not None:
                    websocket.close()
                process.send_signal(signal.SIGTERM)
                process.wait(timeout=10)
                assert process.returncode == 0, process.returncode


def test_jpeg_stream(binary: Path) -> None:
    quality = 92
    port = free_port()
    origin = f"http://127.0.0.1:{port}"
    with tempfile.TemporaryDirectory(prefix="nesd-jpeg-") as temporary:
        root = Path(temporary)
        directories = [root / name for name in ("roms", "saves", "system")]
        for directory in directories:
            directory.mkdir(mode=0o750)
        rom_path = directories[0] / "jpeg-test.nes"
        rom_path.write_bytes(make_test_rom())
        log_path = root / "nesd.log"
        with log_path.open("wb") as log:
            process = subprocess.Popen(
                [
                    str(binary),
                    "--bind", "127.0.0.1",
                    "--port", str(port),
                    "--rom-dir", str(directories[0]),
                    "--save", str(directories[1]),
                    "--system", str(directories[2]),
                    "--auth-token", TOKEN,
                    "--allowed-origin", origin,
                    "--jpeg-video",
                    "--jpeg-quality", str(quality),
                ],
                stdout=log,
                stderr=subprocess.STDOUT,
            )
            try:
                wait_ready(process, port)
                status, loaded = json_request(
                    port,
                    "POST",
                    "/api/load",
                    json.dumps({"path": str(rom_path)}).encode(),
                )
                assert status == 200 and loaded.get("ok") is True, loaded
                websocket = websocket_connect(port)
                try:
                    jpeg_frame = None
                    deadline = time.monotonic() + 8
                    while time.monotonic() < deadline:
                        opcode, frame = receive_ws(websocket)
                        if opcode == 0x9:
                            send_ws(websocket, 0xA, frame)
                        elif opcode == 0x2 and frame and frame[0] == 3:
                            jpeg_frame = frame
                            break
                        elif opcode == 0x8:
                            raise AssertionError(
                                f"unexpected WebSocket close: {frame!r}"
                            )
                    assert jpeg_frame is not None, "no JPEG video frame received"
                    assert jpeg_frame[1] == quality
                    width, height = struct.unpack_from("<HH", jpeg_frame, 2)
                    jpeg_length = struct.unpack_from("<I", jpeg_frame, 6)[0]
                    assert width > 0 and height > 0
                    assert len(jpeg_frame) == 12 + jpeg_length
                    assert_jpeg_quantization(jpeg_frame[12:], quality)

                    # JPEG compression runs in a lower-priority worker.
                    # Keep an end-to-end regression bound on control latency
                    # while that encoder is active so a future queue/codec
                    # change cannot silently starve controller heartbeats.
                    heartbeat_started = time.monotonic()
                    send_ws(
                        websocket,
                        0x1,
                        b'{"t":"heartbeat","mask":0,"seq":1}',
                    )
                    heartbeat_seen = False
                    heartbeat_deadline = heartbeat_started + 1.0
                    while time.monotonic() < heartbeat_deadline:
                        opcode, frame = receive_ws(websocket)
                        if opcode == 0x9:
                            send_ws(websocket, 0xA, frame)
                        elif opcode == 0x1:
                            reply = json.loads(frame)
                            if reply == {"t": "heartbeat", "seq": 1}:
                                heartbeat_seen = True
                                break
                        elif opcode == 0x8:
                            raise AssertionError(
                                f"unexpected WebSocket close: {frame!r}"
                            )
                    heartbeat_latency = time.monotonic() - heartbeat_started
                    assert heartbeat_seen, "JPEG stream starved heartbeat reply"
                    assert heartbeat_latency < 1.0, heartbeat_latency
                    send_ws(websocket, 0x8, struct.pack(">H", 1000))
                finally:
                    websocket.close()
            except Exception:
                process.send_signal(signal.SIGTERM)
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
                log.flush()
                print(log_path.read_text(encoding="utf-8", errors="replace"))
                raise
            else:
                process.send_signal(signal.SIGTERM)
                process.wait(timeout=10)
                assert process.returncode == 0, process.returncode


def test_pal_stream_pacing(binary: Path) -> None:
    """A configured 60 FPS stream follows a PAL core at an even 50 Hz."""
    port = free_port()
    origin = f"http://127.0.0.1:{port}"
    with tempfile.TemporaryDirectory(prefix="nesd-pal-") as temporary:
        root = Path(temporary)
        rom_dir = root / "roms"
        save_dir = root / "saves"
        system_dir = root / "system"
        for directory in (rom_dir, save_dir, system_dir):
            directory.mkdir(mode=0o750)
        rom_path = rom_dir / "pal-test.nes"
        rom_path.write_bytes(make_pal_test_rom())
        log_path = root / "nesd.log"
        with log_path.open("wb") as log:
            process = subprocess.Popen(
                [
                    str(binary),
                    "--bind", "127.0.0.1",
                    "--port", str(port),
                    "--rom-dir", str(rom_dir),
                    "--save", str(save_dir),
                    "--system", str(system_dir),
                    "--auth-token", TOKEN,
                    "--allowed-origin", origin,
                    "--raw-video",
                    "--stream-fps", "60",
                ],
                stdout=log,
                stderr=subprocess.STDOUT,
            )
            try:
                wait_ready(process, port)
                status, loaded = json_request(
                    port,
                    "POST",
                    "/api/load",
                    json.dumps({"path": str(rom_path)}).encode(),
                )
                assert status == 200 and loaded.get("ok") is True, loaded
                status, core_status = json_request(port, "GET", "/api/status")
                assert status == 200 and 49.0 < core_status["fps"] < 51.0, core_status

                # A blocked receiver may retain the in-progress video but
                # must not make the server capture a second stale video
                # behind it. Once reads resume, the following frame is fresh.
                slow_websocket = websocket_connect(port, receive_buffer=4096)
                try:
                    time.sleep(0.2)
                    backlog_ids = []
                    backlog_deadline = time.monotonic() + 3.0
                    while (
                        time.monotonic() < backlog_deadline
                        and len(backlog_ids) < 2
                    ):
                        opcode, frame = receive_ws(slow_websocket)
                        if opcode == 0x9:
                            send_ws(slow_websocket, 0xA, frame)
                        elif opcode == 0x2 and frame and frame[0] == 1:
                            backlog_ids.append(
                                struct.unpack_from("<I", frame, 6)[0]
                            )
                        elif opcode == 0x8:
                            raise AssertionError(
                                f"unexpected WebSocket close: {frame!r}"
                            )
                    assert len(backlog_ids) == 2, backlog_ids
                    backlog_gap = (backlog_ids[1] - backlog_ids[0]) & 0xFFFFFFFF
                    assert backlog_gap >= 4, (
                        "slow client received a second pre-captured video: "
                        f"frame gap was only {backlog_gap}"
                    )
                    send_ws(slow_websocket, 0x8, struct.pack(">H", 1000))
                finally:
                    slow_websocket.close()
                time.sleep(0.1)

                websocket = websocket_connect(port)
                try:
                    arrivals = []
                    frame_ids = []
                    started = None
                    deadline = time.monotonic() + 4.0
                    while time.monotonic() < deadline:
                        opcode, frame = receive_ws(websocket)
                        if opcode == 0x9:
                            send_ws(websocket, 0xA, frame)
                        elif opcode == 0x2 and frame and frame[0] == 1:
                            arrived = time.monotonic()
                            arrivals.append(arrived)
                            frame_ids.append(struct.unpack_from("<I", frame, 6)[0])
                            if started is None:
                                started = arrived
                            elif arrived - started >= 1.5:
                                break
                        elif opcode == 0x8:
                            raise AssertionError(
                                f"unexpected WebSocket close: {frame!r}"
                            )
                    assert len(arrivals) >= 60, (
                        arrivals,
                        [
                            (later - earlier) & 0xFFFFFFFF
                            for earlier, later in zip(frame_ids, frame_ids[1:])
                        ],
                    )
                    duration = arrivals[-1] - arrivals[0]
                    measured = (len(arrivals) - 1) / duration
                    intervals = [
                        later - earlier
                        for earlier, later in zip(arrivals, arrivals[1:])
                    ]
                    assert 46.0 < measured < 54.0, (
                        measured,
                        [round(interval * 1000, 1) for interval in intervals],
                        [
                            (later - earlier) & 0xFFFFFFFF
                            for earlier, later in zip(frame_ids, frame_ids[1:])
                        ],
                    )
                    short = sum(interval < 0.017 for interval in intervals)
                    assert short <= len(intervals) // 4, (
                        "PAL output retained an irregular 60 Hz sampling phase: "
                        f"{short}/{len(intervals)} intervals were below 17 ms"
                    )
                    send_ws(websocket, 0x8, struct.pack(">H", 1000))
                finally:
                    websocket.close()
            except Exception:
                process.send_signal(signal.SIGTERM)
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
                log.flush()
                print(log_path.read_text(encoding="utf-8", errors="replace"))
                raise
            else:
                process.send_signal(signal.SIGTERM)
                process.wait(timeout=10)
                assert process.returncode == 0, process.returncode


def test_browser_heartbeat_reclaims_stream(binary: Path) -> None:
    """A half-open bundled client cannot retain the sole stream slot."""
    port = free_port()
    origin = f"http://127.0.0.1:{port}"
    with tempfile.TemporaryDirectory(prefix="nesd-heartbeat-timeout-") as temporary:
        root = Path(temporary)
        rom_dir = root / "roms"
        save_dir = root / "saves"
        system_dir = root / "system"
        for directory in (rom_dir, save_dir, system_dir):
            directory.mkdir(mode=0o750)
        log_path = root / "nesd.log"
        with log_path.open("wb") as log:
            process = subprocess.Popen(
                [
                    str(binary),
                    "--bind", "127.0.0.1",
                    "--port", str(port),
                    "--rom-dir", str(rom_dir),
                    "--save", str(save_dir),
                    "--system", str(system_dir),
                    "--auth-token", TOKEN,
                    "--allowed-origin", origin,
                    "--raw-video",
                    "--stream-fps", "2",
                ],
                stdout=log,
                stderr=subprocess.STDOUT,
            )
            stale = None
            replacement = None
            try:
                wait_ready(process, port)
                stale = websocket_connect(port)
                stale.sock.settimeout(8)
                send_ws(stale, 0x1, b'{"t":"hello"}')
                started = time.monotonic()
                expect_ws_close(stale, 1001)
                elapsed = time.monotonic() - started
                assert 5.0 <= elapsed < 8.0, (
                    "application heartbeat deadline did not reclaim the stale "
                    f"browser stream promptly: {elapsed:.2f}s"
                )

                # A server-initiated Close begins, rather than completes, the
                # RFC 6455 handshake.  Keep TCP alive long enough for the peer
                # to echo Close, then tear it down promptly once both frames
                # have crossed.  This catches a static-musl race where nesd
                # used to destroy the socket as soon as its send queue emptied.
                stale.sock.settimeout(0.1)
                try:
                    premature = stale.recv(1)
                except socket.timeout:
                    pass
                else:
                    assert False, (
                        "server closed or sent trailing data before the peer "
                        f"Close reply: {premature!r}"
                    )
                send_ws(stale, 0x8, struct.pack(">H", 1001))
                stale.sock.settimeout(2)
                assert stale.recv(1) == b"", (
                    "server did not finish the WebSocket closing handshake"
                )

                replacement = websocket_connect(port)
                send_ws(
                    replacement,
                    0x1,
                    b'{"t":"heartbeat","mask":0,"seq":1}',
                )
                deadline = time.monotonic() + 2.0
                acknowledged = False
                while time.monotonic() < deadline:
                    opcode, frame = receive_ws(replacement)
                    if opcode == 0x9:
                        send_ws(replacement, 0xA, frame)
                    elif opcode == 0x1 and json.loads(frame) == {
                        "t": "heartbeat",
                        "seq": 1,
                    }:
                        acknowledged = True
                        break
                    elif opcode == 0x8:
                        raise AssertionError(
                            f"replacement stream closed unexpectedly: {frame!r}"
                        )
                assert acknowledged, "replacement stream did not receive heartbeat ACK"
            except Exception:
                process.send_signal(signal.SIGTERM)
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
                log.flush()
                print(log_path.read_text(encoding="utf-8", errors="replace"))
                raise
            else:
                process.send_signal(signal.SIGTERM)
                process.wait(timeout=10)
                assert process.returncode == 0, process.returncode
            finally:
                if replacement is not None:
                    replacement.close()
                if stale is not None:
                    stale.close()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True, type=Path)
    args = parser.parse_args()
    binary = args.binary.resolve()
    if not binary.is_file():
        parser.error(f"binary not found: {binary}")
    test_client_visibility_options(binary)
    test_daemon(binary)
    test_savestate_roundtrip(binary)
    test_explicit_demo_mode(binary)
    test_startup_diagnostics_and_small_stack(binary)
    test_invalid_startup_rom_recovers(binary)
    test_idle_exit(binary)
    test_rpc_cli(binary)
    test_input_lease(binary)
    test_jpeg_stream(binary)
    test_pal_stream_pacing(binary)
    test_browser_heartbeat_reclaims_stream(binary)
    print("integration: all regressions passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
