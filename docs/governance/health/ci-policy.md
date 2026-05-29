# LUCE CI and Local Evidence Policy

Date: 2026-05-29

## Contract

Local evidence is the source of truth for firmware and hardware behavior. CI
does not flash devices, open serial ports, or run HIL contract layers. CI only
runs deterministic hardware-free checks and verifies the committed evidence
manifest.

## CI Jobs

`hardware-free-checks` runs:

- `scripts/run_host_tests.sh`
- `python -m pytest tests/test_docs_alignment.py`
- `clang-format --dry-run --Werror` on the tracked `src/` and
  `include/luce/` C/C++ tree

`verify-evidence` runs:

- `python scripts/verify_evidence.py`

The evidence verifier fails when the manifest is missing, stale, records a
required check as anything other than `PASS`, or lists committed artifacts whose
SHA-256 checksums no longer match. Raw logs stay under ignored `docs/work/diag`;
the committed artifact is `docs/governance/health/evidence-latest.json`, a
small snapshot of the local run and its raw-log checksums.

Release contexts are stricter. `scripts/verify_evidence.py` treats evidence as
release-gated when `--release` is passed, `release: true` is present, the
manifest `source.target` starts with `release`, or GitHub runs on `main` or a
`release/*` branch. In that mode the manifest must include `hardware-hil` with
`required: true` and `status: PASS`; advisory or missing device evidence fails
the gate.

## Local Refresh

Before publishing a release candidate, regenerate local evidence from the repo
root:

```sh
source ~/.zshrc
pio run -e default -e net0 -e net1
scripts/run_host_tests.sh
.venv/bin/python -m pytest tests/test_docs_alignment.py
```

Hardware-backed layers remain the release gate:

```sh
./scripts/luce.sh test --layers boot,http,tcp,ws,mqtt,serial --env net1 \
  --host https://<device-ip> --http-token <token> --tcp-token <cli-token> \
  --spawn-test-mqtt-broker
```

Refresh `docs/governance/health/evidence-latest.json` and
`docs/governance/health/evidence-manifest.json` after the local run and commit
them with the evidence state. For release candidates, run the full hardware
suite with `--write-evidence-manifest --release-evidence` so `hardware-hil` is
required and passing. If the manifest points at the parent commit and the
current commit changes only those evidence files, CI accepts that as the
non-self-referential manifest refresh case.
