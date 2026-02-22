# Open Hardware Questions (Bench Verification)

Date: 2026-02-22
Purpose: capture uncertain hardware behaviors requiring bench confirmation.

## 1) Relay Drive Polarity
- Question: are relay channels active-high or active-low at connector `J1` (CH1..CH8)?
- Why it matters: determines safe default state and inversion rules in relay HAL.
- Bench check:
  - Drive one channel at a time from MCP `A*`.
  - Measure relay module input voltage and observed relay click/load state.
  - Record whether `logic 1` means ON or OFF.

## 2) LCD Backpack Address And Voltage Domain
- Question: is the installed LCD backpack address fixed at `0x27`, and is the module electrically 3.3V-safe on the I2C side?
- Why it matters: wrong address or level mismatch causes display bring-up failures and possible bus instability.
- Bench check:
  - Run I2C scan on `GPIO23/GPIO22`.
  - Confirm detected LCD address.
  - Verify pull-up rail and logic-high level on SDA/SCL with scope or DMM.

## 3) Button Wiring And Pull-Up Expectations
- Question: are `J2` buttons wired as active-low to GND with pull-ups (MCP internal or external), matching expected input semantics?
- Why it matters: debounce, interrupt edge selection, and default idle state depend on this.
- Bench check:
  - Measure idle voltage on `B1..B8`.
  - Press each button and capture transition polarity and bounce profile.
  - Confirm interrupt behavior on MCP `ITB` and ESP32 `GPIO19`.

## 4) Optional Clarifications To Capture During Bench Session
- Confirm whether `ITA` is intentionally unused for this revision.
- Confirm relay/button connector orientation labels match physical harness orientation.
- Confirm LED polarity and expected boot-state indications on GPIO25/26/27.
