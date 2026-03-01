# Build Status

- timestamp: 2026-02-28T20:17:00.000Z

## Checks

- lint: PASS (`scripts/luce.sh lint`)
- build: PASS (`pio run -e net1`)
- test: BLOCKED (`scripts/luce.sh test --env net1 --duration 45`; upload failed because `/dev/cu.usbserial-0001` was not available)
- upload: SKIPPED (no hardware attached in this evidence run)
- boot: SKIPPED (no upload path validated in this evidence run)
- e2e: PREREQ_MISSING for NET1 (credentials/network prerequisites absent)

## Notes

- Native host testing retired by policy.
- Canonical test target is real firmware on `net1`.
