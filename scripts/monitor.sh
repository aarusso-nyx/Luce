#!/usr/bin/env bash
set -euo pipefail

if command -v pio >/dev/null 2>&1; then
  pio_cmd=(pio)
elif python3 -m platformio --version >/dev/null 2>&1; then
  pio_cmd=(python3 -m platformio)
else
  echo "error: neither 'pio' nor 'python3 -m platformio' is available" >&2
  exit 1
fi

if [ "${#}" -lt 1 ]; then
  echo "usage: $0 <env> [--port <port>] [--baud <rate>]"
  exit 1
fi

env="$1"
shift || true

monitor_port="${LUCE_MONITOR_PORT:-/dev/cu.usbserial-40110}"
args=()

while [ "$#" -gt 0 ]; do
  if [ "$1" = "--port" ]; then
    if [ "$#" -lt 2 ]; then
      echo "error: --port requires a value" >&2
      exit 1
    fi
    monitor_port="$2"
    shift
  else
    args+=("$1")
  fi
  shift || true
done

echo "==> pio device monitor -e ${env} --timestamp --port ${monitor_port} ${args[*]}"
"${pio_cmd[@]}" device monitor -e "${env}" --timestamp --port "${monitor_port}" "${args[@]}"
