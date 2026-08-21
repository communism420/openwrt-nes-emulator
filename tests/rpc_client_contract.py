#!/usr/bin/env python3
"""Socket-level contracts for nesd's self-contained loopback RPC transport."""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import threading
import time


ROOT = Path(__file__).resolve().parents[1]
TOKEN = (
    "0123456789abcdef0123456789abcdef"
    "0123456789abcdef0123456789abcdef"
)


class ResponseServer:
    def __init__(
        self,
        response: bytes,
        *,
        family: socket.AddressFamily = socket.AF_INET,
        bind_host: str | None = None,
        bytewise: bool = False,
        stall_seconds: float = 0,
    ) -> None:
        self.response = response
        self.family = family
        self.bytewise = bytewise
        self.stall_seconds = stall_seconds
        self.request = b""
        self.error: BaseException | None = None
        self.listener = socket.socket(family, socket.SOCK_STREAM)
        if family == socket.AF_INET6:
            self.listener.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 1)
            self.listener.bind((bind_host or "::1", 0))
        else:
            self.listener.bind((bind_host or "127.0.0.1", 0))
        self.listener.listen(1)
        self.port = int(self.listener.getsockname()[1])
        self.thread = threading.Thread(target=self._serve, daemon=True)

    def __enter__(self) -> "ResponseServer":
        self.thread.start()
        return self

    def _serve(self) -> None:
        try:
            connection, _peer = self.listener.accept()
            with connection:
                connection.settimeout(2)
                request = b""
                while b"\r\n\r\n" not in request:
                    part = connection.recv(4096)
                    if not part:
                        break
                    request += part
                head, separator, pending = request.partition(b"\r\n\r\n")
                content_length = 0
                if separator:
                    for line in head.split(b"\r\n")[1:]:
                        name, found, value = line.partition(b":")
                        if found and name.lower() == b"content-length":
                            content_length = int(value.strip())
                    while len(pending) < content_length:
                        part = connection.recv(4096)
                        if not part:
                            break
                        pending += part
                self.request = head + separator + pending
                if self.stall_seconds:
                    time.sleep(self.stall_seconds)
                if self.bytewise:
                    for byte in self.response:
                        connection.sendall(bytes((byte,)))
                else:
                    connection.sendall(self.response)
        except (BrokenPipeError, ConnectionResetError):
            pass
        except BaseException as error:  # surfaced in __exit__
            self.error = error
        finally:
            self.listener.close()

    def __exit__(self, exc_type, exc, traceback) -> None:
        self.thread.join(timeout=3)
        if self.thread.is_alive():
            raise AssertionError("fake RPC server did not terminate")
        if self.error is not None:
            raise self.error


def build_driver(directory: Path) -> Path:
    compiler = os.environ.get("CC") or shutil.which("cc")
    if not compiler:
        raise RuntimeError("C compiler is required for rpc_client_contract.py")
    output = directory / "rpc-client-driver"
    subprocess.run(
        [
            compiler,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(ROOT / "package/nes-emulator/src"),
            str(ROOT / "package/nes-emulator/src/rpc_client.c"),
            str(ROOT / "tests/rpc_client_driver.c"),
            "-o",
            str(output),
        ],
        cwd=ROOT,
        check=True,
    )
    return output


