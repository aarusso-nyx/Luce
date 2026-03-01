from __future__ import annotations

import json
import os
import queue
import signal
import time

import pytest

from conftest import require_serial, require_token, serial_reboot_and_capture, spawn_local_amqtt_broker


def _wait_for_message(messages, matcher, timeout_s: float = 8.0):
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        try:
            topic, payload = messages.get(timeout=2)
        except queue.Empty:
            continue
        if matcher(topic, payload):
            return topic, payload
    raise AssertionError("timed out waiting for matching MQTT message")


def _drain_messages(messages) -> None:
    while True:
        try:
            messages.get_nowait()
        except queue.Empty:
            return


def _assert_no_matching_message(messages, matcher, timeout_s: float = 2.0) -> None:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        try:
            topic, payload = messages.get(timeout=0.25)
        except queue.Empty:
            continue
        if matcher(topic, payload):
            raise AssertionError(f"unexpected MQTT message topic={topic} payload={payload}")


def _topic_field(payload: str) -> str:
    try:
        return str(json.loads(payload).get("topic", ""))
    except (json.JSONDecodeError, TypeError, ValueError):
        return ""


def _info_payload(http_requester, luce_host: str, token: str):
    headers = {"Accept": "application/json", "Authorization": f"Bearer {token}"}
    resp = http_requester("GET", f"{luce_host}/api/info", headers=headers)
    assert resp.status == 200
    return resp.json()


def _wait_mqtt_connected_state(http_requester, luce_host: str, token: str, connected: bool, timeout_s: float) -> bool:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        payload = _info_payload(http_requester, luce_host, token)
        network = payload.get("network", {})
        if bool(network.get("mqttConnected", False)) is connected:
            return True
        time.sleep(0.5)
    return False


@pytest.mark.contract
@pytest.mark.net
@pytest.mark.hil
def test_mqtt_unsupported_topic_compat_and_led_readback(mqtt_config, mqtt_client_factory):
    client, messages = mqtt_client_factory(mqtt_config)
    base = mqtt_config["base_topic"]

    compat_topic = f"{base}/compat/unsupported"
    led_topic = f"{base}/leds/state"

    try:
        client.subscribe(compat_topic, qos=0)
        client.subscribe(led_topic, qos=0)
        time.sleep(0.3)

        client.publish(f"{base}/relays/legacy", payload="1", qos=0)
        topic, payload = messages.get(timeout=8)
        assert topic == compat_topic
        data = json.loads(payload)
        assert data["status"] == "unsupported"
        assert data["topic"] == "relays/legacy"
        assert data["payload_present"] is True
        assert isinstance(data.get("reason"), str)

        client.publish(f"{base}/leds/state/9", payload="on", qos=0)
        topic, payload = _wait_for_message(messages, lambda t, _p: t == compat_topic, timeout_s=8)
        assert topic == compat_topic
        data = json.loads(payload)
        assert data["status"] == "unsupported"
        assert data["topic"] == "leds/state/9"
        assert data["reason"] == "unsupported_led_index"

        client.publish(led_topic, payload="flash", qos=0)

        _topic, observed = _wait_for_message(
            messages,
            lambda t, p: t == led_topic and p.isdigit() and 0 <= int(p) <= 7,
            timeout_s=8,
        )
        assert observed is not None, "no LEDs state readback 0..7 after leds/state command"
    finally:
        client.loop_stop()
        client.disconnect()


@pytest.mark.contract
@pytest.mark.net
@pytest.mark.hil
def test_mqtt_config_topics_supported_vs_unsupported(mqtt_config, mqtt_client_factory):
    client, messages = mqtt_client_factory(mqtt_config)
    base = mqtt_config["base_topic"]
    compat_topic = f"{base}/compat/unsupported"
    supported_subtopic = "config/mqtt/keepalive_s"
    unsupported_subtopic = "config/this_key_does_not_exist"

    try:
        client.subscribe(compat_topic, qos=0)
        time.sleep(0.3)
        _drain_messages(messages)

        client.publish(f"{base}/{supported_subtopic}", payload="120", qos=0)
        _assert_no_matching_message(
            messages,
            lambda t, p: t == compat_topic and _topic_field(p) == supported_subtopic,
            timeout_s=2.0,
        )

        client.publish(f"{base}/{unsupported_subtopic}", payload="value", qos=0)
        topic, payload = _wait_for_message(
            messages,
            lambda t, p: t == compat_topic and _topic_field(p) == unsupported_subtopic,
            timeout_s=8,
        )
        assert topic == compat_topic
        data = json.loads(payload)
        assert data["status"] == "unsupported"
        assert data["topic"] == unsupported_subtopic
        assert data["reason"] == "unsupported_or_readonly"
        assert data["payload_present"] is True
    finally:
        client.loop_stop()
        client.disconnect()


