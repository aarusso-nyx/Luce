# Stage5 Wi‑Fi Lifecycle Design

## 1) Scope

- Included:
  - Wi‑Fi STA bring-up
  - bounded reconnect + exponential backoff
  - event-driven state transition logging
  - NVS-driven config read (namespace `wifi`)
  - CLI diagnostics: `wifi.status`, `wifi.scan`
  - no runtime disabling toggles (compile-time excluded unless `LUCE_HAS_WIFI`)
- Excluded:
  - no mDNS/TCP server/HTTP/MQTT/NTP/client-server networking
  - no auto-reboot on failure

## 2) State Machine

`DISABLED | INIT | CONNECTING | GOT_IP | BACKOFF | STOPPED`

| State | Transition In | Transition Out |
| --- | --- | --- |
| `DISABLED` | config `enabled=0` or runtime disable | `cfg_disabled`, `scan` call |
| `INIT` | Wi‑Fi stack created | `sta_start` ⇒ `CONNECTING`, config errors ⇒ `DISABLED` |
| `CONNECTING` | `connect()` called | `IP_EVENT_STA_GOT_IP` ⇒ `GOT_IP`, `WIFI_EVENT_STA_DISCONNECTED` ⇒ `BACKOFF`/`STOPPED` |
| `GOT_IP` | `IP_EVENT_STA_GOT_IP` | `IP_EVENT_STA_LOST_IP` or disconnect ⇒ `BACKOFF`/`DISABLED` |
| `BACKOFF` | disconnect/retry failures | timer elapsed ⇒ `CONNECTING`, retries exhausted ⇒ `STOPPED` |
| `STOPPED` | `max_retries` exceeded | no auto-recovery unless reboot/power cycle or config change |

## 3) Retry / Backoff Policy

- Attempt increment on each connect try.
- Backoff base is `backoff_min_ms`.
- Delay grows by doubling with each failure: `delay = min(base << n, backoff_max_ms)`.
- `max_retries = 0` means unbounded attempts.
- No reboot on failure.
- `schedule_backoff()` logs `[WIFI][BACKOFF]`.

## 4) NVS Keys (namespace: `wifi`)

- `enabled` (`u8`) 
- `ssid` (`string`) 
- `pass` (`string`, mask in logs)
- `hostname` (`string`)
- `max_retries` (`u32`)
- `backoff_min_ms` (`u32`)
- `backoff_max_ms` (`u32`)

## 5) Observability Requirements

Log lines must include these tags:

- `[WIFI][NVS]`:
  - per-key read/miss + defaults
  - masked config summary line
- `[WIFI][LIFECYCLE]`:
  - every state transition with reason
- `[WIFI][BACKOFF]`:
  - scheduled backoff delay and remaining delay
- `[WIFI][STATUS]`:
  - periodic runtime status snapshots
- `[WIFI][SCAN]`:
  - CLI scan result count and AP summary lines

## 6) CLI Extensions (Serial only)

- `wifi.status` → status snapshot
- `wifi.scan` → foreground scan + AP lines (if Wi‑Fi enabled)

## 7) Acceptance Criteria

- Build matrix compiles for stages 0..6.
- Stage5 startup logs include:
  - config summary
  - lifecycle transition into CONNECTING
  - either GOT_IP or BACKOFF trace sequence depending on environment
- CLI commands `help`, `wifi.status`, `wifi.scan` parse and return command-line logs.
- Evidence artifacts required:
  - `docs/work/diag/template_stage5_expected_boot.txt`
  - `docs/work/diag/` capture for stage5 boot and CLI status checks
