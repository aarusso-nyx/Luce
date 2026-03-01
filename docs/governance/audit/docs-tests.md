# Docs vs Tests Audit

- date: 2026-03-01
- docs_user_files_detected: 12
- test_files_detected: 9
- status: PASS

## Findings

- No open docs-vs-tests mismatches were found in the previously failing areas.

## Evidence

- MQTT documentation now states explicit coverage scope matching enforced tests.
  - [docs/user/mqtt.md](/Users/aarusso/Development/Luce/docs/user/mqtt.md)
  - [tests/test_mqtt_contract.py](/Users/aarusso/Development/Luce/tests/test_mqtt_contract.py)
- Test workflow documentation now explicitly distinguishes runner-native layers (`build`, `boot`) from pytest layers.
  - [docs/user/testing.md](/Users/aarusso/Development/Luce/docs/user/testing.md:54)
  - [tests/README.md](/Users/aarusso/Development/Luce/tests/README.md:21)
  - [scripts/test_layers.py](/Users/aarusso/Development/Luce/scripts/test_layers.py:24)

## Notes

- Residual partial coverage (for broad `config/*` matrices and reconnect timing bounds) is now documented as partial scope rather than asserted as fully enforced.
