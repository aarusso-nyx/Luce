# Luce Firmware

ESP32 firmware project built with PlatformIO + ESP-IDF.

## Build/Flash/Monitor

Use the repository scripts for deterministic local workflows:

```bash
./scripts/build.sh                 # build all stage environments
./scripts/flash.sh luce_stage0     # flash a specific environment
./scripts/monitor.sh luce_stage0   # serial monitor with timestamps
```

You can also call PlatformIO directly:

```bash
pio run -e luce_stage0
pio run -e luce_stage0 -t upload
pio device monitor -e luce_stage0 --timestamp
```

## LUCE_STAGE

`LUCE_STAGE` is the integer stage gate used across build targets:

- `0` → Stage0 (UART diagnostics only)
- `1` → Stage1 (+NVS)
- `2` → Stage2 (+I2C/MCP diagnostics)
- `3` → Stage3 (+LCD)
- `4` → Stage4 (+CLI)

In `scripts/build.sh`, `LUCE_STAGE` can be used to build matching environments.
If no matching env exists, all declared environments are built.

Ports used by bootstrap scripts:
```bash
Upload:  /dev/cu.usbserial-0001
Monitor: /dev/cu.usbserial-40110
```

## Documentation Locations

- Product/architecture/operations docs: `doc/`
- Governance evidence: `docs/governance/audit/`, `docs/governance/health/`, `docs/governance/compliance/`
- Scratch investigation/diagnostics/plans: `docs/work/inv/`, `docs/work/diag/`, `docs/work/plan/`
