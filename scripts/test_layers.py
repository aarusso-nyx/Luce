#!/usr/bin/env python3
"""LUCE layered test entrypoint.

Runs firmware and protocol tests by explicit layers, writing deterministic evidence
artifacts under docs/work/diag.
"""

from __future__ import annotations

import argparse
import json
import os
import socket
import shlex
import subprocess
import sys
import textwrap
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Sequence


LAYERS = ("build", "boot", "http", "tcp", "ws", "mqtt", "serial")
LAYER_GROUPS = {
    "critical": ("http", "tcp", "ws", "mqtt"),
}


@dataclass
class LayerResult:
    layer: str
    status: str
    rc: int
    log: str
    details: str = ""


@dataclass
class SpawnedBroker:
    proc: subprocess.Popen[str]
    host: str
    port: int
    log_path: Path


def utc_now_compact() -> str:
    return datetime.now(timezone.utc).strftime("%Y%m%d_%H%M%S")


def shell_join(argv: Sequence[str]) -> str:
    return " ".join(shlex.quote(p) for p in argv)


def append_log(log_path: Path, text: str) -> None:
    log_path.parent.mkdir(parents=True, exist_ok=True)
    with log_path.open("a", encoding="utf-8") as fh:
        fh.write(text)
        if not text.endswith("\n"):
            fh.write("\n")


def run_cmd(
    cmd: Sequence[str],
    *,
    log_path: Path,
    cwd: Path | None = None,
    timeout_s: float | None = None,
) -> int:
    append_log(log_path, f"==> {shell_join(cmd)}")
    append_log(log_path, f"# {datetime.now(timezone.utc).isoformat()}")
    try:
        proc = subprocess.run(
            cmd,
            cwd=str(cwd) if cwd else None,
            capture_output=True,
            text=True,
            timeout=timeout_s,
        )
    except subprocess.TimeoutExpired as exc:
        if exc.stdout:
            append_log(log_path, exc.stdout)
        if exc.stderr:
            append_log(log_path, exc.stderr)
        append_log(log_path, f"command_timeout={timeout_s}")
        append_log(log_path, "command_exit=124")
        return 124
    if proc.stdout:
        append_log(log_path, proc.stdout)
    if proc.stderr:
        append_log(log_path, proc.stderr)
    append_log(log_path, f"command_exit={proc.returncode}")
    return proc.returncode


def resolve_layers(raw: str) -> list[str]:
    raw = raw.strip().lower()
    if raw in ("all", "full"):
        return list(LAYERS)
    if raw in LAYER_GROUPS:
        return list(LAYER_GROUPS[raw])
    selected = []
    for part in raw.split(","):
        item = part.strip()
        if not item:
            continue
        if item in LAYER_GROUPS:
            for expanded in LAYER_GROUPS[item]:
                if expanded not in selected:
                    selected.append(expanded)
            continue
        if item not in LAYERS:
            raise ValueError(f"unknown layer: {item}")
        selected.append(item)
    if not selected:
        raise ValueError("no layers selected")
    return selected


def expected_boot_markers(env: str) -> list[str]:
    env = env.strip().lower()
    base = ["LUCE STRATEGY=", "Feature flags:"]
    if env == "net1":
        base.append("Feature flags: NVS=1 I2C=1 LCD=1 CLI=1 WIFI=1 NTP=1 mDNS=1 MQTT=1 HTTP=1")
    return base


def run_build_layer(args: argparse.Namespace, repo_root: Path, out_dir: Path) -> LayerResult:
    log = out_dir / "build.log"
    zsh_cmd = f"source ~/.zshrc && cd {shlex.quote(str(repo_root))} && python3 -m platformio run -e {shlex.quote(args.env)}"
    rc = run_cmd(["/bin/zsh", "-lc", zsh_cmd], log_path=log, timeout_s=args.layer_timeout_s)
    status = "PASS" if rc == 0 else "FAIL"
    return LayerResult("build", status, rc, str(log))


