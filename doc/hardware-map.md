# LUCE Hardware Map (Authoritative)

Date: 2026-02-23

## Board and GPIO mapping

- Board target: `nodemcu-32s` (`ESP32-DEVKIT-V1`)
- I2C:
  - SCL `GPIO22` (`Net-(J4-SCL)`)
  - SDA `GPIO23` (`Net-(J4-SDA)`)
- MCP23017:
  - `A0..A7`: active-LOW relay outputs
  - `B0..B7`: active-LOW inputs
  - Address: `0x20`
- LCD backpack:
  - address fixed at `0x27`
  - bus power `3.3V`
- Interrupt:
  - ITA intentionally unused
  - ITB mapped to `GPIO19`
- Misc:
  - Relay and button active polarity from firmware `kRelayActiveHigh=false`

## Relay mapping

- `A0 -> CH1`, `A1 -> CH2`, `A2 -> CH3`, `A3 -> CH4`
- `A4 -> CH5`, `A5 -> CH6`, `A6 -> CH7`, `A7 -> CH8`

## Button mapping

- `B0 -> B1`, `B1 -> B2`, `B2 -> B3`, `B3 -> B4`
- `B4 -> B5`, `B5 -> B6`, `B6 -> B7`, `B7 -> B8`

## GPIO aliases

- LED heartbeat outputs:
  - `GPIO25`, `GPIO26`, `GPIO27`
- Optional ADC:
  - `GPIO34`, `GPIO35`

