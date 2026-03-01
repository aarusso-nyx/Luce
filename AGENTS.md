# AGENTS

## Repository Context
- **Path**: `/Users/aarusso/Development/Luce`
- **Project type**: ESP32 firmware (ESP-IDF) with PlatformIO
- **Primary stack**: C/C++ firmware + PlatformIO + ESP-IDF
- **Canonical environments**: `default`, `net0`, `net1`
- **Canonical config**: `sdkconfig` (single source for all strategy envs)

## Standards and Layout
- Governance artifacts must stay under:
  - `docs/governance/audit/`
  - `docs/governance/health/`
  - `docs/governance/compliance/`
- Temporary scratch work and investigation artifacts must stay under:
  - `docs/work/inv/`
  - `docs/work/diag/`
  - `docs/work/plan/`
- Product documentation stays in:
  - `docs/user/` (APIs, protocol contracts, operations, architecture)

## Repository Files to Keep Stable
- `README.md` and `AGENTS.md` must remain at repository root.
- `platformio.ini` defines build/test environments.
- Source and include trees remain canonical:
  - `src/`
  - `include/`
  - `scripts/`
  - `docs/`
- PlatformIO strategy environment file list and flags:
  - `default`: no `LUCE_NET_*` flags (baseline)
  - `net0`: `-DLUCE_NET_CORE=1`
  - `net1`: `-DLUCE_NET_CORE=1 -DLUCE_NET_MQTT=1 -DLUCE_NET_HTTP=1`
- SDK config used by all envs is `sdkconfig` via `board_build.esp-idf.sdkconfig_path`.

## Required Governance Evidence
- `docs/governance/audit/structure-conformance.md`
- `docs/governance/compliance/scorecard-current.md`
- `docs/governance/health/preflight.md`
- Evidence outputs and temporary logs may be written under `docs/work/**`.

## PlatformIO Environment Hygiene
- Always initialize shell environment before invoking `pio`:
  - `source ~/.zshrc`
- Prefer `python3 -m platformio` only when `pio` is not on `PATH`.
- Canonical command targets are:
  - `default`, `net0`, `net1`
- Serial ports for firmware workflows:
  - Upload: `/dev/cu.usbserial-0001`
  - Monitor: `/dev/cu.usbserial-40110`
- Do not revive legacy `luce_stage*` build environments in new work.
- Recommended firmware iteration flow:
  1. Build target environment with `python3 -m platformio run -e <env>`.
  2. Upload with `python3 -m platformio run -e <env> -t upload --upload-port /dev/cu.usbserial-0001`.
  3. Open console with `python3 -m platformio device monitor -p /dev/cu.usbserial-40110`.
  4. On `net0`, validate CLI outputs (`help`, `status`, `nvs_dump`, `i2c_scan`, `mcp_read`, `relay_set`, `relay_mask`, `buttons`, `lcd_print`, `reboot`) and capture the first 80 lines after reset.
- If monitor open fails with `termios.error: (19, 'Operation not supported by device')`, use direct serial read fallback:
  ```python
  import sys, serial
  with serial.Serial('/dev/cu.usbserial-40110', 115200, timeout=0.2) as ser:
      for _ in range(80):
          line = ser.readline()
          if line:
              sys.stdout.write(line.decode('utf-8', errors='replace'))
  ```

## Contributor Expectations
- Prefer task-oriented FreeRTOS architecture and explicit state ownership.
- Keep physical-input authority semantics explicit in code and docs.
- Avoid introducing legacy compatibility shims when the canonical interface has changed.
- Use canonical HTTP routes and MQTT topics only (no `/v2` aliases).

## Operational Notes
- Keep security and resilience features in sync across CLI/HTTP/MQTT and docs.
- Any plan or intervention touching behavior should include matching doc updates under `docs/user/`.
- When uncertain, prefer conservative defaults, clear rollback paths, and explicit diagnostics.
