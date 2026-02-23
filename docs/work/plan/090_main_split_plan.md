# Main.cpp Modularization Plan (Current Monolith)

## Scope
This is a mechanical split of the existing `src/main.cpp` only. No features are added, removed, or behavior-altered. Log formats, command names, state semantics, and pin mappings are preserved.

## Source scan used for this plan
Current canonical file: `src/main.cpp`  
Build environment snapshot: `platformio.ini` currently defines `luce_stage0` through `luce_stage10` plus `luce_test_native`.

## Current monolith map (line ranges are approximate)
1. Build/layout declarations: lines `1-345`. Includes LUCE stage macros, task handles, shared structs, enums, and forward declarations for feature blocks.
2. Diagnostics block: lines `358-553`. Includes startup/banner, chip/app/partition/heap status routines.
3. Wi‑Fi block: lines `589-1229`. Includes `WifiState`, `WifiConfig`, config loader, event queue, task lifecycle, config/state logging, and startup hook.
4. mDNS block: lines `1258-1588` under `LUCE_HAS_MDNS`.
5. MQTT block: lines `1637-2198` active implementation under `LUCE_HAS_MQTT`; duplicate legacy block exists around `2953-3304` in `#if 0`.
6. NTP block: lines `3319-3607` under `LUCE_HAS_NTP`.
7. HTTP block: lines `2267-2782` under `LUCE_HAS_HTTP`.
8. NVS boot/state block: lines `3645-3810` under `LUCE_HAS_NVS`.
9. I2C + MCP + LCD + Stage2 diagnostics: lines `3882-4315`.
10. CLI parser + commands + startup: lines `4496-5270` under `LUCE_HAS_CLI` and `LUCE_HAS_TCP_CLI` for subset.
11. Task-level misc + entrypoint: lines `5271-5350`; includes `diagnostics_task`, `blink_alive_task`, `blink_alive`, `app_main`.

## Required extraction regions from existing monolith
1. Core diagnostics/startup functions: `log_startup_banner`, `print_chip_info`, `print_app_info`, `print_partition_summary`, `print_heap_stats`, `log_status_health_lines`, `log_runtime_status_line`, `log_stage4_watermarks`, `log_heap_integrity`, `reset_reason_to_string`.
2. NVS functions: `dump_nvs_value`, `dump_nvs_entries`, `update_boot_state_record`, `nvs_type_name`.
3. LCD transport and rendering: `Pcf8574Hd44780` class + helpers `write_pcf`, `pulse_en`, `write_nibble`, `send_byte`, `send_command`, `set_cursor`, `write_line`, `write_text_line`, `write_status_lines`, `write_text`.
4. I2C + MCP helpers: `init_i2c`, `run_i2c_scan_flow`, `scan_i2c_bus`, `i2c_probe_device`, `mcp_write_reg`, `mcp_read_reg`, `init_mcp23017`, `set_relay_mask`, `set_relay_mask_safe`, `read_button_inputs`, `relay_mask_for_channel`, `relay_mask_for_channel_state`, `configure_int_pin`, `run_stage2_diagnostics`.
5. CLI core/parsing: `cli_trim`, `parse_u32_with_base`, `tokenize_cli_line`, `log_cli_arguments`, command handlers `cli_cmd_*`, `execute_cli_command`, `cli_task`, `cli_startup`.
6. Wi‑Fi/NTP/MDNS/MQTT/HTTP/TCP CLI internals already present and guarded by feature flags must be preserved as mechanical transfers.

## Important observation
1. `main.cpp` already carries compile flags not currently all staged in runtime order (`LUCE_HAS_MDNS`, `LUCE_HAS_TCP_CLI`, `LUCE_HAS_MQTT`, `LUCE_HAS_HTTP`).
2. The current file contains duplicated `MQTT` code: one active block and one disabled duplicate under `#if 0`; split should include only active implementations in migrated modules.
3. No new architectural spine is introduced. Feature init remains explicit in `app_main` orchestration and subsystem modules.

## Target module tree (must remain compatible with `CMakeLists.txt` glob of `src`)
1. `src/app_main.cpp` for orchestration only.
2. `src/boot_diagnostics.cpp` with matching `include/luce/boot_diagnostics.h`.
3. `src/nvs_boot.cpp` with matching `include/luce/nvs_boot.h`.
4. `src/i2c_bus.cpp` with matching `include/luce/i2c_bus.h`.
5. `src/mcp23017.cpp` with matching `include/luce/mcp23017.h`.
6. `src/lcd_pcf8574.cpp` with matching `include/luce/lcd_pcf8574.h`.
7. `src/cli_parser.cpp` with `include/luce/cli_parser.h`.
8. `src/cli_commands.cpp` with `include/luce/cli_commands.h`.
9. `src/cli_uart.cpp` with `include/luce/cli_uart.h`.
10. `src/wifi_stage.cpp` with `include/luce/wifi_stage.h`.
11. `src/ntp_stage.cpp` with `include/luce/ntp_stage.h`.
12. `src/mdns_stage.cpp` with `include/luce/mdns_stage.h` if MDNS remains supported.
13. `src/mqtt_stage.cpp` with `include/luce/mqtt_stage.h` if MQTT remains supported.
14. `src/http_stage.cpp` with `include/luce/http_stage.h` if HTTP remains supported.
15. `src/tcp_cli_stage.cpp` with `include/luce/tcp_cli_stage.h` if TCP CLI remains supported.
16. Keep `include/luce/build.h` as canonical reference for derived stage flags if desired, or continue consuming existing `include/luce_build.h`.

