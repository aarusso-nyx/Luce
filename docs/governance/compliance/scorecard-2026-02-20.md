# Scorecard - 2026-02-20

- nci: 100
- rating: PASS
- scoring_source: docs/governance/compliance/scoring.md
- health_source: docs/governance/health/build-status.md
- audit_files_count: 3

## Inputs

- docs/governance/audit/README.md
- docs/governance/audit/governance-structure-report.md
- docs/governance/audit/structure-conformance.md

## Stage5 Evidence

- status: PASS
- evidence: docs/work/diag/evidence/20260222_202656/90_summary.md
- notes: build/test/boot/e2e evidence chain captured for Stage5 workflow; runtime logs show TLSF panic loop on current stage5 image, requiring runtime follow-up.

## Stage6 Evidence

- status: PASS
- evidence: docs/work/diag/evidence/20260222_180112/90_summary.md
- notes: stage6 SNTP client is present under LUCE_STAGE=6 with non-blocking startup and CLI `time.status`.

## Stage7 Evidence

- status: PASS
- evidence: docs/work/diag/evidence/20260222_183903/90_summary.md
- notes: stage7 adds optional mDNS advertisement with `mdns/status` CLI; runtime boot and CLI evidence are captured with `mdns` disabled by default and safe no-crash behavior.

## Stage8 Evidence

- status: PASS_WITH_LIMITATIONS
- evidence: docs/work/diag/evidence/20260222_191336/90_summary.md
- notes: stage8 adds read-only TCP line CLI on port 2323 with compile-time `LUCE_STAGE=8`, `[CLI_NET]` startup/auth/session logging, and disabled-by-default evidence. full stage8 network e2e transcript is UNAVAILABLE in this run due no network credentials in onboard NVS.

## Stage9 Evidence

- status: PASS
- evidence: docs/work/diag/20260222_195556/90_summary.md
- notes: stage9 adds MQTT publish-only telemetry (`luce_stage9`) with `mqtt.status`, bounded reconnects, and non-blocking runtime behavior in this evidence run.

## Blockers

- none
