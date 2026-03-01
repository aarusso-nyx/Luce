# Main.cpp Split Plan (Current Monolith State)

## Scope
This is a mechanical plan for the current firmware layout, preserving all runtime behavior and log signatures. It aligns implementation with `LUCE_STRATEGY` gating without introducing new architecture.

## Current baseline
- Entrypoint path: `src/main.cpp`.
- Runtime orchestration: `src/main.cpp`.
- Diagnostics: `src/boot_diagnostics.cpp`.
- NVS state record: `src/boot_state.cpp`.
- I2C/MCP/LCD diagnostics/runtime: `src/i2c_io.cpp`.
- CLI UART command stack: `src/cli.cpp`.
- Build flags and staged envs: `platformio.ini` with `luce_core` through `luce_net1` (native test env retired).

## Current monolith map (flattened from actual split implementation)
- Core diagnostics entrypoint block in `src/boot_diagnostics.cpp`:
- `luce_log_startup_banner`
- `luce_print_chip_info`
- `luce_print_app_info`
- `luce_print_partition_summary`
- `luce_print_heap_stats`
- `luce_print_feature_flags`
- `luce_log_status_health`
- `luce_log_runtime_status`
- `luce_log_stage_watermarks`
- `luce_log_heap_integrity`
- `luce_reset_reason_to_string`
- `luce_init_path_reset_reason_line`
- NVS boot block in `src/boot_state.cpp`:
- `dump_nvs_value`
- `dump_nvs_entries`
- `update_boot_state_record`
- `nvs_type_name`
- I2C + MCP + LCD + I/O block in `src/i2c_io.cpp`:
- `i2c_probe_device`
- `init_i2c`
- `scan_i2c_bus`
- `run_i2c_scan_flow`
- `init_mcp23017`
- `mcp_write_reg`
- `mcp_read_reg`
- `set_relay_mask`
- `set_relay_mask_safe`
- `relay_mask_for_channel`
- `relay_mask_for_channel_state`
- `read_button_inputs`
- `configure_int_pin`
- `run_stage2_diagnostics`
- `stage2_lcd_write_text`
- Class `Pcf8574Hd44780` and helpers:
- `write_pcf`, `pulse_en`, `write_nibble`, `send_byte`, `send_command`, `set_cursor`, `write_line`, `write_text_line`, `write_text`, `write_status_lines`, `update_lcd_status`.
- CLI block in `src/cli.cpp`:
- `cli_trim`
- `tokenize_cli_line`
- `parse_u32_with_base`
- `log_cli_arguments`
- `cli_print_help`
- `cli_cmd_status`
- `cli_cmd_nvs_dump`
- `cli_cmd_i2c_scan`
- `cli_cmd_mcp_read`
- `cli_cmd_relay_set`
- `cli_cmd_relay_mask`
- `cli_cmd_buttons`
- `cli_cmd_lcd_print`
- `execute_cli_command`
- `cli_task`
- `cli_startup`
- Runtime orchestration in `src/main.cpp`:
- `blink_alive`
- `blink_alive_task`
- `stage2_task`
- `luce_runtime_main`
- App entry in `src/main.cpp`.

## Important observation
- The current codebase contains compile-time flags for `LUCE_HAS_WIFI`, `LUCE_HAS_NTP`, `LUCE_HAS_MDNS`, `LUCE_HAS_MQTT`, `LUCE_HAS_HTTP`, and `LUCE_HAS_TCP_CLI`.
- These flags are gated in `include/luce_build.h`, but implementations for those feature blocks are not present in `src/` yet.
- The plan must split only what exists and avoid introducing placeholder modules for absent subsystems.

## Target module tree for the current phase
- `src/main.cpp`
- `include/luce/runtime.h`
- `include/luce/boot_diagnostics.h`
- `src/boot_diagnostics.cpp`
- `include/luce/boot_state.h`
- `src/boot_state.cpp`
- `include/luce/i2c_io.h`
- `src/i2c_io.cpp`
- `include/luce/cli.h`
- `src/cli.cpp`
- `src/main.cpp`

