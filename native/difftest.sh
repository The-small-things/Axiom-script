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

# The static checker: `--check --json` on both, compared by checkcmp.js (everything identical
# but a parse error's wording and position). The corpus in tests/check trips every diagnostic;
# every other program must check the same too; then 300 mutants of all of them.
check_case() {
  $NODE_RUN "$1" --no-restack --check --json > /tmp/ax_js.json 2>/dev/null; js_code=$?
  "$NATIVE" "$1" --check --json > /tmp/ax_c.json 2>/dev/null; c_code=$?
  node "$ROOT/native/checkcmp.js" /tmp/ax_js.json /tmp/ax_c.json > /tmp/ax_cmp.txt 2>&1 && [ "$js_code" = "$c_code" ]
}
CHECK_FILES="$(ls "$ROOT"/native/tests/check/*.ax "$ROOT"/native/tests/*.ax "$ROOT"/native/tests/engine/*.ax "$ROOT"/native/tests/json/*.ax "$ROOT"/examples/*.ax)"
cfail=0; cn=0
for f in $CHECK_FILES; do
  cn=$((cn + 1))
  if ! check_case "$f"; then cfail=$((cfail + 1)); printf '  differs: %s\n' "$f"; head -3 /tmp/ax_cmp.txt; fi
done
if [ "$cfail" -eq 0 ]; then pass=$((pass + 1)); printf 'OK   --check identical on %s programs (tests/check trips every diagnostic)\n' "$cn"
else fail=$((fail + 1)); printf 'FAIL --check on %s of %s programs\n' "$cfail" "$cn"; fi
MDIR=$(mktemp -d)
mkdir -p "$MDIR/lib" && cp "$ROOT"/native/tests/check/lib/* "$MDIR/lib/"
node "$ROOT/native/tests/check/mutate.js" "$MDIR" 300 $CHECK_FILES
cfail=0
for f in "$MDIR"/m*.ax; do
  if ! check_case "$f"; then cfail=$((cfail + 1)); [ "$cfail" -le 3 ] && { printf '  differs: %s\n' "$f"; head -3 /tmp/ax_cmp.txt; cp "$f" /tmp/ax_mutant_$(basename "$f"); }; fi
done
if [ "$cfail" -eq 0 ]; then pass=$((pass + 1)); printf 'OK   --check identical on 300 mutants of them\n'
else fail=$((fail + 1)); printf 'FAIL --check on %s of 300 mutants (kept in /tmp/ax_mutant_*)\n' "$cfail"; fi
rm -rf "$MDIR"
if node "$ROOT/native/tools/gen_checknames.js" | cmp -s - "$ROOT/native/checknames.h"; then
  pass=$((pass + 1)); printf 'OK   checknames.h matches checker.js and interpreter.js\n'
else
  fail=$((fail + 1)); printf 'FAIL checknames.h is stale: node tools/gen_checknames.js > checknames.h\n'
fi

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
  rm -rf saves /tmp/ax_js_saves
  $NODE_RUN "$file" --sim "$frames" --json "$@" > /tmp/ax_js.json 2>/dev/null
  js_code=$?
  [ -d saves ] && mv saves /tmp/ax_js_saves
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
sim_case "sim 22_fire.ax with --input (F = fire)" "$ROOT/native/tests/engine/22_fire.ax" 12 --input "$ROOT/native/tests/engine/fire.txt"
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

# Number formatting: String(x) for boundary values, every power of two, integers around 2^53
# and 2^63, and 100 000 random doubles, against ax_fmt_num.
NDIR=$(mktemp -d)
if node "$ROOT/native/tests/numfmt/gen.js" "$NDIR/in.txt" 100000 \
   && cc -std=c11 -O1 -D_GNU_SOURCE -o "$NDIR/numcheck" "$ROOT/native/tests/numfmt/numcheck.c" $(ls "$ROOT"/native/*.c | grep -v -e '/main.c') -lm \
   && "$NDIR/numcheck" "$NDIR/in.txt" > "$NDIR/out.txt"; then
  pass=$((pass + 1)); printf 'OK   number formatting identical to String(x) (%s)\n' "$(tail -1 "$NDIR/out.txt" | cut -d, -f1)"
else
  fail=$((fail + 1)); printf 'FAIL number formatting\n'; head -5 "$NDIR/out.txt" 2>/dev/null
fi
rm -rf "$NDIR"

# .glb meshes: a corpus of 29 files (index types, missing attributes, padding, skins, animations,
# out-of-bounds reads, broken containers) and 400 mutants of them, parsed by interpreter.js and by
# glb.c — every vertex, index, material and failure must agree. Then the model is drawn, and must
# not look like a box.
GDIR=$(mktemp -d)
if node "$ROOT/native/tests/glb/make.js" "$GDIR" 400 \
   && cc -std=c11 -O1 -D_GNU_SOURCE -o "$GDIR/glbcheck" "$ROOT/native/tests/glb/glbcheck.c" $(ls "$ROOT"/native/*.c | grep -v -e '/main.c') -lm \
   && "$GDIR/glbcheck" $(ls "$GDIR"/*.glb | sort) > "$GDIR/c.txt" && diff "$GDIR/js.txt" "$GDIR/c.txt" > "$GDIR/diff.txt"; then
  pass=$((pass + 1)); printf 'OK   .glb parsing identical to parseGLBMulti (%s files)\n' "$(grep -c '^==' "$GDIR/js.txt")"
else
  fail=$((fail + 1)); printf 'FAIL .glb parsing\n'; head -8 "$GDIR/diff.txt" 2>/dev/null
fi
sed 's/!mesh(#Model_0/!mesh(#Box/' "$ROOT/native/tests/glb/scene.ax" > "$GDIR/box.ax"
if "$NATIVE" "$ROOT/native/tests/glb/scene.ax" --headless 1 --width 120 --height 90 > /dev/null && mv screenshots/frame_00001.png "$GDIR/glb.png" \
   && "$NATIVE" "$GDIR/box.ax" --headless 1 --width 120 --height 90 > /dev/null && mv screenshots/frame_00001.png "$GDIR/box.png" \
   && [ "$(node "$ROOT/native/tests/glb/pngdiff.js" "$GDIR/glb.png" "$GDIR/box.png" | cut -d' ' -f1)" -gt 100 ]; then
  pass=$((pass + 1)); printf 'OK   render a .glb model (its own triangles, not the fallback box)\n'
else
  fail=$((fail + 1)); printf 'FAIL render a .glb model\n'
fi
rmdir screenshots 2>/dev/null
rm -rf "$GDIR"

# Big integers: a seeded program of limb-boundary and random operands through every operator
# (tests/bigint/gen.js); V8's BigInt is the reference.
BDIR=$(mktemp -d)
node "$ROOT/native/tests/bigint/gen.js" "$BDIR/arith.ax" 400
$NODE_RUN "$BDIR/arith.ax" --no-restack > "$BDIR/js.txt" 2>&1; bj=$?
"$NATIVE" "$BDIR/arith.ax" > "$BDIR/c.txt" 2>&1; bc=$?
if [ "$bj" = "$bc" ] && cmp -s "$BDIR/js.txt" "$BDIR/c.txt"; then
  pass=$((pass + 1)); printf 'OK   big-integer arithmetic identical to V8 (%s lines)\n' "$(wc -l < "$BDIR/js.txt" | tr -d ' ')"
else
  fail=$((fail + 1)); printf 'FAIL big-integer arithmetic\n'; diff "$BDIR/js.txt" "$BDIR/c.txt" | head -8
fi
rm -rf "$BDIR"

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

# The library sweep: every library function and common method called with every argument
# shape, sensible or not (tests/libsweep/gen.js); each call's result or error must match.
SDIR=$(mktemp -d)
node "$ROOT/native/tests/libsweep/gen.js" "$SDIR/sweep.ax"
(cd "$SDIR" && $NODE_RUN sweep.ax > js.txt 2>&1; "$NATIVE" sweep.ax > c.txt 2>&1)
calls=$(grep -c '^  t("' "$SDIR/sweep.ax")
if node "$ROOT/native/tests/libsweep/compare.js" "$SDIR/js.txt" "$SDIR/c.txt" --expect "$calls" > /tmp/ax_sweep.txt 2>&1; then
  pass=$((pass + 1)); printf 'OK   library sweep: %s\n' "$(tail -1 /tmp/ax_sweep.txt)"
else
  fail=$((fail + 1)); printf 'FAIL library sweep\n'; head -24 /tmp/ax_sweep.txt; tail -2 /tmp/ax_sweep.txt
fi
rm -rf "$SDIR"

# The embedding API: a C host driving every entry point (tests/api/apitest.c).
if make -s -C "$ROOT/native" tests/api/apitest > /dev/null 2>&1 && "$ROOT/native/tests/api/apitest" > /tmp/ax_api.txt 2>&1; then
  pass=$((pass + 1)); printf 'OK   embedding API: %s\n' "$(tail -1 /tmp/ax_api.txt)"
else
  fail=$((fail + 1)); printf 'FAIL embedding API\n'; grep -v '^OK' /tmp/ax_api.txt | head -12
fi

# WebAssembly, when a wasm32-wasi toolchain is installed: the command build and the library
# build (through wasm/axiom.mjs) against this binary (tests/api/wasmtest.mjs).
if printf 'int main(void){return 0;}' | ${WASI_CC:-clang} --target=wasm32-wasi -x c -o /dev/null - > /dev/null 2>&1; then
  if make -s -C "$ROOT/native" wasm > /tmp/ax_wasm_build.txt 2>&1 && node "$ROOT/native/tests/api/wasmtest.mjs" > /tmp/ax_wasm.txt 2>&1; then
    pass=$((pass + 1)); printf 'OK   WebAssembly: %s\n' "$(tail -1 /tmp/ax_wasm.txt)"
  else
    fail=$((fail + 1)); printf 'FAIL WebAssembly\n'; cat /tmp/ax_wasm_build.txt /tmp/ax_wasm.txt 2>/dev/null | grep -v '^OK' | head -20
  fi
else
  printf 'SKIP WebAssembly (no wasm32-wasi toolchain: clang with wasi-libc)\n'
fi

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
