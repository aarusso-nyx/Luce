from __future__ import annotations

import time
from urllib.parse import urlsplit

import pytest

from conftest import require_token


def _json_headers(token: str | None = None) -> dict[str, str]:
    headers = {"Accept": "application/json"}
    if token:
        headers["Authorization"] = f"Bearer {token}"
    return headers


def _captive_base_url(luce_host: str) -> str:
    parsed = urlsplit(luce_host)
    host = parsed.hostname or "127.0.0.1"
    return f"http://{host}:80"


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
def test_state_and_ota_payload_contract(http_requester, luce_host, luce_http_token):
    require_token(luce_http_token, "--luce-http-token")

    headers = _json_headers(luce_http_token)

    state = http_requester("GET", f"{luce_host}/api/state", headers=headers)
    assert state.status == 200
    state_payload = state.json()
    assert {"state", "wifi_ip", "relay", "buttons", "service", "strategy"}.issubset(set(state_payload.keys()))

    ota = http_requester("GET", f"{luce_host}/api/ota", headers=headers)
    assert ota.status == 200
    ota_payload = ota.json()
    assert {"enabled", "state", "running", "checks", "success", "fail", "last_error"}.issubset(set(ota_payload.keys()))


@pytest.mark.contract
@pytest.mark.net
def test_ota_check_post_put_and_source_precedence(http_requester, luce_host, luce_http_token):
    require_token(luce_http_token, "--luce-http-token")

    headers = _json_headers(luce_http_token)
    post_default = http_requester("POST", f"{luce_host}/api/ota/check", headers=headers)
    assert post_default.status == 202
    assert post_default.json() == {"status": "queued", "source": "default"}

    post_body = http_requester(
        "POST",
        f"{luce_host}/api/ota/check",
        headers={**headers, "Content-Type": "text/plain"},
        data=b"https://example.com/firmware.bin",
    )
    assert post_body.status == 202
    assert post_body.json().get("source") == "body"

    put_query = http_requester("PUT", f"{luce_host}/api/ota/check?url=https://example.com/fw.bin", headers=headers)
    assert put_query.status == 202
    assert put_query.json().get("source") == "query"

    both = http_requester(
        "POST",
        f"{luce_host}/api/ota/check?url=https://example.com/query.bin",
        headers={**headers, "Content-Type": "text/plain"},
        data=b"https://example.com/body.bin",
    )
    assert both.status == 202
    assert both.json().get("source") == "query"


@pytest.mark.contract
@pytest.mark.net
def test_ota_lifecycle_progress_after_manual_check(http_requester, luce_host, luce_http_token):
    require_token(luce_http_token, "--luce-http-token")
    headers = _json_headers(luce_http_token)

    before = http_requester("GET", f"{luce_host}/api/ota", headers=headers)
    assert before.status == 200
    before_payload = before.json()
    before_checks = int(before_payload.get("checks", 0))
    before_state = str(before_payload.get("state", ""))
    before_enabled = bool(before_payload.get("enabled", False))

    trigger = http_requester("POST", f"{luce_host}/api/ota/check", headers=headers)
    assert trigger.status == 202

    snapshots = []
    deadline = time.monotonic() + 8.0
    while time.monotonic() < deadline:
        current = http_requester("GET", f"{luce_host}/api/ota", headers=headers)
        assert current.status == 200
        payload = current.json()
        snapshots.append(payload)
        if int(payload.get("checks", 0)) > before_checks:
            break
        time.sleep(0.4)

    assert snapshots, "missing OTA status snapshots"
    final = snapshots[-1]
    final_checks = int(final.get("checks", 0))

    assert final_checks >= before_checks
    if before_enabled:
        progressed = any(
            bool(item.get("running", False))
            or int(item.get("checks", 0)) > before_checks
            or str(item.get("state", "")) != before_state
            for item in snapshots
        )
        assert progressed, "enabled OTA should show lifecycle progress after manual check request"
    else:
        assert bool(final.get("enabled", False)) is False
        assert final_checks == before_checks