@pytest.mark.contract
@pytest.mark.net
@pytest.mark.hil
def test_mqtt_config_http_token_persists_after_reboot(
    mqtt_config,
    mqtt_client_factory,
    http_requester,
    luce_host,
    luce_http_token,
    serial_config,
):
    require_token(luce_http_token, "--luce-http-token")
    require_serial(serial_config)

    client, _messages = mqtt_client_factory(mqtt_config)
    base = mqtt_config["base_topic"]
    old_token = luce_http_token
    new_token = f"luce-temp-{int(time.time())}"
    restored = False

    def _auth_status(token: str) -> int:
        headers = {"Accept": "application/json", "Authorization": f"Bearer {token}"}
        return http_requester("GET", f"{luce_host}/api/info", headers=headers).status

    def _set_http_token(value: str) -> None:
        client.publish(f"{base}/config/http/token", payload=value, qos=0)
        time.sleep(0.8)

    try:
        _set_http_token(new_token)
        serial_reboot_and_capture(serial_config)
        assert _auth_status(old_token) == 401

        deadline = time.monotonic() + 25.0
        while time.monotonic() < deadline:
            if _auth_status(new_token) == 200:
                break
            time.sleep(0.6)
        else:
            pytest.fail("new HTTP token did not authenticate after reboot")

        _set_http_token(old_token)
        serial_reboot_and_capture(serial_config)
        deadline = time.monotonic() + 25.0
        while time.monotonic() < deadline:
            if _auth_status(old_token) == 200:
                restored = True
                break
            time.sleep(0.6)
        assert restored, "failed to restore original HTTP token after reboot"
    finally:
        client.loop_stop()
        client.disconnect()


@pytest.mark.contract
@pytest.mark.net
@pytest.mark.hil
def test_mqtt_control_paths_reflect_in_http_state(
    mqtt_config,
    mqtt_client_factory,
    http_requester,
    luce_host,
    luce_http_token,
):
    require_token(luce_http_token, "--luce-http-token")
    headers = {"Accept": "application/json", "Authorization": f"Bearer {luce_http_token}"}

    client, _messages = mqtt_client_factory(mqtt_config)
    base = mqtt_config["base_topic"]

    try:
        client.publish(f"{base}/relays/state", payload="5", qos=0)
        time.sleep(0.6)
        info = http_requester("GET", f"{luce_host}/api/info", headers=headers)
        assert info.status == 200
        assert int(info.json().get("relays")) == 5

        client.publish(f"{base}/relays/state/1", payload="on", qos=0)
        time.sleep(0.6)
        info = http_requester("GET", f"{luce_host}/api/info", headers=headers)
        assert info.status == 200
        assert int(info.json().get("relays")) == 7

        threshold_value = "1234"
        client.publish(f"{base}/sensor/threshold", payload=threshold_value, qos=0)
        time.sleep(0.6)
        info = http_requester("GET", f"{luce_host}/api/info", headers=headers)
        assert info.status == 200
        info_payload = info.json()
        assert int(info_payload.get("threshold")) == int(threshold_value)

        client.publish(f"{base}/relays/night", payload="3", qos=0)
        time.sleep(0.6)
        info = http_requester("GET", f"{luce_host}/api/info", headers=headers)
        assert info.status == 200
        assert int(info.json().get("nightMask")) == 3

        client.publish(f"{base}/relays/night/2", payload="off", qos=0)
        time.sleep(0.6)
        info = http_requester("GET", f"{luce_host}/api/info", headers=headers)
        assert info.status == 200
        assert int(info.json().get("nightMask")) == 3
    finally:
        client.loop_stop()
        client.disconnect()


@pytest.mark.contract
@pytest.mark.net
@pytest.mark.hil
def test_mqtt_reconnect_after_managed_broker_restart(
    mqtt_config,
    managed_mqtt_broker_pid,
    http_requester,
    luce_host,
    luce_http_token,
):
    require_token(luce_http_token, "--luce-http-token")
    if managed_mqtt_broker_pid <= 0:
        pytest.skip("requires --luce-test-mqtt-broker-pid from managed broker mode")

    if not _wait_mqtt_connected_state(http_requester, luce_host, luce_http_token, connected=True, timeout_s=20.0):
        pytest.skip("device MQTT client is not connected to the managed broker")

    try:
        os.kill(managed_mqtt_broker_pid, signal.SIGTERM)
    except ProcessLookupError:
        pytest.skip("managed broker process no longer running")

    disconnected = _wait_mqtt_connected_state(http_requester, luce_host, luce_http_token, connected=False, timeout_s=30.0)
    assert disconnected, "mqttConnected did not drop after broker termination"

    replacement = spawn_local_amqtt_broker(mqtt_config["host"], int(mqtt_config["port"]))
    try:
        reconnected = _wait_mqtt_connected_state(http_requester, luce_host, luce_http_token, connected=True, timeout_s=45.0)
        assert reconnected, "mqttConnected did not recover after broker restart"
    finally:
        if replacement.poll() is None:
            replacement.terminate()
            try:
                replacement.wait(timeout=5)
            except Exception:
                replacement.kill()
