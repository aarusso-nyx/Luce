#!/usr/bin/env bash
set -euo pipefail

if [ -f "$(pwd)/.git/config" ] || [ -d "$(pwd)/.git" ]; then
  project_root="$(pwd)"
else
  project_root="$(cd "$(dirname "$0")/.." && pwd)"
fi

cd "${project_root}"

if command -v pio >/dev/null 2>&1; then
  pio_cmd=(pio)
elif python3 -m platformio --version >/dev/null 2>&1; then
  pio_cmd=(python3 -m platformio)
else
  echo "error: neither 'pio' nor 'python3 -m platformio' is available" >&2
  exit 1
fi

# Parse environments from platformio.ini and normalize names (no env: prefix).
raw_envs=(
  $(awk -F'[][]' '/^\[env:/{print $2}' platformio.ini)
)
envs=()
for e in "${raw_envs[@]}"; do
  e="${e#env:}"
  case "${e}" in
    luce_core|luce_net0|luce_net1)
      envs+=("${e}")
      ;;
  esac
done
if [ "${#envs[@]}" -eq 0 ]; then
  echo "error: no PlatformIO environments found in platformio.ini" >&2
  exit 1
fi

severity_to_rank() {
  case "$1" in
    low) echo 1 ;;
    medium) echo 2 ;;
    high) echo 3 ;;
    error) echo 4 ;;
    *) echo 0 ;;
  esac
}

MIN_SEVERITY="${LINT_MIN_SEVERITY:-medium}"
MIN_RANK="$(severity_to_rank "$MIN_SEVERITY")"

waiver_file="${project_root}/scripts/lint_waivers.txt"
is_waived_finding() {
  local finding="$1"
  if [ ! -f "${waiver_file}" ]; then
    return 1
  fi
  while IFS= read -r waiver || [ -n "$waiver" ]; do
    [ -z "$waiver" ] && continue
    [[ "$waiver" == \#* ]] && continue
    if [[ "$finding" == *"$waiver"* ]]; then
      return 0
    fi
  done < "${waiver_file}"
  return 1
}

run_ts="$(date +%Y%m%d_%H%M%S)"
output_dir="docs/work/diag/${run_ts}/lint"
mkdir -p "${output_dir}"

overall=0

for env in "${envs[@]}"; do
  out_file="${output_dir}/platformio_check_${env}.txt"
  echo "${project_root}: running lint for env=${env}" | tee "${out_file}"
  echo "command: ${pio_cmd[*]} check -e ${env} --skip-packages" | tee -a "${out_file}"

  # Run platformio check with package skip to avoid toolchain include false positives.
  set +e
  "${pio_cmd[@]}" check -e "${env}" --skip-packages >"${out_file}".raw 2>&1
  check_rc="$?"
  set -e

  cat "${out_file}".raw >>"${out_file}"

  unmatched_count=0
  finding_count=0
  while IFS= read -r line; do
    if [[ "$line" != src*:*"["*":"* ]]; then
      continue
    fi
  if [[ "$line" =~ ^([^:]+):([0-9]+):\ \[(low|medium|high|error)[:] ]]; then
    severity="${BASH_REMATCH[3]}"
    rank=$(severity_to_rank "$severity")
    if [ "$rank" -lt "$MIN_RANK" ]; then
      continue
    fi

    finding_count=$((finding_count + 1))
    finding_text="$line"

    if is_waived_finding "$finding_text"; then
      echo "WAIVED: ${finding_text}" >> "${out_file}"
      continue
      fi

      unmatched_count=$((unmatched_count + 1))
      echo "UNWAIVED: ${finding_text}" | tee -a "${out_file}"
    fi
  done < "${out_file}"

  if [ "$check_rc" -ne 0 ]; then
    status="FAIL"
  elif [ "$finding_count" -eq 0 ]; then
    status="PASS"
  elif [ "$unmatched_count" -eq 0 ]; then
    status="PASS_WITH_WAIVER"
    echo "NOTE: findings detected in ${env} are fully covered by waivers." | tee -a "${out_file}"
  else
    status="FAIL"
  fi

  echo "STATUS=${status}" | tee -a "${out_file}"

  if [ "$status" = "FAIL" ]; then
    overall=1
  fi

done

if [ "$overall" -ne 0 ]; then
  echo "lint: FAIL (unwaived findings detected)"
  exit 1
fi

echo "lint: PASS"

echo "STATUS_SUMMARY="
for env in "${envs[@]}"; do
  echo "  ${env}: $(awk '/^STATUS=/{print $1}' "${output_dir}/platformio_check_${env}.txt")"
 done
