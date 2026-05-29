#!/usr/bin/env bash
set -u

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
run_id="${LUCE_RUN_ID:-$(date -u +%Y%m%dT%H%M%SZ)}"
out_dir="${repo_root}/docs/work/diag/${run_id}/host-tests"
build_dir="${out_dir}/build"
log_file="${out_dir}/host-tests.log"
junit_file="${out_dir}/junit.xml"
summary_file="${out_dir}/summary.json"

mkdir -p "${build_dir}"

status="PASS"
exit_code=0

if command -v cmake >/dev/null 2>&1; then
  if ! cmake -S "${repo_root}/test/host" -B "${build_dir}" >"${log_file}" 2>&1; then
    status="FAIL"
    exit_code=1
  elif ! cmake --build "${build_dir}" >>"${log_file}" 2>&1; then
    status="FAIL"
    exit_code=1
  fi
else
  compiler="${CXX:-c++}"
  {
    printf 'cmake not found; using direct compiler fallback: %s\n' "${compiler}"
    "${compiler}" -std=c++17 -Wall -Wextra -Werror -I"${repo_root}/include" \
      "${repo_root}/test/host/test_main.cpp" -o "${build_dir}/luce_host_tests"
  } >"${log_file}" 2>&1 || {
    status="FAIL"
    exit_code=1
  }
fi

if [[ "${status}" == "PASS" ]] && ! "${build_dir}/luce_host_tests" >>"${log_file}" 2>&1; then
  status="FAIL"
  exit_code=1
fi

failures=0
if [[ "${status}" != "PASS" ]]; then
  failures=1
fi

cat >"${junit_file}" <<XML
<?xml version="1.0" encoding="UTF-8"?>
<testsuite name="luce-host" tests="1" failures="${failures}">
  <testcase classname="luce.host" name="sanity">
XML

if [[ "${failures}" -ne 0 ]]; then
  cat >>"${junit_file}" <<XML
    <failure message="host test command failed">See host-tests.log</failure>
XML
fi

cat >>"${junit_file}" <<XML
  </testcase>
</testsuite>
XML

cat >"${summary_file}" <<JSON
{
  "run_id": "${run_id}",
  "status": "${status}",
  "tests": 1,
  "failures": ${failures},
  "junit": "${junit_file}",
  "log": "${log_file}"
}
JSON

printf 'host-tests %s\nsummary: %s\n' "${status}" "${summary_file}"
exit "${exit_code}"
