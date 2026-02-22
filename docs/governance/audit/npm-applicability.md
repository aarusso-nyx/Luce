# NPM Applicability Note

## Verdict

`NPM`/`npm-security-upgrade` audits are **not applicable** to this repository at this time.

## Why

This project is firmware-only (ESP32 + PlatformIO + ESP-IDF).
- The repository does not contain a JavaScript runtime for application/server execution.
- There is no Node dependency graph in this workspace (no `package.json`/`package-lock.json`/`yarn.lock`/`pnpm-lock.yaml` files).
- Build, diagnostics, and flash workflows are driven by PlatformIO/ESP-IDF tooling, not npm.

## Scope implication

- Security and compliance checks should use firmware-relevant tooling and evidence paths.
- If Node.js tooling is introduced in future (for example a web front-end or backend service), this note must be revisited and npm/security audit gates should then be re-enabled.
