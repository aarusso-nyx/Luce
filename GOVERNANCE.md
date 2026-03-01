# Luce Governance

This repository is governed by the `.codex/system.md` runtime instructions and `AGENTS.md` project contract.

## Governance Authorities

- Canonical governance evidence:
  - `docs/governance/audit/` (required audit traces)
  - `docs/governance/health/` (preflight / health status)
  - `docs/governance/compliance/` (scorecard and compliance state)
- Authoritative docs for behavior and operations:
  - `docs/user/`
- Working scratch space:
  - `docs/work/inv/`
  - `docs/work/diag/`
  - `docs/work/plan/`

## Repository State Baseline

- Build system: PlatformIO + ESP-IDF
- Canonical build environments:
  - `default` — minimal baseline (NVS, I2C, LCD, CLI)
  - `net0` — `LUCE_NET_CORE=1` (Wi-Fi/NTP/mDNS/TCP CLI)
  - `net1` — `LUCE_NET_CORE=1`, `LUCE_NET_MQTT=1`, `LUCE_NET_HTTP=1`
- Canonical sdkconfig: `sdkconfig`
- Removed/legacy stage artifacts are intentionally deprecated:
  - `luce_stage*` environment naming is retained only in historical artifacts
  - duplicate stage sdkconfig variants

## Source of Truth for Tooling

- `platformio.ini` is authoritative for environments and active SDK config mapping.
- `scripts/luce.sh` is the canonical batch build/deploy/test/health entry point.
- Before any PlatformIO use, load shell environment:

```bash
source ~/.zshrc
```

This ensures `pio` resolves correctly in local shells.

## Required Evidence References

- `docs/governance/audit/structure-conformance.md`
- `docs/governance/compliance/scorecard-current.md`
- `docs/governance/health/preflight.md`

## Conformance Notes

- Keep `AGENTS.md`, `README.md`, and root layout stable and aligned.
- Keep CLI / networking / persistence behavior updates paired with matching updates under `docs/user/`.
- Prefer conservative rollout and explicit diagnostics when changing strategy gates.
