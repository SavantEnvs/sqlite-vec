#!/usr/bin/env bash
#
# mayhem/test.sh — RUN sqlite-vec's functional tests (built by mayhem/build.sh with normal flags):
# a SQL-level known-answer test (mayhem/test-sql.c).
# Counts come from each program's own output, so a neutered (exit(0)) binary fails loudly.
set -uo pipefail
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH
cd "$SRC"

emit_ctrf() {
  local tool="$1" passed="$2" failed="$3" skipped="${4:-0}" pending="${5:-0}" other="${6:-0}"
  local tests=$(( passed + failed + skipped + pending + other ))
  cat > "${CTRF_REPORT:-$SRC/ctrf-report.json}" <<JSON
{
  "results": {
    "tool": { "name": "$tool" },
    "summary": {
      "tests": $tests,
      "passed": $passed,
      "failed": $failed,
      "pending": $pending,
      "skipped": $skipped,
      "other": $other
    }
  }
}
JSON
  printf 'CTRF {"results":{"tool":{"name":"%s"},"summary":{"tests":%d,"passed":%d,"failed":%d,"pending":%d,"skipped":%d,"other":%d}}}\n' \
    "$tool" "$tests" "$passed" "$failed" "$pending" "$skipped" "$other"
  [ "$failed" -eq 0 ]
}

[ -x /mayhem/test-sql ] || { echo "missing /mayhem/test-sql — mayhem/build.sh should have built it" >&2; emit_ctrf sqlite-vec 0 1; exit 1; }

# One PASS/FAIL line per check; the test must also print its own summary line (a neutered binary prints nothing).
out="$(/mayhem/test-sql 2>&1)"; rc=$?
echo "$out" | grep -v '^PASS' | tail -10
pass=$(echo "$out" | grep -c '^PASS ')
fail=$(echo "$out" | grep -c '^FAIL ')
if [ "$rc" -ne 0 ] || ! echo "$out" | grep -q '^SQLTEST passed='; then fail=$(( fail + 1 )); fi
if [ "$pass" -lt 45 ]; then fail=$(( fail + 1 )); fi
emit_ctrf sqlite-vec "$pass" "$fail"