def run_client(
    driver: Path,
    port: int,
    *,
    host: str = "127.0.0.1",
    path: str = "/api/status",
    method: str = "GET",
    body: str = "",
    timeout_ms: int = 1000,
) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(
        [
            str(driver),
            host,
            str(port),
            path,
            method,
            body,
            str(timeout_ms),
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=3,
        check=False,
    )


def framed(status: int, body: bytes, extra_headers: bytes = b"") -> bytes:
    return (
        f"HTTP/1.1 {status} Test\r\n".encode()
        + f"Content-Length: {len(body)}\r\n".encode()
        + extra_headers
        + b"Connection: close\r\n\r\n"
        + body
    )


def assert_request(server: ResponseServer, method: bytes, path: bytes) -> None:
    assert server.request.startswith(method + b" " + path + b" HTTP/1.1\r\n")
    assert f"Authorization: Bearer {TOKEN}\r\n".encode() in server.request
    assert b"Proxy-Authorization:" not in server.request


def test_success_and_http_errors(driver: Path) -> None:
    success = b'{"running":false,"core_loaded":false}'
    with ResponseServer(framed(200, success), bytewise=True) as server:
        result = run_client(driver, server.port)
    assert result.returncode == 0, result.stderr
    assert result.stdout == success
    assert_request(server, b"GET", b"/api/status")

    denied = b'{"ok":false,"error":"bad token"}'
    with ResponseServer(framed(401, denied)) as server:
        result = run_client(driver, server.port)
    assert result.returncode == 22, result.stderr
    assert result.stdout == denied

    rejected = b'{"ok":false,"error":"ROM is unreadable"}'
    with ResponseServer(framed(403, rejected)) as server:
        result = run_client(
            driver,
            server.port,
            path="/api/load",
            method="POST",
            body='{"path":"/etc/nes-emulator/roms/smb.nes"}',
        )
    assert result.returncode == 22, result.stderr
    assert result.stdout == rejected
    assert_request(server, b"POST", b"/api/load")
    assert b"Content-Type: application/json\r\n" in server.request
    assert server.request.endswith(
        b'{"path":"/etc/nes-emulator/roms/smb.nes"}'
    )


def test_chunked_close_delimited_and_large_body(driver: Path) -> None:
    chunked_body = b'{"ok":false,"error":"chunked failure"}'
    chunks = (
        f"{10:x}\r\n".encode()
        + chunked_body[:10]
        + b"\r\n"
        + f"{len(chunked_body) - 10:x};safe=yes\r\n".encode()
        + chunked_body[10:]
        + b"\r\n0\r\nX-Test: complete\r\n\r\n"
    )
    response = (
        b"HTTP/1.1 500 Test\r\n"
        b"Transfer-Encoding: chunked\r\n"
        b"Connection: close\r\n\r\n"
        + chunks
    )
    with ResponseServer(response, bytewise=True) as server:
        result = run_client(driver, server.port)
    assert result.returncode == 22, result.stderr
    assert result.stdout == chunked_body

    close_body = b'{"ok":true,"framing":"close"}'
    with ResponseServer(
        b"HTTP/1.0 200 Test\r\nConnection: close\r\n\r\n" + close_body
    ) as server:
        result = run_client(driver, server.port)
    assert result.returncode == 0, result.stderr
    assert result.stdout == close_body

    large_body = b"x" * (270 * 1024)
    with ResponseServer(framed(200, large_body)) as server:
        result = run_client(driver, server.port, timeout_ms=2000)
    assert result.returncode == 0, result.stderr
    assert result.stdout == large_body


def assert_protocol_failure(
    driver: Path,
    response: bytes,
    *,
    stall_seconds: float = 0,
    timeout_ms: int = 1000,
) -> None:
    with ResponseServer(response, stall_seconds=stall_seconds) as server:
        result = run_client(driver, server.port, timeout_ms=timeout_ms)
    assert result.returncode != 0
    assert result.stdout == b"", result.stdout[:100]


def test_protocol_failures_are_atomic(driver: Path) -> None:
    assert_protocol_failure(
        driver,
        b"HTTP/1.1 200 Test\r\nContent-Length: 20\r\n\r\npartial",
    )
    assert_protocol_failure(
        driver,
        b"HTTP/1.1 200 Test\r\n"
        b"Content-Length: 2\r\nContent-Length: 3\r\n\r\n{}",
    )
    assert_protocol_failure(
        driver,
        b"HTTP/1.1 200 Test\r\n"
        b"Content-Length: 2\r\nTransfer-Encoding: chunked\r\n\r\n"
        b"0\r\n\r\n",
    )
    assert_protocol_failure(
        driver,
        b"HTTP/1.1 200 Test\r\nContent-Length: 524289\r\n\r\n",
    )
    assert_protocol_failure(
        driver,
        b"HTTP/1.1 200 Test\r\nContent-Length: 2\r\n\r\n{}",
        stall_seconds=0.5,
        timeout_ms=150,
    )


def test_loopback_and_ipv6(driver: Path) -> None:
    result = run_client(driver, 9, host="192.0.2.1")
    assert result.returncode != 0
    assert result.stdout == b""

    if not socket.has_ipv6:
        return
    try:
        server = ResponseServer(framed(200, b"{}"), family=socket.AF_INET6)
    except OSError:
        return
    with server:
        result = run_client(driver, server.port, host="::1")
    assert result.returncode == 0, result.stderr
    assert result.stdout == b"{}"
    assert f"Host: [::1]:{server.port}\r\n".encode() in server.request

    candidate = None
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        probe.connect(("192.0.2.1", 9))
        candidate = str(probe.getsockname()[0])
    except OSError:
        pass
    finally:
        probe.close()
    if candidate and not candidate.startswith("127.") and candidate != "0.0.0.0":
        try:
            lan_server = ResponseServer(
                framed(200, b'{"local":true}'), bind_host=candidate
            )
        except OSError:
            return
        with lan_server:
            result = run_client(driver, lan_server.port, host=candidate)
        assert result.returncode == 0, result.stderr
        assert result.stdout == b'{"local":true}'


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="nes-rpc-client-") as temporary:
        driver = build_driver(Path(temporary))
        test_success_and_http_errors(driver)
        test_chunked_close_delimited_and_large_body(driver)
        test_protocol_failures_are_atomic(driver)
        test_loopback_and_ipv6(driver)
    print("nesd RPC client contract: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
