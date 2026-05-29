# LUCE Firmware Test Process — Complete Reference

This is the authoritative operational reference for the entire LUCE firmware test
process: testing philosophy, environments, prerequisites, every command and
argument, per-layer procedures, the evidence artifacts produced, and the
CI/release gating.

---

## 1. Testing philosophy

LUCE separates verification into two complementary lanes:

1. **Hardware-free checks** — deterministic, fast, run anywhere (laptop or CI):
   - **Host unit tests** of pure C++ logic (no ESP-IDF, no device).
   - **Docs-alignment** test (source ↔ contract-doc parity).
   - **clang-format** style gate.
2. **Hardware-backed evidence** — exercises real firmware behavior:
   - **Build** (PlatformIO compile of the firmware).
   - **Boot** (flash + serial capture + boot-marker assertions).
   - **Contract layers** (HTTP, TCP CLI, WebSocket, MQTT, serial CLI) against a
     running device.

Hardware-backed evidence is the **release gate**. It is always produced
**locally** (it needs a board and a network). CI never flashes hardware; it only
runs the hardware-free checks directly and **validates the committed local
evidence manifest** (freshness + checksums). See §10–12.

Native host tests cover pure C++ helpers without ESP32 hardware. They do **not**
replace firmware, serial, relay, button, LCD, I2C, or network contract evidence.

```
                         ┌─────────────────────────────────────────────┐
                         │  Hardware-free (laptop or CI, deterministic) │
                         │   • scripts/run_host_tests.sh  (unit logic)  │
                         │   • pytest tests/test_docs_alignment.py      │
                         │   • clang-format --dry-run --Werror          │
                         └─────────────────────────────────────────────┘
                                            │
   ┌────────────────────────────────────────────────────────────────────────┐
   │  Hardware-backed (local only — needs board + network)                    │
   │   scripts/luce.sh test  ──▶  scripts/test_layers.py                      │
   │     build → boot → [http, tcp, ws, mqtt, serial]                         │
   │     (pytest layers delegate to tests/test_*.py via conftest.py fixtures) │
   └────────────────────────────────────────────────────────────────────────┘
                                            │
                         ┌─────────────────────────────────────────────┐
                         │  Evidence: docs/work/diag/<run_id>/...        │
                         │  Manifest: docs/governance/health/*.json      │
                         │  CI gate:  scripts/verify_evidence.py         │
                         └─────────────────────────────────────────────┘
```

---

## 2. Build environments (PlatformIO)

Defined in `platformio.ini`; all share the single canonical `sdkconfig`.

| Env | Feature flags | Includes | Typical test target |
|-----|---------------|----------|---------------------|
| `default` | none | NVS, I2C, LCD, CLI | build only |
| `net0` | `LUCE_NET_CORE=1` | + Wi-Fi, NTP, mDNS, TCP CLI | build, boot, serial |
| `net1` | `LUCE_NET_CORE=1` `LUCE_NET_MQTT=1` `LUCE_NET_HTTP=1` `LUCE_NET_OTA=1` | + MQTT, HTTPS, OTA | **all layers (canonical)** |

`net1` is the canonical contract-test target because it enables every transport.

---

## 3. Prerequisites & one-time setup

### 3.1 Toolchain
- **PlatformIO + ESP-IDF v6.0.1** (pinned in `dependencies.lock`). `pio` must be
  on `PATH`. The scripts source `~/.zshrc` first so `pio` resolves in
  non-login shells:
  ```bash
  source ~/.zshrc
  ```
  Prefer `pio`; fall back to `python3 -m platformio` only when `pio` is absent.
- **CMake** + a host C++17 compiler (`c++`/`g++`/`clang++`) for host unit tests.
- **clang-format** for the style gate.

### 3.2 Python contract-test dependencies (repo-local venv)
Homebrew-managed Python may reject system installs; always use a repo-local venv:
```bash
python3 -m venv .venv
.venv/bin/python -m pip install -r tests/requirements.txt
```
`tests/requirements.txt`:
```
pytest>=8.0
paho-mqtt>=2.0          # MQTT client used by the mqtt layer
amqtt>=0.11             # ephemeral in-process broker (--spawn-test-mqtt-broker)
pyserial>=3.5           # serial/HIL layers
esp-idf-nvs-partition-gen>=0.1   # Wokwi NVS image generation
```

