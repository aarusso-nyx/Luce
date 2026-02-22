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
  echo "usage: $0 <env> [<tag>] [<duration_seconds>] [--upload-port <port>] [--monitor-port <port>]" >&2
  echo "  - default tag: manual" >&2
  echo "  - default duration: 120" >&2
  exit 1
fi

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd "${script_dir}/.." && pwd)"
cd "${project_root}"

env="$1"
shift || true

tag="manual"
if [ "${#}" -gt 0 ] && [[ "$1" != --* ]]; then
  tag="$1"
  shift || true
fi

duration="120"
if [ "${#}" -gt 0 ] && [[ "$1" != --* ]]; then
  duration="$1"
  shift || true
fi

if [[ ! "${duration}" =~ ^[0-9]+$ ]]; then
  echo "error: duration must be an integer in seconds: '${duration}'" >&2
  exit 1
fi

upload_port="${LUCE_UPLOAD_PORT:-/dev/cu.usbserial-0001}"
monitor_port="${LUCE_MONITOR_PORT:-/dev/cu.usbserial-40110}"

while [ "${#}" -gt 0 ]; do
  case "$1" in
    --upload-port)
      if [ "${#}" -lt 2 ]; then
        echo "error: --upload-port requires a value" >&2
        exit 1
      fi
      upload_port="$2"
      shift 2
      ;;
    --monitor-port)
      if [ "${#}" -lt 2 ]; then
        echo "error: --monitor-port requires a value" >&2
        exit 1
      fi
      monitor_port="$2"
      shift 2
      ;;
    --*)
      echo "error: unknown flag '$1'" >&2
      echo "usage: $0 <env> [<tag>] [<duration_seconds>] [--upload-port <port>] [--monitor-port <port>]" >&2
      exit 1
      ;;
    *)
      echo "error: too many positional arguments" >&2
      echo "usage: $0 <env> [<tag>] [<duration_seconds>] [--upload-port <port>] [--monitor-port <port>]" >&2
      exit 1
      ;;
  esac
done

log_dir="${project_root}/docs/work/diag"
mkdir -p "${log_dir}"
timestamp="$(date +%Y%m%d_%H%M%S)"
boot_dir="${log_dir}/${timestamp}/boot"
mkdir -p "${boot_dir}"
log_file="${boot_dir}/${env}_${tag}.txt"

{
  echo "# LUCE log capture"
  echo "# date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "# env: ${env}"
  echo "# tag: ${tag}"
  echo "# duration_seconds: ${duration}"
  echo "# upload_port: ${upload_port}"
  echo "# monitor_port: ${monitor_port}"
  echo "# pio_cmd: ${pio_cmd[*]}"
  echo
} > "${log_file}"

upload_cmd=( "${pio_cmd[@]}" run -e "${env}" -t upload --upload-port "${upload_port}" )
capture_cmd=( python3 "${project_root}/scripts/capture_serial.py" --port "${monitor_port}" --baud 115200 --seconds "${duration}" --output "${log_file}" )

echo "==> ${upload_cmd[*]}" | tee -a "${log_file}"
"${upload_cmd[@]}" 2>&1 | tee -a "${log_file}"

echo "==> ${capture_cmd[*]} (timed out after ${duration}s)" | tee -a "${log_file}"
"${capture_cmd[@]}" >> "${log_file}" 2>&1
monitor_exit="$?"

if [ "${monitor_exit}" -ne 0 ]; then
  echo "error: monitor command failed with exit code ${monitor_exit}" | tee -a "${log_file}" >&2
  exit "${monitor_exit}"
fi

echo "Saved capture: ${log_file}" | tee -a "${log_file}"
