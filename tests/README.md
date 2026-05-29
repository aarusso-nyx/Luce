# LUCE Pytest Suite

Install dependencies:

```bash
python3 -m pip install -r tests/requirements.txt
```

Use the test suite to spawn a temporary Python broker (no Docker):

```bash
python3 scripts/test_layers.py --layers mqtt --spawn-test-mqtt-broker --mqtt-topic luce/net1
```

Note: MQTT reconnect/backoff coverage requires managed broker mode (`--spawn-test-mqtt-broker`) so tests can control broker outage/recovery.

Run all tests through the layered entrypoint:

```bash
python3 scripts/test_layers.py --layers all --env net1 --host https://<device-ip> --http-token <token> --tcp-token <cli-token>
```

Run the local Wokwi simulator lane:

```bash
./scripts/luce.sh test --target wokwi --layers build,boot,http,tcp,ws,mqtt --env net1 --spawn-test-mqtt-broker
```

Wokwi mode requires `wokwi-cli` and `WOKWI_CLI_TOKEN`. Current `wokwi-cli`
covers build, boot, and UART CLI smoke. HTTP, TCP, WS, and MQTT layers are
recorded as `DESELECTED` with `deselected:wokwi` by default because CLI
simulations do not attach to the Private IoT Gateway for incoming forwarded
ports. Hardware-in-loop remains the release gate for protocol contracts.

Fail-loud policy:

- A selected layer must have its prerequisites. Missing tokens, serial ports,
  broker connectivity, or Python dependencies fail preflight.
- Skips are reserved for layers that are not selected. The summary reports
  `ran`, `fail`, and `deselected` counts so coverage loss is visible.

Layer model:

- Runner-native layers: `build`, `boot`
- Pytest layers: `http`, `tcp`, `ws`, `mqtt`, `serial`
- Direct `pytest` invocation does not run `build` or `boot`.

Run serial lifecycle/parser coverage layer:

```bash
python3 scripts/test_layers.py --layers serial --host https://<device-ip> --http-token <token> --monitor-port /dev/cu.usbserial-40110
```

Run only critical network contract layers:

```bash
python3 scripts/test_layers.py --layers critical --host https://<device-ip> --http-token <token> --tcp-token <cli-token>
```

Direct pytest invocation:

```bash
python3 -m pytest tests/test_http_contract.py --junitxml docs/work/diag/<run_id>/test-layers/junit-http.xml
```

Useful markers:

- `-m contract`
- `-m "contract and net"`
- `-m hil`
