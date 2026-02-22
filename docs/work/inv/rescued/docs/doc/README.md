# Luce v2 Firmware

Luce v2 is an ESP-IDF firmware reference design for ESP32 automation nodes with:

- MCP23017 I2C relay expander with physical buttons
- 20x4 I2C LCD (PCF8574 + HD44780)
- DHT22 + LDR + VCC sensing
- FreeRTOS task/queue architecture
- NVS-backed config/state
- Serial + Telnet CLI
- HTTP API + static UI serving from LittleFS
- MQTT command/telemetry integration
- OTA update service
- Session-based logging (`/storage/log/<boot>.log`)

## Design Goals

1. Physical controls are always authoritative.
2. External writes are funneled through command queue and policy reducer.
3. Runtime features (HTTP/MQTT/Telnet/OTA/session log) can be toggled and persisted.
4. Failure modes degrade predictably with explicit health/error signalling.

## Current Layout

- Source: `/Users/aarusso/Documents/PlatformIO/Projects/Luce/src`
- Legacy snapshot: `/Users/aarusso/Documents/PlatformIO/Projects/Luce/shelf`
- Documentation: `/Users/aarusso/Documents/PlatformIO/Projects/Luce/doc`

## Build

PlatformIO environment: `nodemcu-32s` (framework `espidf`).

Typical commands:

```bash
pio run
pio run -t upload
pio device monitor
```

### Local quality gate

Run both lint and rebuild in one step with the project helper:

```bash
./scripts/lint-and-build.sh
```

The script uses your local virtualenv first (`./.venv_ci/bin/pio`), then falls back to
`./.venv/bin/pio` or `pio` on `PATH`.

## Runtime Flow

1. Boot: NVS + LittleFS mount.
2. Initialize queues, store, logger, supervisor, I2C, HALs.
3. Start core tasks (`logger`, `supervisor`, `reducer`, `command`, `relays`, `sensors`, `wifi`, `cli`, `lcd`).
4. Start feature-gated tasks (`http`, `mqtt`, `ota`) when enabled.

See `/Users/aarusso/Documents/PlatformIO/Projects/Luce/doc/ARCHITECTURE.md` for full details.
