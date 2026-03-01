#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: scripts/http_api_smoke.sh [--host http://host] [--token TOKEN] [--skip-unauth]

Smoke checks:
  - /api/health         (GET)
  - /api/info           (GET)
  - /api/version        (GET)
  - /api/ota            (GET)
  - /api/ota/check      (POST and PUT)

Optional args:
  --host http://host   Base URL (default: https://127.0.0.1)
  --token TOKEN        Bearer token for protected endpoints
  --skip-unauth        Skip negative tests expecting 401
EOF
}

HOST="https://127.0.0.1"
TOKEN="${LUCE_HTTP_TOKEN:-}"
SKIP_UNAUTH=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --host)
      if [[ $# -lt 2 ]]; then
        echo "error: --host requires value" >&2
        exit 1
      fi
      HOST="$2"
      shift 2
      ;;
    --token)
      if [[ $# -lt 2 ]]; then
        echo "error: --token requires value" >&2
        exit 1
      fi
      TOKEN="$2"
      shift 2
      ;;
    --skip-unauth)
      SKIP_UNAUTH=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "error: unknown option $1" >&2
      usage
      exit 1
      ;;
  esac
done

AUTH_ARGS=()
if [[ -n "${TOKEN}" ]]; then
  AUTH_ARGS=(-H "Authorization: Bearer ${TOKEN}")
fi

expect_status() {
  local method="$1"
  local path="$2"
  local expected_code="$3"
  local extra_args=("${@:4}")
  local url="${HOST}${path}"

  local code
  code="$(curl -sS -o /tmp/luce_http_smoke.body -w "%{http_code}" \
    -X "${method}" "${extra_args[@]}" "${url}" )" || {
    echo "error: request failed (${method} ${path})" >&2
    return 1
  }

  if [[ "${code}" != "${expected_code}" ]]; then
    echo "fail: ${method} ${path} expected ${expected_code}, got ${code}" >&2
    cat /tmp/luce_http_smoke.body >&2 || true
    return 1
  fi

  echo "ok: ${method} ${path} -> ${expected_code}"
  return 0
}

check_json_field() {
  local field="$1"
  local value="$2"
  if ! jq -e ".${field} | tostring == \"${value}\" or .${field} == ${value}" /tmp/luce_http_smoke.body >/dev/null 2>&1; then
    echo "warn: response missing/invalid ${field}" >&2
    cat /tmp/luce_http_smoke.body >&2
    return 1
  fi
  return 0
}

if [[ -z "$(command -v curl 2>/dev/null)" ]]; then
  echo "error: curl is required" >&2
  exit 1
fi
if [[ -z "$(command -v jq 2>/dev/null)" ]]; then
  echo "error: jq is required for response checks" >&2
  exit 1
fi

echo "smoke: host=${HOST}"
echo "smoke: token=${TOKEN:+(set)}${TOKEN:+ }"

expect_status GET "/api/health" 200 -k "${AUTH_ARGS[@]}" -H "Accept: application/json"
expect_status GET "/api/version" 200 -k "${AUTH_ARGS[@]}" -H "Accept: application/json"
check_json_field "service" "luce"
expect_status GET "/api/info" 200 -k "${AUTH_ARGS[@]}" -H "Accept: application/json"
expect_status GET "/api/ota" 200 -k "${AUTH_ARGS[@]}" -H "Accept: application/json"
expect_status POST "/api/ota/check" 202 -k "${AUTH_ARGS[@]}" -H "Accept: application/json" || true
expect_status PUT "/api/ota/check" 202 -k "${AUTH_ARGS[@]}" -H "Accept: application/json" || true

if [[ "${SKIP_UNAUTH}" -eq 0 ]]; then
  expect_status GET "/api/info" 401 -k -H "Accept: application/json"
  expect_status GET "/api/ota" 401 -k -H "Accept: application/json"
  expect_status POST "/api/ota/check" 401 -k -H "Accept: application/json"
fi

expect_status POST "/api/info" 405 -k -H "Accept: application/json"
expect_status PUT "/api/version" 405 -k -H "Accept: application/json"
expect_status PATCH "/api/health" 405 -k -H "Accept: application/json"

echo "smoke: complete"
