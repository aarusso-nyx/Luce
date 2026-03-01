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

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd "${script_dir}/.." && pwd)"
cd "${project_root}"

env="net1"
duration="${1:-45}"

if [[ ! "${duration}" =~ ^[0-9]+$ ]]; then
  echo "error: duration must be integer seconds" >&2
  exit 1
fi

upload_port="${LUCE_UPLOAD_PORT:-/dev/cu.usbserial-0001}"
monitor_port="${LUCE_MONITOR_PORT:-/dev/cu.usbserial-40110}"

ts="$(date +%Y%m%d_%H%M%S)"
log_dir="${project_root}/docs/work/diag/${ts}/test"
mkdir -p "${log_dir}"
log_file="${log_dir}/${env}_firmware_smoke.log"

upload_cmd=("${pio_cmd[@]}" run -e "${env}" -t upload --upload-port "${upload_port}")
capture_cmd=(python3 "${project_root}/scripts/capture_serial.py" --port "${monitor_port}" --baud 115200 --seconds "${duration}" --output "${log_file}")

echo "==> ${upload_cmd[*]}"
"${upload_cmd[@]}"

echo "==> ${capture_cmd[*]}"
"${capture_cmd[@]}"

required_markers=(
  "LUCE STRATEGY=NET1"
  "Feature flags: NVS=1 I2C=1 LCD=1 CLI=1 WIFI=1 NTP=1 mDNS=1 MQTT=1 HTTP=1"
)

for marker in "${required_markers[@]}"; do
  if ! rg -Fq "${marker}" "${log_file}"; then
    echo "FAIL: missing marker '${marker}' in ${log_file}" >&2
    exit 2
  fi
done

echo "PASS: firmware smoke markers verified"
echo "Log: ${log_file}"
