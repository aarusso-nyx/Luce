# LUCE mDNS (NET0+)

Date: 2026-02-28

## Activation

Compiled in when `LUCE_STRATEGY >= LUCE_STRATEGY_NET0`.
Configured by namespace `mdns` plus fallback hostname from `net/hostname`.

## NVS schema (`mdns` and `net`)

- `mdns/enabled` (u8, default `0`)
- `mdns/instance` (string, default `Luce Strategy`)
- `net/hostname` (string, fallback deterministic `luce-xxxx` from MAC)

## Runtime behavior

- Only starts when Wi-Fi IP is present.
- TXT records include:
  - `fw`
  - `strategy`
  - `device`
  - `build`
- Advertised service: `_luce._tcp` at port `80`.
- Stop path runs on IP loss.

## CLI

- `mdns.status` reports runtime state, enabled flag, hostname, service state, and instance.

## Logging

- `[mDNS] enabled=<0|1> ...`
- `[mDNS] state=... reason=...`
- `[mDNS] started` / `failed`

## Verification

- Evidence: `docs/work/diag/evidence/20260222_221921/90_summary.md`
- Evidence SHA: `2a3b9df`
