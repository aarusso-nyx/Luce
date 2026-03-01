# Luce Firmware

ESP32 firmware project built with PlatformIO + ESP-IDF.

## Build/Flash/Monitor

Use the repository scripts for deterministic local workflows:

```bash
./scripts/build.sh                 # build all strategy environments
./scripts/flash.sh luce_core        # flash a specific environment
./scripts/monitor.sh luce_core      # serial monitor with timestamps
```

You can also call PlatformIO directly:

```bash
pio run -e luce_core
pio run -e luce_core -t upload
pio device monitor -e luce_core --timestamp
```

## LUCE_STRATEGY

`LUCE_STRATEGY` selects one of the compile-time strategy gates passed to `build_flags` as `LUCE_STRATEGY=<value>`.

- `CORE` (`LUCE_STRATEGY=LUCE_STRATEGY_CORE`) — NVS, I2C, LCD, Serial CLI
- `NET0` (`LUCE_STRATEGY=LUCE_STRATEGY_NET0`) — CORE + Wi-Fi + NTP + mDNS + TCP CLI
- `NET1` (`LUCE_STRATEGY=LUCE_STRATEGY_NET1`) — NET0 + MQTT + HTTP

In `scripts/build.sh`, `LUCE_STRATEGY` can be used to build matching environments (`luce_core`, `luce_net0`, `luce_net1`).
If no matching env exists, all declared environments are built.

Ports used by bootstrap scripts:
```bash
Upload:  /dev/cu.usbserial-0001
Monitor: /dev/cu.usbserial-40110
```

## Documentation Locations

- Product/architecture/operations docs: `docs/user/`
- Governance evidence: `docs/governance/audit/`, `docs/governance/health/`, `docs/governance/compliance/`
- Scratch investigation/diagnostics/plans: `docs/work/inv/`, `docs/work/diag/`, `docs/work/plan/`
