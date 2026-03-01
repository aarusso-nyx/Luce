# LUCE Pytest Suite

Install dependencies:

```bash
python3 -m pip install -r tests/requirements.txt
```

Use the test suite to spawn a temporary Python broker (no Docker):

```bash
python3 scripts/test_layers.py --layers mqtt --spawn-test-mqtt-broker --mqtt-topic luce/net1
```

Run all tests through the layered entrypoint:

```bash
python3 scripts/test_layers.py --layers all --env net1 --host https://<device-ip> --http-token <token> --tcp-token <cli-token>
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
