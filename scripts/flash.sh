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
  echo "usage: $0 <env> [--upload-port <port>]"
  exit 1
fi

env="$1"
shift || true

upload_port="${LUCE_UPLOAD_PORT:-/dev/cu.usbserial-0001}"
args=()

while [ "$#" -gt 0 ]; do
  if [ "$1" = "--upload-port" ]; then
    if [ "$#" -lt 2 ]; then
      echo "error: --upload-port requires a value" >&2
      exit 1
    fi
    upload_port="$2"
    shift
  else
    args+=("$1")
  fi
  shift || true
done

echo "==> pio run -e ${env} -t upload --upload-port ${upload_port} ${args[*]}"
"${pio_cmd[@]}" run -e "${env}" -t upload --upload-port "${upload_port}" "${args[@]}"
