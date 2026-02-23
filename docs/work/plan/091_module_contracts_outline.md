# Module Contracts Outline (Current Modules)

## Ownership and module responsibilities
- Diagnostics and startup logs: `boot_diagnostics`
- Boot/NVS persistence: `boot_state`
- I2C/MCP/LCD/stage2 runtime: `stage2_io`
- CLI parser/command task: `cli`
- Orchestration: `main_runtime` and `app_main`

## `include/luce/runtime.h`
- Purpose: entrypoint contract for orchestration.
- API:
- `void luce_runtime_main();`
- Tags: `[luce_boot]`
- Ownership: startup sequencing only; no mutable firmware state.

## `include/luce/boot_diagnostics.h`
- Purpose: startup/system diagnostics and health reporting.
- API:
- `const char* luce_reset_reason_to_string(esp_reset_reason_t reason);`
- `std::size_t luce_init_path_reset_reason_line(char* out, std::size_t out_size, esp_reset_reason_t reason);`
- `void luce_log_heap_integrity(const char* context);`
- `void luce_log_startup_banner();`
- `void luce_print_feature_flags();`
- `void luce_print_chip_info();`
- `void luce_print_app_info();`
- `void luce_print_partition_summary();`
- `void luce_print_heap_stats();`
- `void luce_log_status_health();`
- `void luce_log_runtime_status(std::uint64_t uptime_s, bool i2c_ok, bool mcp_ok, std::uint8_t relay_mask, std::uint8_t button_mask);`
- `void luce_log_stage_watermarks(const char* context);`
- Logging tag: `"luce_boot"`.
- Ownership: all health/status text and diagnostics counters.

## `include/luce/boot_state.h`
- Purpose: NVS boot counters and record dump.
- API:
- `void dump_nvs_entries();`
- `void update_boot_state_record();`
- `const char* nvs_type_name(nvs_type_t type);` (internal `nvs_type` conversion helper may remain source-local if include refactoring is needed; header currently exposes only two public functions.)
- Logging tag: `"luce_boot"`.
- Ownership: namespace `boot` state and persisted boot counters.
- Gating:
- API functions are compiled only when `LUCE_HAS_NVS` is true.

## `include/luce/stage2_io.h`
- Purpose: I2C bus scan, MCP23017 control, LCD status rendering, and Stage2 diagnostics.
- API:
- `enum class InitPathStatus : uint8_t { kUnknown = 0, kSuccess, kFailure };`
- `struct InitPathResult { bool ok; esp_err_t error; InitPathStatus status; };`
- `struct I2cScanResult { bool mcp; bool lcd; int found_count; };`
- `struct Mcp23017State { bool connected; uint8_t relay_mask; };`
- `constexpr std::uint8_t kMcpRegGpioA`
- `constexpr std::uint8_t kMcpRegGpioB`
- `extern bool g_i2c_initialized;`
- `extern bool g_mcp_available;`
- `extern uint8_t g_relay_mask;`
- `extern uint8_t g_button_mask;`
- `#if LUCE_HAS_LCD`
- `extern bool g_lcd_present;`
- `#endif`
- `const char* init_status_name(InitPathStatus status);`
- `InitPathResult init_result_success();`
- `InitPathResult init_result_failure(esp_err_t error);`
- `I2cScanResult scan_i2c_bus();`
- `InitPathResult run_i2c_scan_flow(I2cScanResult& scan, const char* context, bool attach_lcd);`
- `esp_err_t i2c_probe_device(uint8_t address, TickType_t timeout_ticks = pdMS_TO_TICKS(20));`
- `esp_err_t mcp_read_reg(uint8_t reg, uint8_t* value);`
- `uint8_t relay_mask_for_channel(int channel);`
- `esp_err_t set_relay_mask_safe(uint8_t mask);`
- `uint8_t relay_mask_for_channel_state(int channel, bool on, uint8_t current_mask);`
- `bool read_button_inputs(uint8_t* value);`
- `void configure_int_pin();`
- `void run_stage2_diagnostics();`
- `bool stage2_lcd_write_text(const char* text);`
- Logging tag: `"luce_boot"`.
- State ownership:
- `g_i2c_initialized`, `g_mcp_available`, `g_relay_mask`, `g_button_mask`, `g_lcd_present`.
- Includes:
- `Pcf8574Hd44780` driver class and internal helpers (`write_pcf`, `pulse_en`, `write_nibble`, `send_byte`, `send_command`, `set_cursor`, `write_line`, `write_text_line`, `write_status_lines`) currently implemented inside `src/stage2_io.cpp`.
- No separate module is created at this stage for LCD/MCP/IO internals.
- Gating:
- Entire file is active only under `LUCE_HAS_I2C`.
- LCD-related symbols are additionally gated by `LUCE_HAS_LCD` and `LUCE_STAGE4_LCD`.

## `include/luce/cli.h`
- Purpose: CLI startup.
- API:
- `void cli_startup();`
- Logging tag: `"luce_boot"`.
- Ownership: command parser and task startup boundary.
- Behavior:
- Command parser and handlers remain in `src/cli.cpp`.
- Gating:
- `cli_startup()` compiles and no-ops when `!LUCE_HAS_CLI`.

## Not-yet-existing modules (do not create placeholders)
- `include/luce/net_wifi.h`
- `src/net_wifi.cpp`
- `include/luce/ntp.h`
- `src/ntp.cpp`
- `include/luce/mdns.h`
- `src/mdns.cpp`
- `include/luce/mqtt.h`
- `src/mqtt.cpp`
- `include/luce/http.h`
- `src/http.cpp`
- `include/luce/cli_tcp.h`
- `src/cli_tcp.cpp`
- Reason: flags exist in `luce_build.h`, but bodies are not present in `src` and would be premature to split.

## Header policy
- Keep public headers free of unnecessary heavy ESP-IDF headers.
- If a function currently requires an ESP-IDF type in a signature, include the narrowest required ESP-IDF header in the header to preserve compilation.
- Preserve compile-time gating through `LUCE_HAS_*` in header declarations, mirroring current source behavior.

## Logging contracts
- Diagnostics + CLI + Stage2 logs all currently emit through `ESP_LOGI/ESP_LOGW` tag `"luce_boot"`.
- No command output or status logger should change without explicit evidence update.

## Slice acceptance commands
- Always run from `platformio.ini` env list:
- `pio run -e luce_stage0`
- `pio run -e luce_stage1`
- `pio run -e luce_stage2`
- `pio run -e luce_stage3`
- `pio run -e luce_stage4`
- `pio run -e luce_stage5`
- `pio run -e luce_stage6`
- `pio run -e luce_stage7`
- `pio run -e luce_stage8`
- `pio run -e luce_stage9`
- `pio run -e luce_stage10`
- `pio run -e luce_test_native` (if applicable)
- `pio test -e luce_test_native` (UNAVAILABLE if no runnable tests).

## Plan note for execution
- Stage gate constants and future feature macro comments remain in `include/luce_build.h`.
- This outline is a split-contract snapshot, not a behavior change.
