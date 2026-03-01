# Build Status

- timestamp: 2026-02-28T20:17:00.000Z

## Checks

- lint: PASS (`scripts/lint.sh`)
- build: PASS (`pio run -e luce_net1`)
- test: BLOCKED (`scripts/test_firmware_net1.sh 10`; upload failed because `/dev/cu.usbserial-0001` was not available)
- upload: SKIPPED (no hardware attached in this evidence run)
- boot: SKIPPED (no upload path validated in this evidence run)
- e2e: PREREQ_MISSING for NET1 (credentials/network prerequisites absent)

## Notes

- Native host testing retired by policy.
- Canonical test target is real firmware on `luce_net1`.
