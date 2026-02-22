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

## STAGE2 complete
- [x] I2C bring-up and bus scan for stage2.
- [x] MCP23017 relay/button diagnostics with graceful degradation when MCP absent.
- [x] Relay sweep with masked control and debounced button-change logging.
- [x] MCP INT pin sampled/configured and logged.
- [x] Stage0..stage2 build gating confirmed in compile.

## Evidence
- Build command: `python3 -m platformio run -e luce_stage2`
- Build result: `SUCCESS` on 2026-02-22
- Build command: `python3 -m platformio run -e luce_stage0`
- Build result: `SUCCESS` on 2026-02-22
- Build command: `python3 -m platformio run -e luce_stage1`
- Build result: `SUCCESS` on 2026-02-22

## STAGE3 complete
- [x] LCD detection on I2C address `0x27` with graceful degrade when missing.
- [x] 20x4 status rendering implemented (status lines + periodic updates).
- [x] Console mirrors status every few seconds.
- [x] `luce_stage3` build environment added.
- [x] Stage2/Stage3 gating continues to compile/execute through env build matrix.

## Evidence
- Build command: `python3 -m platformio run -e luce_stage3`
- Build result: `SUCCESS` on 2026-02-22
