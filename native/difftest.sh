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
cd "$ROOT/native" || exit 2   # engine tests name their .nav files relative to this directory
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

# Script mode's --json: {main_result, log, diagnostics, exit_code}, including when ^main
# faults or calls exit(). Compared like the simulation dumps (below): exactly, except the prose
# of a raw error message.
json_case() {
  desc="$1"; file="$2"; shift 2
  $NODE_RUN "$file" --no-restack --json "$@" > /tmp/ax_js.json 2>/dev/null
  js_code=$?
  "$NATIVE" "$file" --json "$@" > /tmp/ax_c.json 2>/dev/null
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
for f in "$ROOT"/native/tests/json/*.ax; do
  [ -e "$f" ] || continue
  run_case "$(basename "$f")" "$f" --run
  json_case "$(basename "$f") --json" "$f" --run
done

# The REPL: the same transcript through both, stdout and exit code identical.
for f in "$ROOT"/native/tests/repl/*.txt; do
  [ -e "$f" ] || continue
  js_out=$($NODE_RUN --repl --no-restack < "$f" 2>/dev/null); js_code=$?
  c_out=$("$NATIVE" --repl < "$f" 2>/dev/null); c_code=$?
  if [ "$js_out" = "$c_out" ] && [ "$js_code" = "$c_code" ]; then
    pass=$((pass + 1)); printf 'OK   repl %s\n' "$(basename "$f")"
  else
    fail=$((fail + 1)); printf 'FAIL repl %s (js exit %s, native exit %s)\n' "$(basename "$f")" "$js_code" "$c_code"
    printf '%s\n' "$js_out" > /tmp/ax_js.txt; printf '%s\n' "$c_out" > /tmp/ax_c.txt
    diff /tmp/ax_js.txt /tmp/ax_c.txt | head -12
  fi
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
  rm -rf saves
  $NODE_RUN "$file" --sim "$frames" --json "$@" > /tmp/ax_js.json 2>/dev/null
  js_code=$?
  [ -d saves ] && rm -rf /tmp/ax_js_saves && mv saves /tmp/ax_js_saves
  "$NATIVE" "$file" --sim "$frames" --json "$@" > /tmp/ax_c.json 2>/dev/null
  c_code=$?
  # !save writes saves/<slot>.json; the files must match too.
  if [ -d /tmp/ax_js_saves ] || [ -d saves ]; then
    if ! diff -r /tmp/ax_js_saves saves > /tmp/ax_cmp_saves.txt 2>&1; then
      rm -rf saves /tmp/ax_js_saves
      fail=$((fail + 1)); printf 'FAIL %s (save files differ)\n' "$desc"; head -6 /tmp/ax_cmp_saves.txt; return
    fi
    rm -rf saves /tmp/ax_js_saves
  fi
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
  sim_case "sim 11_misc.ax with --input" "$ROOT/native/tests/engine/11_misc.ax" 240 --input "$ROOT/native/tests/engine/input.txt"
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

# Terminal output: the same pixel buffers through terminal.js and term.c must give the same bytes.
TDIR=$(mktemp -d)
if cc -O2 -o "$TDIR/termcheck" "$ROOT/native/tests/term/termcheck.c" "$ROOT/native/term.c" -lm \
   && node "$ROOT/native/tests/term/gen.js" "$TDIR" && "$TDIR/termcheck" "$TDIR" > "$TDIR/out.txt" \
   && COLORTERM=truecolor node "$ROOT/native/tests/term/gen.js" "$TDIR" && COLORTERM=truecolor "$TDIR/termcheck" "$TDIR" >> "$TDIR/out.txt"; then
  pass=$((pass + 1)); printf 'OK   terminal output byte-identical to terminal.js (60 cases)\n'
else
  fail=$((fail + 1)); printf 'FAIL terminal output\n'; cat "$TDIR/out.txt" 2>/dev/null
fi
rm -rf "$TDIR"

# Rendering has no JavaScript counterpart (render3d.js is not part of the code base), so this is
# a smoke test: the scene renders, and the PNG is a valid image that is not just sky.
RDIR=$(mktemp -d)
if (cd "$RDIR" && "$NATIVE" "$ROOT/examples/scene.ax" --headless 30 --png-every 30 --width 160 --height 120 > /dev/null) \
   && node -e "const b=require('fs').readFileSync('$RDIR/screenshots/frame_00030.png'); process.exit(b.slice(1,4).toString()==='PNG' && b.length > 50000 ? 0 : 1)"; then
  pass=$((pass + 1)); printf 'OK   render examples/scene.ax to PNG\n'
else
  fail=$((fail + 1)); printf 'FAIL render examples/scene.ax\n'
fi
rm -rf "$RDIR"

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
