#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
PLATFORMIO_PENV_BIN="${HOME}/.platformio/penv/bin"

PLATFORMIO_INI="${PROJECT_ROOT}/platformio.ini"
DIAG_DIR="${LUCE_DIAG_DIR:-${PROJECT_ROOT}/docs/work/diag}"
RUN_ID="$(date +%Y%m%d_%H%M%S)"
REQUESTED_ENV="${LUCE_ENV:-}"
UPLOAD_PORT="${LUCE_UPLOAD_PORT:-/dev/cu.usbserial-0001}"
MONITOR_PORT="${LUCE_MONITOR_PORT:-/dev/cu.usbserial-40110}"
LINT_WAIVER_FILE="${LUCE_LINT_WAIVERS:-${PROJECT_ROOT}/scripts/lint_waivers.txt}"
LINT_MIN_SEVERITY="${LINT_MIN_SEVERITY:-medium}"
DRY_RUN=0
VERBOSE=0

declare -a PIO_CMD=()

refresh_pio_path_from_zshrc() {
  local zsh_pio=""

  if [ ! -f "${HOME}/.zshrc" ]; then
    return 0
  fi

  if command -v zsh >/dev/null 2>&1; then
    zsh_pio="$(zsh -lc 'source ~/.zshrc >/dev/null 2>&1; command -v pio' 2>/dev/null | tr -d '\r' | xargs)"
  fi

  if [ -n "${zsh_pio}" ]; then
    PATH="$(dirname "${zsh_pio}"):${PATH}"
  fi
}

usage() {
  cat <<'EOF'
Usage:
  scripts/luce.sh <command> [options]

Global options:
  -e, --env <env>         Select environment (defaults to LUCE_ENV when set, else command-specific default).
  --upload-port <path>     Serial upload port (default from LUCE_UPLOAD_PORT or /dev/cu.usbserial-0001)
  --monitor-port <path>    Serial monitor port (default from LUCE_MONITOR_PORT or /dev/cu.usbserial-40110)
  --diag-dir <path>       Base diagnostics directory (default: ./docs/work/diag)
  --dry-run               Print resolved commands without running them.
  -v, --verbose           Print command lines before execution.
  -h, --help              Show this help.

Commands:
  build                 Build one env (if --env set) or all envs.
  upload                Upload firmware for selected env.
  monitor               Open device monitor for selected env.
  collect               Upload + capture serial logs for selected env.
  test                  Smoke test selected env (default: net1) using serial marker assertions.
  lint                  Run static checks. Defaults to all envs when no --env.
  health                Run quick tooling + environment preflight checks.
  clean                 Run PlatformIO clean for selected env(s) or all.
  all                   Convenience chain; defaults to build.

Common command examples:
  ./scripts/luce.sh build
  ./scripts/luce.sh build --env net1
  ./scripts/luce.sh upload --env net1
  ./scripts/luce.sh monitor --env default
  ./scripts/luce.sh collect --env net1 --tag boot --duration 120
  ./scripts/luce.sh test --env net1 --duration 45
  ./scripts/luce.sh lint --env net1 --min-severity medium
  ./scripts/luce.sh health --run-build
EOF
}

resolve_pio_cmd() {
  if ! command -v pio >/dev/null 2>&1; then
    refresh_pio_path_from_zshrc
  fi

  if [ -d "${PLATFORMIO_PENV_BIN}" ] && [[ ":${PATH}:" != *":${PLATFORMIO_PENV_BIN}:"* ]]; then
    PATH="${PLATFORMIO_PENV_BIN}:${PATH}"
  fi

  if command -v pio >/dev/null 2>&1; then
    PIO_CMD=(pio)
  elif python3 -m platformio --version >/dev/null 2>&1; then
    PIO_CMD=(python3 -m platformio)
  else
    echo "error: neither 'pio' nor 'python3 -m platformio' is available" >&2
    exit 1
  fi
}

platformio_envs() {
  local -a envs=()
  mapfile -t envs < <(awk -F'[][]' '/^\[env:/{print $2}' "${PLATFORMIO_INI}")
  printf '%s\n' "${envs[@]}"
}

