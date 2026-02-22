# Luce Test Suite Playbook

This repository ships a three-tier local test stack:

- `host`: deterministic logic and parser tests executed on native toolchain.
- `embedded`: firmware-native checks through PlatformIO unit-test targets.
- `hil`: optional physical integration runner for HTTP/MQTT/CLI/OTA contracts.

### Run everything

```bash
./scripts/test/run_all.sh
```

### Run a single HIL scenario

```bash
python3 scripts/test/run_hil.py --config test/hil/hil_config.example.yaml
```

Scenarios are files in `test/hil/scenarios/*.py`.
