#!/usr/bin/env bash
#
# mayhem/build.sh — build sqlite-vec's fuzz harnesses (tests/fuzz/*.c) and its unit/functional tests.
# Runs in the commit image as `mayhem` in /mayhem; also re-run offline (SPEC §6.5). SQLite comes from
# the image's libsqlite3-dev (apt), so nothing is fetched here.
set -euo pipefail

[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH

: "${SANITIZER_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer}"
: "${DEBUG_FLAGS:=-g -gdwarf-3}"
: "${CC:=clang}" ; : "${CXX:=clang++}" ; : "${LIB_FUZZING_ENGINE:=-fsanitize=fuzzer}"
: "${MAYHEM_JOBS:=$(nproc)}"
: "${COVERAGE_FLAGS=}"
export SANITIZER_FLAGS DEBUG_FLAGS CC CXX LIB_FUZZING_ENGINE MAYHEM_JOBS COVERAGE_FLAGS

cd "$SRC"

# sqlite-vec.h is generated from sqlite-vec.h.tmpl and gitignored upstream: (re)create it here.
VERSION="$(cat VERSION)"
export VERSION
export DATE="$(git log -n 1 --pretty=format:%cI -- VERSION 2>/dev/null || echo unknown)"
export SOURCE="$(git log -n 1 --pretty=format:%H -- VERSION 2>/dev/null || echo unknown)"
export VERSION_MAJOR="$(echo "$VERSION" | cut -d. -f1)"
export VERSION_MINOR="$(echo "$VERSION" | cut -d. -f2)"
export VERSION_PATCH="$(echo "$VERSION" | cut -d. -f3 | cut -d- -f1)"
envsubst < sqlite-vec.h.tmpl > sqlite-vec.h

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# 1) Sanitized + coverage-instrumented sqlite-vec (the fuzzed library), in the build variants upstream's
#    tests/fuzz/Makefile uses: plain, and -DSQLITE_VEC_TEST (exposes _test_* internals to two harnesses).
#    -fsanitize=fuzzer-no-link puts SanCov in the library; without it Mayhem sees 0 edges.
FUZZ_CFLAGS="$SANITIZER_FLAGS $DEBUG_FLAGS -O1 -fsanitize=fuzzer-no-link"
# IVF cells index raw blob bytes at slot*vecSize, so float loads are misaligned (benign on x86, upstream design);
# halting on it kills every multi-cell input before any coverage. Only this one UBSan check is relaxed, library only.
VEC_DEFS="-fno-sanitize=alignment -DSQLITE_CORE -DSQLITE_VEC_ENABLE_DISKANN=1 -DSQLITE_VEC_EXPERIMENTAL_IVF_ENABLE=1"
$CC $FUZZ_CFLAGS $VEC_DEFS -I. -c sqlite-vec.c -o "$WORK/vec.o" &
$CC $FUZZ_CFLAGS $VEC_DEFS -DSQLITE_VEC_TEST -I. -c sqlite-vec.c -o "$WORK/vec-test.o" &
# 2) leak sanitizer off at build time (SPEC §6.2 item 15).
$CXX $SANITIZER_FLAGS $DEBUG_FLAGS -c mayhem/lsan_off.cc -o "$WORK/lsan_off.o" &
wait

# 3) Each harness twice: libFuzzer binary + a run-once -standalone reproducer.
build_target() {
  local t="$1" src vec
  src="tests/fuzz/${t//_/-}.c"
  # corrected copies of upstream harnesses (harness bugs, not library bugs) shadow the upstream file
  [ -f "mayhem/harness/${t//_/-}.c" ] && src="mayhem/harness/${t//_/-}.c"
  vec="$WORK/vec.o"
  case "$t" in rescore_quantize|rescore_quantize_edge) vec="$WORK/vec-test.o" ;; esac
  $CC $FUZZ_CFLAGS -DSQLITE_CORE -I. -c "$src" -o "$WORK/$t.o"
  $CC $FUZZ_CFLAGS $LIB_FUZZING_ENGINE "$WORK/$t.o" "$vec" "$WORK/lsan_off.o" -lsqlite3 -lm -o "/mayhem/$t"
  $CC $FUZZ_CFLAGS "$STANDALONE_FUZZ_MAIN" "$WORK/$t.o" "$vec" "$WORK/lsan_off.o" -lsqlite3 -lm -o "/mayhem/$t-standalone"
}
export -f build_target
export WORK FUZZ_CFLAGS
for f in tests/fuzz/*.c; do b="$(basename "$f" .c)"; echo "${b//-/_}"; done \
  | xargs -P "$MAYHEM_JOBS" -I{} bash -c 'build_target {}'

# 4) Test binary with the project's NORMAL flags (no sanitizers, no DWARF pin): a SQL-level
#    known-answer test. mayhem/test.sh only runs it. (Upstream's tests/test-unit.c does not compile at
#    this commit — sqlite-vec-internal.h redefines Vec0RescoreQuantizerType and test-unit.c has a
#    mangled #if/function nesting — so it cannot serve as the oracle.)
$CC -O2 $COVERAGE_FLAGS -DSQLITE_CORE -I. mayhem/test-sql.c sqlite-vec.c -lsqlite3 -lm -o /mayhem/test-sql
