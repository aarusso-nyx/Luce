# Legacy vs Current Firmware Comparison & Parity Plan

Date: 2026-03-01
Scope: compare `./src` against `./data/src` and `./data/src` legacy contracts.
Goal: document concrete deltas and actionable plan to bring missing legacy behavior into current firmware.

## 1) High-level architecture diff

- Current (`./src`) is split into compile-time feature planes:
  - Baseline: `LUCE_HAS_*` for `WIFI`, `NTP`, `mDNS`, `TCP_CLI`; `LUCE_HAS_MQTT`; `LUCE_HAS_HTTP`.
  - Independent startup in `app_main.cpp`: LED/IO -> CLI -> Wi-Fi -> NTP -> mDNS -> TCP CLI -> MQTT -> HTTP.
  - Runtime modules are mostly self-contained finite-state tasks with compact status snapshots.

- Legacy (`./data/src`) is behavior-centric and event-driven:
  - Global `Settings` object owns config/state (`config` + `relays` NVS namespaces).
  - EventBus dispatch (`EventBus::dispatch`) to let HTTP/MQTT/relays consume sensor events.
  - `networkInit()` initializes Wi-Fi/SNTP/mDNS together; then service tasks are started conditionally.
  - OTA as explicit task in startup and settings-based lifecycle.

Net impact: current is cleaner and more explicit, but several legacy behaviors are currently omitted or reduced.

## 2) Feature parity matrix

### A. CLI and command transport

| Area | Legacy (`data/src`) | Current (`./src`) | Gap status |
|---|---|---|---|
| Serial command parser | `esp_console` + command table with rich command set | Custom UART parser + fixed command table | Partial parity (functionally custom)
| Transport | Serial + Telnet (`config::TELNET_PORT`) | Serial + TCP auth-only `cli_tcp` (`AUTH <token>`) | Partial: telnet removed, net policy changed
| Command set | `version`, `info`, `wakeup`, `reset`, `test`, `log`, `set`, `free`, `sensor`, `uptime`, `nvs`, `wifi`, `parts`, `reboot`, `reboot` etc | `help`, `status`, `nvs_dump`, `i2c_scan`, `mcp_read`, `relay_set`, `relay_mask`, `buttons`, `lcd_print`, `reboot`, `wifi.status`, `wifi.scan`, `time.status`, `mdns.status`, `mqtt.status`, `http.status`, `cli_net.status`, `mqtt.pubtest` | Missing many diagnostics/control commands
| Mutating policy in remote CLI | Telnet uses same policy as serial in legacy model | Dedicated `tcp_readonly` in `cli_tcp.cpp` and explicit deny for mutating ops | Safer but narrower command surface
| TCP auth behavior | Not explicit in legacy; telnet-like input loop | `AUTH <token>`, max failures then drop | Different semantics with clearer auth boundary

### B. HTTP API and web surface

| Area | Legacy | Current | Gap status |
|---|---|---|---|
| Endpoints | `GET /api/info`, `GET /api/version`, static files, websocket `/ws` | `GET /api/health`, `GET /api/info`, `GET /api/state` | Major gap
| Auth model | Public-ish endpoints on HTTP layer, mostly informational | Bearer token for `/api/info` and `/api/state`; `/api/health` unauthenticated | More restrictive auth and fewer endpoints
| Payload depth | `info` includes relay masks, wifi state, sensors, version, thresholds, uptime | current `info` is compact strategy/uplink/ports fields | Reduced observability
| Extra web contracts | Captive + `/*` static mapping + ws handler from `http.cpp` | None

### C. MQTT behavior

| Area | Legacy (`data/src/mqtt.cpp`) | Current (`./src/mqtt.cpp`) | Gap status |
|---|---|---|---|
| Subscriptions/inbound control | Subscribes to `device/config/#`, `relays/#`, `sensor/#`, `leds/#`; in-process handlers mutate `Settings`/relays | No subscriptions / no inbound command route | Large regression in control
| Publish topics | Multiple state/sensor topics (`sensor/*`, `relays/state`, `relays/state/*`, `leds/state`) with direct device prefix | Single periodic topic `telemetry/state` only | Topic fan-in compressed
| Topic base | device name from settings `Settings.getName()` | configurable `mqtt/base_topic` in NVS | Partial compatibility impact (namespace mismatch)
| Publish payload | sensor numeric values + logs/events | Current JSON telemetry only (`fw`, `strategy`, ip, relay/button/rssi) | Narrower signal to downstream

### D. OTA

| Area | Legacy | Current | Gap status |
|---|---|---|---|
| Module | `otaInit` + `otaLoop` task exists and started at boot (`app_main.cpp`) | Not started in `main.cpp` runtime | Missing full OTA pipeline |
| URL/password | `Settings` stores `otaPass` and runtime config available | No OTA namespace in current boot path |
| CLI/API triggers | OTA implied via task + settings | None |

