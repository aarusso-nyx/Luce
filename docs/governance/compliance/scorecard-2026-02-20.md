# Scorecard - 2026-02-20

- nci: 100
- rating: PASS
- last_updated: 2026-02-22

## BOOTSTRAP complete
- [x] Required governance evidence files present
- [x] Scratch work directories present (`docs/work/plan`, `docs/work/inv`, `docs/work/diag`)
- [x] Root `README.md` updated with build/flash/monitor and `LUCE_STAGE`
- [x] PlatformIO/ESP-IDF `.gitignore` baseline present
- [x] `.editorconfig` and minimal `.clang-format` present
- [x] Deterministic build entrypoints present under `scripts/`
- [x] Bootstrap plan documented at `docs/work/plan/000_bootstrap.md`
- [x] At least one PlatformIO environment build executed

## STAGE1 complete
- [x] Added `luce_stage1` env with `-DLUCE_STAGE=1`
- [x] NVS boot state record implemented (`boot_count`, `last_reset_reason`)
- [x] NVS entries logged with namespace/key/type/value where possible
- [x] Device/app metadata and partition summary logged
- [x] Stage0 and stage1 builds run to confirm LUCE stage gating

## Evidence
- Build command: `python3 -m platformio run -e nodemcu-32s`
- Build result: `SUCCESS` on 2026-02-22
- Build command: `python3 -m platformio run -e luce_stage0`
- Build result: `SUCCESS` on 2026-02-22
- Build command: `python3 -m platformio run -e luce_stage1`
- Build result: `SUCCESS` on 2026-02-22
