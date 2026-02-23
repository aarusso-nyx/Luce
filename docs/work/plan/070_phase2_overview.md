# Phase 2 Overview

## Stage Ordering

Phase 2 advances platform capabilities in deterministic compile-time slices:

- Stage5: Wi‑Fi lifecycle (required before time/NTP)
- Stage6: NTP synchronization (depends on Stage5/`LUCE_HAS_WIFI`)
- Stage7+: services on top of Wi‑Fi transport

## Dependency Graph (compile-time)

```mermaid
flowchart TD
  A[LUCE_STAGE=0] --> B[LUCE_HAS_NVS]
  B --> C[LUCE_HAS_I2C]
  C --> D[LUCE_HAS_LCD]
  D --> E[LUCE_HAS_CLI]
  E --> F[LUCE_HAS_WIFI (Stage5)]
  F --> G[LUCE_HAS_NTP (Stage6)]
  G --> H[LUCE_HAS_MDNS / TCP / MQTT / HTTP]
```

## Notes

- Stage5 and Stage6 are first-class networking milestones in this branch.
- Stage6 must never compile or run when `LUCE_STAGE < 6`.
- Any network command extensions stay CLI-only in this phase.
