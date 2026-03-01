from __future__ import annotations

import socket

import pytest

from conftest import recv_line, send_line


@pytest.mark.contract
@pytest.mark.net
@pytest.mark.hil
def test_tcp_cli_auth_fail_then_abort(tcp_config):
    host, port, _token = tcp_config

    with socket.create_connection((host, port), timeout=5.0) as sock:
        prompt = recv_line(sock)
        assert "AUTH <token>" in prompt

        for idx in range(3):
            send_line(sock, "AUTH wrong-token")
            line = recv_line(sock)
            assert "auth fail" in line
            if idx == 2:
                line2 = recv_line(sock)
                assert "session aborted" in line2


@pytest.mark.contract
@pytest.mark.net
@pytest.mark.hil
def test_tcp_cli_readonly_policy(tcp_config):
    host, port, token = tcp_config
    if not token:
        pytest.skip("missing --luce-tcp-token")

    with socket.create_connection((host, port), timeout=5.0) as sock:
        prompt = recv_line(sock)
        assert "AUTH <token>" in prompt

        send_line(sock, f"AUTH {token}")
        assert "AUTH ok" in recv_line(sock)

        send_line(sock, "status")
        assert "OK" in recv_line(sock)

        send_line(sock, "help")
        assert "OK" in recv_line(sock)

        send_line(sock, "cli_net.status")
        assert "DENIED" in recv_line(sock)

        send_line(sock, "relay_set 0 1")
        assert "DENIED" in recv_line(sock)

        send_line(sock, "mqtt.pubtest")
        assert "DENIED" in recv_line(sock)