@pytest.mark.contract
@pytest.mark.net
def test_ota_failure_class_mapping_with_invalid_url(http_requester, luce_host, luce_http_token):
    require_token(luce_http_token, "--luce-http-token")
    headers = _json_headers(luce_http_token)

    before = http_requester("GET", f"{luce_host}/api/ota", headers=headers)
    assert before.status == 200
    before_payload = before.json()
    before_fail = int(before_payload.get("fail", 0))
    before_enabled = bool(before_payload.get("enabled", False))

    trigger = http_requester(
        "POST",
        f"{luce_host}/api/ota/check",
        headers={**headers, "Content-Type": "text/plain"},
        data=b"not-a-valid-url",
    )
    assert trigger.status == 202

    deadline = time.monotonic() + 10.0
    snapshots = []
    while time.monotonic() < deadline:
        current = http_requester("GET", f"{luce_host}/api/ota", headers=headers)
        assert current.status == 200
        payload = current.json()
        snapshots.append(payload)
        if int(payload.get("fail", 0)) > before_fail:
            break
        time.sleep(0.4)

    assert snapshots
    final = snapshots[-1]
    if before_enabled:
        assert int(final.get("fail", 0)) >= before_fail
        assert str(final.get("state", "")) in {"FAILED", "INVALID_CONFIG", "NO_PARTITION", "CHECKING", "IDLE", "SUCCESS"}
        assert isinstance(final.get("last_error", ""), str)
        assert final.get("last_error", "") != ""
    else:
        assert bool(final.get("enabled", False)) is False
        assert int(final.get("fail", 0)) == before_fail


@pytest.mark.contract
@pytest.mark.net
def test_ota_periodic_cadence_when_configured(http_requester, luce_host, luce_http_token):
    require_token(luce_http_token, "--luce-http-token")
    headers = _json_headers(luce_http_token)

    before = http_requester("GET", f"{luce_host}/api/ota", headers=headers)
    assert before.status == 200
    before_payload = before.json()

    if not bool(before_payload.get("enabled", False)):
        pytest.skip("OTA disabled")

    interval_s = int(before_payload.get("interval_s", 0))
    if interval_s <= 0:
        pytest.skip("periodic OTA disabled by config (interval_s=0)")
    if interval_s > 60:
        pytest.skip(f"periodic OTA interval too long for contract run ({interval_s}s)")

    before_checks = int(before_payload.get("checks", 0))
    deadline = time.monotonic() + interval_s + 8.0
    while time.monotonic() < deadline:
        current = http_requester("GET", f"{luce_host}/api/ota", headers=headers)
        assert current.status == 200
        if int(current.json().get("checks", 0)) > before_checks:
            return
        time.sleep(0.5)
    pytest.fail(f"expected periodic OTA check increment within {interval_s + 8}s")


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

    set_idx0 = http_requester("PUT", f"{luce_host}/api/leds/state/0?value=auto", headers=headers)
    assert set_idx0.status == 200
    assert set_idx0.json().get("index") == 0

    set_idx2 = http_requester("PUT", f"{luce_host}/api/leds/state/2?value=1", headers=headers)
    assert set_idx2.status == 200
    assert set_idx2.json().get("index") == 2

    get_idx0 = http_requester("GET", f"{luce_host}/api/leds/state/0", headers=headers)
    get_idx2 = http_requester("GET", f"{luce_host}/api/leds/state/2", headers=headers)
    assert get_idx0.status == 200
    assert get_idx2.status == 200

    invalid = http_requester("PUT", f"{luce_host}/api/leds/state?value=invalid", headers=headers)
    assert invalid.status == 400
    invalid_idx = http_requester("PUT", f"{luce_host}/api/leds/state/0?value=invalid", headers=headers)
    assert invalid_idx.status == 400


@pytest.mark.contract
@pytest.mark.net
def test_captive_routes_and_spa_fallback(http_requester, luce_host):
    captive = _captive_base_url(luce_host)

    root = http_requester("GET", f"{captive}/", headers={"Accept": "text/html"})
    index = http_requester("GET", f"{captive}/index.html", headers={"Accept": "text/html"})
    css = http_requester("GET", f"{captive}/app.css")
    script = http_requester("GET", f"{captive}/script.js")
    fallback = http_requester("GET", f"{captive}/ui/deep/link")

    assert root.status == 200
    assert index.status == 200
    assert css.status == 200
    assert script.status == 200
    assert fallback.status == 200

    index_body = index.body
    assert len(index_body) > 0
    assert root.body == index_body
    assert fallback.body == index_body

    assert "text/html" in index.headers.get("Content-Type", "")
    assert "text/css" in css.headers.get("Content-Type", "")
    assert "text/javascript" in script.headers.get("Content-Type", "")


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

    headers_auth = _json_headers(luce_http_token) if luce_http_token else _json_headers()
    post_led_idx = http_requester("POST", f"{luce_host}/api/leds/state/0", headers=headers_auth)
    assert post_led_idx.status == 405
