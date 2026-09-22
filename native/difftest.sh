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

# Imports: a three-file program with a diamond, built in a temp directory.
IMPDIR=$(mktemp -d)
printf '~TWO: 2\n^fn triple(x) = x * 3\n' > "$IMPDIR/mathlib.ax"
printf '^use "mathlib.ax"\n^fn sextuple(x) = triple(x) * TWO\n' > "$IMPDIR/util.ax"
printf '^use "util.ax", "mathlib.ax"\n^main:\n  print(sextuple(2), triple(3), TWO)\n' > "$IMPDIR/app.ax"
run_case "^use imports" "$IMPDIR/app.ax"
rm -rf "$IMPDIR"

run_case "examples/fizzbuzz.ax" "$ROOT/examples/fizzbuzz.ax"
run_case "examples/stats.ax"    "$ROOT/examples/stats.ax"
run_case "examples/calc.ax"     "$ROOT/examples/calc.ax"
run_case "examples/calc.ax (expr)" "$ROOT/examples/calc.ax" -- "2 * (3 + 4) - 10 / 5"
run_case "examples/life.ax"     "$ROOT/examples/life.ax" -- 3
run_case "examples/wordcount.ax" "$ROOT/examples/wordcount.ax" -- "$ROOT/examples/calc.ax" 5

# Engine: step each simulation on both runtimes and compare the final state as JSON (entities,
# fields, positions, log, sim time — exactly; diagnostics by code, entity, block and line).
sim_case() {
  desc="$1"; file="$2"; frames="$3"; shift 3
  $NODE_RUN "$file" --sim "$frames" --json "$@" > /tmp/ax_js.json 2>/dev/null
  js_code=$?
  "$NATIVE" "$file" --sim "$frames" --json "$@" > /tmp/ax_c.json 2>/dev/null
  c_code=$?
  if node "$ROOT/native/simcmp.js" /tmp/ax_js.json /tmp/ax_c.json > /tmp/ax_cmp.txt 2>&1 && [ "$js_code" = "$c_code" ]; then
    pass=$((pass + 1))
    printf 'OK   %s\n' "$desc"
  else
    fail=$((fail + 1))
    printf 'FAIL %s (js exit %s, native exit %s)\n' "$desc" "$js_code" "$c_code"
    head -6 /tmp/ax_cmp.txt
  fi
}

for f in "$ROOT"/native/tests/engine/*.ax; do
  [ -e "$f" ] || continue
  sim_case "sim $(basename "$f")" "$f" 240
done
sim_case "sim examples/sim.ax (1000 frames)" "$ROOT/examples/sim.ax" 1000
if [ -e "$ROOT/native/tests/engine/input.txt" ]; then
  sim_case "sim with --input" "$ROOT/native/tests/engine/01_physics.ax" 120 --input "$ROOT/native/tests/engine/input.txt"
fi

# Math: V8 computes Math.* with fdlibm, and so does jsmath.c; check them against each other.
MDIR=$(mktemp -d)
if node "$ROOT/native/tests/math/gen.js" "$MDIR/in.txt" "$MDIR/js.txt" 100000 \
   && cc -O2 -o "$MDIR/mathcheck" "$ROOT/native/tests/math/mathcheck.c" "$ROOT/native/jsmath.c" -lm \
   && "$MDIR/mathcheck" "$MDIR/in.txt" "$MDIR/js.txt" > "$MDIR/out.txt"; then
  pass=$((pass + 1)); printf 'OK   math: 18 functions x 100000 inputs bit-identical to V8\n'
else
  fail=$((fail + 1)); printf 'FAIL math\n'; cat "$MDIR/out.txt" 2>/dev/null | tail -3
fi
rm -rf "$MDIR"

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
