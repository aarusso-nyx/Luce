# Main.cpp Split Planner (Luce)

Date: 2026-02-23
Status: Planned
Owner: Luce firmware team

## Objective

Split `src/main.cpp` into low-churn modules while preserving:

- compile-time stage gating via `LUCE_STAGE`
- direct task orchestration in `app_main` / top-level startup helpers
- runtime behavior and CLI/network CLI semantics
- no `service_manager`/`supervisor`/`reducer` style architecture

This plan is mechanical only (no behavior changes, no runtime logic edits in this step).

## Constraints from AGENTS and project policy

- Keep code under `src/` and `include/`.
- Keep docs under `docs/work/plan/`.
- Keep reference material under `docs/work/inv/`.
- Keep evidence under `docs/work/diag/`.
- `platformio.ini` remains the compile-time source of env/stage gating.
- Every split slice must be followed by at least:
  - `pio run -e luce_stage0..luce_stage10` matrix
  - optional unit evidence (`luce_test_native`) if test-related files changed
- No change in external behavior for this plan pass.

## Current monolith responsibility map from `src/main.cpp` (already present)

- Boot diagnostics:
  - startup banner, chip/app/heap logs, reset reason formatting, partition report
- Stage/feature macros and compile-time constants
  - `LUCE_HAS_*` dispatch, task stack constants, debug/tuning knobs
- NVS config and dump helpers
- I2C/MCP/LCD/I/O core:
  - bus init/probe/scan, MCP init/read/write, relay/button mask update
  - LCD init, optional status rendering, ITB-only line
- CLI parser/dispatch:
  - tokenization, arg parsing helpers, command mapping, serial command task
- Networking surfaces:
  - Wi-Fi state machine/events
  - SNTP sync loop and status API
  - mDNS advertising lifecycle
  - MQTT lifecycle/publish/test flows
  - HTTPS server and protected handlers
  - TCP CLI transport + auth + line protocol session
- `main.cpp` orchestrator:
  - blink + stage2 diagnostics + stage-gated startup + infinite wait loop

## Target module layout (keep `src/*` glob build behavior)

### Public headers (`include/luce/`)

- `include/luce/build_features.h`
  - LUCE feature aliases and compile-stage helper constants (thin, no ESP IDs)
- `include/luce/diagnostics.h`
  - startup/status formatting contracts (`format_mcp_mask_line`, reset/feature status helpers)
- `include/luce/state_types.h`
  - POD structs and enums that cross modules (no ESP task objects)
- `include/luce/nvs_keys.h`
  - canonical namespace/key names and small readers/writers helpers (decl)
- `include/luce/drivers/i2c_types.h`
  - driver-facing MCP/LCD constants + simple utility signatures
- `include/luce/cli/cli_parser.h`
  - parse helpers and dispatcher contract
- `include/luce/net/*`
  - `wifi.h`, `ntp.h`, `mdns.h`, `mqtt.h`, `http.h`, `cli_net.h`

### Implementations (`src/luce/`)

- `src/luce/diagnostics.cpp`
  - startup banner, chip/app/heap logs, status formatting implementation
- `src/luce/stage_gating.cpp`
  - stage constants, compile-time capability logs, safe stubs for disabled features
- `src/luce/nvs_config.cpp`
  - unified namespace/key read helpers and schema defaults
- `src/luce/drivers/i2c_bus.cpp`
  - i2c init + scan + IT interrupt config
- `src/luce/drivers/mcp23017.cpp`
  - MCP read/write/state control + constants
- `src/luce/drivers/lcd2004.cpp`
  - thin wrapper around existing `Pcf8574Hd44780` flow
- `src/luce/cli/cli_parser.cpp`
  - `tokenize_cli_line`, `parse_u32_with_base`, command dispatch table + helpers
- `src/luce/cli/cli_serial.cpp`
  - UART task + serial command integration points
- `src/luce/net/wifi.cpp`
  - Wi-Fi config/state machine/events/task
- `src/luce/net/ntp.cpp`
  - SNTP runtime task + CLI status contract
- `src/luce/net/mdns.cpp`
  - mDNS config + TXT/state helpers + startup/shutdown
- `src/luce/net/mqtt.cpp`
  - MQTT task + config + status/log/publish helpers
- `src/luce/net/http.cpp`
  - HTTPS startup/route handlers (`/api/health`, `/api/info`, `/api/state`)
- `src/luce/net/cli_tcp.cpp`
  - TCP session handler + auth/allowlist enforcement