resolve_envs() {
  local requested="${1:-}"
  local include_all="${2:-0}"
  local -a envs
  mapfile -t envs < <(platformio_envs)

  if [ "${#envs[@]}" -eq 0 ]; then
    echo "error: no PlatformIO environments found in platformio.ini" >&2
    return 1
  fi

  if [ -n "${requested}" ]; then
    for env in "${envs[@]}"; do
      if [ "${env}" = "${requested}" ]; then
        printf '%s\n' "${env}"
        return 0
      fi
    done
    echo "error: unknown environment '${requested}'" >&2
    return 1
  fi

  if [ "${include_all}" -eq 1 ]; then
    printf '%s\n' "${envs[@]}"
  else
    printf '%s\n' "${envs[0]}"
  fi
}

default_env() {
  local default_env="default"
  while IFS= read -r env; do
    default_env="${env}"
    break
  done < <(platformio_envs)
  echo "${default_env}"
}

artifact_dir() {
  local cmd="${1}"
  local env="${2:-}"
  local base="${DIAG_DIR}/${RUN_ID}"
  if [ -n "${env}" ]; then
    echo "${base}/${cmd}/${env}"
  else
    echo "${base}/${cmd}"
  fi
}

require_positive_int() {
  local value="${1}"
  local label="${2}"
  if ! [[ "${value}" =~ ^[1-9][0-9]*$ ]]; then
    echo "error: ${label} must be a positive integer (got '${value}')" >&2
    exit 1
  fi
}

run_and_capture() {
  local logfile="$1"
  shift
  local -a cmd=("$@")

  mkdir -p "$(dirname "${logfile}")"
  {
    echo "==> ${cmd[*]}"
    echo "# $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  } | tee -a "${logfile}"

  if [ "${DRY_RUN}" -eq 1 ]; then
    echo "DRY RUN: skipped execution" | tee -a "${logfile}"
    return 0
  fi

  set +e
  "${cmd[@]}" 2>&1 | tee -a "${logfile}"
  local rc="${PIPESTATUS[0]}"
  set -e

  if [ "${VERBOSE}" -eq 1 ]; then
    echo "command_exit=${rc}" | tee -a "${logfile}"
  fi

  return "${rc}"
}

severity_to_rank() {
  case "${1}" in
    low) echo 1 ;;
    medium) echo 2 ;;
    high) echo 3 ;;
    error) echo 4 ;;
    *) echo 0 ;;
  esac
}

is_waived_finding() {
  local finding="${1}"
  local waiver
  if [ ! -f "${LINT_WAIVER_FILE}" ]; then
    return 1
  fi

  while IFS= read -r waiver || [ -n "${waiver}" ]; do
    [[ -z "${waiver}" ]] && continue
    [[ "${waiver}" == \#* ]] && continue
    if [[ "${finding}" == *"${waiver}"* ]]; then
      return 0
    fi
  done < "${LINT_WAIVER_FILE}"

  return 1
}

cmd_build() {
  local include_all=1
  while [ "${#}" -gt 0 ]; do
    case "${1}" in
      --all) include_all=1; shift ;;
      --help|-h)
        usage
        exit 0
        ;;
      *)
        echo "error: unknown build option '${1}'" >&2
        exit 1
        ;;
    esac
  done

  local -a envs
  mapfile -t envs < <(resolve_envs "${REQUESTED_ENV}" "${include_all}")
  if [ "${#envs[@]}" -eq 0 ]; then
    echo "error: no build environments resolved" >&2
    exit 1
  fi

  local overall=0
  for env in "${envs[@]}"; do
    local out_dir
    local log_file
    out_dir="$(artifact_dir build "${env}")"
    log_file="${out_dir}/build.txt"
    echo "env=${env}" | tee "${log_file}"
    if ! run_and_capture "${log_file}" "${PIO_CMD[@]}" run -e "${env}"; then
      overall=1
    fi
  done

  if [ "${overall}" -ne 0 ]; then
    echo "build: FAIL"
    exit 1
  fi
  echo "build: PASS"
}