## Dependency and header constraints
- Keep all public headers under `include/luce`.
- Keep ESP-IDF-heavy includes in `.cpp` files.
- No cross-cycle call chains:
- CLI and runtime modules consume status/state from `i2c_io` only through `include/luce/i2c_io.h`.
- Diagnostics reads I/O globals for optional `luce_log_status_health` reporting.
- Boot state and diagnostics stay independent from CLI command logic.
- `LUCE_HAS_*` gates remain compile-time and no runtime feature flags are introduced.

## Slice-by-slice plan
- S0: Baseline snapshot and no-op verification
- Files: `docs/work/plan/090_main_split_plan.md`, `docs/work/plan/091_module_contracts_outline.md`.
- Actions:
- Confirm actual module layout, signatures, and state ownership.
- Confirm there are no new root files to modify.
- Acceptance gates:
- `pio run -e luce_core`
- `pio run -e luce_core`
- `pio run -e luce_core`
- `pio run -e luce_core`
- `pio run -e luce_core`
- `pio run -e luce_net0`
- `pio run -e luce_net0`
- `pio run -e luce_net0`
- `pio run -e luce_net0`
- `pio run -e luce_net1`
- `pio run -e luce_net1`
- `pio run -e luce_net1`
- `scripts/test_firmware_stage10.sh` (requires connected hardware)

- S1: Preserve and isolate orchestration entry
- Files: `src/main.cpp`.
- Actions:
- Verify `src/main.cpp` remains the only non-test C entrypoint for ESP-IDF.
- Keep `luce_runtime_main()` as module orchestration only.
- Keep non-I2C path behavior (`blink_alive()` blocking loop) unchanged.
- Keep I2C path behavior (tasking + blocking main loop) unchanged.
- Acceptance gates: same command matrix as S0.

- S2: Freeze diagnostics contract
- Files: `include/luce/boot_diagnostics.h`, `src/boot_diagnostics.cpp`.
- Actions:
- Ensure all signature names in docs match implementation exactly.
- Keep log format and ordering unchanged.
- Keep optional heap integrity path gated by `LUCE_DEBUG_STAGE4_DIAG`.
- Acceptance gates:
- `pio run -e luce_core`
- `pio run -e luce_core`
- `pio run -e luce_core`
- `pio run -e luce_core`
- `pio run -e luce_core`

- S3: Freeze NVS contract
- Files: `include/luce/boot_state.h`, `src/boot_state.cpp`.
- Actions:
- Keep compile-time gating in header and source with `LUCE_HAS_NVS`.
- Keep boot record schema and dump output stable.
- Acceptance gates:
- `pio run -e luce_core`
- `pio run -e luce_core`
- `pio run -e luce_core`
- `pio run -e luce_core`

- S4: Freeze Stage2 I/O contract
- Files: `include/luce/i2c_io.h`, `src/i2c_io.cpp`.
- Actions:
- Keep combined I2C/MCP/LCD block in one module for this stage.
- Keep MCP absent/present degraded mode logic unchanged.
- Keep relay polarity explicit (`kRelayActiveHigh`/`kRelayOffValue`) and startup safe defaults unchanged.
- Acceptance gates:
- `pio run -e luce_core`
- `pio run -e luce_core`
- `pio run -e luce_core`

- S5: Freeze CLI contract
- Files: `include/luce/cli.h`, `src/cli.cpp`.
- Actions:
- Keep command parser and execution unchanged.
- Keep UART task stack/task priority unchanged.
- Keep diagnostics and relay command output unchanged.
- Acceptance gates:
- `pio run -e luce_core`

- S6: Defer future stage modules
- Files: none.
- Actions:
- Do not create placeholder modules for Wi‑Fi/NTP/mDNS/TCP CLI/MQTT/HTTP until implementations exist in source.
- When implemented, add explicit module boundaries with disabled stubs only where needed for call-site stability.
- Acceptance gates:
- `pio run -e luce_net0`
- `pio run -e luce_net0`
- `pio run -e luce_net0`
- `pio run -e luce_net0`
- `pio run -e luce_net1`
- `pio run -e luce_net1`

## AGENTS/layout sanity check
- All plan artifacts are under `docs/work/plan/`.
- Source remains under `src/`.
- Headers remain under `include/`.
- `platformio.ini` and root stable files are untouched by this plan.
