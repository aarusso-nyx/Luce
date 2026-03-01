# Legacy vs Current Firmware Comparison (gap register)

Date: 2026-03-01  
Scope: `./data/src` (legacy) vs `./src` (current canonical firmware)

## 1) Canonical current baseline (current status)

- Build-time feature model:
  - `LUCE_NET_CORE` → `LUCE_HAS_WIFI`, `LUCE_HAS_NVS`, `LUCE_HAS_MDNS`, `LUCE_HAS_TCP_CLI` (always true on core baseline in current architecture).
  - `LUCE_NET_MQTT` → `LUCE_HAS_MQTT`.
  - `LUCE_NET_HTTP` → `LUCE_HAS_HTTP`.
  - `LUCE_NET_OTA` → `LUCE_HAS_OTA`.
- Startup order in [`src/main.cpp`]:
  - `led_status_startup -> io_startup -> cli_startup -> wifi -> ntp -> mdns -> cli_net -> mqtt -> http -> ota`.
- HTTP server now includes:
  - TLS HTTPS API service, authenticated endpoints (`/api/health`, `/api/info`, `/api/state`, `/api/ota`, `/api/ota/check`) and a captive HTTP portal on port `80`.
- OTA pipeline exists end-to-end (`src/ota.*`) with CLI/API checks and HTTP trigger.
- NVS layout is explicit per module namespaces (`wifi`, `ntp`, `mdns`, `mqtt`, `http`, `ota`, `relays`) plus shared boot/runtime keys.
- Relay state is persisted via `relays/state` key and restored on boot.
- LED behavior is centralized (`src/led_status.cpp`) and supports startup/error/sequence semantics.
- MQTT supports runtime telemetry publish + inbound `config/#`, `relays/#`, `sensor/#`, `leds/#`.

## 2) Current parity against legacy (`data/src`)

### A. HTTP/API parity

- **Legacy currently provided**:
  - `/api/info`
  - `/api/version`
  - static/captive files via LittleFS with wildcard redirect
- **Current implementation**:
  - `/api/health`
  - `/api/info`
  - `/api/version`
  - `/api/state`
  - `/api/ota`
  - `/api/ota/check`
  - wildcard captive route on port `80` (TLS data embedded into binary in current build)
- **Gap status**:
  - `info` payload is intentionally reduced compared with legacy (legacy has richer hardware/network/sensor fields).
  - Legacy WebSocket endpoint pathing remains (`websocket_server.h`) but is not present in current build.

### B. CLI parity

- **Common coverage now present**:
  - `version`, `info`, `wakeup`, `uptime`, `state`, `system`, `nvs`, `free`, `sensor`, `parts`, `reset`, `reboot`, `test`, `log`, `set`.
  - Wi‑Fi and time status plus MQTT/HTTP/CLI TCP/OTA helpers are added in current firmware.
  - `nvs_dump`, `i2c_scan`, `mcp_read`, `relay_set`, `relay_mask`, `buttons`, `lcd_print`.
- **Deliberate/runtime differences from legacy**:
  - Legacy `log` has richer behavior (buffer content, logger-level + format management).
  - Legacy `sensor` command can be used as polling loop (legacy supports interval/count with run control; current supports bounded repeat and does not support interactive interrupt semantics).
  - Legacy `free` exposes stack diagnostics; current exposes heap-only snapshot.
  - Current hardens mutating TCP commands (some mutating commands are blocked on net CLI path).
- **Legacy-only intent preserved partly by warning/compat paths**:
  - `set led` is explicitly unsupported in current firmware.
  - `log buffer`/`console`/`logfile` commands are mostly informational/pseudo-ops in current.

### C. MQTT parity

- **Legacy subscription pattern**:
  - `device/config/#`, `device/relays/#`, `device/sensor/#`, `device/leds/#` in legacy (`device` is runtime device name).
- **Current subscription pattern**:
  - `config/#`, `relays/#`, `sensor/#`, `leds/#` under `base_topic` (`luce/net1` default).