- `src/luce/main_runtime.cpp` (optional, small)
  - common helper functions currently only in `main.cpp` that are not strictly modules above

### Orchestrator (`src/main.cpp`)

- Keep only:
  - includes
  - compile-time flag setup
  - global task handles
  - startup procedure (`app_main`) and direct module startup calls in stage order
  - minimal stage entrypoint bridging into modules

## Dependency rules to avoid cycles

- `main.cpp` includes only public headers and never includes module `.cpp`.
- No module should include another module’s `.cpp` or private header internals.
- ESP-IDF/C headers stay in:
  - `main.cpp`
  - `src/luce/*` implementations
  - and must be kept out of public headers unless limited to forward declarations.
- `include/luce/state_types.h` owns cross-module structs/enums only.
- Network modules may depend on `nvs_config` + `diagnostics`, but not on each other except via
  lifecycle hooks needed in `main.cpp`.
- `cli_*` dispatch only calls command handler APIs from feature modules; no networking handler should
  directly mutate another module’s internals without public contracts.
- Compile-time exclusion is maintained as:
  - build flags in `luce_build.h` plus optional runtime no-op stubs (for unsupported stage values).
- Use narrow ABI-style headers and keep heavy include chains local to each `.cpp`.

## Planned slice plan (commit-friendly)

Each slice must be validated with `pio run -e luce_stage0..luce_stage10` and, where parser/NVS
helpers are touched, `pio test -e luce_test_native`.

### S0 — Inventory and extraction map (docs-only, no file moves)
- Move:
  - Create this plan and freeze ownership mapping + line ownership references.
- No behavior changes.
- Validate:
  - none required (documentation-only slice).
- Evidence:
  - `docs/work/diag/evidence/<ts>/plan/090/s0_plan_capture.txt` (plan manifest + file map).

### S1 — Publish module interfaces
- Move:
  - create `include/luce/build_features.h`, `state_types.h`, `nvs_keys.h`.
  - replace duplicated `constexpr` structs/enums/namespace constants with declarations.
- No behavior changes.
- Validate:
  - `pio run -e luce_stage0`
  - `pio run -e luce_stage10`
- Evidence:
  - `docs/work/diag/evidence/<ts>/build/luce_stage0.txt`
  - `docs/work/diag/evidence/<ts>/build/luce_stage10.txt`

### S2 — Diagnostics extraction
- Move:
  - banner/reset/format helpers from `main.cpp` into `src/luce/diagnostics.cpp`.
- No behavior changes.
- Validate:
  - full matrix `pio run -e luce_stage0..luce_stage10`.
- Evidence:
  - build artifacts and `docs/work/diag/evidence/<ts>/boot/stage10_boot_capture.txt` equivalent.

### S3 — NVS + config helpers extraction
- Move:
  - NVS open/read/readers/log helpers into `src/luce/nvs_config.cpp`.
  - keep stage-specific key names in `nvs_keys.h`.
- No behavior changes.
- Validate:
  - full matrix.
- Evidence:
  - build matrix success
  - `docs/work/diag/evidence/<ts>/boot/luce_stage1_boot.txt`

### S4 — Driver extraction (I2C/MCP/LCD)
- Move:
  - i2c bus init/probe/scan and MCP/LCD constants/functions into
    `src/luce/drivers/*`.
- Add:
  - `src/luce/drivers/mcp23017.cpp`, `src/luce/drivers/i2c_bus.cpp`, `src/luce/drivers/lcd2004.cpp`.
- No behavior changes.
- Validate:
  - stages 2/3/4 startup command smoke:
    - `i2c_scan`, `mcp_read`, `buttons`, `status`, `help`.
- Evidence:
  - build matrix
  - `docs/work/diag/evidence/<ts>/30_unit/unit_native.txt`
  - `docs/work/diag/evidence/<ts>/60_e2e/stage2_i2c_cli.txt` (if hardware available)

### S5 — CLI parser/dispatch extraction
- Move:
  - `parse_u32_with_base`, `tokenize_cli_line`, command table/dispatcher into
    `src/luce/cli/cli_parser.cpp` + `include/luce/cli/cli_parser.h`.
- Preserve command strings and argument semantics exactly.
- Validate:
  - full matrix
  - host tests (if touched parser unit functions): `pio test -e luce_test_native`
- Evidence:
  - build matrix
  - unit outputs under `docs/work/diag/evidence/<ts>/unit/cli_parser_stage_task0.md`