cmd_upload() {
  local env="${REQUESTED_ENV:-}"
  if [ -z "${env}" ]; then
    env="$(default_env)"
  fi

  local target_env
  mapfile -t target_env < <(resolve_envs "${env}" 0)
  env="${target_env[0]}"

  local out_dir
  local log_file
  out_dir="$(artifact_dir upload "${env}")"
  log_file="${out_dir}/upload.txt"

  run_and_capture "${log_file}" "${PIO_CMD[@]}" run -e "${env}" -t upload --upload-port "${UPLOAD_PORT}"
  echo "upload: PASS (${env} -> ${UPLOAD_PORT})"
}

cmd_monitor() {
  local env="${REQUESTED_ENV:-}"
  if [ -z "${env}" ]; then
    env="$(default_env)"
  fi

  local target_env
  mapfile -t target_env < <(resolve_envs "${env}" 0)
  env="${target_env[0]}"

  local out_dir
  local log_file
  out_dir="$(artifact_dir monitor "${env}")"
  log_file="${out_dir}/monitor_boot.txt"

  echo "env=${env} port=${MONITOR_PORT}" | tee "${log_file}"
  if [ "${DRY_RUN}" -eq 1 ]; then
    echo "DRY RUN: skipped execution" | tee -a "${log_file}"
    return 0
  fi

  echo "==> ${PIO_CMD[*]} device monitor -e ${env} --timestamp --port ${MONITOR_PORT}" | tee -a "${log_file}"
  "${PIO_CMD[@]}" device monitor -e "${env}" --timestamp --port "${MONITOR_PORT}"
}

cmd_collect() {
  local env="${REQUESTED_ENV:-}"
  local tag="manual"
  local duration=120

  while [ "${#}" -gt 0 ]; do
    case "${1}" in
      --env)
        env="${2:-}"
        shift 2
        ;;
      --tag)
        tag="${2:-}"
        shift 2
        ;;
      --duration)
        duration="${2:-}"
        shift 2
        ;;
      --upload-port)
        if [ "${#}" -lt 2 ]; then
          echo "error: --upload-port requires a value" >&2
          exit 1
        fi
        UPLOAD_PORT="${2}"
        shift 2
        ;;
      --monitor-port)
        if [ "${#}" -lt 2 ]; then
          echo "error: --monitor-port requires a value" >&2
          exit 1
        fi
        MONITOR_PORT="${2}"
        shift 2
        ;;
      --help|-h)
        usage
        exit 0
        ;;
      *)
        echo "error: unknown collect option '${1}'" >&2
        exit 1
        ;;
    esac
  done

  if [ -z "${env}" ]; then
    echo "error: collect requires an environment. Use --env <env>." >&2
    exit 1
  fi
  require_positive_int "${duration}" "duration"

  local target_env
  mapfile -t target_env < <(resolve_envs "${env}" 0)
  env="${target_env[0]}"

  local out_dir
  local log_file
  out_dir="$(artifact_dir collect "${env}")"
  log_file="${out_dir}/${env}_${tag}.txt"

  {
    echo "# LUCE log capture"
    echo "# command=collect"
    echo "# env=${env}"
    echo "# tag=${tag}"
    echo "# duration_seconds=${duration}"
    echo "# upload_port=${UPLOAD_PORT}"
    echo "# monitor_port=${MONITOR_PORT}"
    echo "# date=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  } > "${log_file}"

  run_and_capture "${log_file}" "${PIO_CMD[@]}" run -e "${env}" -t upload --upload-port "${UPLOAD_PORT}"
  run_and_capture "${log_file}" python3 "${SCRIPT_DIR}/capture_serial.py" --port "${MONITOR_PORT}" --baud 115200 --seconds "${duration}" --output "${log_file}"
  echo "collect: PASS (${log_file})"
}

