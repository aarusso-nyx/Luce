# CLI Reference (Canonical)

CLI is available on serial UART and telnet (when enabled).

## Telnet Security

- When telnet auth is enabled, session starts unauthenticated.
- Authenticate with: `auth <password>`.
- Lockout applies after configured failed attempts.

## Core Commands

- `help`
- `network`
- `runtime`
- `device`
- `config`
- `parts`
- `tasks`
- `health`
- `log [N]`

## Relay and Sensor Commands

- `relays` (show desired/effective)
- `relays <mask>`
- `night` (show)
- `night <mask>`
- `light <threshold>`
- `sensors`

## Config and Feature Commands

- `param <name> <value>`
- `feature <http|mqtt|telnet|ota|sessionlog> <0|1>`
- `save`
- `defaults`

## Service Commands

- `wifi`
- `mqtt`
- `http`
- `ota start [url]`

## System Commands

- `reboot`
- `reset yes`
- `event`

## Notes

- Writes are queued and applied asynchronously by reducer.
- Physical button events remain authoritative.
