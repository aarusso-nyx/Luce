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

requested_env="${LUCE_ENV:-}"
mapfile -t all_envs < <(awk -F'[][]' '/^\[env:/{print $2}' platformio.ini)

if [ "${#all_envs[@]}" -eq 0 ]; then
  echo "error: no PlatformIO environments found" >&2
  exit 1
fi

if [ -n "${requested_env}" ]; then
  targets=()
  for env in "${all_envs[@]}"; do
    if [ "${env}" = "${requested_env}" ]; then
      targets+=("${env}")
      break
    fi
  done

  if [ "${#targets[@]}" -eq 0 ]; then
    echo "LUCE_ENV=${requested_env}: no matching environment found" >&2
    exit 1
  fi

  echo "LUCE_ENV=${requested_env}: building selected environment"
else
  targets=("${all_envs[@]}")
  echo "LUCE_ENV not set: building all environments"
fi

for env in "${targets[@]}"; do
  echo "==> pio run -e ${env}"
  "${pio_cmd[@]}" run -e "${env}"
done