### 3.3 Hardware (for hardware-backed layers)
- An ESP32 board (`nodemcu-32s`) connected via USB serial.
- Ports (override if different):
  - Upload: `LUCE_UPLOAD_PORT` (default `/dev/cu.usbserial-0001`)
  - Monitor: `LUCE_MONITOR_PORT` (default `/dev/cu.usbserial-40110`)
- The device must be provisioned (Wi-Fi credentials, HTTP/CLI tokens, MQTT
  broker) for the network layers. See `docs/user/nvs-schema.md`.

### 3.4 Optional: Wokwi simulator lane
- `wokwi-cli` on `PATH`; `WOKWI_CLI_TOKEN` set.
- Supplementary only — relay/button/LCD/I2C peripherals are not modeled, so it
  is **not** release evidence.

---

## 4. The three lanes in detail

### Lane A — Host unit tests (pure logic, no hardware)

**Command:**
```bash
scripts/run_host_tests.sh
```
**What it does:**
1. Configures + builds `test/host/` with CMake (falls back to a direct
   `c++ -std=c++17 -Wall -Wextra -Werror` compile if CMake is absent).
2. Runs the `luce_host_tests` binary (custom `minitest.h` harness).
3. Writes evidence under `docs/work/diag/<run_id>/host-tests/`:
   `summary.json`, `junit.xml`, `host-tests.log`.
4. Exits non-zero on any build or assertion failure.

**Covered pure modules** (`test/host/test_*.cpp`, headers under `include/luce/`):

| Test file | Header under test | Focus |
|-----------|-------------------|-------|
| `test_id_set_parser.cpp` | `id_set_parser.h` | `"1,3-5,all"` parsing, ranges, reversed, out-of-range, truncation |
| `test_relay_logic.cpp` | `relay_logic.h` | state↔output involution, night-policy truth table |
| `test_backoff.cpp` | `backoff.h` | capped exponential growth, jitter bounds, reset |
| `test_json_writer.cpp` | `json_writer.h` | escaping, truncation safety, field types |
| `test_str_utils.cpp` | `str_utils.h` | starts_with, trim, bool/u32 token parsing |
| `test_nvs_helpers.cpp` | `nvs_helpers.h` | `StringReadStatus` (missing/oversized/read-error) |
| `test_pki_helpers.cpp` | `pki_helpers.h` | CSR subject formatting, fingerprint hex, state transitions |
| `test_http_route_logic.cpp` | `http_route_logic.h` | method-mask + auth-required decision |
| `test_led_manual_mode.cpp` | `led_manual_mode.h` | LED manual-mode token parsing |

Run override: set `LUCE_RUN_ID` to control the output folder name; set `CXX` to
choose the fallback compiler.

### Lane B — Layered firmware/contract tests

The wrapper `scripts/luce.sh test` delegates to `python3 scripts/test_layers.py`,
adding `--diag-root` and `--run-id` and translating `--duration` →
`--boot-duration`. A wrapper log is written to
`docs/work/diag/<run_id>/test/test.txt`. `build` and `boot` are executed by
`test_layers.py` itself (they are not pytest modules); running `pytest` alone
validates the contract layers only.

**Layers** (`build`, `boot` are runner-native; the rest are pytest):

| Layer | Kind | pytest file | Asserts |
|-------|------|-------------|---------|
| `build` | native | — | PlatformIO compile of `--env` succeeds |
| `boot` | native | — | Upload, capture serial for `--boot-duration`, match boot markers |
| `http` | pytest | `test_http_contract.py` | HTTPS auth/method/payload, LED + OTA routes, captive SPA |
| `tcp` | pytest | `test_tcp_cli_contract.py` | TCP CLI AUTH, 3-fail abort, read-only policy |
| `ws` | pytest | `test_ws_contract.py` | `/ws` handshake + snapshot payload + ping/pong |
| `mqtt` | pytest | `test_mqtt_contract.py` | compat responses, control paths, token persistence, reconnect |
| `serial` | pytest | `test_serial_cli_contract.py` | lifecycle reboot markers + CLI parser matrix |

**Layer groups** (alias for `--layers`):
- `all` / `full` → `build,boot,http,tcp,ws,mqtt,serial`
- `critical` → `http,tcp,ws,mqtt`

