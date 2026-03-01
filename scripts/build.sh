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

strategy="${LUCE_STRATEGY:-}"
strategy_lc="${strategy,,}"
mapfile -t all_envs < <(awk -F'[][]' '/^\[env:/{print $2}' platformio.ini)

if [ "${#all_envs[@]}" -eq 0 ]; then
  echo "error: no PlatformIO environments found" >&2
  exit 1
fi

if [ -n "${strategy}" ]; then
  mapfile -t matched_envs < <(printf '%s\n' "${all_envs[@]}" | rg -F "luce_${strategy_lc}" || true)
else
  matched_envs=()
fi

if [ "${#matched_envs[@]}" -gt 0 ]; then
  targets=("${matched_envs[@]}")
  echo "LUCE_STRATEGY=${strategy}: building strategy-matched environments"
else
  targets=("${all_envs[@]}")
  if [ -n "${strategy}" ]; then
    echo "LUCE_STRATEGY=${strategy}: no matching environments found; building all environments"
  else
    echo "LUCE_STRATEGY not set: building all environments"
  fi
fi

for env in "${targets[@]}"; do
  echo "==> pio run -e ${env}"
  "${pio_cmd[@]}" run -e "${env}"
done
