from __future__ import annotations

import json
import time

import pytest


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
        topic, payload = messages.get(timeout=8)
        assert topic == compat_topic
        data = json.loads(payload)
        assert data["status"] == "unsupported"
        assert data["topic"] == "leds/state/9"
        assert data["reason"] == "unsupported_led_index"

        client.publish(led_topic, payload="flash", qos=0)

        observed = None
        deadline = time.monotonic() + 8.0
        while time.monotonic() < deadline:
            topic, payload = messages.get(timeout=2)
            if topic == led_topic and payload.isdigit() and 0 <= int(payload) <= 7:
                observed = payload
                break

        assert observed is not None, "no LEDs state readback 0..7 after leds/state command"
    finally:
        client.loop_stop()
        client.disconnect()
