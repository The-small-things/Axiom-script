#!/bin/sh
# difftest.sh — run every program under test on BOTH runtimes and compare.
#
# The JavaScript implementation is the specification while the port is in progress, so any
# difference in output, exit code, or diagnostics is a bug in the native build until proven
# otherwise. This script is the whole verification story for the port.

set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
NATIVE="$ROOT/native/axiom"
NODE_RUN="node $ROOT/main.js"
pass=0
fail=0

run_case() {
  desc="$1"; file="$2"; shift 2
  js_out=$($NODE_RUN "$file" --no-restack "$@" 2>/dev/null)
  js_code=$?
  c_out=$("$NATIVE" "$file" "$@" 2>/dev/null)
  c_code=$?
  if [ "$js_out" = "$c_out" ] && [ "$js_code" = "$c_code" ]; then
    pass=$((pass + 1))
    printf 'OK   %s\n' "$desc"
  else
    fail=$((fail + 1))
    printf 'FAIL %s (js exit %s, native exit %s)\n' "$desc" "$js_code" "$c_code"
    printf '%s\n' "$js_out" > /tmp/ax_js.txt
    printf '%s\n' "$c_out" > /tmp/ax_c.txt
    diff /tmp/ax_js.txt /tmp/ax_c.txt | head -12
  fi
}

for f in "$ROOT"/native/tests/*.ax; do
  [ -e "$f" ] || continue
  run_case "$(basename "$f")" "$f"
done

run_case "examples/fizzbuzz.ax" "$ROOT/examples/fizzbuzz.ax"
run_case "examples/stats.ax"    "$ROOT/examples/stats.ax"
run_case "examples/calc.ax"     "$ROOT/examples/calc.ax"
run_case "examples/calc.ax (expr)" "$ROOT/examples/calc.ax" -- "2 * (3 + 4) - 10 / 5"
run_case "examples/life.ax"     "$ROOT/examples/life.ax" -- 3
run_case "examples/wordcount.ax" "$ROOT/examples/wordcount.ax" -- "$ROOT/examples/calc.ax" 5

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
