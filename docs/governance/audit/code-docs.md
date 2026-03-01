# Code vs Docs Audit

- date: 2026-03-01
- docs_user_files_detected: 12
- code_files_detected: 38
- status: PASS

## Findings

- No open code-vs-doc contract mismatches were found in current authoritative docs (`AGENTS.md` + `docs/user/*`) versus implementation.

## Evidence

- Platform flags:
  - [AGENTS.md](/Users/aarusso/Development/Luce/AGENTS.md:33)
  - [platformio.ini](/Users/aarusso/Development/Luce/platformio.ini:57)
- TCP CLI policy and wire responses:
  - [docs/user/cli-contract.md](/Users/aarusso/Development/Luce/docs/user/cli-contract.md:54)
  - [docs/user/cli-contract.md](/Users/aarusso/Development/Luce/docs/user/cli-contract.md:62)
  - [src/cli.cpp](/Users/aarusso/Development/Luce/src/cli.cpp:167)
  - [src/cli_tcp.cpp](/Users/aarusso/Development/Luce/src/cli_tcp.cpp:126)
- HTTP OTA-check + captive behavior:
  - [docs/user/http.md](/Users/aarusso/Development/Luce/docs/user/http.md:31)
  - [docs/user/http.md](/Users/aarusso/Development/Luce/docs/user/http.md:63)
  - [src/http_server.cpp](/Users/aarusso/Development/Luce/src/http_server.cpp:516)
  - [src/http_server.cpp](/Users/aarusso/Development/Luce/src/http_server.cpp:728)
- Testing output path and layer model:
  - [docs/user/testing.md](/Users/aarusso/Development/Luce/docs/user/testing.md:29)
  - [scripts/test_layers.py](/Users/aarusso/Development/Luce/scripts/test_layers.py:500)

## Notes

- `GOVERNANCE.md` was read as descriptive context (non-binding by instruction). Any wording drift there was not treated as a contract failure.