def run_boot_layer(args: argparse.Namespace, repo_root: Path, out_dir: Path) -> LayerResult:
    log = out_dir / "boot.log"
    serial_log = out_dir / "boot_serial.log"

    upload_cmd = (
        "source ~/.zshrc && "
        f"cd {shlex.quote(str(repo_root))} && "
        f"python3 -m platformio run -e {shlex.quote(args.env)} -t upload --upload-port {shlex.quote(args.upload_port)}"
    )
    rc = run_cmd(["/bin/zsh", "-lc", upload_cmd], log_path=log, timeout_s=args.layer_timeout_s)
    if rc != 0:
        return LayerResult("boot", "FAIL", rc, str(log), "upload failed")

    capture_cmd = [
        "python3",
        str(repo_root / "scripts" / "capture_serial.py"),
        "--port",
        args.monitor_port,
        "--baud",
        "115200",
        "--seconds",
        str(args.boot_duration),
        "--output",
        str(serial_log),
    ]
    rc = run_cmd(capture_cmd, log_path=log, cwd=repo_root, timeout_s=args.layer_timeout_s)
    if rc != 0:
        return LayerResult("boot", "FAIL", rc, str(log), "serial capture failed")

    text = serial_log.read_text(encoding="utf-8", errors="replace") if serial_log.exists() else ""
    required = list(expected_boot_markers(args.env)) + list(args.require_marker)
    missing = [marker for marker in required if marker not in text]
    if missing:
        append_log(log, "missing_markers:")
        for marker in missing:
            append_log(log, f"- {marker}")
        return LayerResult("boot", "FAIL", 2, str(log), f"missing markers: {len(missing)}")

    return LayerResult("boot", "PASS", 0, str(log))


def pytest_cmd_for_layer(args: argparse.Namespace, repo_root: Path, out_dir: Path, layer: str) -> list[str]:
    test_map = {
        "http": "tests/test_http_contract.py",
        "tcp": "tests/test_tcp_cli_contract.py",
        "ws": "tests/test_ws_contract.py",
        "mqtt": "tests/test_mqtt_contract.py",
        "serial": "tests/test_serial_cli_contract.py",
    }
    junit = out_dir / f"junit-{layer}.xml"
    cmd = [
        "python3",
        "-m",
        "pytest",
        str(repo_root / test_map[layer]),
        "--junitxml",
        str(junit),
        "--luce-host",
        args.host,
        "--luce-http-token",
        args.http_token,
        "--luce-tcp-host",
        args.tcp_host,
        "--luce-tcp-port",
        str(args.tcp_port),
        "--luce-tcp-token",
        args.tcp_token,
        "--luce-ws-host",
        args.ws_host,
        "--luce-ws-port",
        str(args.ws_port),
        "--luce-ws-path",
        args.ws_path,
        "--luce-mqtt-host",
        args.mqtt_host,
        "--luce-mqtt-port",
        str(args.mqtt_port),
        "--luce-mqtt-topic",
        args.mqtt_topic,
        "--luce-mqtt-username",
        args.mqtt_username,
        "--luce-mqtt-password",
        args.mqtt_password,
        "--luce-env",
        args.env,
        "--luce-serial-port",
        args.monitor_port,
        "--luce-serial-baud",
        "115200",
        "--luce-serial-reboot-capture-s",
        str(max(15, int(args.boot_duration))),
    ]
    if getattr(args, "test_mqtt_broker_pid", 0):
        cmd.extend(["--luce-test-mqtt-broker-pid", str(args.test_mqtt_broker_pid)])
    if args.ws_tls:
        cmd.append("--luce-ws-tls")
    cmd.extend(args.pytest_arg)
    return cmd


def run_pytest_layer(args: argparse.Namespace, repo_root: Path, out_dir: Path, layer: str) -> LayerResult:
    log = out_dir / f"{layer}.log"
    cmd = pytest_cmd_for_layer(args, repo_root, out_dir, layer)
    rc = run_cmd(cmd, log_path=log, cwd=repo_root, timeout_s=args.layer_timeout_s)
    status = "PASS" if rc == 0 else "FAIL"
    return LayerResult(layer, status, rc, str(log))


def wait_for_tcp(host: str, port: int, timeout_s: float) -> bool:
    deadline = time_monotonic() + timeout_s
    while time_monotonic() < deadline:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
            sock.settimeout(0.5)
            try:
                sock.connect((host, port))
                return True
            except OSError:
                pass
        sleep_short()
    return False