- **Legacy outbound topics supported**:
  - `sensor/lighting`, `sensor/voltage`, `sensor/temperature`, `sensor/humidity`.
  - `relays/state`, `relays/state/<idx>`.
- **Current outbound topics**:
  - `telemetry/state`, `sensor/<...>` aliases above, and `relays/state`, `relays/state/<idx>`.
- **Gap status**:
  - `config/*` compatibility has been expanded for common legacy aliases and compat-only fields:
    - `config/ssid`, `config/pass`, `config/ssid2`, `config/pass2`, `config/wifi/*`
    - `config/mqtt` aliases and logger compatibility fields under `compat` namespace
    - `relays/night` and `relays/night/<idx>` are now persisted.
  - `leds/*` now returns deterministic status snapshots for compatibility reads, but does not provide direct LED channel control.
    - `sensor/threshold` now applies live relay scheduling with `relays/night` and `light` threshold control.
  - Full legacy config migration for all `Settings` keys is still missing and would be needed for strict parity.

### D. mDNS/NTP/network config parity

- **Legacy**:
  - mDNS service registration occurs as part of network init with service advertisements for `_http` and `_mqtt`.
  - NTP is brought up from network init and waits for first epoch sync.
- **Current**:
  - Dedicated modules for Wi‑Fi, NTP and mDNS with independent state machines and NVS config.
  - mDNS defaults to `_luce` service and TXT fields.
- **Gap status**:
  - Legacy config surfaces that depended on `Settings` monolith are no longer available as-is; require migration for legacy tooling that expects those exact legacy keys and service names.

### E. LCD/UI parity

- **Legacy**:
  - Multi-page LCD support (summary/network/relays/sensors/etc.) plus key-driven navigation and event bus render model.
- **Current**:
  - LCD renders operational status and button-driven wake/status updates; page navigation API is limited/partially present but not equivalent behaviorally.
- **Gap status**:
  - Full legacy page model and interaction contracts are not restored in current UI loop.

### F. Automation and event-flow parity

- **Legacy**:
  - `eventBus` driven architecture with richer publish/consume paths.
  - Relay night/day concepts (`setNight`, `setLight`) and sensor-threshold automation hooks are present.
- **Current**:
  - Event bus removed from routing path; command/state updates are direct calls.
  - Relay persistence restored, and automatic day/night/rule-driven relay scheduling is reintroduced.
- **Gap status**:
  - Event-bus parity and legacy timing semantics are still open differences; relay scheduling behavior is restored but routed through current task flow.

## 3) Current critical gaps to close (ranked)

1. **Restore remaining legacy MQTT command compatibility**  
   Expand remaining routes for full legacy parity: runtime migration/shims for all legacy `config` keys and compatibility LED control semantics.

2. **Expand `/api/info` with legacy-compatible fields while retaining canonical payload.**

3. **NVS migration/shim from legacy `config` namespace**  
   Compatibility read-through of legacy `config` keys:
   - `name`, `ssid`, `ssid2`, `pass`, `pass2`, `mqtt`, `otaPass`, `interval`, `log*`, etc.

4. **Restore legacy operational semantics in LED/LCD where it is still used by operators**  
   Decide if full 20x4 page model is required beyond current compact status line(s).

5. **Define explicit compatibility for legacy automation features**  
   Decide whether to reintroduce `night/day` + threshold relay scheduling using current `ntp` + sensor snapshot path.

## 4) Concrete implementation plan (next)

1. Expand `/api/info` with legacy-compatible fields while retaining canonical payload.
2. Add migration reads from legacy `config` namespace and persist into canonical namespaces (read-once or dual-write policy).
3. Extend MQTT handlers for legacy control routes that are safe and deterministic (especially `config/*` and `leds/*`).
4. Add CLI/HTTP affordances for legacy settings aliases where practical.
5. Reintroduce automation mapping behind explicit compatibility flag to avoid destabilizing baseline behavior.
