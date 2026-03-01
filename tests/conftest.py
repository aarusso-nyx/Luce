from __future__ import annotations

import base64
import hashlib
import json
import os
import queue
import socket
import ssl
import struct
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any
from urllib import error as urlerror
from urllib import request as urlrequest

import pytest


def pytest_addoption(parser: pytest.Parser) -> None:
    parser.addoption("--luce-host", action="store", default=os.getenv("LUCE_HOST", "https://127.0.0.1"))
    parser.addoption("--luce-http-token", action="store", default=os.getenv("LUCE_HTTP_TOKEN", ""))
    parser.addoption("--luce-tcp-host", action="store", default=os.getenv("LUCE_TCP_HOST", "127.0.0.1"))
    parser.addoption("--luce-tcp-port", action="store", type=int, default=int(os.getenv("LUCE_TCP_PORT", "2323")))
    parser.addoption("--luce-tcp-token", action="store", default=os.getenv("LUCE_CLI_NET_TOKEN", ""))
    parser.addoption("--luce-ws-host", action="store", default=os.getenv("LUCE_WS_HOST", "127.0.0.1"))
    parser.addoption("--luce-ws-port", action="store", type=int, default=int(os.getenv("LUCE_WS_PORT", "80")))
    parser.addoption("--luce-ws-path", action="store", default=os.getenv("LUCE_WS_PATH", "/ws"))
    parser.addoption("--luce-ws-tls", action="store_true", default=os.getenv("LUCE_WS_TLS", "0") == "1")
    parser.addoption("--luce-mqtt-host", action="store", default=os.getenv("LUCE_MQTT_HOST", "127.0.0.1"))
    parser.addoption("--luce-mqtt-port", action="store", type=int, default=int(os.getenv("LUCE_MQTT_PORT", "1883")))
    parser.addoption("--luce-mqtt-topic", action="store", default=os.getenv("LUCE_MQTT_BASE_TOPIC", "luce/net1"))
    parser.addoption("--luce-mqtt-username", action="store", default=os.getenv("LUCE_MQTT_USERNAME", ""))
    parser.addoption("--luce-mqtt-password", action="store", default=os.getenv("LUCE_MQTT_PASSWORD", ""))
    parser.addoption("--luce-env", action="store", default=os.getenv("LUCE_ENV", "net1"))
    parser.addoption("--luce-duration", action="store", type=int, default=int(os.getenv("LUCE_TEST_DURATION", "45")))
    parser.addoption("--luce-run-build", action="store_true", default=os.getenv("LUCE_RUN_BUILD", "0") == "1")
    parser.addoption("--luce-run-boot", action="store_true", default=os.getenv("LUCE_RUN_BOOT", "0") == "1")


@pytest.fixture(scope="session")
def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


@pytest.fixture(scope="session")
def luce_host(pytestconfig: pytest.Config) -> str:
    return str(pytestconfig.getoption("--luce-host")).rstrip("/")


@pytest.fixture(scope="session")
def luce_http_token(pytestconfig: pytest.Config) -> str:
    return str(pytestconfig.getoption("--luce-http-token"))


@pytest.fixture(scope="session")
def tcp_config(pytestconfig: pytest.Config) -> tuple[str, int, str]:
    host = str(pytestconfig.getoption("--luce-tcp-host"))
    port = int(pytestconfig.getoption("--luce-tcp-port"))
    token = str(pytestconfig.getoption("--luce-tcp-token"))
    return host, port, token


@pytest.fixture(scope="session")
def ws_config(pytestconfig: pytest.Config) -> tuple[str, int, str, bool]:
    host = str(pytestconfig.getoption("--luce-ws-host"))
    port = int(pytestconfig.getoption("--luce-ws-port"))
    path = str(pytestconfig.getoption("--luce-ws-path"))
    tls = bool(pytestconfig.getoption("--luce-ws-tls"))
    return host, port, path, tls


@pytest.fixture(scope="session")
def mqtt_config(pytestconfig: pytest.Config) -> dict[str, Any]:
    return {
        "host": str(pytestconfig.getoption("--luce-mqtt-host")),
        "port": int(pytestconfig.getoption("--luce-mqtt-port")),
        "base_topic": str(pytestconfig.getoption("--luce-mqtt-topic")),
        "username": str(pytestconfig.getoption("--luce-mqtt-username")),
        "password": str(pytestconfig.getoption("--luce-mqtt-password")),
    }


@dataclass
class HttpResponse:
    status: int
    body: bytes
    headers: dict[str, str]

    def json(self) -> dict[str, Any]:
        return json.loads(self.body.decode("utf-8", errors="replace"))


@pytest.fixture(scope="session")
def http_requester() -> Any:
    insecure_ctx = ssl.create_default_context()
    insecure_ctx.check_hostname = False
    insecure_ctx.verify_mode = ssl.CERT_NONE

    def _request(method: str, url: str, headers: dict[str, str] | None = None, data: bytes | None = None,
                 timeout: float = 8.0) -> HttpResponse:
        req = urlrequest.Request(url=url, method=method, data=data)
        for k, v in (headers or {}).items():
            req.add_header(k, v)

        try:
            with urlrequest.urlopen(req, timeout=timeout, context=insecure_ctx) as resp:
                body = resp.read()
                return HttpResponse(status=resp.status, body=body, headers=dict(resp.headers.items()))
        except urlerror.HTTPError as exc:
            body = exc.read() if hasattr(exc, "read") else b""
            return HttpResponse(status=exc.code, body=body, headers=dict(exc.headers.items()) if exc.headers else {})

    return _request


