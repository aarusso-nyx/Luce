from __future__ import annotations

import pytest

from conftest import require_serial, require_token, serial_command_expect, serial_reboot_and_capture


@pytest.mark.contract
@pytest.mark.net
@pytest.mark.hil
def test_network_lifecycle_markers_after_reboot(http_requester, luce_host, luce_http_token, serial_config):
    require_token(luce_http_token, "--luce-http-token")
    require_serial(serial_config)

    headers = {"Accept": "application/json", "Authorization": f"Bearer {luce_http_token}"}
    info = http_requester("GET", f"{luce_host}/api/info", headers=headers)
    assert info.status == 200
    network = info.json().get("network", {})
    assert isinstance(network.get("wifiConnected"), bool)
    assert isinstance(network.get("mqttConnected"), bool)
    assert isinstance(network.get("ntpSynced"), bool)

    lines = serial_reboot_and_capture(serial_config)
    text = "\n".join(lines)
    assert "[WIFI][LIFECYCLE]" in text
    assert "[NTP][LIFECYCLE]" in text
    assert "[mDNS]" in text and "state=" in text


@pytest.mark.contract
@pytest.mark.net
@pytest.mark.hil
def test_serial_cli_parser_matrix(serial_config):
    require_serial(serial_config)

    serial_command_expect(serial_config, "relay_set", ["CLI command relay_set usage"])
    serial_command_expect(serial_config, "relay_set 9 1", ["CLI command relay_set: parse error or out-of-range"])
    serial_command_expect(serial_config, "relay_mask zz", ["CLI command relay_mask: parse error"])
    serial_command_expect(serial_config, "mcp_read nope", ["CLI command mcp_read: invalid port"])
    serial_command_expect(serial_config, "led_set 3 on", ["CLI command led_set: invalid index"])
    serial_command_expect(serial_config, "led_set 1 fast", ["CLI command led_set: led[1]=fast"])
    serial_command_expect(serial_config, "led_clear all", ["CLI command led_clear: all manual overrides cleared"])
    serial_command_expect(serial_config, "definitely_unknown_cmd", ["CLI unknown command"])
