# Luce Firmware

ESP32 firmware project built with PlatformIO + ESP-IDF.

> `scripts/luce.sh` refreshes PlatformIO discovery, including sourcing your `~/.zshrc` environment when required.

## Build/Flash/Monitor

Use the repository scripts for deterministic local workflows:

```bash
source ~/.zshrc
./scripts/luce.sh build                    # build all environments
./scripts/luce.sh upload --env default      # flash a specific environment
./scripts/luce.sh monitor --env default     # serial monitor with timestamps
./scripts/luce.sh test --env net1 --duration 45 # smoke test net1
```
You can also call PlatformIO directly when preferred.

## Serial Ports and Runtime Iteration

Operational serial port policy for Luce CORE/NET workflows:

- Flash/upload port: `/dev/cu.usbserial-0001`
- Mirror monitor/console port: `/dev/cu.usbserial-40110`

Use the following exact commands unless the port assignment is changed intentionally:

```bash
python3 -m platformio run -e default -t upload --upload-port /dev/cu.usbserial-0001
python3 -m platformio device monitor -p /dev/cu.usbserial-40110
python3 -m platformio run -e net0 -t upload --upload-port /dev/cu.usbserial-0001
```

Autonomous firmware iteration:

```bash
source ~/.zshrc
python3 -m platformio run -e <env>
python3 -m platformio run -e <env> -t upload --upload-port /dev/cu.usbserial-0001
python3 -m platformio device monitor -p /dev/cu.usbserial-40110
```

For `net0`, run CLI commands (`help`, `status`, `nvs_dump`, `i2c_scan`, `mcp_read`, `relay_set`, `relay_mask`, `buttons`, `lcd_print`, `reboot`) and collect the first 80 monitor lines after reset.

Monitor fallback (if termios fails on this host):

```bash
python3 - <<'PY'
import sys, serial
with serial.Serial('/dev/cu.usbserial-40110', 115200, timeout=0.2) as ser:
    for _ in range(80):
        line = ser.readline()
        if line:
            sys.stdout.write(line.decode('utf-8', errors='replace'))
PY
```

## Feature flags

`LUCE_NET_*` selects network feature groups in `build_flags`.

- `default` — no `LUCE_NET_*` flags (baseline): NVS, I2C, LCD, CLI
- `net0` — `-DLUCE_NET_CORE=1`: baseline + Wi-Fi + NTP + mDNS + TCP CLI
- `net1` — `-DLUCE_NET_CORE=1 -DLUCE_NET_MQTT=1 -DLUCE_NET_HTTP=1`: net0 + MQTT + HTTP

In `scripts/luce.sh build`, `LUCE_ENV` can optionally build a single env. If unset, all declared envs are built.
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
