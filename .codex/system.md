# Repository Scope

This repository is firmware-only (ESP32 + PlatformIO + ESP-IDF).
- No backend or frontend application stacks are in scope.
- Networking services and data-store backends are out of scope for firmware boot/diagnostic stages.

## Governance Scope Exception

- NPM security auditing is explicitly not applicable to this repository unless a Node.js
  dependency graph is introduced. This project is platformio/esp-idf firmware only and does
  not execute dependency-managed server/browser runtimes. Repository-local rationale:
  `docs/governance/audit/npm-applicability.md`.
