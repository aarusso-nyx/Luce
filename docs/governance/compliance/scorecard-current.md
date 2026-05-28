# LUCE Scorecard (canonical pointer)

- latest_scorecard: `docs/governance/compliance/scorecard-2026-02-23.md`
- scoring_source: `docs/governance/compliance/scoring.md`
- local_evidence_policy: current evidence snapshots are generated under `docs/work/diag/` and remain local-only.
- nci: stale; historical scorecard reported 100, but current checkout requires fresh local evidence before reasserting NCI.
- latest_evidence_anchor: regenerate with `./scripts/luce.sh health`, `./scripts/luce.sh build --env net1`, and hardware-backed `./scripts/luce.sh test --layers boot --env net1 --boot-duration 45`.
- policy_update: native/stub tests removed on 2026-02-28; firmware-only test path is `./scripts/luce.sh test --env net1`.
- current_tooling_note: `source ~/.zshrc` exposes working `pio`; current `default`, `net0`, and `net1` firmware builds pass. Python contract-test dependencies are installed in repo-local `.venv`; hardware-backed protocol layers still require device/network prerequisites.

## Historical scorecards

- `docs/governance/compliance/archive/scorecard-2026-02-20.md`
- `docs/governance/compliance/archive/scorecard-2026-02-22.md`
