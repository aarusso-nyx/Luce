from __future__ import annotations

import json

import pytest

from conftest import ws_connect, ws_recv_frame, ws_send_frame


REQUIRED_WS_FIELDS = {
    "type",
    "tstamp",
    "state",
    "night",
    "day",
    "threshold",
    "light",
    "voltage",
    "temperature",
    "humidity",
    "sensor_ok",
}


def _assert_snapshot(payload: bytes) -> None:
    data = json.loads(payload.decode("utf-8", errors="replace"))
    assert REQUIRED_WS_FIELDS.issubset(set(data.keys()))
    assert data["type"] == "state"


@pytest.mark.contract
@pytest.mark.net
@pytest.mark.hil
def test_ws_handshake_and_snapshot(ws_config):
    host, port, path, tls = ws_config

    sock = ws_connect(host, port, path, tls)
    try:
        opcode, payload = ws_recv_frame(sock)
        assert opcode == 0x1
        _assert_snapshot(payload)

        ws_send_frame(sock, 0x9, b"ping")
        opcode, payload = ws_recv_frame(sock)
        assert opcode in (0xA, 0x1)
        if opcode == 0x1:
            _assert_snapshot(payload)

        ws_send_frame(sock, 0x1, b"state")
        opcode, payload = ws_recv_frame(sock)
        assert opcode == 0x1
        _assert_snapshot(payload)
    finally:
        try:
            ws_send_frame(sock, 0x8, b"\x03\xe8")
        except Exception:  # noqa: BLE001
            pass
        sock.close()
