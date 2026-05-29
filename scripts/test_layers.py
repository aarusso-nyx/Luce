#!/usr/bin/env python3
"""LUCE layered test entrypoint.

Runs firmware and protocol tests by explicit layers, writing deterministic evidence
artifacts under docs/work/diag.
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import shutil
import socket
import shlex
import subprocess
import sys
import textwrap
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from ipaddress import IPv4Address
from pathlib import Path
from typing import Sequence


LAYERS = ("build", "boot", "http", "tcp", "ws", "mqtt", "serial")
LAYER_GROUPS = {
    "critical": ("http", "tcp", "ws", "mqtt"),
}
WOKWI_NETWORK_LAYERS = {"http", "tcp", "ws", "mqtt"}


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


@dataclass
class WokwiWorkspace:
    root: Path
    firmware: Path
    elf: Path
    serial_log: Path
    scenario: Path
    stdout_log: Path


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


def platformio_shell_command(repo_root: Path, args: Sequence[str]) -> str:
    pio_args = shell_join(args)
    return (
        "source ~/.zshrc >/dev/null 2>&1 || true; "
        f"cd {shlex.quote(str(repo_root))} && "
        "if command -v pio >/dev/null 2>&1 && pio --version >/dev/null 2>&1; then "
        f"pio {pio_args}; "
        "elif command -v platformio >/dev/null 2>&1 && platformio --version >/dev/null 2>&1; then "
        f"platformio {pio_args}; "
        "elif python3 -m platformio --version >/dev/null 2>&1; then "
        f"python3 -m platformio {pio_args}; "
        "else "
        "echo \"error: no working PlatformIO command found (tried pio, platformio, python3 -m platformio)\" >&2; "
        "exit 1; "
        "fi"
    )


def platformio_python() -> str:
    candidate = Path.home() / ".platformio" / "penv" / "bin" / "python"
    if candidate.exists():
        return str(candidate)
    return sys.executable


def repo_python(repo_root: Path) -> str:
    candidate = repo_root / ".venv" / "bin" / "python"
    if candidate.exists():
        return str(candidate)
    return sys.executable


def platformio_package_path(*parts: str) -> Path:
    return Path.home().joinpath(".platformio", "packages", *parts)


def find_nvs_partition_gen(extra_pythons: Sequence[str] = ()) -> list[str]:
    python = platformio_python()
    script = platformio_package_path(
        "framework-espidf",
        "components",
        "nvs_flash",
        "nvs_partition_generator",
        "nvs_partition_gen.py",
    )
    pythons = []
    for item in [*extra_pythons, python, sys.executable]:
        if item and item not in pythons:
            pythons.append(item)
    candidates = [[item, "-m", "esp_idf_nvs_partition_gen"] for item in pythons]
    if script.exists():
        candidates.extend([[item, str(script)] for item in pythons])

    for candidate in candidates:
        try:
            subprocess.run(candidate + ["--help"], capture_output=True, text=True, timeout=10, check=True)
            return candidate
        except (OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired):
            continue
    raise RuntimeError(
        "ESP-IDF NVS partition generator is unavailable; install test dependencies with "
        "'python3 -m pip install -r tests/requirements.txt'"
    )


def find_esptool() -> list[str]:
    python = platformio_python()
    script = platformio_package_path("tool-esptoolpy", "esptool.py")
    if script.exists():
        return [python, str(script)]
    return [python, "-m", "esptool"]


def require_wokwi_cli() -> str:
    cli = shutil.which("wokwi-cli")
    if cli is None:
        raise RuntimeError("wokwi-cli not found on PATH; install Wokwi CLI and set WOKWI_CLI_TOKEN")
    if not os.getenv("WOKWI_CLI_TOKEN"):
        raise RuntimeError("WOKWI_CLI_TOKEN is not set; Wokwi CLI requires a CI token")
    return cli


def require_wokwi_gateway() -> str:
    gateway = shutil.which("wokwigw")
    if gateway is None:
        raise RuntimeError("wokwigw not found on PATH; install Wokwi Private IoT Gateway for forwarded network tests")
    return gateway


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


def generate_dev_tls_material(cert_path: Path, key_path: Path) -> None:
    try:
        from cryptography import x509
        from cryptography.hazmat.primitives import hashes, serialization
        from cryptography.hazmat.primitives.asymmetric import ec
        from cryptography.x509.oid import NameOID
    except ModuleNotFoundError:
        openssl = shutil.which("openssl")
        if openssl is None:
            raise RuntimeError("cryptography and openssl are both unavailable; cannot generate simulator TLS material")
        subprocess.run(
            [
                openssl,
                "req",
                "-x509",
                "-newkey",
                "ec",
                "-pkeyopt",
                "ec_paramgen_curve:prime256v1",
                "-nodes",
                "-keyout",
                str(key_path),
                "-out",
                str(cert_path),
                "-days",
                "1",
                "-subj",
                "/CN=localhost",
                "-addext",
                "subjectAltName=DNS:localhost,IP:127.0.0.1",
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        return

    key = ec.generate_private_key(ec.SECP256R1())
    subject = issuer = x509.Name(
        [
            x509.NameAttribute(NameOID.COUNTRY_NAME, "US"),
            x509.NameAttribute(NameOID.ORGANIZATION_NAME, "LUCE simulator"),
            x509.NameAttribute(NameOID.COMMON_NAME, "localhost"),
        ]
    )
    cert = (
        x509.CertificateBuilder()
        .subject_name(subject)
        .issuer_name(issuer)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(datetime.now(timezone.utc) - timedelta(minutes=1))
        .not_valid_after(datetime.now(timezone.utc) + timedelta(days=1))
        .add_extension(
            x509.SubjectAlternativeName([x509.DNSName("localhost"), x509.IPAddress(IPv4Address("127.0.0.1"))]),
            critical=False,
        )
        .sign(key, hashes.SHA256())
    )
    cert_path.write_bytes(cert.public_bytes(serialization.Encoding.PEM))
    key_path.write_bytes(
        key.private_bytes(
            serialization.Encoding.PEM,
            serialization.PrivateFormat.TraditionalOpenSSL,
            serialization.NoEncryption(),
        )
    )


def write_wokwi_nvs_csv(args: argparse.Namespace, csv_path: Path, cert_path: Path, key_path: Path) -> None:
    http_token = args.sim_http_token
    cli_token = args.sim_cli_token
    mqtt_uri = f"mqtt://host.wokwi.internal:{int(args.mqtt_port)}"
    rows = [
        ["key", "type", "encoding", "value"],
        ["wifi", "namespace", "", ""],
        ["enabled", "data", "u8", "1"],
        ["ssid", "data", "string", "Wokwi-GUEST"],
        ["pass", "data", "string", ""],
        ["hostname", "data", "string", "luce-wokwi"],
        ["max_retries", "data", "u32", "20"],
        ["backoff_min_ms", "data", "u32", "500"],
        ["backoff_max_ms", "data", "u32", "8000"],
        ["net", "namespace", "", ""],
        ["hostname", "data", "string", "luce-wokwi"],
        ["http", "namespace", "", ""],
        ["enabled", "data", "u8", "1"],
        ["port", "data", "u16", "443"],
        ["token", "data", "string", http_token],
        ["tls_key_alg", "data", "string", "ec-p256"],
        ["tls_key_pem", "file", "string", str(key_path)],
        ["tls_cert_pem", "file", "string", str(cert_path)],
        ["cli_net", "namespace", "", ""],
        ["enabled", "data", "u8", "1"],
        ["port", "data", "u16", "2323"],
        ["token", "data", "string", cli_token],
        ["idle_timeout_s", "data", "u32", "120"],
        ["mqtt", "namespace", "", ""],
        ["enabled", "data", "u8", "1"],
        ["uri", "data", "string", mqtt_uri],
        ["client_id", "data", "string", "luce-wokwi"],
        ["base_topic", "data", "string", args.mqtt_topic],
        ["username", "data", "string", args.mqtt_username],
        ["password", "data", "string", args.mqtt_password],
        ["tls_enabled", "data", "u8", "0"],
        ["ca_pem_source", "data", "string", "none"],
        ["qos", "data", "u32", "0"],
        ["keepalive_s", "data", "u32", "30"],
    ]
    with csv_path.open("w", encoding="utf-8", newline="") as fh:
        writer = csv.writer(fh)
        writer.writerows(rows)


def write_wokwi_project_files(workspace: WokwiWorkspace) -> None:
    (workspace.root / "wokwi.toml").write_text(
        textwrap.dedent(
            """\
            [wokwi]
            version = 1
            firmware = 'luce-wokwi.bin'
            elf = 'firmware.elf'

            [net]
            gateway = "ws://localhost:9011"

            [[net.forward]]
            from = "localhost:8443"
            to = "target:443"

            [[net.forward]]
            from = "localhost:8080"
            to = "target:80"

            [[net.forward]]
            from = "localhost:2323"
            to = "target:2323"
            """
        ),
        encoding="utf-8",
    )
    (workspace.root / "diagram.json").write_text(
        json.dumps(
            {
                "version": 1,
                "author": "LUCE",
                "editor": "wokwi",
                "parts": [{"type": "wokwi-esp32-devkit-v1", "id": "esp", "top": 0, "left": 0, "attrs": {}}],
                "connections": [
                    ["esp:TX0", "$serialMonitor:RX", "", []],
                    ["esp:RX0", "$serialMonitor:TX", "", []],
                ],
                "dependencies": {},
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )
    workspace.scenario.write_text(
        textwrap.dedent(
            """\
            name: "LUCE Wokwi boot and UART CLI smoke"
            version: 1
            author: "LUCE"

            steps:
              - wait-serial: "Feature flags: NVS=1 I2C=1 LCD=1 CLI=1 WIFI=1 NTP=1 mDNS=1 MQTT=1 HTTP=1"
              - write-serial: "help\\r\\n"
              - wait-serial: "  - status"
              - write-serial: "status\\r\\n"
              - wait-serial: "status: strategy="
              - write-serial: "wifi.status\\r\\n"
              - wait-serial: "wifi.status state="
              - write-serial: "http.status\\r\\n"
              - wait-serial: "http.status state="
              - write-serial: "cli_net.status\\r\\n"
              - wait-serial: "cli_net.status enabled="
              - write-serial: "mqtt.status\\r\\n"
              - wait-serial: "mqtt.status state="
              - write-serial: "definitely_unknown_cmd\\r\\n"
              - wait-serial: "CLI unknown command"
              - write-serial: "led_set 3 on\\r\\n"
              - wait-serial: "CLI command led_set: invalid index"
            """
        ),
        encoding="utf-8",
    )


def ensure_wokwi_workspace(
    args: argparse.Namespace,
    repo_root: Path,
    out_dir: Path,
    *,
    log_path: Path,
) -> WokwiWorkspace:
    existing = getattr(args, "wokwi_workspace", None)
    if existing is not None:
        return existing

    build_dir = repo_root / ".pio" / "build" / args.env
    required_build_files = {
        "bootloader": build_dir / "bootloader.bin",
        "partition_table": build_dir / "partitions.bin",
        "otadata": build_dir / "ota_data_initial.bin",
        "app": build_dir / "firmware.bin",
        "elf": build_dir / "firmware.elf",
    }
    if not all(path.exists() for path in required_build_files.values()):
        zsh_cmd = platformio_shell_command(repo_root, ["run", "-e", args.env])
        rc = run_cmd(["/bin/zsh", "-lc", zsh_cmd], log_path=log_path, timeout_s=args.layer_timeout_s)
        if rc != 0:
            raise RuntimeError("PlatformIO build failed while preparing Wokwi workspace")

    missing = [f"{name}={path}" for name, path in required_build_files.items() if not path.exists()]
    if missing:
        raise RuntimeError("missing Wokwi build artifacts: " + ", ".join(missing))

    workspace_root = out_dir.parent / "wokwi"
    workspace_root.mkdir(parents=True, exist_ok=True)
    workspace = WokwiWorkspace(
        root=workspace_root,
        firmware=workspace_root / "luce-wokwi.bin",
        elf=workspace_root / "firmware.elf",
        serial_log=out_dir / "wokwi_serial.log",
        scenario=workspace_root / "boot-uart-cli.yaml",
        stdout_log=out_dir / "wokwi.log",
    )

    cert_path = workspace_root / "http_cert.pem"
    key_path = workspace_root / "http_key.pem"
    nvs_csv = workspace_root / "nvs.csv"
    nvs_bin = workspace_root / "nvs.bin"
    generate_dev_tls_material(cert_path, key_path)
    write_wokwi_nvs_csv(args, nvs_csv, cert_path, key_path)

    nvs_cmd = find_nvs_partition_gen([getattr(args, "repo_python", sys.executable)]) + [
        "generate",
        str(nvs_csv),
        str(nvs_bin),
        "0x6000",
    ]
    nvs_rc = run_cmd(nvs_cmd, log_path=log_path, cwd=repo_root, timeout_s=args.layer_timeout_s)
    if nvs_rc != 0:
        raise RuntimeError("failed to generate simulator NVS partition")

    shutil.copyfile(required_build_files["elf"], workspace.elf)
    merge_cmd = find_esptool() + [
        "--chip",
        "esp32",
        "merge_bin",
        "--output",
        str(workspace.firmware),
        "--flash_mode",
        "dio",
        "--flash_freq",
        "40m",
        "--flash_size",
        "4MB",
        "0x1000",
        str(required_build_files["bootloader"]),
        "0x8000",
        str(required_build_files["partition_table"]),
        "0x9000",
        str(nvs_bin),
        "0xf000",
        str(required_build_files["otadata"]),
        "0x20000",
        str(required_build_files["app"]),
    ]
    merge_rc = run_cmd(merge_cmd, log_path=log_path, cwd=repo_root, timeout_s=args.layer_timeout_s)
    if merge_rc != 0:
        raise RuntimeError("failed to merge simulator flash image")

    write_wokwi_project_files(workspace)
    args.wokwi_workspace = workspace
    return workspace


def run_build_layer(args: argparse.Namespace, repo_root: Path, out_dir: Path) -> LayerResult:
    log = out_dir / "build.log"
    zsh_cmd = platformio_shell_command(repo_root, ["run", "-e", args.env])
    rc = run_cmd(["/bin/zsh", "-lc", zsh_cmd], log_path=log, timeout_s=args.layer_timeout_s)
    status = "PASS" if rc == 0 else "FAIL"
    return LayerResult("build", status, rc, str(log))


def run_boot_layer(args: argparse.Namespace, repo_root: Path, out_dir: Path) -> LayerResult:
    log = out_dir / "boot.log"
    serial_log = out_dir / "boot_serial.log"

    upload_cmd = platformio_shell_command(
        repo_root,
        ["run", "-e", args.env, "-t", "upload", "--upload-port", args.upload_port],
    )
    rc = run_cmd(["/bin/zsh", "-lc", upload_cmd], log_path=log, timeout_s=args.layer_timeout_s)
    if rc != 0:
        return LayerResult("boot", "FAIL", rc, str(log), "upload failed")

    capture_cmd = [
        getattr(args, "repo_python", sys.executable),
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


def run_wokwi_boot_layer(args: argparse.Namespace, repo_root: Path, out_dir: Path) -> LayerResult:
    log = out_dir / "boot.log"
    try:
        workspace = ensure_wokwi_workspace(args, repo_root, out_dir, log_path=log)
        cli = require_wokwi_cli()
    except RuntimeError as exc:
        append_log(log, str(exc))
        return LayerResult("boot", "FAIL", 1, str(log), str(exc))

    serial_log = out_dir / "boot_serial.log"
    cmd = [
        cli,
        ".",
        "--serial-log-file",
        str(serial_log),
        "--scenario",
        workspace.scenario.name,
        "--timeout",
        str(args.wokwi_timeout_ms),
        "--timeout-exit-code",
        "0",
    ]
    rc = run_cmd(cmd, log_path=log, cwd=workspace.root, timeout_s=args.layer_timeout_s)
    if rc != 0:
        return LayerResult("boot", "FAIL", rc, str(log), "wokwi-cli failed")

    text = serial_log.read_text(encoding="utf-8", errors="replace") if serial_log.exists() else ""
    required = list(expected_boot_markers(args.env)) + list(args.require_marker)
    missing = [marker for marker in required if marker not in text]
    if missing:
        append_log(log, "missing_markers:")
        for marker in missing:
            append_log(log, f"- {marker}")
        return LayerResult("boot", "FAIL", 2, str(log), f"missing markers: {len(missing)}")

    return LayerResult("boot", "PASS", 0, str(log))


def start_wokwi_network(args: argparse.Namespace, repo_root: Path, out_dir: Path) -> subprocess.Popen[str]:
    existing = getattr(args, "wokwi_proc", None)
    if existing is not None and existing.poll() is None:
        return existing

    log = out_dir / "wokwi-network.log"
    workspace = ensure_wokwi_workspace(args, repo_root, out_dir, log_path=log)
    cli = require_wokwi_cli()
    gateway = require_wokwi_gateway()
    serial_log = out_dir / "wokwi_network_serial.log"

    gateway_log = out_dir / "wokwi-gateway.log"
    gateway_fh = gateway_log.open("w", encoding="utf-8")
    gateway_cmd = [
        gateway,
        "--listenPort",
        "9011",
        "--forward",
        "8443:10.13.37.2:443",
        "--forward",
        "8080:10.13.37.2:80",
        "--forward",
        "2323:10.13.37.2:2323",
    ]
    gateway_fh.write(f"==> {shell_join(gateway_cmd)}\n")
    gateway_fh.flush()
    gateway_proc = subprocess.Popen(gateway_cmd, stdout=gateway_fh, stderr=subprocess.STDOUT, text=True)
    args.wokwi_gateway_proc = gateway_proc
    args.wokwi_gateway_stdout_fh = gateway_fh

    if not wait_for_tcp("127.0.0.1", 9011, timeout_s=8.0):
        stop_wokwi_network(args)
        raise RuntimeError("wokwigw did not become ready on 127.0.0.1:9011")

    stdout_fh = log.open("w", encoding="utf-8")
    cmd = [
        cli,
        ".",
        "--serial-log-file",
        str(serial_log),
        "--timeout",
        str(args.wokwi_timeout_ms),
        "--timeout-exit-code",
        "0",
    ]
    stdout_fh.write(f"==> {shell_join(cmd)}\n")
    stdout_fh.flush()
    proc = subprocess.Popen(cmd, cwd=str(workspace.root), stdout=stdout_fh, stderr=subprocess.STDOUT, text=True)
    args.wokwi_proc = proc
    args.wokwi_stdout_fh = stdout_fh

    if not wait_for_file_text(serial_log, "[HTTP] started", timeout_s=args.wokwi_ready_timeout_s):
        stop_wokwi_network(args)
        raise RuntimeError("Wokwi HTTP server did not report ready before timeout")

    if not wait_for_tcp("127.0.0.1", 8443, timeout_s=args.wokwi_ready_timeout_s):
        stop_wokwi_network(args)
        raise RuntimeError("Wokwi HTTPS forward did not become ready on 127.0.0.1:8443")
    return proc


def stop_wokwi_network(args: argparse.Namespace) -> None:
    proc = getattr(args, "wokwi_proc", None)
    if proc is not None and proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=3)
    stdout_fh = getattr(args, "wokwi_stdout_fh", None)
    if stdout_fh is not None:
        stdout_fh.close()
    gateway_proc = getattr(args, "wokwi_gateway_proc", None)
    if gateway_proc is not None and gateway_proc.poll() is None:
        gateway_proc.terminate()
        try:
            gateway_proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            gateway_proc.kill()
            gateway_proc.wait(timeout=3)
    gateway_stdout_fh = getattr(args, "wokwi_gateway_stdout_fh", None)
    if gateway_stdout_fh is not None:
        gateway_stdout_fh.close()


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
        getattr(args, "repo_python", sys.executable),
        "-m",
        "pytest",
        str(repo_root / test_map[layer]),
        "--junitxml",
        str(junit),
        "--luce-host",
        args.host,
        "--luce-captive-host",
        args.captive_host,
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


def wait_for_file_text(path: Path, needle: str, timeout_s: float) -> bool:
    deadline = time_monotonic() + timeout_s
    while time_monotonic() < deadline:
        if path.exists():
            text = path.read_text(encoding="utf-8", errors="replace")
            if needle in text:
                return True
        sleep_short()
    return False


def time_monotonic() -> float:
    import time

    return time.monotonic()


def sleep_short() -> None:
    import time

    time.sleep(0.1)


def spawn_test_mqtt_broker(args: argparse.Namespace, out_dir: Path) -> SpawnedBroker:
    check = subprocess.run(
        [getattr(args, "repo_python", sys.executable), "-c", "import amqtt"],
        capture_output=True,
        text=True,
    )
    if check.returncode != 0:
        raise RuntimeError("amqtt not installed; install with 'python3 -m pip install -r tests/requirements.txt'")

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
        [getattr(args, "repo_python", sys.executable), str(broker_script)],
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


def layer_preflight(args: argparse.Namespace, layer: str, out_dir: Path) -> LayerResult | None:
    log = out_dir / f"{layer}-preflight.log"

    def fail(message: str) -> LayerResult:
        append_log(log, message)
        return LayerResult(layer, "FAIL", 2, str(log), f"preflight: {message}")

    if layer == "boot" and args.target == "hardware":
        missing = [path for path in (args.upload_port, args.monitor_port) if path and not Path(path).exists()]
        if missing:
            return fail("missing serial device(s): " + ", ".join(missing))
    if layer == "http" and not args.http_token:
        return fail("missing HTTP token; provide --http-token or LUCE_HTTP_TOKEN")
    if layer == "tcp" and not args.tcp_token:
        return fail("missing TCP CLI token; provide --tcp-token or LUCE_CLI_NET_TOKEN")
    if layer == "serial":
        if not args.monitor_port or not Path(args.monitor_port).exists():
            return fail("missing monitor serial port; provide --monitor-port or LUCE_MONITOR_PORT")
        serial_check = subprocess.run([args.repo_python, "-c", "import serial"], capture_output=True, text=True)
        if serial_check.returncode != 0:
            return fail("pyserial unavailable; install tests/requirements.txt")
    if layer == "mqtt":
        if not args.http_token:
            return fail("missing HTTP token required for MQTT state/reconnect assertions")
        if not args.spawn_test_mqtt_broker and not wait_for_tcp(args.mqtt_host, int(args.mqtt_port), timeout_s=1.5):
            return fail(f"MQTT broker unreachable at {args.mqtt_host}:{args.mqtt_port}; use --spawn-test-mqtt-broker or provide broker")
    return None


def write_summaries(out_dir: Path, run_id: str, results: list[LayerResult]) -> None:
    pass_count = sum(1 for r in results if r.status == "PASS")
    fail_count = sum(1 for r in results if r.status == "FAIL")
    skip_count = sum(1 for r in results if r.status == "SKIP")
    deselected_count = sum(1 for r in results if r.status == "DESELECTED")
    ran_count = sum(1 for r in results if r.status in {"PASS", "FAIL"})

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
        f"- deselected: {deselected_count}",
        f"- ran: {ran_count}",
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
        "deselected": deselected_count,
        "ran": ran_count,
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

              python3 scripts/test_layers.py --target wokwi \
                --layers build,boot,http,tcp,ws,mqtt --env net1 --spawn-test-mqtt-broker
            """
        ),
    )

    parser.add_argument("--target", choices=("hardware", "wokwi"), default=os.getenv("LUCE_TEST_TARGET", "hardware"))
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
    parser.add_argument("--captive-host", default=os.getenv("LUCE_CAPTIVE_HOST", ""), help="Plain HTTP captive host override.")
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
    parser.add_argument(
        "--wokwi-timeout-ms",
        type=int,
        default=int(os.getenv("LUCE_SIM_WOKWI_TIMEOUT_MS", "120000")),
        help="Wokwi simulation timeout in simulation milliseconds.",
    )
    parser.add_argument(
        "--wokwi-ready-timeout-s",
        type=float,
        default=float(os.getenv("LUCE_SIM_WOKWI_READY_TIMEOUT_S", "75")),
        help="Seconds to wait for Wokwi forwarded HTTPS readiness.",
    )
    parser.add_argument(
        "--wokwi-network-mode",
        choices=("skip", "gateway"),
        default=os.getenv("LUCE_WOKWI_NETWORK_MODE", "skip"),
        help=(
            "Wokwi protocol layer mode. Default 'skip' records HTTP/TCP/WS/MQTT as unsupported "
            "for current wokwi-cli. 'gateway' attempts experimental wokwigw forwarding."
        ),
    )

    parser.add_argument("--pytest-arg", action="append", default=[], help="Extra pytest arg (repeatable).")

    args = parser.parse_args(argv)
    if args.layer_timeout_s <= 0:
        args.layer_timeout_s = None
    try:
        args.layer_list = resolve_layers(args.layers)
    except ValueError as exc:
        parser.error(str(exc))
    if args.target == "wokwi":
        raw_layers = args.layers.strip().lower()
        if "serial" in args.layer_list:
            if raw_layers in ("all", "full"):
                args.layer_list = [layer for layer in args.layer_list if layer != "serial"]
            else:
                parser.error("serial layer is hardware-only; Wokwi UART CLI coverage runs as part of boot")
        args.sim_http_token = os.getenv("LUCE_SIM_HTTP_TOKEN", args.http_token or "luce-token")
        args.sim_cli_token = os.getenv("LUCE_SIM_CLI_TOKEN", args.tcp_token or "luce-cli")
        args.host = "https://127.0.0.1:8443"
        args.captive_host = "http://127.0.0.1:8080"
        args.http_token = args.sim_http_token
        args.tcp_host = "127.0.0.1"
        args.tcp_port = 2323
        args.tcp_token = args.sim_cli_token
        args.ws_host = "127.0.0.1"
        args.ws_port = 8080
        args.ws_tls = False
    else:
        args.sim_http_token = args.http_token
        args.sim_cli_token = args.tcp_token
    return args