**Boot markers asserted** (NET1):
- `LUCE STRATEGY=NET1`
- `Feature flags: NVS=1 I2C=1 LCD=1 CLI=1 WIFI=1 NTP=1 mDNS=1 MQTT=1 HTTP=1`
- plus any extra `--require-marker` values.

**Preflight — fail-loud, no silent skips.** When a layer is *selected* but its
prerequisites are missing, the runner emits a `FAIL`/`PREREQ_MISSING` result and
(unless `--continue-on-fail`) stops. Missing HTTP/TCP tokens, serial ports,
broker connectivity, or Python dependencies are reported as layer failures in
`summary.md` and `summary.json` — never silent skips.

| Layer | Prerequisite checked | Failure message |
|-------|----------------------|-----------------|
| `boot` (hardware) | upload + monitor ports exist | missing device files |
| `http` | `--http-token` present | "missing HTTP token; provide --http-token or LUCE_HTTP_TOKEN" |
| `tcp` | `--tcp-token` present | "missing TCP CLI token; provide --tcp-token or LUCE_CLI_NET_TOKEN" |
| `serial` | monitor port + pyserial installed | missing port or pyserial |
| `mqtt` | `--http-token` set **and** broker reachable (1.5 s) unless `--spawn-test-mqtt-broker` | missing token or broker unreachable |

A `SKIP`/`DESELECTED` status is reserved for layers that were **not selected**, or
for simulator-unsupported layers (`deselected:wokwi`). This keeps coverage loss
visible in the summary rather than hidden.

**Result statuses:** `PASS`, `FAIL`, `PREREQ_MISSING`, `DESELECTED`.

### Lane C — CI gate (hardware-free + evidence validation)

`.github/workflows/ci.yml` runs two jobs on PR / push to `main`, `codex/**`,
`tls-prod-provisioning`:

1. **`hardware-free-checks`** (runs directly — no device needed):
   - `scripts/run_host_tests.sh`
   - `python -m pytest tests/test_docs_alignment.py`
   - `clang-format --dry-run --Werror` over the whole tracked
     `src/**` + `include/luce/**` tree.
2. **`verify-evidence`**: `python scripts/verify_evidence.py` — validates the
   committed evidence manifest (see §10). On a **release context** it requires
   `hardware-hil` to be `required:true` and `status:PASS`.

---

## 5. Command reference

### 5.1 `scripts/luce.sh` (canonical entry point)

Global options: `-e/--env <env>`, `--upload-port <path>`, `--monitor-port <path>`,
`--diag-dir <path>`, `--dry-run`, `-v/--verbose`, `-h/--help`.

| Subcommand | Purpose | Key args |
|------------|---------|----------|
| `build` | Build one env (or all) | `--env <env>`, `--all` |
| `upload` | Flash firmware | `--env <env>` |
| `monitor` | Open serial monitor | `--env <env>` |
| `collect` | Upload + capture serial logs | `--env`, `--tag`, `--duration`, `--upload-port`, `--monitor-port` |
| `lint` | PlatformIO static check | `--env`, `--all`, `--min-severity {low\|medium\|high\|error}`, `--waiver-file` |
| `test` | Layered firmware/contract tests | `--env`, `--duration`→`--boot-duration`, `--upload-port`, `--monitor-port`, + all `test_layers.py` flags |
| `health` | Tooling/env preflight | `--run-build`, `--run-lint` |
| `http-smoke` | HTTP API smoke vs running device | `--host <url>`, `--token`, `--skip-unauth` |
| `clean` | PlatformIO clean | `--env`, `--all` |
| `all` | Convenience chain | `--upload`, `--monitor` |

`luce.sh test --help` delegates to `test_layers.py --help`.

### 5.2 `scripts/test_layers.py` arguments

