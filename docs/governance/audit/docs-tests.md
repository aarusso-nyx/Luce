# Docs vs Tests Audit

- date: 2026-03-01
- docs_user_files_detected: 12
- test_files_detected: 8
- status: FAIL

## Mismatches and gaps

1. **MQTT docs define extensive control/persistence behavior; tests enforce a partial slice.**
- Documented: broad `config/*`, relay/night, threshold, and reconnect semantics.
- Tests enforce unsupported-topic compatibility, LED readback, relay/night/threshold control effects, and config-topic supported-vs-unsupported routing.
- Reboot-persistence verification for `config/*` and reconnect semantics are still not covered.
- Evidence:
  - [docs/user/mqtt.md](/Users/aarusso/Development/Luce/docs/user/mqtt.md)
  - [tests/test_mqtt_contract.py](/Users/aarusso/Development/Luce/tests/test_mqtt_contract.py)

2. **Layer model documentation and test implementation are only partially symmetric.**
- Docs list `build` and `boot` as suite modules.
- `build`/`boot` are implemented in `scripts/test_layers.py`, not as pytest test modules; pytest-only runs do not cover them.
- Evidence:
  - [docs/user/testing.md](/Users/aarusso/Development/Luce/docs/user/testing.md:56)
  - [scripts/test_layers.py](/Users/aarusso/Development/Luce/scripts/test_layers.py:360)
  - [tests](/Users/aarusso/Development/Luce/tests)

## Overall

- Documentation still over-specifies behavior relative to tests in MQTT durability/reconnect and lifecycle depth, but prior HTTP route-contract gaps are now covered.
