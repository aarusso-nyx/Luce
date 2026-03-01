# Luce Firmware

ESP32 firmware project built with PlatformIO + ESP-IDF.

> Before running any `pio` command, ensure your shell environment is initialized:
> `source ~/.zshrc`

## Build/Flash/Monitor

Use the repository scripts for deterministic local workflows:

```bash
source ~/.zshrc
./scripts/build.sh                 # build all environments
./scripts/flash.sh default         # flash a specific environment
./scripts/monitor.sh default       # serial monitor with timestamps
```

You can also call PlatformIO directly:

```bash
source ~/.zshrc
pio run -e default
pio run -e net0
pio run -e net1
pio run -e default -t upload
pio device monitor -e default --timestamp
```

## Feature flags

`LUCE_NET_*` selects network feature groups in `build_flags`.

- `default` — no `LUCE_NET_*` flags (baseline): NVS, I2C, LCD, CLI
- `net0` — `-DLUCE_NET_CORE=1`: baseline + Wi-Fi + NTP + mDNS + TCP CLI
- `net1` — `-DLUCE_NET_CORE=1 -DLUCE_NET_MQTT=1 -DLUCE_NET_HTTP=1`: net0 + MQTT + HTTP

In `scripts/build.sh`, `LUCE_ENV` can optionally build a single env. If unset, all declared envs are built.

Default PlatformIO environment is `default`.
Canonical SDK config is a single file: `sdkconfig` (shared across all envs).

Ports used by bootstrap scripts:
```bash
Upload:  /dev/cu.usbserial-0001
Monitor: /dev/cu.usbserial-40110
```

## Documentation Locations

- Product/architecture/operations docs: `docs/user/`
- Governance evidence: `docs/governance/audit/`, `docs/governance/health/`, `docs/governance/compliance/`
- Scratch investigation/diagnostics/plans: `docs/work/inv/`, `docs/work/diag/`, `docs/work/plan/`
