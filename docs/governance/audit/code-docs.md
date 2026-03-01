# Code vs Docs Audit

- date: 2026-03-01
- docs_user_files_detected: 12
- code_files_detected: 38
- status: PASS

## Findings

- No open code-vs-doc contract mismatches detected in the previously failing areas.

## Notes

- Verified aligned items:
  - `net1` compile flags in [AGENTS.md](/Users/aarusso/Development/Luce/AGENTS.md:33) match [platformio.ini](/Users/aarusso/Development/Luce/platformio.ini:57).
  - TCP read-only list and wire-response semantics in [docs/user/cli-contract.md](/Users/aarusso/Development/Luce/docs/user/cli-contract.md:54) align with [src/cli.cpp](/Users/aarusso/Development/Luce/src/cli.cpp:167) and [src/cli_tcp.cpp](/Users/aarusso/Development/Luce/src/cli_tcp.cpp:189).
  - Test evidence output path in [docs/user/testing.md](/Users/aarusso/Development/Luce/docs/user/testing.md:29) aligns with [scripts/test_layers.py](/Users/aarusso/Development/Luce/scripts/test_layers.py:373).