### E. NVS schema and settings model

| Area | Legacy | Current | Gap status |
|---|---|---|---|
| Config model | Global `Settings` (`config` + `relays` + `state`) with migration helpers and persistence | Namespace-per-feature (`wifi`, `ntp`, `mdns`, `cli_net`, `mqtt`, `http`) and runtime globals | Architectural change
| Device name | `config/name` drives topic prefix, mDNS naming, display labels | No direct canonical `config/name` equivalent in current config model | Breaks compatibility expectations
| Relay state persistence | `relays/state`, `relays/night`, `relays/light` | Runtime relay state only in RAM (`g_relay_mask`); no `relays` namespace writes | Missing persistence/restore
| Logging settings | `Settings` has log format/level controls and log buffer size | No serial/log config commands nor storage in current path | Loss of observability tools

### F. Relay/IO and automation semantics

| Area | Legacy | Current | Gap status |
|---|---|---|---|
| Input behavior | Physical inputs handled as interrupt-like polling in MCP task + command API for range operations | Button polling still present, now tied to runtime diagnostics loop | Reduced command abstraction and no automation helpers |
| Automation | Relay/night mode helpers (`relaysUpdate`, `setNight`, `setState`) tied to sensor thresholds | No persisted day/night mode logic and no threshold-driven relay scheduling | Missing higher-level behavior |
| Event fan-out | Events published on relays/sensors and consumed by MQTT/HTTP paths | EventBus no longer part of current module contract | Reduced decoupling and reactivity

### G. LCD behavior

| Area | Legacy | Current | Gap status |
|---|---|---|---|
| UI model | Page framework (`PAGE_SUMMARY`, `PAGE_SENSORS`, `PAGE_NETWORK`, `PAGE_RELAYS`, `PAGE_LOGS`, `PAGE_SYSTEM`, `PAGE_SYSTEM2`) | Fixed 4-line status page | UX and diagnostics reduced |
| Key navigation | `lcdHandleKey` + page switch semantics | Key reads exist but no page-mode behavior in active runtime surface | Behavior reduction |
| Data surface | Sensor + threshold + night/day + logs + system info in pages | Compact status with SSID/relay/buttons/sensor/light

### H. LED behavior

| Area | Legacy (`data/src/leds.*`) | Current (`./src/led_status.cpp`) | Gap status |
|---|---|---|---|
| API model | `Leds.set(blink/pulse)` with direct calls from modules | `led_status` state machine (`device/network/operation`) and event queue | More sophisticated intent model |
| Semantics | Mixed direct imperative updates | Structured startup/network/user/error patterns with service-sequence blinking | Behavior shifted; legacy mapping not equivalent |
| Compatibility risk | Legacy code calls Leds API directly | Calls replaced; no compat wrapper for old `LedsClass` usage |

### I. Startup/lifecycle and transport readiness

| Area | Legacy | Current | Gap status |
|---|---|---|---|
| Startup order | Network+sensor init in one path with task bootstrap | Ordered explicit tasks, independent feature gates | More deterministic but less legacy coupled flow |
| NVS bootstrap | `nvs_flash_init()` then `Settings.begin()` | init behavior in each module with robust missing-namespace defaults | Similar intent, different ownership |
| Discovery naming | `mdns_init` with `deviceName`, `_http`, `_mqtt` services | configurable instance/port and `_luce` service | Not interoperable with old service discovery |

## 3) Concrete parity gaps to close (current implementation tasks)

Priority by impact:

1) **Restore legacy CLI commands in compatibility mode**
- Add commands in `src/cli.cpp` and docs:
  - `info`, `version`, `state`, `wakeup`, `reset`, `test`, `log`, `set`, `free`, `sensor`, `parts`, `nvs` aliases.
- Keep transport policy split:
  - Serial: full command behavior preserved where safe.
  - TCP CLI: remain denylist on mutating commands by default (as already designed).

2) **Reintroduce legacy settings compatibility facade**
- Add shim mapping for old keys (`config/name`, `config/ssid`, `config/ssid2`, `config/pass`, `config/pass2`, `config/relay/state`, `config/relays/night`, `config/light`) into current namespaces.
- Implement in dedicated helper with read-once migration function:
  - if legacy namespaces exist, project into `wifi`, `mqtt`, `mdns`, and IO state defaults.
- Keep runtime authoritative values in current namespaces so migration remains one-way and non-destructive.

3) **Restore OTA task path behind explicit compatibility feature**
- Add `LUCE_NET_OTA` (or equivalent) and startup call `ota_startup` where supported.
- Add NVS keys in `ota` module equivalent to legacy `otaPass`, and optional trigger via command/API.
- Wire secure failure/error logging for diagnostics and LED status feedback.

