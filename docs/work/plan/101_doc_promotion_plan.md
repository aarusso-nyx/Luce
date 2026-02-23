# LUCE Documentation Promotion Plan (Post-Implementation)

Date: 2026-02-23

## Authoritative docs that must exist in `docs/user`

1. `docs/user/architecture.md`
2. `docs/user/cli-contract.md`
3. `docs/user/wifi-lifecycle.md`
4. `docs/user/ntp.md`
5. `docs/user/mdns.md`
6. `docs/user/mqtt.md`
7. `docs/user/http.md`
8. `docs/user/nvs-schema.md`
9. `docs/user/hardware-map.md`

## Source locations and actions

1. `docs/user/architecture.md`  
   Source references: `include/luce_build.h`, `src/main.cpp`, `docs/work/plan/070_phase2_overview.md`, `docs/work/plan/090_main_split_plan.md`.  
   Action: create/update for single source of truth on stage gating and bootstrap observability.
2. `docs/user/cli-contract.md`  
   Source references: `src/main.cpp` command handlers and `docs/user/CLI.md`.  
   Action: create/update with Stage8 TCP auth/allowlist policy and serial command matrix.
3. `docs/user/wifi-lifecycle.md`  
   Source references: `src/main.cpp` Wi-Fi section, `docs/work/plan/070_phase2_overview.md`.  
   Action: create/update with event lifecycle and NVS defaults.
4. `docs/user/ntp.md`  
   Source references: `docs/work/plan/077_stage6_ntp.md`, `src/main.cpp`.  
   Action: create/update with `ntp` schema and sync states.
5. `docs/user/mdns.md`  
   Source references: `docs/work/plan/079_stage7_mdns.md`, `src/main.cpp`.  
   Action: create/update with hostname policy and TXT fields.
6. `docs/user/mqtt.md`  
   Source references: `docs/work/plan/083_stage9_mqtt.md`, `src/main.cpp`.  
   Action: create/update with publish-only contract and security controls.
7. `docs/user/http.md`  
   Source references: `docs/work/plan/086_stage10_https.md`, `src/main.cpp`.  
   Action: create/update with endpoints, auth policy, TLS note.
8. `docs/user/nvs-schema.md`  
   Source references: `docs/work/plan/071_phase2_nvs_schema.md`, `src/main.cpp`.  
   Action: create/update as canonical key list.
9. `docs/user/hardware-map.md`  
   Source references: `docs/work/inv/hardware-map.md`, historical rescue artifacts as needed.  
   Action: migrate hardware mapping to canonical doc location and keep legacy file for continuity.

## Verification section

Each promoted doc includes:
Verification path: `docs/work/diag/20260222_214039/90_summary.md`
git SHA: `2a3b9df`

## Contradictions found and resolved

1. `docs/user/CLI.md` remains historical and useful, but full authority is now `docs/user/cli-contract.md`.
2. `docs/dev/hardware-map.md` remains historical; `docs/user/hardware-map.md` is now canonical.
3. `docs/work/plan/070+` contain stage dependency notes; `docs/user/*` now flatten those into user-facing canonical docs.