| Flag | Type | Default (env) | Purpose |
|------|------|---------------|---------|
| `--target` | `hardware`\|`wokwi` | `hardware` (`LUCE_TEST_TARGET`) | Execution target |
| `--layers` | csv/group | `all` | Layers or group to run |
| `--continue-on-fail` | flag | off | Run all layers despite failures |
| `--layer-timeout-s` | float | `0.0` (`LUCE_LAYER_TIMEOUT_S`) | Per-command timeout; 0 = none |
| `--env` | str | `net1` (`LUCE_ENV`) | PlatformIO env for build/boot |
| `--upload-port` | str | `/dev/cu.usbserial-0001` (`LUCE_UPLOAD_PORT`) | Flash port |
| `--monitor-port` | str | `/dev/cu.usbserial-40110` (`LUCE_MONITOR_PORT`) | Serial capture port |
| `--boot-duration` | int | `45` (`LUCE_TEST_DURATION`) | Boot serial-capture seconds |
| `--require-marker` | str (repeat) | — | Extra required boot marker |
| `--host` | str | `https://127.0.0.1` (`LUCE_HOST`) | HTTPS host |
| `--captive-host` | str | empty (`LUCE_CAPTIVE_HOST`) | Plain-HTTP captive host |
| `--http-token` | str | empty (`LUCE_HTTP_TOKEN`) | HTTP bearer token |
| `--tcp-host` / `--tcp-port` | str/int | `127.0.0.1` / `2323` | TCP CLI endpoint |
| `--tcp-token` | str | empty (`LUCE_CLI_NET_TOKEN`) | TCP CLI AUTH token |
| `--ws-host`/`--ws-port`/`--ws-path` | str/int/str | `127.0.0.1`/`80`/`/ws` | WebSocket endpoint |
| `--ws-tls` | flag | off (`LUCE_WS_TLS`) | TLS for WebSocket |
| `--mqtt-host`/`--mqtt-port` | str/int | `127.0.0.1`/`1883` | Broker endpoint |
| `--mqtt-topic` | str | `luce/net1` (`LUCE_MQTT_BASE_TOPIC`) | Base topic |
| `--mqtt-username`/`--mqtt-password` | str | empty | Broker creds |
| `--spawn-test-mqtt-broker` | flag | off | Spawn ephemeral amqtt broker (no Docker) |
| `--test-mqtt-host`/`--test-mqtt-port` | str/int | `127.0.0.1`/`18883` | Spawned broker bind |
| `--test-mqtt-startup-timeout` | float | `8.0` | Spawned broker readiness wait |
| `--diag-root` | str | `docs/work/diag` (`LUCE_DIAG_DIR`) | Evidence root |
| `--run-id` | str | `YYYYMMdd_HHMMSS` | Run-id override |
| `--write-evidence-manifest` | flag | off | Refresh `evidence-manifest.json` |
| `--release-evidence` | flag | off | Mark manifest release-gated (hardware-hil must pass) |
| `--wokwi-timeout-ms` | int | `120000` (`LUCE_SIM_WOKWI_TIMEOUT_MS`) | Wokwi sim timeout |
| `--wokwi-ready-timeout-s` | float | `75` (`LUCE_SIM_WOKWI_READY_TIMEOUT_S`) | Wokwi forwarded-HTTPS readiness wait |
| `--wokwi-network-mode` | `skip`\|`gateway` | `skip` (`LUCE_WOKWI_NETWORK_MODE`) | Wokwi protocol-layer handling |
| `--pytest-arg` | str (repeat) | — | Extra arg passed to pytest |

### 5.3 Running pytest directly (contract layers only)

`build`/`boot` are runner-native and are skipped when calling pytest directly.
All endpoint config comes from `conftest.py` options (or env vars):

```bash
.venv/bin/python -m pytest tests/test_http_contract.py \
  --luce-host https://<device-ip> --luce-http-token <token>
```

`conftest.py` options mirror the runner flags: `--luce-host`,
`--luce-captive-host`, `--luce-http-token`, `--luce-tcp-host/-port/-token`,
`--luce-ws-host/-port/-path/-tls`, `--luce-mqtt-host/-port/-topic/-username/
-password`, `--luce-test-mqtt-broker-pid`, `--luce-serial-port/-baud`,
`--luce-serial-reboot-capture-s`, `--luce-serial-reopen-timeout-s`,
`--luce-env`, `--luce-duration`, `--luce-run-build`, `--luce-run-boot`.

---

## 6. Contract-test inventory

`pytest.ini` markers: `build`, `hil`, `contract`, `net`, `slow`. Contract files
are tagged `contract`+`net`; device-dependent ones add `hil`.

