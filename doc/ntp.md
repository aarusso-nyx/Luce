# LUCE SNTP (Stage6)

Date: 2026-02-23

## Purpose

Provides non-blocking time synchronization as a prerequisite for secure operations.

## NVS schema (`ntp` namespace)

- `enabled` (u8, default `0`)
- `server1` (string, default `pool.ntp.org`)
- `server2` (string, default `time.google.com`)
- `server3` (string, optional)
- `sync_timeout_s` (u32, clamped 5..600)
- `sync_interval_s` (u32, clamped 60..86400)

Missing namespace or missing keys are treated as disabled/missing defaults and logged.

## Runtime behavior

- Enabled only when compiled and `ntp/enabled = 1`.
- Starts only with valid Wi-Fi IP.
- State machine logs:
  - `DISABLED`
  - `UNSYNCED`
  - `SYNCING`
  - `SYNCED`
  - `FAILED`
- Retries are bounded by `sync_timeout_s`, max retry attempts, and backoff delay.
- On max-retry exhaustion it returns to `UNSYNCED` after interval delay.

## CLI

- `time.status` output includes:
  - sync state
  - unix timestamp
  - sync age
  - UTC string
  - explicit not-synced reason when unavailable

## Logging

Typical tags:

- `[NTP][LIFECYCLE]`
- `[NTP] status`
- `[NTP][NVS]`
- `time not synced`

## Verification

- Evidence: `docs/work/diag/evidence/20260222_211814/90_summary.md`
- Evidence SHA: `ecd0768b22d41e07df8b1f025a0416c4e0f753c8`