4) **Restore inbound MQTT control + richer outbound topics**
- Add subscribe topics for at least: `config/#`, `relays/#`, `sensor/#`, `leds/#`.
- Add outbound aliases so old consumers still receive:
  - `sensor/lighting`, `sensor/voltage`, `sensor/temperature`, `sensor/humidity`
  - `relays/state` + `relays/state/<idx>`
- Keep existing `base_topic` topic for new integrations.

5) **Restore HTTP `/api/version` and optional static/socket scaffolding in compatibility mode**
- Add `/api/version` (lightweight).
- Preserve `/api/health|info|state` model currently in use for new tooling.
- Add optional static handler under build flag (or compile-time include) for `/` + captive-style redirect and future asset serving.
- Add minimal websocket compatibility route (`/ws`) as read-only broadcaster where feasible.

6) **Reintroduce page-oriented LCD view mode in compatibility mode**
- Add key-driven page state and legacy page renderer behind `LUCE_HAS_LCD_PAGES`.
- Keep current compact status page as default fast-path; keep legacy pages for legacy users via explicit mode.

7) **Restore relay persistence and automation hooks**
- Add NVS-backed relay persist (`relays` namespace) for:
  - `state`, `night`, `light`
- Restore day/night/state update path using existing thresholds and button/relay events when mode is enabled.

## 4) Deep differences for key contracts (diagnostic details)

### A. HTTP contract deltas
- **Legacy GET /api/info** includes:
  - `version`, `uptimeMs`, `relays`, `nightMask`, `day`, `threshold`, `light`, sensor values, network summary.
- **Current GET /api/info** includes:
  - `service`, `strategy`, `wifi_ip`, `http_enabled`, `http_port`, `tls`, `uptime_s`.
- **Legacy GET /api/version** returns firmware-only version json.
- **Current** has no `/api/version`.

### B. MQTT topic map delta
- **Legacy subscribe prefixes**: `<device>/config/#`, `<device>/relays/#`, `<device>/sensor/#`, `<device>/leds/#`.
- **Legacy publish examples**:
  - `<device>/sensor/temperature`, `<device>/sensor/humidity`, `<device>/sensor/lighting`, `<device>/sensor/voltage`, `<device>/relays/state/<idx>`.
- **Current publish**: `<base_topic>/telemetry/state` only.

### C. CLI command intent delta
- **Legacy `set`** supports composite selectors and LED state writes.
- **Current** has direct relay set/mask only.
- **Legacy `log`** can show/edit formatter levels and ring buffer size.
- **Current** has no log-control command surface and no in-command diagnostics for memory/stack beyond boot dumps.

### D. NVS compatibility delta
- Legacy expects `config` namespace for device identity and Wi‑Fi credentials.
- Current requires `wifi` namespace and keeps hostname only in that namespace.
- Legacy boot can derive defaults by saving `config` keys and then using `Settings::loadConfig()`.
- Current boot reads per-feature config independently and does not consume `config` namespace.

## 5) Plan with implementation milestones

### Milestone A (1–2 days): Observability parity
- Add legacy commands `info/version/sensor/free/log/set/wakeup/reset/test/parts/nvs` in `src/cli.cpp`.
- Add HTTP `/api/version` and route/alias docs in `docs/user`.

### Milestone B (2–3 days): Compatibility shim + config migration
- Implement `include/luce/legacy_settings.h`/`.cpp` loader.
- Add unit-like integration check: validate `wifi/ntp/mdns/mqtt/http` defaults get set from legacy keys when missing.

### Milestone C (3–4 days): Migrate command/control channels
- Add inbound MQTT handlers for `config/*`, `relays/*`, `sensor/threshold`, optional `leds/*`.
- Add topic alias publisher for legacy topic families.
- Add one compatibility boolean (non-default) to gate legacy inbound commands.

### Milestone D (2–3 days): OTA + transport recovery
- Add OTA module and entrypoint in `main.cpp` behind feature flag.
- Add `/api/version` and static/ws compatibility scaffolding in HTTP module behind feature flag.

### Milestone E (2–4 days): Relay persistence and behavior
- Persist relay/night/light into NVS namespace and restore on boot.
- Gate any automated behavior (night/day) behind feature flags to avoid size/regression risk.

## 6) Current risks and cautions

- **Namespace migration risk**: preserving topic/service compatibility requires explicit aliasing to avoid duplicate command handling.
- **Behavioral mismatch on startup**: legacy had `networkInit` all-in-one semantics; current modular startup can expose transient states longer but is cleaner for recovery.
- **Storage compatibility**: legacy key migration should be read-through/one-shot and logged, then optionally scrubed.
- **Security posture**: legacy had no authenticated MQTT/CLI transport hardening; compatibility commands should remain optional and off by default in production.
- **Size budget**: adding full legacy pages/mode + OTA + inbound MQTT can significantly grow binary; keep feature flags default-off.