### `tests/test_http_contract.py` — `contract`,`net`
1. `test_public_health_and_version` — `/api/health`, `/api/version` → 200, service/version.
2. `test_protected_endpoints_require_auth` — protected routes → 401 without token.
3. `test_info_payload_contract` — `/api/info` key/shape contract incl. TLS/cert fields.
4. `test_state_and_ota_payload_contract` — `/api/state` + `/api/ota` key contract.
5. `test_ota_check_post_put_and_source_precedence` — query > body > default URL precedence.
6. `test_ota_lifecycle_progress_after_manual_check` — checks counter / running advances.
7. `test_ota_failure_class_mapping_with_invalid_url` — fail counter + error populated.
8. `test_ota_periodic_cadence_when_configured` — periodic checks fire within interval.
9. `test_led_routes_and_validation` — GET/PUT LED modes; 400 on invalid.
10. `test_captive_routes_and_spa_fallback` — assets + SPA deep-link fallback.
11. `test_method_not_allowed` — 405 on disallowed methods.

### `tests/test_tcp_cli_contract.py` — `contract`,`net`,`hil`
1. `test_tcp_cli_auth_fail_then_abort` — 3 wrong tokens → auth fail → session aborted.
2. `test_tcp_cli_readonly_policy` — read-only commands OK; mutating commands DENIED.

### `tests/test_ws_contract.py` — `contract`,`net`,`hil`
1. `test_ws_handshake_and_snapshot` — handshake, snapshot field-set, ping/pong, close.

### `tests/test_mqtt_contract.py` — `contract`,`net`,`hil`
1. `test_mqtt_unsupported_topic_compat_and_led_readback`
2. `test_mqtt_config_topics_supported_vs_unsupported`
3. `test_mqtt_config_http_token_persists_after_reboot` (serial reboot required)
4. `test_mqtt_control_paths_reflect_in_http_state`
5. `test_mqtt_reconnect_after_managed_broker_restart` (managed-broker pid required)

### `tests/test_serial_cli_contract.py` — `contract`,`net`,`hil`
1. `test_network_lifecycle_markers_after_reboot` — `[WIFI][LIFECYCLE]`, `[NTP][LIFECYCLE]`, `[mDNS]`.
2. `test_serial_cli_parser_matrix` — usage/parse-error/invalid-index/unknown-command matrix.

### `tests/test_docs_alignment.py` — no markers (filesystem only, CI-safe)
1. `test_every_registered_command_is_documented` — `src/cli.cpp` `kCliCommands[]` ⊆ docs.
2. `test_no_documented_command_is_phantom` — docs ⊆ `kCliCommands[]`.

---

## 7. Procedures (runbooks)

### 7.1 Hardware-free pre-commit check (no board)
```bash
source ~/.zshrc
scripts/run_host_tests.sh
.venv/bin/python -m pytest tests/test_docs_alignment.py
git ls-files 'src/*.cpp' 'src/*.h' 'include/luce/*.h' | xargs clang-format --dry-run --Werror
```

### 7.2 Build all environments
```bash
source ~/.zshrc
./scripts/luce.sh build            # all envs
./scripts/luce.sh build --env net1 # single env
```

### 7.3 Smoke boot test (board attached)
```bash
./scripts/luce.sh test --layers boot --env net1 --boot-duration 45
```
Uploads `net1`, captures ~45 s of serial, asserts the NET1 boot markers.

### 7.4 Full hardware contract suite
```bash
./scripts/luce.sh test --layers all --env net1 \
  --host https://<device-ip> \
  --http-token <http-token> \
  --tcp-token <cli-token> \
  --ws-host <device-ip> \
  --mqtt-host <broker-ip> --mqtt-topic luce/net1
```
Order: `build → boot → http → tcp → ws → mqtt → serial`. Each selected layer
fails loudly if its prerequisites are missing.

### 7.5 MQTT layer with an ephemeral broker (no external broker / Docker)
```bash
./scripts/luce.sh test --layers mqtt --spawn-test-mqtt-broker --mqtt-topic luce/net1
```
The runner starts a temporary in-process `amqtt` broker (bind
`--test-mqtt-host:--test-mqtt-port`, default `127.0.0.1:18883`), passes its PID
to the reconnect test, and tears it down afterward. The device must be
configured to use that broker for the control-path tests to observe effects.