def time_monotonic() -> float:
    import time

    return time.monotonic()


def sleep_short() -> None:
    import time

    time.sleep(0.1)


def spawn_test_mqtt_broker(args: argparse.Namespace, out_dir: Path) -> SpawnedBroker:
    try:
        import amqtt  # noqa: F401
    except ModuleNotFoundError as exc:
        raise RuntimeError("amqtt not installed; install with 'python3 -m pip install -r tests/requirements.txt'") from exc

    broker_log = out_dir / "test-mqtt-broker.log"
    broker_script = out_dir / "_spawn_test_mqtt_broker.py"
    broker_script.write_text(
        textwrap.dedent(
            f"""\
            #!/usr/bin/env python3
            import asyncio
            import signal
            from amqtt.broker import Broker

            HOST = {args.test_mqtt_host!r}
            PORT = {int(args.test_mqtt_port)!r}

            CONFIG = {{
                "listeners": {{
                    "default": {{
                        "type": "tcp",
                        "bind": f"{{HOST}}:{{PORT}}",
                    }}
                }},
                "sys_interval": 0,
                "topic-check": {{
                    "enabled": False
                }},
                "auth": {{
                    "allow-anonymous": True
                }},
            }}

            async def main() -> None:
                broker = Broker(CONFIG)
                await broker.start()
                stop_event = asyncio.Event()

                def _stop(*_args):
                    stop_event.set()

                loop = asyncio.get_running_loop()
                for sig in (signal.SIGINT, signal.SIGTERM):
                    loop.add_signal_handler(sig, _stop)

                print(f"AMQTT broker started on {{HOST}}:{{PORT}}", flush=True)
                await stop_event.wait()
                await broker.shutdown()

            if __name__ == "__main__":
                asyncio.run(main())
            """
        ),
        encoding="utf-8",
    )

    proc = subprocess.Popen(
        ["python3", str(broker_script)],
        stdout=broker_log.open("w", encoding="utf-8"),
        stderr=subprocess.STDOUT,
        text=True,
    )

    if not wait_for_tcp(args.test_mqtt_host, int(args.test_mqtt_port), timeout_s=args.test_mqtt_startup_timeout):
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
        raise RuntimeError(f"failed to start test MQTT broker on {args.test_mqtt_host}:{args.test_mqtt_port}")

    return SpawnedBroker(proc=proc, host=args.test_mqtt_host, port=int(args.test_mqtt_port), log_path=broker_log)


def stop_test_mqtt_broker(broker: SpawnedBroker) -> None:
    if broker.proc.poll() is not None:
        return
    broker.proc.terminate()
    try:
        broker.proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        broker.proc.kill()
        broker.proc.wait(timeout=3)