cmd_test() {
  local env="${REQUESTED_ENV:-net1}"
  local tag="smoke"
  local duration=45
  local -a required_markers=(
    "LUCE STRATEGY=NET1"
    "Feature flags: NVS=1 I2C=1 LCD=1 CLI=1 WIFI=1 NTP=1 mDNS=1 MQTT=1 HTTP=1"
  )

  while [ "${#}" -gt 0 ]; do
    case "${1}" in
      --env)
        env="${2:-}"
        shift 2
        ;;
      --tag)
        tag="${2:-}"
        shift 2
        ;;
      --duration)
        duration="${2:-}"
        shift 2
        ;;
      --require)
        required_markers+=("${2:-}")
        shift 2
        ;;
      --upload-port)
        UPLOAD_PORT="${2:-}"
        shift 2
        ;;
      --monitor-port)
        MONITOR_PORT="${2:-}"
        shift 2
        ;;
      --help|-h)
        usage
        exit 0
        ;;
      *)
        echo "error: unknown test option '${1}'" >&2
        exit 1
        ;;
    esac
  done

  if [ -z "${env}" ]; then
    echo "error: test requires an environment" >&2
    exit 1
  fi
  require_positive_int "${duration}" "duration"

  local target_env
  mapfile -t target_env < <(resolve_envs "${env}" 0)
  env="${target_env[0]}"

  local out_dir
  local log_file
  out_dir="$(artifact_dir test "${env}")"
  log_file="${out_dir}/${env}_${tag}.log"

  local tmp_env="${REQUESTED_ENV}"
  REQUESTED_ENV="${env}"
  cmd_collect --tag "${tag}" --duration "${duration}"
  local collected_log="${out_dir}/${env}_${tag}.txt"
  if [ -f "${collected_log}" ]; then
    mv "${collected_log}" "${log_file}"
  fi
  REQUESTED_ENV="${tmp_env}"

  local missing=0
  for marker in "${required_markers[@]}"; do
    if ! rg -Fq "${marker}" "${log_file}"; then
      echo "FAIL: missing marker '${marker}' in ${log_file}" >&2
      missing=1
    fi
  done

  if [ "${missing}" -ne 0 ]; then
    echo "test: FAIL"
    exit 2
  fi

  echo "test: PASS (${log_file})"
}

cmd_lint() {
  local include_all=1
  local min_sev="${LINT_MIN_SEVERITY}"
  local waiver_file="${LINT_WAIVER_FILE}"

  while [ "${#}" -gt 0 ]; do
    case "${1}" in
      --env)
        REQUESTED_ENV="${2:-}"
        include_all=0
        shift 2
        ;;
      --all)
        include_all=1
        shift
        ;;
      --min-severity)
        min_sev="${2:-medium}"
        shift 2
        ;;
      --waiver-file)
        waiver_file="${2:-${LINT_WAIVER_FILE}}"
        shift 2
        ;;
      --help|-h)
        usage
        exit 0
        ;;
      *)
        echo "error: unknown lint option '${1}'" >&2
        exit 1
        ;;
    esac
  done

  LINT_MIN_SEVERITY="${min_sev}"
  LINT_WAIVER_FILE="${waiver_file}"

  local min_rank
  min_rank="$(severity_to_rank "${LINT_MIN_SEVERITY}")"
  if [ "${min_rank}" -eq 0 ]; then
    echo "error: invalid severity '${LINT_MIN_SEVERITY}' (expected low|medium|high|error)" >&2
    exit 1
  fi

  local -a envs
  mapfile -t envs < <(resolve_envs "${REQUESTED_ENV}" "${include_all}")
  if [ "${#envs[@]}" -eq 0 ]; then
    echo "error: no lint environments resolved" >&2
    exit 1
  fi

  local out_dir
  out_dir="$(artifact_dir lint)"
  mkdir -p "${out_dir}"

  local overall=0
  for env in "${envs[@]}"; do
    local log_file="${out_dir}/platformio_check_${env}.txt"
    {
      echo "${PROJECT_ROOT}: running lint for env=${env}"
      echo "command: ${PIO_CMD[*]} check -e ${env} --skip-packages"
      echo "min_severity: ${LINT_MIN_SEVERITY}"
      echo "waiver_file: ${LINT_WAIVER_FILE}"
      echo
    } > "${log_file}"

    if ! "${PIO_CMD[@]}" check -e "${env}" --skip-packages >"${log_file}.raw" 2>&1; then
      :
    fi

    cat "${log_file}.raw" >> "${log_file}"

    local finding_count=0
    local unwaived_count=0
    local waived_count=0

    while IFS= read -r line; do
      if [[ "${line}" =~ ^([^:]+):([0-9]+):\ \[(low|medium|high|error)\:] ]]; then
        local severity="${BASH_REMATCH[3]}"
        local rank
        rank="$(severity_to_rank "${severity}")"
        if [ "${rank}" -lt "${min_rank}" ]; then
          continue
        fi
        finding_count=$((finding_count + 1))
        if is_waived_finding "${line}"; then
          waived_count=$((waived_count + 1))
          echo "WAIVED: ${line}" >> "${log_file}"
        else
          unwaived_count=$((unwaived_count + 1))
          echo "UNWAIVED: ${line}" >> "${log_file}"
        fi
      fi
    done < "${log_file}.raw"

    local status="PASS"
    if [ "${finding_count}" -gt 0 ] && [ "${unwaived_count}" -gt 0 ]; then
      status="FAIL"
      overall=1
    elif [ "${finding_count}" -gt 0 ] && [ "${unwaived_count}" -eq 0 ]; then
      status="PASS_WITH_WAIVER"
      echo "NOTE: findings detected in ${env} are fully covered by waivers." >> "${log_file}"
    fi
    echo "STATUS=${status}" >> "${log_file}"
    echo "findings=${finding_count}" >> "${log_file}"
    echo "unwaived_findings=${unwaived_count}" >> "${log_file}"
    echo "waived_findings=${waived_count}" >> "${log_file}"
    rm -f "${log_file}.raw"
  done

  {
    echo "STATUS_SUMMARY="
    for env in "${envs[@]}"; do
      local env_status
      env_status="$(awk -F= '/^STATUS=/{print $2; exit}' "${out_dir}/platformio_check_${env}.txt")"
      echo "  ${env}: ${env_status}"
    done
    if [ "${overall}" -ne 0 ]; then
      echo "lint: FAIL"
    else
      echo "lint: PASS"
    fi
  } | tee -a "${out_dir}/summary.txt"

  if [ "${overall}" -ne 0 ]; then
    exit 1
  fi
}

