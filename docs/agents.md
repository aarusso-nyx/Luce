# Agent Serial Ports

Operational serial port policy for Luce CORE/NET workflows:

- Flash/upload port: `/dev/cu.usbserial-0001`
- Mirror monitor/console port: `/dev/cu.usbserial-40110`

Use the following exact commands unless the port assignment is changed intentionally:

- Flash CORE firmware: `python3 -m platformio run -e luce_core -t upload --upload-port /dev/cu.usbserial-0001`
- Monitor serial console continuously: `python3 -m platformio device monitor -p /dev/cu.usbserial-40110`
- NET0/NEXT CLI target: `python3 -m platformio run -e luce_net0 -t upload --upload-port /dev/cu.usbserial-0001`

## Autonomous execution flow

For autonomous firmware iteration:

1. Build the target env: `python3 -m platformio run -e <env>`
2. Upload firmware: `python3 -m platformio run -e <env> -t upload --upload-port /dev/cu.usbserial-0001`
3. Capture boot and runtime output on monitor port: `python3 -m platformio device monitor -p /dev/cu.usbserial-40110`
4. For `luce_net0`, run CLI commands (`help`, `status`, `nvs_dump`, `i2c_scan`, `mcp_read`, `relay_set`, `relay_mask`, `buttons`, `lcd_print`, `reboot`) and paste back output.
5. Paste back the first 80 lines after reset for diagnosis.

Default target during bootstrap is `luce_core` until a later strategy env is requested.

### Monitor behavior note

On this host, monitor command using `/dev/cu.usbserial-40110` can fail with:

`termios.error: (19, 'Operation not supported by device')`

Fallback direct-reader when that happens:

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
