#!/usr/bin/env python3
"""Robust serial capture helper for non-interactive environments."""

from __future__ import annotations

import argparse
import os
import signal
import sys
import time


def _fail(message: str, code: int = 1) -> None:
    print(f"error: {message}", file=sys.stderr)
    raise SystemExit(code)


def _open_serial():
    try:
        import serial  # type: ignore
    except ModuleNotFoundError as exc:
        _fail(f"pyserial not installed; install with 'python3 -m pip install pyserial' ({exc})")

    try:
        return serial.Serial(
            port=args.port,
            baudrate=args.baud,
            timeout=0.25,
            bytesize=serial.EIGHTBITS,
            parity=serial.PARITY_NONE,
            stopbits=serial.STOPBITS_ONE,
            xonxoff=False,
            rtscts=False,
            dsrdtr=False,
        )
    except Exception as exc:
        _fail(f"failed to open serial port '{args.port}' at {args.baud}: {exc}")


def _ensure_output_dir(path: str) -> None:
    directory = os.path.dirname(path)
    if directory:
        os.makedirs(directory, exist_ok=True)


def _timestamped_line(raw: bytes) -> str:
    line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
    return f"{time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())} {line}\n"


def main() -> None:
    parser = argparse.ArgumentParser(description="Capture serial output for evidence.")
    parser.add_argument("--port", required=True)
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--seconds", type=float, default=30.0)
    parser.add_argument("--output", required=True)
    parser.add_argument("--raw", action="store_true")
    args_obj = parser.parse_args()
    global args
    args = args_obj

    if args.seconds <= 0:
        _fail("duration must be greater than zero")

    _ensure_output_dir(args.output)

    with open(args.output, "ab") as output:
        if not args.raw:
            output.write(f"TS={time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}\n".encode())
            output.write(f"PORT={args.port}\n".encode())
            output.write(f"BAUD={args.baud}\n".encode())
            output.write(f"DURATION={int(args.seconds)}\n".encode())
            output.write(b"RESULT=RUNNING\n")
            output.write(b"\n")
            output.flush()

        serial_port = _open_serial()
        deadline = time.monotonic() + args.seconds
        interrupted = False

        def _handle_sigterm(*_unused):
            nonlocal interrupted
            interrupted = True

        signal.signal(signal.SIGINT, _handle_sigterm)
        signal.signal(signal.SIGTERM, _handle_sigterm)

        while time.monotonic() < deadline and not interrupted:
            chunk = serial_port.read(1024)
            if not chunk:
                continue
            if args.raw:
                output.write(chunk)
            else:
                text = _timestamped_line(chunk)
                output.write(text.encode())
            output.flush()

        serial_port.close()

        if interrupted:
            output.write(b"RESULT=INTERRUPTED\n")
            raise SystemExit(2)

        output.seek(0, 2)
        if args.raw:
            # no canonical markers for machine parsing; treat as successful capture
            raise SystemExit(0)
        output.write(b"RESULT=PASS\n")


if __name__ == "__main__":
    main()