cmd_health() {
  local run_build=0
  local run_lint=0
  local out_dir
  out_dir="$(artifact_dir health)"
  mkdir -p "${out_dir}"
  local log_file="${out_dir}/health.txt"

  while [ "${#}" -gt 0 ]; do
    case "${1}" in
      --run-build)
        run_build=1
        shift
        ;;
      --run-lint)
        run_lint=1
        shift
        ;;
      --help|-h)
        usage
        exit 0
        ;;
      *)
        echo "error: unknown health option '${1}'" >&2
        exit 1
        ;;
    esac
  done

  {
    echo "# health preflight"
    echo "timestamp: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "pio_cmd: ${PIO_CMD[*]}"
    echo "requested_env: ${REQUESTED_ENV:-<unset>}"
    echo "upload_port: ${UPLOAD_PORT}"
    echo "monitor_port: ${MONITOR_PORT}"
    echo
  } > "${log_file}"

  echo "health: tooling check passed" >> "${log_file}"

  local -a envs
  mapfile -t envs < <(platformio_envs)
  if [ "${#envs[@]}" -eq 0 ]; then
    echo "health: FAIL (no envs in platformio.ini)" | tee -a "${log_file}"
    exit 1
  fi

  echo "envs: ${envs[*]}" | tee -a "${log_file}"

  if [ "${run_build}" -eq 1 ]; then
    local env="${REQUESTED_ENV:-${envs[0]}}"
    local saved_request="${REQUESTED_ENV}"
    REQUESTED_ENV="${env}"
    cmd_build
    REQUESTED_ENV="${saved_request}"
  fi

  if [ "${run_lint}" -eq 1 ]; then
    local saved_request="${REQUESTED_ENV}"
    REQUESTED_ENV="${REQUESTED_ENV:-${envs[0]}}"
    cmd_lint
    REQUESTED_ENV="${saved_request}"
  fi

  echo "health: PASS" | tee -a "${log_file}"
}

