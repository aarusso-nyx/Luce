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

stage="${LUCE_STAGE:-dev}"
mapfile -t all_envs < <(awk -F'[][]' '/^\[env:/{print $2}' platformio.ini)

if [ "${#all_envs[@]}" -eq 0 ]; then
  echo "error: no PlatformIO environments found" >&2
  exit 1
fi

mapfile -t stage_envs < <(printf '%s\n' "${all_envs[@]}" | rg -E "(^|[-_])${stage}($|[-_])" || true)

if [ "${#stage_envs[@]}" -gt 0 ]; then
  targets=("${stage_envs[@]}")
  echo "LUCE_STAGE=${stage}: building stage-matched environments"
else
  targets=("${all_envs[@]}")
  echo "LUCE_STAGE=${stage}: no stage-matched environments found; building all environments"
fi

for env in "${targets[@]}"; do
  echo "==> pio run -e ${env}"
  "${pio_cmd[@]}" run -e "${env}"
done