### 7.6 Wokwi simulator lane (supplementary)
```bash
./scripts/luce.sh test --target wokwi \
  --layers build,boot,http,tcp,ws,mqtt --env net1 --spawn-test-mqtt-broker
```
- `build` + `boot` run in the simulator (boot includes a non-I2C UART CLI smoke).
- `http/tcp/ws/mqtt` are recorded `DESELECTED` (`deselected:wokwi`) by default
  because `wokwi-cli` does not attach forwarded ports; `--wokwi-network-mode
  gateway` is an experimental `wokwigw` attempt.
- The runner generates simulator NVS (tokens `LUCE_SIM_HTTP_TOKEN`/`luce-token`,
  `LUCE_SIM_CLI_TOKEN`/`luce-cli`), an EC-P256 TLS cert/key (1-day), and a merged
  firmware image; ports are forwarded `8443→443`, `8080→80`, `2323→2323`.
- Generated simulator NVS, TLS material, Wokwi project files, merged firmware,
  serial logs, and pytest evidence are written under `docs/work/diag/<run_id>/`.

### 7.7 Produce release evidence (board + network attached)
```bash
./scripts/luce.sh test --layers all --env net1 \
  --host https://<device-ip> --http-token <t> --tcp-token <c> \
  --ws-host <device-ip> --mqtt-host <broker> \
  --write-evidence-manifest --release-evidence
```
Refreshes `docs/governance/health/evidence-manifest.json` with `release:true` and
`hardware-hil` `required:true/status:PASS`. Then commit the manifest and confirm:
```bash
python3 scripts/verify_evidence.py --release   # must print "evidence manifest OK"
```

---

## 8. Configuration matrix (flags ↔ env vars)

Every runner flag has an env-var fallback (the runner flag wins):

| Concern | Flag | Env var | Default |
|---------|------|---------|---------|
| Env | `--env` | `LUCE_ENV` | `net1` |
| Upload port | `--upload-port` | `LUCE_UPLOAD_PORT` | `/dev/cu.usbserial-0001` |
| Monitor port | `--monitor-port` | `LUCE_MONITOR_PORT` | `/dev/cu.usbserial-40110` |
| Boot duration | `--boot-duration` | `LUCE_TEST_DURATION` | `45` |
| HTTP host | `--host` | `LUCE_HOST` | `https://127.0.0.1` |
| HTTP token | `--http-token` | `LUCE_HTTP_TOKEN` | (empty) |
| TCP token | `--tcp-token` | `LUCE_CLI_NET_TOKEN` | (empty) |
| MQTT host/port | `--mqtt-host/-port` | `LUCE_MQTT_HOST`/`_PORT` | `127.0.0.1`/`1883` |
| MQTT topic | `--mqtt-topic` | `LUCE_MQTT_BASE_TOPIC` | `luce/net1` |
| Serial baud | (conftest `--luce-serial-baud`) | `LUCE_SERIAL_BAUD` | `115200` |
| Diag root | `--diag-root` | `LUCE_DIAG_DIR` | `docs/work/diag` |
| Wokwi sim timeout | `--wokwi-timeout-ms` | `LUCE_SIM_WOKWI_TIMEOUT_MS` | `120000` |

---

## 9. Evidence artifacts

All runs write under `docs/work/diag/<run_id>/` (local scratch; gitignored by
convention). Release evidence is summarized into the tracked governance manifest.

| Path | Producer | Content |
|------|----------|---------|
| `host-tests/summary.json`, `host-tests/junit.xml`, `host-tests/host-tests.log` | `run_host_tests.sh` | Host unit-test result + assertion count |
| `test-layers/<layer>.log` | `test_layers.py` | stdout/stderr per layer |
| `test-layers/junit-<layer>.xml` | `test_layers.py` | pytest JUnit per pytest layer |
| `test-layers/summary.md` / `summary.json` | `test_layers.py` | per-layer status (`PASS/FAIL/PREREQ_MISSING/DESELECTED`), rc, log path, ran/pass/fail/deselected counts |
| `test-layers/boot_serial.log` / `wokwi_serial.log` | boot layer | captured serial |
| `test-layers/test-mqtt-broker.log` | `--spawn-test-mqtt-broker` | ephemeral broker output |
| `test-layers/wokwi-network.log`, `wokwi-gateway.log` | Wokwi modes | simulator/gateway logs |
| `test/test.txt` | `luce.sh test` wrapper | wrapper invocation log |