cmd_clean() {
  local include_all=1
  while [ "${#}" -gt 0 ]; do
    case "${1}" in
      --all) include_all=1; shift ;;
      --env)
        REQUESTED_ENV="${2:-}"
        include_all=0
        shift 2
        ;;
      --help|-h)
        usage
        exit 0
        ;;
      *)
        echo "error: unknown clean option '${1}'" >&2
        exit 1
        ;;
    esac
  done

  local -a envs
  mapfile -t envs < <(resolve_envs "${REQUESTED_ENV}" "${include_all}")
  if [ "${#envs[@]}" -eq 0 ]; then
    echo "error: no environments resolved" >&2
    exit 1
  fi

  for env in "${envs[@]}"; do
    local out_dir
    local log_file
    out_dir="$(artifact_dir clean "${env}")"
    log_file="${out_dir}/clean.txt"
    run_and_capture "${log_file}" "${PIO_CMD[@]}" run -e "${env}" -t clean
  done
}

cmd_all() {
  local run_upload=0
  local run_monitor=0
  while [ "${#}" -gt 0 ]; do
    case "${1}" in
      --upload) run_upload=1; shift ;;
      --monitor) run_monitor=1; shift ;;
      --help|-h)
        usage
        exit 0
        ;;
      *)
        echo "error: unknown all option '${1}'" >&2
        exit 1
        ;;
    esac
  done

  cmd_build --all
  if [ "${run_upload}" -eq 1 ]; then
    cmd_upload
  fi
  if [ "${run_monitor}" -eq 1 ]; then
    cmd_monitor
  fi
}

parse_globals() {
  while [ "${#}" -gt 0 ]; do
    case "${1}" in
      -h|--help)
        usage
        exit 0
      ;;
    -e|--env)
        if [ "${#}" -lt 2 ]; then
          echo "error: ${1} requires a value" >&2
          exit 1
        fi
        REQUESTED_ENV="${2:-}"
        shift 2
        ;;
      --upload-port)
        if [ "${#}" -lt 2 ]; then
          echo "error: --upload-port requires a value" >&2
          exit 1
        fi
        UPLOAD_PORT="${2}"
        shift 2
        ;;
      --monitor-port)
        if [ "${#}" -lt 2 ]; then
          echo "error: --monitor-port requires a value" >&2
          exit 1
        fi
        MONITOR_PORT="${2}"
        shift 2
        ;;
      --diag-dir)
        if [ "${#}" -lt 2 ]; then
          echo "error: --diag-dir requires a value" >&2
          exit 1
        fi
        DIAG_DIR="${2}"
        shift 2
        ;;
      --dry-run)
        DRY_RUN=1
        shift
        ;;
      -v|--verbose)
        VERBOSE=1
        shift
        ;;
      --)
        shift
        break
        ;;
      -*) 
        echo "error: unknown global option '${1}'" >&2
        usage
        exit 1
        ;;
      *)
        break
        ;;
    esac
  done
}

has_help_request() {
  local arg
  for arg in "$@"; do
    if [ "${arg}" = "--help" ] || [ "${arg}" = "-h" ]; then
      return 0
    fi
  done
  return 1
}

main() {
  if [ "${#}" -eq 0 ]; then
    usage
    exit 1
  fi

  if [ ! -f "${PLATFORMIO_INI}" ]; then
    echo "error: missing platformio.ini at ${PLATFORMIO_INI}" >&2
    exit 1
  fi

  parse_globals "$@"
  local cmd="$1"
  shift || true

  if has_help_request "$@"; then
    usage
    exit 0
  fi

  if [ ! -f "${PLATFORMIO_INI}" ]; then
    echo "error: missing platformio.ini at ${PLATFORMIO_INI}" >&2
    exit 1
  fi

  resolve_pio_cmd

  case "${cmd}" in
    build) cmd_build "$@" ;;
    upload) cmd_upload "$@" ;;
    monitor) cmd_monitor "$@" ;;
    collect) cmd_collect "$@" ;;
    test) cmd_test "$@" ;;
    lint) cmd_lint "$@" ;;
    health) cmd_health "$@" ;;
    clean) cmd_clean "$@" ;;
    all) cmd_all "$@" ;;
    *)
      echo "error: unknown command '${cmd}'" >&2
      usage
      exit 1
      ;;
  esac
}

main "$@"