def main(argv: Sequence[str]) -> int:
    args = parse_args(argv)
    repo_root = Path(__file__).resolve().parents[1]
    args.repo_python = repo_python(repo_root)
    run_id = args.run_id or utc_now_compact()
    diag_root = Path(args.diag_root)
    out_dir = diag_root / run_id / "test-layers"
    out_dir.mkdir(parents=True, exist_ok=True)

    if any(layer in {"http", "tcp", "ws", "mqtt"} for layer in args.layer_list):
        pytest_check = subprocess.run([args.repo_python, "-c", "import pytest"], capture_output=True, text=True)
        if pytest_check.returncode != 0:
            print("error: pytest not installed; install with 'python3 -m pip install -r tests/requirements.txt'", file=sys.stderr)
            return 1

    results: list[LayerResult] = []
    spawned_broker: SpawnedBroker | None = None

    boot_handler = run_wokwi_boot_layer if args.target == "wokwi" else run_boot_layer
    layer_handlers = {
        "build": lambda: run_build_layer(args, repo_root, out_dir),
        "boot": lambda: boot_handler(args, repo_root, out_dir),
        "http": lambda: run_pytest_layer(args, repo_root, out_dir, "http"),
        "tcp": lambda: run_pytest_layer(args, repo_root, out_dir, "tcp"),
        "ws": lambda: run_pytest_layer(args, repo_root, out_dir, "ws"),
        "mqtt": lambda: run_pytest_layer(args, repo_root, out_dir, "mqtt"),
        "serial": lambda: run_pytest_layer(args, repo_root, out_dir, "serial"),
    }

    try:
        mqtt_layer_will_run = not (
            args.target == "wokwi" and args.wokwi_network_mode == "skip" and "mqtt" in args.layer_list
        )
        if args.spawn_test_mqtt_broker and "mqtt" in args.layer_list and mqtt_layer_will_run:
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
            if args.target == "wokwi" and layer in WOKWI_NETWORK_LAYERS and args.wokwi_network_mode == "skip":
                log = out_dir / f"{layer}.log"
                details = (
                    "wokwi-cli 0.26.x does not attach to Wokwi Private IoT Gateway; "
                    "run protocol layers against hardware, or opt into experimental "
                    "--wokwi-network-mode gateway"
                )
                append_log(log, details)
                result = LayerResult(layer, "DESELECTED", 0, str(log), f"deselected:wokwi {details}")
                results.append(result)
                print(f"{layer}: {result.status} (rc={result.rc})")
                continue
            preflight = layer_preflight(args, layer, out_dir)
            if preflight is not None:
                results.append(preflight)
                print(f"{layer}: {preflight.status} (rc={preflight.rc})")
                if not args.continue_on_fail:
                    break
                continue
            if args.target == "wokwi" and layer in WOKWI_NETWORK_LAYERS:
                try:
                    start_wokwi_network(args, repo_root, out_dir)
                except RuntimeError as exc:
                    result = LayerResult("wokwi", "FAIL", 1, str(out_dir / "wokwi-network.log"), str(exc))
                    results.append(result)
                    print(f"wokwi: {result.status} (rc={result.rc})")
                    if not args.continue_on_fail:
                        break
                    continue
            result = layer_handlers[layer]()
            results.append(result)
            print(f"{layer}: {result.status} (rc={result.rc})")
            if result.status == "FAIL" and not args.continue_on_fail:
                break
    finally:
        if args.target == "wokwi":
            stop_wokwi_network(args)
        if spawned_broker is not None:
            stop_test_mqtt_broker(spawned_broker)
            print("test-mqtt-broker: STOPPED")

    write_summaries(out_dir, run_id, results)

    failed = any(r.status == "FAIL" for r in results)
    print(f"summary: {out_dir / 'summary.md'}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