---

## 10. Evidence manifest & release gating

`scripts/test_layers.py --write-evidence-manifest` produces
`docs/governance/health/evidence-manifest.json` (plus a detailed
`evidence-latest.json`). Schema (abridged):
```json
{
  "schema": 1,
  "run_id": "<id>",
  "source": { "git_sha": "<sha>", "target": "wave-f-local" },
  "release": false,
  "checks": [
    { "name": "platformio:net1",  "status": "PASS", "required": true },
    { "name": "host-tests",       "status": "PASS", "required": true },
    { "name": "docs-alignment",   "status": "PASS", "required": true },
    { "name": "hardware-hil",     "status": "PREREQ_MISSING", "required": false }
  ],
  "artifacts": [ { "path": "...", "sha256": "..." } ]
}
```

`scripts/verify_evidence.py` (run by CI job `verify-evidence`):
1. **Freshness** — the manifest `source.git_sha` must equal `HEAD`, or be `HEAD^`
   when the only delta to HEAD is the manifest files themselves
   (`parent-plus-manifest`). Stale evidence → fail.
2. **Required checks** — every `required:true` check must be `PASS`.
3. **Artifact integrity** — each listed artifact must exist and match its SHA-256.
4. **Release gate** — when release context is detected (`--release`, manifest
   `release:true`, `source.target` starting `release`, or GitHub ref
   `main`/`release/*`), `hardware-hil` must be `required:true` **and** `PASS`;
   otherwise the gate fails. This is what prevents shipping a release without
   fresh device evidence.

Quick local checks:
```bash
python3 scripts/verify_evidence.py            # non-release: passes on current manifest
python3 scripts/verify_evidence.py --release  # release: fails unless hardware-hil PASS
```

---

## 11. Selective execution & markers

- By layer: `--layers boot`, `--layers critical`, `--layers http,mqtt`.
- By pytest marker (direct pytest): `-m "contract and not hil"` runs contract
  tests that do not need a device; `-m hil` runs device-dependent ones.
- `--pytest-arg` forwards extra args (e.g. `--pytest-arg -k --pytest-arg ota`).

---

## 12. Troubleshooting

| Symptom | Cause / fix |
|---------|-------------|
| `pio: command not found` | `source ~/.zshrc` first; or use `python3 -m platformio`. |
| Layer fails "missing HTTP/TCP token" | Provide `--http-token`/`--tcp-token` (or env). Selected layers fail loud by design. |
| `serial` layer fails on pyserial | `pip install -r tests/requirements.txt` into `.venv`; ensure `--luce-serial-port`. |
| `termios.error (19, ...)` opening monitor | Use the direct serial-read fallback in `README.md`, or a different monitor port. |
| MQTT layer "broker unreachable" | Start a broker, point `--mqtt-host/-port` at it, or use `--spawn-test-mqtt-broker`. |
| Wokwi network layers all `DESELECTED` | Expected with `--wokwi-network-mode skip`; hardware is the protocol gate. |
| `verify_evidence.py` "git_sha does not match HEAD" | Regenerate evidence at the current commit with `--write-evidence-manifest`. |
| `verify_evidence.py --release` fails on `hardware-hil` | Run the full HIL suite on a board with `--release-evidence`, commit the manifest. |

---

## 13. Quick command index

```bash
# Hardware-free
scripts/run_host_tests.sh
.venv/bin/python -m pytest tests/test_docs_alignment.py

# Build
./scripts/luce.sh build --env net1

# Smoke boot
./scripts/luce.sh test --layers boot --env net1 --boot-duration 45

# Full contract suite (hardware)
./scripts/luce.sh test --layers all --env net1 \
  --host https://<ip> --http-token <t> --tcp-token <c> --ws-host <ip> --mqtt-host <broker>

# MQTT with ephemeral broker
./scripts/luce.sh test --layers mqtt --spawn-test-mqtt-broker --mqtt-topic luce/net1

# Wokwi simulator
./scripts/luce.sh test --target wokwi --layers build,boot --env net1

# Release evidence + verification
./scripts/luce.sh test --layers all --env net1 [endpoints...] --write-evidence-manifest --release-evidence
python3 scripts/verify_evidence.py --release
```