## Dependency and header rules
1. Keep public include files under `include/luce/*` minimal and free of heavy ESP-IDF includes unless unavoidable.
2. ESP-IDF headers stay in implementation files (`src/*.cpp`) where they are required by feature code.
3. Global cross-module state (relay mask, button mask, wifi state, ntp state) is owned by subsystem modules and accessed via dedicated accessors.
4. Compile-time gating is enforced at implementation boundary with stub branches:
   1. Active branch under `#if LUCE_HAS_*`.
   2. Disabled branch returns neutral defaults and preserves call sites.

## Slice plan (mechanical, test-gated)
Slice S0: split app entry only.
1. Move `extern "C" void app_main(void)` to `src/app_main.cpp` and reduce it to startup orchestration.
2. Replace in-file orchestration details with module start calls.
3. Keep order deterministic and stage-respecting.
4. Validate with `pio run -e luce_stage0` and `pio run -e luce_stage1`.

Slice S1: extract diagnostics/build loggers.
1. Move startup banner and health helpers to `src/boot_diagnostics.cpp`.
2. Preserve all log lines/keywords used by existing evidence workflows.
3. Ensure all functions remain callable before feature startup gates.
4. Validation commands:
   1. `pio run -e luce_stage0`
   2. `pio run -e luce_stage1`
   3. `pio run -e luce_stage2`

Slice S2: extract NVS boot layer.
1. Move NVS dump and boot counter updates to `src/nvs_boot.cpp`.
2. Keep shared key readers (`wifi_read_u32_key`, etc.) accessible to Wi‑Fi/NTP loaders.
3. Add disabled stubs in `nvs_boot` for `LUCE_HAS_NVS==0`.
4. Validation commands:
   1. `pio run -e luce_stage0`
   2. `pio run -e luce_stage1`

Slice S3: extract I2C/MCP/LCD and Stage2 diagnostics.
1. Move I2C scan and MCP register operations to dedicated modules.
2. Move LCD class and status line rendering to dedicated LCD module.
3. Move `run_stage2_diagnostics` as-is with loop logic unchanged.
4. Add I2C-disabled and LCD-disabled no-op branches.
5. Validation commands:
   1. `pio run -e luce_stage2`
   2. `pio run -e luce_stage3`
   3. `pio run -e luce_stage4`

Slice S4: extract CLI parser and command routing.
1. Move CLI helper functions, parsing, and execution dispatch to `src/cli_parser.cpp`.
2. Move command handler functions to `src/cli_commands.cpp`.
3. Keep UART startup and task wiring in `src/cli_uart.cpp`.
4. Add stubs for `LUCE_HAS_CLI==0` and preserve command strings and behavior.
5. Validation commands:
   1. `pio run -e luce_stage4`
   2. `pio run -e luce_stage5`

Slice S5: extract gated networking subsystems.
1. Move `wifi_stage.cpp` (`wifi_task`, startup, event callbacks, status helpers).
2. Move `ntp_stage.cpp` (state machine + status formatting) and keep `cli` dependencies intact.
3. Move existing MDNS/MQTT/HTTP/TCP CLI blocks to their own modules with disabled stubs.
4. Keep direct `LUCE_HAS_*` gating and stage semantics untouched.
5. Validation commands:
   1. `pio run -e luce_stage5`
   2. `pio run -e luce_stage6`
   3. `pio run -e luce_stage7`
   4. `pio run -e luce_stage8`
   5. `pio run -e luce_stage9`
   6. `pio run -e luce_stage10`

Slice S6: finalize orchestration and full build matrix.
1. Wire orchestration sequence in `app_main`: diagnostics, NVS, I2C and optional diagnostics task, CLI, networking features.
2. Remove duplicated declarations from old monolith and include headers for new modules.
3. Keep build/test command list stable and complete.
4. Validation commands:
   1. `pio run -e luce_stage0`
   2. `pio run -e luce_stage1`
   3. `pio run -e luce_stage2`
   4. `pio run -e luce_stage3`
   5. `pio run -e luce_stage4`
   6. `pio run -e luce_stage5`
   7. `pio run -e luce_stage6`
   8. `pio run -e luce_stage7`
   9. `pio run -e luce_stage8`
   10. `pio run -e luce_stage9`
   11. `pio run -e luce_stage10`
   12. `pio run -e luce_test_native` (if required by local workflow)
5. Unit tests: if test harness exists, run `pio test -e luce_test_native`.

## Slice acceptance gates
1. PASS per slice only if all required `pio run -e <env>` commands succeed.
2. PASS only if compile-time gates preserve behavior for all `LUCE_HAS_*` branches without cross-feature leakage.
3. PASS only if CLI command paths still resolve and produce previous signature text.
4. PASS only if no subsystem call graph inversion is introduced.
5. UNAVAILABLE if tests are not runnable from current environment.

## AGENTS layout check
1. Plan docs live under `docs/work/plan/` as required.
2. Source stays under `src/`; headers remain under `include/luce` and `include/`.
3. No stable-root files are modified by this planning task.
