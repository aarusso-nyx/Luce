from __future__ import annotations

import pytest

from conftest import require_token


def _json_headers(token: str | None = None) -> dict[str, str]:
    headers = {"Accept": "application/json"}
    if token:
        headers["Authorization"] = f"Bearer {token}"
    return headers


@pytest.mark.contract
@pytest.mark.net
def test_public_health_and_version(http_requester, luce_host):
    health = http_requester("GET", f"{luce_host}/api/health", headers=_json_headers())
    assert health.status == 200
    assert health.json().get("service") == "luce"

    version = http_requester("GET", f"{luce_host}/api/version", headers=_json_headers())
    assert version.status == 200
    payload = version.json()
    assert payload.get("service") == "luce"
    assert isinstance(payload.get("version"), str)
    assert isinstance(payload.get("strategy"), str)


@pytest.mark.contract
@pytest.mark.net
def test_protected_endpoints_require_auth(http_requester, luce_host):
    for route in ("/api/info", "/api/state", "/api/ota", "/api/leds/state"):
        resp = http_requester("GET", f"{luce_host}{route}", headers=_json_headers())
        assert resp.status == 401, route


@pytest.mark.contract
@pytest.mark.net
def test_info_payload_contract(http_requester, luce_host, luce_http_token):
    require_token(luce_http_token, "--luce-http-token")
    resp = http_requester("GET", f"{luce_host}/api/info", headers=_json_headers(luce_http_token))
    assert resp.status == 200

    payload = resp.json()
    required_keys = {
        "service",
        "name",
        "version",
        "strategy",
        "sha",
        "build",
        "uptimeMs",
        "uptime_s",
        "wifi_ip",
        "relays",
        "nightMask",
        "threshold",
        "network",
    }
    assert required_keys.issubset(set(payload.keys()))

    network = payload["network"]
    assert {"ip", "wifiConnected", "mqttConnected", "ntpSynced"}.issubset(set(network.keys()))


@pytest.mark.contract
@pytest.mark.net
def test_ota_check_post_put(http_requester, luce_host, luce_http_token):
    require_token(luce_http_token, "--luce-http-token")

    headers = _json_headers(luce_http_token)
    post_default = http_requester("POST", f"{luce_host}/api/ota/check", headers=headers)
    assert post_default.status == 202
    assert post_default.json().get("status") == "queued"

    post_body = http_requester(
        "POST",
        f"{luce_host}/api/ota/check",
        headers={**headers, "Content-Type": "text/plain"},
        data=b"https://example.com/firmware.bin",
    )
    assert post_body.status == 202

    put_query = http_requester("PUT", f"{luce_host}/api/ota/check?url=https://example.com/fw.bin", headers=headers)
    assert put_query.status == 202


@pytest.mark.contract
@pytest.mark.net
def test_led_routes_and_validation(http_requester, luce_host, luce_http_token):
    require_token(luce_http_token, "--luce-http-token")

    headers = _json_headers(luce_http_token)
    state = http_requester("GET", f"{luce_host}/api/leds/state", headers=headers)
    assert state.status == 200

    payload = state.json()
    assert "state" in payload
    assert "manual_enabled" in payload
    assert "manual_value" in payload

    set_all = http_requester("PUT", f"{luce_host}/api/leds/state?value=flash", headers=headers)
    assert set_all.status == 200
    all_payload = set_all.json()
    assert all_payload.get("mode0") == "flash"
    assert all_payload.get("mode1") == "flash"
    assert all_payload.get("mode2") == "flash"

    set_idx = http_requester("PUT", f"{luce_host}/api/leds/state/1?value=slow", headers=headers)
    assert set_idx.status == 200
    idx_payload = set_idx.json()
    assert idx_payload.get("index") == 1
    assert idx_payload.get("mode") == "slow"

    invalid = http_requester("PUT", f"{luce_host}/api/leds/state?value=invalid", headers=headers)
    assert invalid.status == 400


@pytest.mark.contract
@pytest.mark.net
def test_method_not_allowed(http_requester, luce_host, luce_http_token):
    headers_auth = _json_headers(luce_http_token) if luce_http_token else _json_headers()

    post_info = http_requester("POST", f"{luce_host}/api/info", headers=headers_auth)
    assert post_info.status == 405

    patch_health = http_requester("PATCH", f"{luce_host}/api/health", headers=_json_headers())
    assert patch_health.status == 405

    put_version = http_requester("PUT", f"{luce_host}/api/version", headers=_json_headers())
    assert put_version.status == 405
