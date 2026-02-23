# LUCE mDNS (Stage7/8)

Date: 2026-02-23

## Activation

Compiled in when `LUCE_STAGE` is `7` or `8`.
Configured by namespace `mdns` plus fallback hostname from `net/hostname`.

## NVS schema (`mdns` and `net`)

- `mdns/enabled` (u8, default `0`)
- `mdns/instance` (string, default `Luce Stage`)
- `net/hostname` (string, fallback deterministic `luce-xxxx` from MAC)

## Runtime behavior

- Only starts when Wi-Fi IP is present.
- TXT records include:
  - `fw`
  - `stage`
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

- Evidence: `docs/work/diag/evidence/20260222_211814/90_summary.md`
- Evidence SHA: `ecd0768b22d41e07df8b1f025a0416c4e0f753c8`

