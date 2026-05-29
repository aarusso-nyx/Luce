# Preflight

- timestamp: 2026-05-28T01:53:00Z
- status: PARTIAL_PASS
- latest_scorecard: docs/governance/compliance/scorecard-current.md
- nci: stale
- min_nci: 75
- override_used: no
- allowed_work_scope: docs, scripts, build environments, repo hygiene

## Violated Rules

- none for health/config/build evidence in the current checkout.
- hardware-backed boot/protocol contract layers still require attached device/network prerequisites.

## Required User Action

- Keep using `source ~/.zshrc` so `~/.platformio/penv/bin/pio` is selected instead of the broken Homebrew `platformio` launcher.
- Use repo-local `.venv` for Python contract-test dependencies; direct Homebrew Python system installs are blocked by PEP 668.