def write_summaries(out_dir: Path, run_id: str, results: list[LayerResult]) -> None:
    pass_count = sum(1 for r in results if r.status == "PASS")
    fail_count = sum(1 for r in results if r.status == "FAIL")
    skip_count = sum(1 for r in results if r.status == "SKIP")

    md = out_dir / "summary.md"
    js = out_dir / "summary.json"

    lines = [
        "# Layered Test Summary",
        "",
        f"- run_id: {run_id}",
        f"- out_dir: {out_dir}",
        f"- pass: {pass_count}",
        f"- fail: {fail_count}",
        f"- skip: {skip_count}",
        "",
        "## Results",
    ]
    for r in results:
        detail = f" details={r.details}" if r.details else ""
        lines.append(f"- {r.layer}: {r.status} (rc={r.rc}) log={r.log}{detail}")
    md.write_text("\n".join(lines) + "\n", encoding="utf-8")

    payload = {
        "run_id": run_id,
        "out_dir": str(out_dir),
        "pass": pass_count,
        "fail": fail_count,
        "skip": skip_count,
        "results": [r.__dict__ for r in results],
    }
    js.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        prog="scripts/test_layers.py",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        description="Run LUCE tests by explicit layers with structured outputs.",
        epilog=textwrap.dedent(
            """
            Layers:
              build  : PlatformIO build for selected env.
              boot   : upload + bounded serial capture + boot marker assertions.
              http   : pytest HTTP contract checks.
              tcp    : pytest TCP CLI auth/readonly checks.
              ws     : pytest WebSocket /ws handshake + payload checks.
              mqtt   : pytest MQTT compatibility + LED readback checks.
              serial : pytest serial CLI parser + lifecycle reboot checks.

            Selection:
              --layers all
              --layers critical
              --layers build,boot,http,tcp,ws,mqtt,serial
              --layers http,ws

            Output layout:
              docs/work/diag/<run_id>/test-layers/
                - <layer>.log
                - junit-<layer>.xml (pytest layers)
                - summary.md
                - summary.json

            Examples:
              python3 scripts/test_layers.py --layers all --env net1 \
                --host https://192.168.1.99 --http-token TOKEN --tcp-token CLI_TOKEN \
                --ws-host 192.168.1.99 --mqtt-host 192.168.1.20

              python3 scripts/test_layers.py --layers http,ws \
                --host https://192.168.1.99 --http-token TOKEN --ws-host 192.168.1.99

              python3 scripts/test_layers.py --layers mqtt \
                --spawn-test-mqtt-broker --mqtt-topic luce/net1
            """
        ),
    )

    parser.add_argument("--layers", default="all", help="Comma-separated layers or 'all'.")
    parser.add_argument("--continue-on-fail", action="store_true", help="Run all selected layers even after failures.")
    parser.add_argument(
        "--layer-timeout-s",
        type=float,
        default=float(os.getenv("LUCE_LAYER_TIMEOUT_S", "0")),
        help="Per-command timeout in seconds (0 disables timeout).",
    )

    parser.add_argument("--env", default=os.getenv("LUCE_ENV", "net1"), help="PlatformIO environment for build/boot.")
    parser.add_argument("--upload-port", default=os.getenv("LUCE_UPLOAD_PORT", "/dev/cu.usbserial-0001"))
    parser.add_argument("--monitor-port", default=os.getenv("LUCE_MONITOR_PORT", "/dev/cu.usbserial-40110"))
    parser.add_argument("--boot-duration", type=int, default=int(os.getenv("LUCE_TEST_DURATION", "45")))
    parser.add_argument("--require-marker", action="append", default=[], help="Additional required boot marker (repeatable).")

    parser.add_argument("--host", default=os.getenv("LUCE_HOST", "https://127.0.0.1"), help="HTTPS host for HTTP tests.")
    parser.add_argument("--http-token", default=os.getenv("LUCE_HTTP_TOKEN", ""), help="Bearer token for HTTP protected endpoints.")

    parser.add_argument("--tcp-host", default=os.getenv("LUCE_TCP_HOST", "127.0.0.1"))
    parser.add_argument("--tcp-port", type=int, default=int(os.getenv("LUCE_TCP_PORT", "2323")))
    parser.add_argument("--tcp-token", default=os.getenv("LUCE_CLI_NET_TOKEN", ""), help="TCP CLI AUTH token.")

    parser.add_argument("--ws-host", default=os.getenv("LUCE_WS_HOST", "127.0.0.1"))
    parser.add_argument("--ws-port", type=int, default=int(os.getenv("LUCE_WS_PORT", "80")))
    parser.add_argument("--ws-path", default=os.getenv("LUCE_WS_PATH", "/ws"))
    parser.add_argument("--ws-tls", action="store_true", default=os.getenv("LUCE_WS_TLS", "0") == "1")

    parser.add_argument("--mqtt-host", default=os.getenv("LUCE_MQTT_HOST", "127.0.0.1"))
    parser.add_argument("--mqtt-port", type=int, default=int(os.getenv("LUCE_MQTT_PORT", "1883")))
    parser.add_argument("--mqtt-topic", default=os.getenv("LUCE_MQTT_BASE_TOPIC", "luce/net1"))
    parser.add_argument("--mqtt-username", default=os.getenv("LUCE_MQTT_USERNAME", ""))
    parser.add_argument("--mqtt-password", default=os.getenv("LUCE_MQTT_PASSWORD", ""))
    parser.add_argument(
        "--spawn-test-mqtt-broker",
        action="store_true",
        help="Spawn a temporary Python MQTT broker (amqtt) for MQTT layer tests (no Docker).",
    )
    parser.add_argument(
        "--test-mqtt-host",
        default=os.getenv("LUCE_TEST_MQTT_HOST", "127.0.0.1"),
        help="Bind host for spawned test MQTT broker.",
    )
    parser.add_argument(
        "--test-mqtt-port",
        type=int,
        default=int(os.getenv("LUCE_TEST_MQTT_PORT", "18883")),
        help="Bind port for spawned test MQTT broker.",
    )
    parser.add_argument(
        "--test-mqtt-startup-timeout",
        type=float,
        default=float(os.getenv("LUCE_TEST_MQTT_STARTUP_TIMEOUT", "8.0")),
        help="Seconds to wait for spawned test MQTT broker readiness.",
    )

    parser.add_argument("--diag-root", default=os.getenv("LUCE_DIAG_DIR", "docs/work/diag"), help="Diagnostics root path.")
    parser.add_argument("--run-id", default="", help="Optional run id override; default UTC timestamp.")

    parser.add_argument("--pytest-arg", action="append", default=[], help="Extra pytest arg (repeatable).")

    args = parser.parse_args(argv)
    if args.layer_timeout_s <= 0:
        args.layer_timeout_s = None
    try:
        args.layer_list = resolve_layers(args.layers)
    except ValueError as exc:
        parser.error(str(exc))
    return args


