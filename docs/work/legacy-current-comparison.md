# Legacy vs Current Firmware Comparison (open gaps only)

Date: 2026-03-01
Scope: `./data/src` (legacy) vs `./src` (current canonical firmware)

This register intentionally lists only unresolved gaps for legacy parity.

## Open gaps (ranked)

1. Legacy config migration is incomplete.
- Missing a general read-through or one-time migration from legacy monolithic `config`/`Settings` keys into canonical namespaces (`wifi`, `mqtt`, `http`, `cli_net`, `mdns`, `sensor`, `relays`, `compat`).

2. MQTT legacy command parity is incomplete.
- Unsupported legacy subtopics still degrade to warning logs only (for example `relays/<legacy-command>`), without deterministic compatibility response payloads.

3. LED control compatibility is not restored.
- `leds/*` remains status-only (`leds/state`, `leds/state/<idx>` snapshots).
- Direct legacy LED actuation semantics are not implemented.

4. Legacy websocket HTTP path is not implemented.
- Legacy `/ws` behavior from `data/src/http.cpp` is absent in current HTTP stack.

5. CLI legacy behavior parity is partial.
- `log buffer|console|logfile` controls remain compatibility-only and are not fully persisted.
- Legacy long-running sensor-loop interaction semantics are reduced to bounded reads.
- `set led` direct control remains unsupported.

6. LCD legacy UI contract is not restored.
- Full legacy 20x4 multi-page interaction model is not reproduced as-is in current runtime UI flow.

7. Legacy event-bus architecture is not restored.
- Current behavior uses direct task/module routing instead of legacy event-bus orchestration.

## Next actions

1. Implement idempotent legacy-key migration helper and migration policy (read-through or one-time upgrade).
2. Add deterministic compatibility responses for unsupported MQTT legacy topics.
3. Decide and document LED/LCD/CLI parity stance: restore or formally deprecate legacy semantics.
4. Decide whether websocket compatibility is required; if yes, implement `/ws` contract or provide migration path.
