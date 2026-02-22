# Hardware Map (Authoritative)

Date: 2026-02-22
Status: Authoritative for firmware pin mapping
Scope: Derived from rescued KiCAD project at `docs/work/inv/rescued/pcb/`.

## ESP32 Board And Core GPIO Mapping

Board:
- PlatformIO target: `nodemcu-32s`
- KiCAD footprint: `ESP32-DEVKIT-V1` (`U4`)

Core mappings used by firmware:
- I2C SCL: `GPIO22` (`U4 D22`) -> net `Net-(J4-SCL)`
- I2C SDA: `GPIO23` (`U4 D23`) -> net `Net-(J4-SDA)`
- DHT data: `GPIO13` (`U4 D13`) -> net `Net-(U1-SDA)`
- LDR ADC: `GPIO34` (`U4 D34`) -> net `Net-(U4-D34)`
- MCP interrupt input: `GPIO19` (`U4 D19`) via level shifter (`U3 LV1` <- `U3 HV1` <- `U2 ITB`)

Status LEDs:
- LED0: `GPIO25` (`U4 D25`) -> LED `D1` (via resistor network)
- LED1: `GPIO26` (`U4 D26`) -> LED `D2` (via resistor network)
- LED2: `GPIO27` (`U4 D27`) -> LED `D3` (via resistor network)

Secondary ADC channel present on board:
- Voltage sense: `GPIO35` (`U4 D35`) -> net `Net-(U4-D35)`

## MCP23017 Mapping (`U2`)

Module/part:
- Reference: `U2`
- Value: `CJMCU-2317` (MCP23017 breakout)

I2C address strapping:
- `A0` strapped to GND
- `A1` strapped to GND
- `A2` strapped to GND
- Effective address: `0x20`
- LCD backpack is fixed on `0x27` and is validated on 3.3V I2C bus.

Interrupt lines:
- `ITB` wired and used (to ESP32 `GPIO19` through `U3`)
- `ITA` not connected (intentionally unused on this hardware)

Port mapping:
- Relay outputs:
  - Active polarity: active-LOW (`1` = OFF, `0` = ON)
  - `A0 -> J1 CH1`
  - `A1 -> J1 CH2`
  - `A2 -> J1 CH3`
  - `A3 -> J1 CH4`
  - `A4 -> J1 CH5`
  - `A5 -> J1 CH6`
  - `A6 -> J1 CH7`
  - `A7 -> J1 CH8`
- Button inputs:
  - Active-LOW (`1` = released/idle, `0` = pressed)
  - `B0 -> J2 B1`
  - `B1 -> J2 B2`
  - `B2 -> J2 B3`
  - `B3 -> J2 B4`
  - `B4 -> J2 B5`
  - `B5 -> J2 B6`
  - `B6 -> J2 B7`
  - `B7 -> J2 B8`

## Connector Pinouts

### J1 (`Relays`, 1x10)
- Pin 1: `+5V`
- Pin 2: `GND`
- Pin 3: `CH1` (MCP `A0`)
- Pin 4: `CH2` (MCP `A1`)
- Pin 5: `CH3` (MCP `A2`)
- Pin 6: `CH4` (MCP `A3`)
- Pin 7: `CH5` (MCP `A4`)
- Pin 8: `CH6` (MCP `A5`)
- Pin 9: `CH7` (MCP `A6`)
- Pin 10: `CH8` (MCP `A7`)

### J2 (`BUTTONS`, 1x09)
- Pin 1: `GND`
- Pin 2: `B1` (to MCP `B0`)
- Pin 3: `B2` (to MCP `B1`)
- Pin 4: `B3` (to MCP `B2`)
- Pin 5: `B4` (to MCP `B3`)
- Pin 6: `B5` (to MCP `B4`)
- Pin 7: `B6` (to MCP `B5`)
- Pin 8: `B7` (to MCP `B6`)
- Pin 9: `B8` (to MCP `B7`)

### J3 (`UART`, 1x03)
- Pin 1: `GND`
- Pin 2: `TX` (to ESP32 `RX0`)
- Pin 3: `RX` (to ESP32 `TX0`)

### J4 (`I2C`, 1x04)
- Pin 1: `SDA` (ESP32 `GPIO23`)
- Pin 2: `SCL` (ESP32 `GPIO22`)
- Pin 3: `GND`
- Pin 4: `3.3V`

## Notes

- Rescued source under `docs/work/inv/rescued/src/` was used only as a consistency check.
- Firmware should treat this document as canonical until superseded by a new board revision audit.
