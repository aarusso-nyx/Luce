# LUCE Documentation Promotion Plan (Post-Implementation)

Date: 2026-02-23

## Authoritative docs that must exist in `doc/`

1. `doc/architecture.md`
2. `doc/cli-contract.md`
3. `doc/wifi-lifecycle.md`
4. `doc/ntp.md`
5. `doc/mdns.md`
6. `doc/mqtt.md`
7. `doc/http.md`
8. `doc/nvs-schema.md`
9. `doc/hardware-map.md`

## Source locations and actions

1. `doc/architecture.md`  
   Source references: `include/luce_build.h`, `src/main.cpp`, `docs/work/plan/070_phase2_overview.md`, `docs/work/plan/090_main_split_plan.md`.  
   Action: create/update for single source of truth on stage gating and bootstrap observability.
2. `doc/cli-contract.md`  
   Source references: `src/main.cpp` command handlers and `docs/user/CLI.md`.  
   Action: create/update with Stage8 TCP auth/allowlist policy and serial command matrix.
3. `doc/wifi-lifecycle.md`  
   Source references: `src/main.cpp` Wi-Fi section, `docs/work/plan/070_phase2_overview.md`.  
   Action: create/update with event lifecycle and NVS defaults.
4. `doc/ntp.md`  
   Source references: `docs/work/plan/077_stage6_ntp.md`, `src/main.cpp`.  
   Action: create/update with `ntp` schema and sync states.
5. `doc/mdns.md`  
   Source references: `docs/work/plan/079_stage7_mdns.md`, `src/main.cpp`.  
   Action: create/update with hostname policy and TXT fields.
6. `doc/mqtt.md`  
   Source references: `docs/work/plan/083_stage9_mqtt.md`, `src/main.cpp`.  
   Action: create/update with publish-only contract and security controls.
7. `doc/http.md`  
   Source references: `docs/work/plan/086_stage10_https.md`, `src/main.cpp`.  
   Action: create/update with endpoints, auth policy, TLS note.
8. `doc/nvs-schema.md`  
   Source references: `docs/work/plan/071_phase2_nvs_schema.md`, `src/main.cpp`.  
   Action: create/update as canonical key list.
9. `doc/hardware-map.md`  
   Source references: `docs/dev/hardware-map.md`, `docs/work/inv/rescued/src/*` as historical reference.  
   Action: migrate hardware mapping to canonical doc location and keep legacy file for continuity.

## Verification section

Each promoted doc includes:
Verification path: `docs/work/diag/evidence/20260222_211814/90_summary.md`
git SHA: `ecd0768b22d41e07df8b1f025a0416c4e0f753c8`

## Contradictions found and resolved

1. `docs/user/CLI.md` is historical and remains useful, but full authority is now `doc/cli-contract.md`.
2. `docs/dev/hardware-map.md` remains historical; `doc/hardware-map.md` is now canonical.
3. `docs/work/plan/070+` contain stage dependency notes; `doc/*` now flatten those into user-facing canonical docs.
