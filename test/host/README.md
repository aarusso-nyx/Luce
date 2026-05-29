# LUCE Host Tests

This is the native C++17 unit-test lane for pure helper headers. It is
decoupled from PlatformIO and ESP-IDF so it can run without an attached device.

Run from the repository root:

```bash
scripts/run_host_tests.sh
```

The runner writes build output, JUnit XML, and `summary.json` under
`docs/work/diag/<run-id>/host-tests/`. It uses CMake when available and falls
back to a direct C++17 compiler invocation on minimal developer machines.
