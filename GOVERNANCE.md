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
  - `luce_core` — minimal strategy (`LUCE_STRATEGY_CORE`)
  - `luce_net0` — CORE + networking control plane (`LUCE_STRATEGY_NET0`)
  - `luce_net1` — CORE + NET0 + telemetry plane (`LUCE_STRATEGY_NET1`)
- Canonical sdkconfig: `sdkconfig`
- Removed/legacy stage artifacts are intentionally deprecated:
  - `luce_stage*` env naming in PlatformIO/docs plans
  - duplicate stage sdkconfig variants

## Source of Truth for Tooling

- `platformio.ini` is authoritative for environments and active SDK config mapping.
- `scripts/build.sh` is the canonical batch build entry point.
- `scripts/flash.sh` and `scripts/monitor.sh` are canonical deployment/monitor commands.
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