def require_token(token: str, flag_name: str) -> None:
    if not token:
        pytest.skip(f"missing token; provide {flag_name} or env")


def recv_line(sock: socket.socket, timeout: float = 3.0) -> str:
    sock.settimeout(timeout)
    buf = bytearray()
    while True:
        b = sock.recv(1)
        if not b:
            break
        if b == b"\n":
            break
        if b == b"\r":
            continue
        buf.extend(b)
    return buf.decode("utf-8", errors="replace").strip()


def send_line(sock: socket.socket, text: str) -> None:
    sock.sendall(text.encode("utf-8") + b"\r\n")


GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def ws_connect(host: str, port: int, path: str, tls: bool, timeout: float = 5.0) -> socket.socket:
    raw = socket.create_connection((host, port), timeout=timeout)
    if tls:
        ctx = ssl.create_default_context()
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        sock: socket.socket = ctx.wrap_socket(raw, server_hostname=host)
    else:
        sock = raw

    key = base64.b64encode(os.urandom(16)).decode("ascii")
    req = (
        f"GET {path} HTTP/1.1\r\n"
        f"Host: {host}:{port}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n"
    )
    sock.sendall(req.encode("utf-8"))

    data = b""
    while b"\r\n\r\n" not in data:
        chunk = sock.recv(4096)
        if not chunk:
            break
        data += chunk
    if b"\r\n\r\n" not in data:
        raise AssertionError("incomplete websocket upgrade response")

    head = data.split(b"\r\n\r\n", 1)[0].decode("utf-8", errors="replace")
    lines = head.split("\r\n")
    status_line = lines[0]
    if "101" not in status_line:
        raise AssertionError(f"websocket upgrade failed: {status_line}")

    headers = {}
    for line in lines[1:]:
        if ":" in line:
            k, v = line.split(":", 1)
            headers[k.strip().lower()] = v.strip()

    expected = base64.b64encode(hashlib.sha1((key + GUID).encode("ascii")).digest()).decode("ascii")
    if headers.get("sec-websocket-accept", "") != expected:
        raise AssertionError("invalid websocket accept key")

    return sock


def recv_exact(sock: socket.socket, n: int) -> bytes:
    out = b""
    while len(out) < n:
        chunk = sock.recv(n - len(out))
        if not chunk:
            raise AssertionError("socket closed")
        out += chunk
    return out


def ws_recv_frame(sock: socket.socket, timeout: float = 4.0) -> tuple[int, bytes]:
    sock.settimeout(timeout)
    b1, b2 = recv_exact(sock, 2)
    opcode = b1 & 0x0F
    masked = (b2 & 0x80) != 0
    length = b2 & 0x7F
    if length == 126:
        length = struct.unpack("!H", recv_exact(sock, 2))[0]
    elif length == 127:
        length = struct.unpack("!Q", recv_exact(sock, 8))[0]
    mask = recv_exact(sock, 4) if masked else b""
    payload = recv_exact(sock, length)
    if masked:
        payload = bytes(payload[i] ^ mask[i % 4] for i in range(length))
    return opcode, payload


def ws_send_frame(sock: socket.socket, opcode: int, payload: bytes) -> None:
    fin_opcode = 0x80 | (opcode & 0x0F)
    length = len(payload)
    mask_flag = 0x80
    if length < 126:
        header = bytes([fin_opcode, mask_flag | length])
    elif length < 65536:
        header = bytes([fin_opcode, mask_flag | 126]) + struct.pack("!H", length)
    else:
        header = bytes([fin_opcode, mask_flag | 127]) + struct.pack("!Q", length)
    mask = os.urandom(4)
    masked_payload = bytes(payload[i] ^ mask[i % 4] for i in range(length))
    sock.sendall(header + mask + masked_payload)


@pytest.fixture(scope="session")
def mqtt_client_factory() -> Any:
    try:
        import paho.mqtt.client as mqtt  # type: ignore
    except ModuleNotFoundError:
        pytest.skip("paho-mqtt not installed")

    def _factory(cfg: dict[str, Any]) -> tuple[Any, queue.Queue[tuple[str, str]]]:
        messages: queue.Queue[tuple[str, str]] = queue.Queue()
        client = mqtt.Client(protocol=mqtt.MQTTv311)
        if cfg["username"]:
            client.username_pw_set(cfg["username"], cfg["password"])

        def on_message(_client: Any, _userdata: Any, msg: Any) -> None:
            payload = msg.payload.decode("utf-8", errors="replace")
            messages.put((msg.topic, payload))

        client.on_message = on_message
        client.connect(cfg["host"], cfg["port"], 8)
        client.loop_start()
        return client, messages

    return _factory


@pytest.fixture(scope="session")
def luce_env(pytestconfig: pytest.Config) -> str:
    return str(pytestconfig.getoption("--luce-env"))


@pytest.fixture(scope="session")
def luce_duration(pytestconfig: pytest.Config) -> int:
    return int(pytestconfig.getoption("--luce-duration"))


@pytest.fixture(scope="session")
def run_build(pytestconfig: pytest.Config) -> bool:
    return bool(pytestconfig.getoption("--luce-run-build"))


@pytest.fixture(scope="session")
def run_boot(pytestconfig: pytest.Config) -> bool:
    return bool(pytestconfig.getoption("--luce-run-boot"))


@pytest.fixture(scope="session")
def run_cmd() -> Any:
    def _run(cmd: list[str], cwd: Path) -> subprocess.CompletedProcess[str]:
        return subprocess.run(cmd, cwd=cwd, check=False, capture_output=True, text=True)

    return _run
