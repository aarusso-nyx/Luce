# Test Alignment Plan (Post-Implementation)

Date: 2026-02-23

## Current test inventory (from evidence)

Host unit: `pio test -e luce_test_native` -> PASS. Host unit case count: 8. Artifact: `docs/work/diag/evidence/20260222_214039/30_unit/unit_luce_test_native.txt`.
On-device captures: boot capture stage0 and stage10 -> `docs/work/diag/evidence/20260222_214039/50_boot/`; e2e CLI transcript -> `docs/work/diag/evidence/20260222_214039/60_e2e/`.

## Missing or incomplete tests

1. Allowlist enforcement and auth fail lockout for TCP CLI (stage8).  
Scope: TCP command path only, include deny path and lockout reason.  
Evidence target: `docs/work/diag/evidence/<ts>/60_e2e/cli_stage8_tcp_auth.csv` (planned).  
Acceptance: `AUTH` rejects wrong token, mutating command on TCP returns denied code, lockout occurs after repeated failed auth.
2. Host parser edge cases.  
Scope: empty lines, extreme token count, malformed UTF-8 token handling.  
Evidence target: `docs/work/diag/evidence/<ts>/30_unit/unit_pure_parser.txt` (planned).  
Acceptance: 100% deterministic outputs for whitespace handling.
3. MQTT payload schema validation.  
Scope: status payload fields and MQTT topic naming.  
Evidence target: `docs/work/diag/evidence/<ts>/30_unit/unit_mqtt_schema.txt` or host-side script logs.  
Acceptance: deterministic JSON keys and non-empty topic.
4. HTTP route auth and status enforcement.  
Scope: `/api/health`, `/api/info`, `/api/state` auth behavior.  
Evidence target: `docs/work/diag/evidence/<ts>/60_e2e/http_stage10.txt` (planned).  
Acceptance: `/api/health` available without token; protected routes return 401/403 without token.
5. Stage2 sensor/component sanity on hardware.  
Scope: `i2c_scan`, `mcp_read`, `buttons` output stability.  
Evidence target: `docs/work/diag/evidence/<ts>/60_e2e/stage2_component.txt` (planned).  
Acceptance: deterministic mask output and graceful unavailable logs when MCP absent.

## Stage coverage map

1. Unit: Stage4+ parser helpers.
2. Component: Stage2 and Stage3 hardware commands.
3. E2E: Stage4 `help/status/i2c_scan/mcp_read/buttons`.
4. E2E: Stage5+ `wifi.status`.
5. E2E: Stage6 `time.status`.
6. E2E: Stage8 TCP command allowlist.
7. E2E: Stage9 `mqtt.status` and `mqtt.pubtest`.
8. E2E: Stage10 `/api/*` endpoints.

## Evidence naming convention for alignment

Every planned run must create evidence directories:
1. `docs/work/diag/evidence/<timestamp>/20_build`
2. `docs/work/diag/evidence/<timestamp>/30_unit`
3. `docs/work/diag/evidence/<timestamp>/50_boot`
4. `docs/work/diag/evidence/<timestamp>/60_e2e`

## Immediate next pass

- Complete TCP allowlist auth tests in stage8 and HTTPS auth tests in stage10 prior to any topology changes.