### S6 — Serial CLI task extraction
- Move:
  - UART setup/read loop and serial dispatch loop into `src/luce/cli/cli_serial.cpp`.
- Keep command handlers in module contracts; `main.cpp` owns startup timing.
- Validate:
  - stage4 `help/status` and relay-safe command checks unchanged.
- Evidence:
  - build matrix + boot/e2e command transcript.

### S7 — Network module extraction (Wi-Fi + NTP + mDNS + MQTT + HTTP + TCP CLI)
- Move each stack into its own module and header:
  - `src/luce/net/wifi.cpp`, `ntp.cpp`, `mdns.cpp`, `mqtt.cpp`, `http.cpp`, `cli_tcp.cpp`.
- Keep event-handler wiring localized per module.
- Add only stage-gated startup calls in `main.cpp`.
- Validate:
  - stage5..10 matrix
  - `docs/work/diag/evidence/<ts>/60_e2e/` for `wifi.status`, `time.status`,
    `mdns.status`, `mqtt.status`, `http.status`, TCP CLI auth + readonly commands.
- Evidence:
  - full matrix for stages 0..10
  - minimum network e2e artifacts per active stage.

### S8 — Final orchestrator simplification (`main.cpp`)
- Move orchestration to direct startup sequence:
  - feature initialization order + task creation only.
- Remove all in-file subsystem logic except:
  - task handle declarations
  - stage-gated startup path selection
  - watchdog-safe perpetual wait path.
- Validate:
  - full matrix
  - at least stage0 boot evidence + stage10 smoke evidence.
- Evidence:
  - `docs/work/diag/evidence/<ts>/20_build/build_luce_stage10.txt`
  - `docs/work/diag/evidence/<ts>/50_boot/boot_stage10.txt`

## Per-slice required verification commands

- Matrix build:
  - `python3 -m platformio run -e luce_stage0`
  - `python3 -m platformio run -e luce_stage1`
  - `python3 -m platformio run -e luce_stage2`
  - `python3 -m platformio run -e luce_stage3`
  - `python3 -m platformio run -e luce_stage4`
  - `python3 -m platformio run -e luce_stage5`
  - `python3 -m platformio run -e luce_stage6`
  - `python3 -m platformio run -e luce_stage7`
  - `python3 -m platformio run -e luce_stage8`
  - `python3 -m platformio run -e luce_stage9`
  - `python3 -m platformio run -e luce_stage10`
- Host tests (if parser/NVS/diagnostics modules modified):
  - `python3 -m platformio test -e luce_test_native`
- Boot/E2E smoke checks for gated milestones:
  - `pio run -t upload -e luce_stage0 && capture boot`
  - `pio run -t upload -e luce_stage10 && capture boot/status/help`

## Evidence plan (expected)

- Every major slice:
  - `docs/work/diag/evidence/<timestamp>/00_index.md`
  - `docs/work/diag/evidence/<timestamp>/90_summary.md`
- Slice-by-slice evidence path convention:
  - `docs/work/diag/evidence/<timestamp>/split/s0_<name>.md`
  - include command set, build command list, and PASS/FAIL plus signatures.

## Risk notes

- Heavy ESP headers in public headers:
  - Prevent by using opaque types / forward declarations and only moving types required by module APIs.
- Compile-time exclusion drift:
  - Never move `#if LUCE_HAS_*` behavior into runtime `if` gates only.
  - Keep stage compile-time stubs for disabled features.
- Hidden coupling through globals:
  - Replace broad `extern` usage with explicit accessors in headers where practical.
  - Keep ownership of critical state close to domain module.
- Linker/ODR collisions:
- No duplicate TU-local symbols after extraction.
  - Keep all helper implementations single-definition in module translation units.
- Command path changes:
  - Keep command parser output and logging verbatim until behavior lock-in slice.
- CMake globs:
  - current `src/*.*` glob should continue to compile new subfolder files without edits.

## Minimal-risk sequence order (recommendation)

1. S1 -> S2 -> S3 (low risk, no ESP task behavior change)
2. S5 -> S6 (parser + serial command determinism)
3. S4 -> S7 (drivers/networks), then
4. S8 last (orchestrator cleanup)

## Completion criteria

- `src/main.cpp` becomes orchestration-only plus thin wrappers.
- No stage behavior change observed in stage0..10 boot/e2e smoke checks.
- Stage-gated compile-time surfaces remain unchanged (`LUCE_STAGE=0..10`).
- Immediate follow-up can execute code refactor slices against this plan with incremental commits.