def main(argv: Sequence[str]) -> int:
    args = parse_args(argv)
    repo_root = Path(__file__).resolve().parents[1]
    run_id = args.run_id or utc_now_compact()
    diag_root = Path(args.diag_root)
    out_dir = diag_root / run_id / "test-layers"
    out_dir.mkdir(parents=True, exist_ok=True)

    if any(layer in {"http", "tcp", "ws", "mqtt"} for layer in args.layer_list):
        try:
            import pytest  # noqa: F401
        except ModuleNotFoundError:
            print("error: pytest not installed; install with 'python3 -m pip install -r tests/requirements.txt'", file=sys.stderr)
            return 1

    results: list[LayerResult] = []
    spawned_broker: SpawnedBroker | None = None

    layer_handlers = {
        "build": lambda: run_build_layer(args, repo_root, out_dir),
        "boot": lambda: run_boot_layer(args, repo_root, out_dir),
        "http": lambda: run_pytest_layer(args, repo_root, out_dir, "http"),
        "tcp": lambda: run_pytest_layer(args, repo_root, out_dir, "tcp"),
        "ws": lambda: run_pytest_layer(args, repo_root, out_dir, "ws"),
        "mqtt": lambda: run_pytest_layer(args, repo_root, out_dir, "mqtt"),
        "serial": lambda: run_pytest_layer(args, repo_root, out_dir, "serial"),
    }

    try:
        if args.spawn_test_mqtt_broker and "mqtt" in args.layer_list:
            try:
                spawned_broker = spawn_test_mqtt_broker(args, out_dir)
                args.mqtt_host = spawned_broker.host
                args.mqtt_port = spawned_broker.port
                args.test_mqtt_broker_pid = int(spawned_broker.proc.pid)
                print(f"test-mqtt-broker: RUNNING ({spawned_broker.host}:{spawned_broker.port})")
            except RuntimeError as exc:
                results.append(
                    LayerResult(
                        "mqtt-broker",
                        "FAIL",
                        1,
                        str(out_dir / "test-mqtt-broker.log"),
                        str(exc),
                    )
                )
                write_summaries(out_dir, run_id, results)
                print(f"summary: {out_dir / 'summary.md'}")
                return 1

        for layer in args.layer_list:
            result = layer_handlers[layer]()
            results.append(result)
            print(f"{layer}: {result.status} (rc={result.rc})")
            if result.status == "FAIL" and not args.continue_on_fail:
                break
    finally:
        if spawned_broker is not None:
            stop_test_mqtt_broker(spawned_broker)
            print("test-mqtt-broker: STOPPED")

    write_summaries(out_dir, run_id, results)

    failed = any(r.status == "FAIL" for r in results)
    print(f"summary: {out_dir / 'summary.md'}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
