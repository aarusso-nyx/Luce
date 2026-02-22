# Luce Firmware

ESP32 firmware project built with PlatformIO + ESP-IDF.

## Build/Flash/Monitor

Use the repository scripts for deterministic local workflows:

```bash
./scripts/build.sh                 # build all stage environments
./scripts/flash.sh nodemcu-32s     # flash a specific environment
./scripts/monitor.sh nodemcu-32s   # serial monitor with timestamps
```

You can also call PlatformIO directly:

```bash
pio run -e nodemcu-32s
pio run -e nodemcu-32s -t upload
pio device monitor -e nodemcu-32s --timestamp
```

## LUCE_STAGE

`LUCE_STAGE` defines the intended deployment stage (`dev`, `stage`, or `prod`).

- In scripts, if no stage-specific PlatformIO environments are present yet, all declared environments are built.
- If stage-specific environments exist, `LUCE_STAGE` can be used to scope commands to that stage.
- Default stage is `dev`.

Example:

```bash
LUCE_STAGE=stage ./scripts/build.sh
```

## Documentation Locations

- Product/architecture/operations docs: `doc/`
- Governance evidence: `docs/governance/audit/`, `docs/governance/health/`, `docs/governance/compliance/`
- Scratch investigation/diagnostics/plans: `docs/work/inv/`, `docs/work/diag/`, `docs/work/plan/`
