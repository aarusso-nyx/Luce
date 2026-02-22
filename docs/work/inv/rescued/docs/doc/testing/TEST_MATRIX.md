# Luce Test Matrix

## Host (native) suites

1. `protocol_extract_auth_token` supports `Bearer` and `X-Auth-Token`.
2. `protocol_parse_query_int` validates query parsing.
3. `protocol_parse_mqtt_command_payload` accepts token-prefixed and bare payloads.
4. `protocol_parse_query_key` edge-case keys and missing values.
5. `policy_apply_button_authority` blocks/lets remote changes in guard window.
6. `policy_compute_automation` applies night masks and override windows correctly.

## Embedded (target) suites

7. Boot initializes schema defaults and migration-compatible config when NVS is fresh.
8. Runtime save/load roundtrip preserves relay/night/threshold state.
9. Event bus depth reporting is valid and non-regressing.

## HIL suites

10. `boot`: canonical `/api/info`, `/api/version`, `/api/health`.
11. `http_contract`: canonical GET routes and auth enforcement on mutating endpoints.
12. `mqtt_contract`: canonical command topic handling and request acks.
13. `relays`: relay state mutation through canonical HTTP path.
14. `security`: auth errors emit canonical `{error:{...}}` envelope.
15. `cli_contract`: serial CLI help/network command visibility.
16. `ota`: OTA endpoint available and input validation contract.

## Security / reliability acceptance

17. Unauthorized write attempts are rejected with `401` and structured errors.
18. Bad token attempts are rejected consistently.
19. OTA command rejects non-HTTPS or malformed trust material.
20. Event stream emits `text/event-stream`.
21. No `/v2` routes are required by scenario paths.
22. Physical override behavior is reflected in relay status endpoints.
23. Retry behavior handles infra failures (broker, serial, transient HTTP errors) with one retry.
