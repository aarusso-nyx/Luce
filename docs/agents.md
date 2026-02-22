# Agent Serial Ports

Operational serial port policy for Luce Stage0/Stage1 work:

- Flash/upload port: `/dev/cu.usbserial-0001`
- Mirror monitor/console port: `/dev/cu.usbserial-40110`

Use the following exact commands unless the port assignment is changed intentionally:

- Flash stage firmware: `python3 -m platformio run -e luce_stage0 -t upload --upload-port /dev/cu.usbserial-0001`
- Monitor serial console continuously: `python3 -m platformio device monitor -p /dev/cu.usbserial-40110`

