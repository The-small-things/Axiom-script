# AxiomScript — native runtime (C11)

A single dependency-free binary that runs AxiomScript programs — scripts and simulations alike.
No Node, no packages, no runtime to install: `cc`, libc, libm, ~600 KB.

```
make
./axiom ../examples/calc.ax -- "2 * (3 + 4) - 10 / 5"    # a script
./axiom ../examples/sim.ax --sim 400 --json              # a simulation, stepped headless
./axiom ../examples/scene.ax                             # a scene, drawn in the terminal
./axiom ../examples/scene.ax --headless 60               # … or to PNG frames in screenshots/
```

It also embeds: a C API (`axiom_api.h`, `libaxiom.a`) and a WebAssembly build that runs the same
programs in Node and in browsers (below).

## What it runs

Everything the JavaScript runtime runs, with the same results:

| | |
|---|---|
| **The language** | `^main`, `^fn`/`^proc`, `^type` records, `~globals`, closures and lambdas, pattern matching with bindings and guards, `^try`/`^catch`/`^fin`/`^throw`, destructuring, comprehensions, pipelines, `^use` imports, big integers, and ~190 standard-library functions |
| **The engine** | `@entities` with `~fields`, `^mix` mixins, one-line blocks and `at` poses; `&physics` (60 Hz fixed step), `&tick(Nhz)`, `&render`, `&on(Event)`; `^event` broadcasts — global, `to #Tag`, `within(r)`, deferred when sent from `&tick`; `&Body3D` gravity, drag, `!force`/`!impulse`; the ground plane; sphere/box/capsule collisions with layers, masks and `Collide` events; timers (`~cd: 0.5s`), tweens and easing, transition chains; `#Tag` reads and cross-entity writes; `?nearest`/`?exists`/`?dist`, the `?>` raycast; `!spawn`/`!despawn`/`!move`; `&Pool`/`&Vec`/`&Map` fields; `$` grid distributions with `~=` and `~>`; NavMesh3D `?path` (single- and multi-layer A*), `?raycast` and dynamic obstacles; `!save`/`!load` with the schema check; `^emit` channels; Vec2/Vec3/Quat/Mat4/Transform values |
| **Output** | `--sim N [--json]` prints exactly main.js's state dump; `--terminal` draws each frame with the same half-block/ASCII encoder as terminal.js — live, steered from the keyboard, when run at a terminal; `--headless N` writes PNGs |
| **Tools** | `--check` (the full static checker, `--json` too; programs it rejects do not run, as in main.js); `--repl` (the same session as `node main.js --repl`); `--json` for scripts, faults and `exit()` included |

### What differs, and why

* **Rendering is new code, not a port.** main.js draws through `render3d.js`, which is not part
  of this code base, so there was nothing to port. `render.c` is a small software rasterizer
  written to the same contract — `!mesh(#Res, color: …)` commands from `&render` blocks, seen
  through the first `&Camera` (`~target`, `~fov`) and lit by the first `&Light` (`~dir`,
  `~intensity`, `~ambient`). A `#Mesh3D` loaded from a `.glb` (a file, `base64("…")` or a
  `glb:` heredoc) is drawn from its own triangles; any other is procedural (a path naming a
  sphere, a plane, or anything else → a box). Textures, skinning, morph targets and animation
  clips are not drawn — models appear in their rest pose. The `.glb` *loader* is a port
  (`glb.c`): the same primitives, vertices, indices, materials and `#X_0, #X_1, …` resources,
  and the same files fail to load. The terminal encoder is a port too: `term.c` produces the
  same bytes as terminal.js.
* A loaded mesh's `vertices` and `indices` are arrays here and typed arrays in the reference:
  the same length, elements and keys, but printing the whole array shows `[…]` rather than
  JavaScript's `{"0": …}`.
* **Audio is not played.** `!play`/`!music` are recorded as in the reference, but the native
  build does not spawn a player process.
* **Particle-filter distributions** draw from the program's seeded generator (`seed(n)`) rather
  than Math.random, so they are reproducible here and random there. `infer: exact`
  distributions use no randomness and match exactly.
* `--json` combined with `--terminal` prints the `--sim` state format.

## Embedding: the C API and WebAssembly

**From C** — link `libaxiom.a` (built by `make`) and include `axiom_api.h`:

```c
axiom *ax = axiom_new();                         // one program: its globals, world and output
axiom_set_output(ax, my_writer, ctx);            // print()/eprint() → (stream 1|2, whole lines)
axiom_define(ax, "fetch_price", my_fn, ctx);     // a host function: JSON arguments in, JSON out
int code = axiom_run(ax, source, "prog.ax");     // check + run ^main → the exit code
char *res = axiom_result(ax);                    // the script's --json object
axiom_release(res);
axiom_free(ax);
```

The rest of the surface: `axiom_check` (`--check --json` without running), `axiom_load` +
`axiom_main`, `axiom_eval` (REPL semantics: statements run, declarations join the session, an
expression returns its value), `axiom_set_input` / `axiom_step` / `axiom_state` (a simulation
driven by the host, state as `--sim --json`), `axiom_render` (a frame as RGBA pixels),
`axiom_error`. Nothing in it exits the host: `exit(n)`, a runtime error and a failed compile are
return values. An instance starts **sandboxed** — no files, no `sh()` — until the host allows
them (`axiom_allow_read`/`axiom_allow_write`/`axiom_allow_exec`, or `axiom_sandbox(ax, 0)`).
Instances are independent (one thread each) and free completely: each parses into its own AST
arena, and freeing the VM breaks the function ↔ scope cycles reference counting cannot.
`tests/api/apitest.c` drives every entry point (`make apitest`).

**WebAssembly** — `make wasm` (clang with the wasi-libc sysroot; on Debian/Ubuntu the `clang`,
`lld`, `wasi-libc` and `libclang-rt-18-dev-wasm32` packages, the last for your clang version)
builds two modules:

```
node wasm/axiom-wasi.js prog.ax [flags]       # axiom.wasm: the command, all flags, files included
```

```js
import { loadAxiom } from './wasm/axiom.mjs';  // axiom-lib.wasm: the C API, for Node and browsers
const axiom = await loadAxiom();
const ax = axiom.create({ onOutput: (stream, text) => log(text) });
ax.define('now_ms', () => Date.now());         // callable from AxiomScript; throwing → ^catch
ax.run(source);                                // → exit code
ax.eval('x = 20'); ax.eval('x * 2').value;     // → "40"
ax.load(world); ax.setInput({ x: 1, jump: true }); ax.step(60); ax.state(); ax.render(480, 360);
ax.free();
```

`axiom.mjs` needs nothing from the host — it answers the runtime's few system calls itself —
and the library build has no file system. `wasm/index.html` is a playground: edit a program,
run it, and steer a world drawn by the software renderer onto a canvas (serve `wasm/` over HTTP
after `make wasm`). WebAssembly has no `setjmp`, so errors unwind as WebAssembly exceptions
(`wasm/sjlj.c` is the runtime half of clang's lowering); a runtime needs exception-handling
support (Node 17+, current browsers).

## Conformance

The JavaScript implementation is the specification, so verification is differential: run each
program on **both** runtimes and require the same result.

```
./difftest.sh        # 86 checks: language, --json, the REPL, the checker, imports, examples,
                     # 25 engine programs, math, number formatting, big integers, .glb loading,
                     # terminal, rendering, the C API, and WebAssembly against this binary
make debug           # ASan + UBSan build; the corpus runs clean under both
```

* **Scripts** — stdout and exit code must be byte-identical (`tests/*.ax`, `^use`, examples).
* **Simulations** — each `tests/engine/*.ax` is stepped 240 frames on both runtimes and the
  final `--sim --json` dumps are compared field by field by `simcmp.js`: every entity, field,
  position, log line and the simulation clock must match exactly; runtime faults must match by
  code, entity, block and line (only the prose of the raw error, which comes from two different
  host languages, may differ). Files written by `!save` are compared too. `examples/sim.ax` runs
  1000 frames, and one program replays a `--input` script of key presses.
* **Math** — 18 `Math` functions × 100 000 inputs, bit-identical to V8 (below).
* **Number formatting** — `String(x)` for 100 000 random doubles plus every power of two and
  the notation boundaries.
* **Big integers** — a seeded program (`tests/bigint/gen.js`) of 400 operand pairs, from the
  32- and 64-bit limb boundaries to 80-digit randoms, through every operator and comparison,
  plus conversions from doubles and parsing edge cases: identical to V8's BigInt.
* **The checker** — `--check --json` on a corpus that trips every diagnostic, on every other
  program here, and on 300 mutants of them: identical but for a parse error's wording.
* **`.glb` loading** — 29 hand-made files (index types, missing attributes, skins, animations,
  out-of-bounds reads, broken containers) and 400 mutants of them: the same geometry, or the
  same failure, from both loaders. Then a model is rendered and must not look like the box
  fallback.
* **Terminal output** — 60 pixel buffers × option combinations through terminal.js and
  `term.c`, byte-identical.
* **The C API** — `tests/api/apitest.c`: 44 checks of every entry point, also run under ASan
  with leak detection (300 instances created and freed).
* **WebAssembly** — `tests/api/wasmtest.mjs` (run by difftest when a wasm32-wasi toolchain is
  installed): the JavaScript API's own checks; every script and engine test through the library
  build; and the command build against this binary on the scripts, every engine test's
  `--sim --json`, the checker corpus and the REPL transcripts — 126 checks, all identical.

### Why the math is its own file

Physics runs sine, square root and `Math.hypot` thousands of times a second, and a difference
in the last bit compounds: two runtimes that disagree by one ulp in `sin` disagree visibly after
a few hundred frames. V8 does not use the platform's libm — it carries fdlibm — and glibc
disagrees with fdlibm in the last bit on 3–47% of inputs depending on the function. So
`jsmath.c` carries the same algorithms, checked against Node on 100 000 inputs per function:

| | glibc vs Node | `jsmath.c` vs Node |
|---|---|---|
| `sin`, `cos`, `tan` | 3–4% differ | identical |
| `exp`, `pow`, `atan2` | 9–20% differ | identical |
| `cbrt` | 47% differ | identical |
| `Math.hypot` (3 args) | C `hypot` differs | identical (V8's compensated sum) |

Two details had to come from V8's source rather than fdlibm: V8's `pow` keeps a different form
of fdlibm's last division, and its `log10` is fdlibm's original rather than FreeBSD's rewrite.

### Divergences testing turned up

Getting to identical turned up real bugs, fixed on whichever side was wrong:

* **the native side** — `0..5` lexed as a malformed number; `%g` reaching for exponent notation
  inside the plain-decimal range; f-string literal parts not decoding escapes; a call not
  skipping a same-named local that held data.
* **the JavaScript side** — `1e21` lexed as `1` with the unit `e21`; `[1] + [2]` producing
  `"12"`; `type({})` reporting `object`; `print` rendering arguments as it evaluated them;
  loading any `#Mesh3D` crashing when `render3d.js` is absent; calling a lambda stored in an
  entity field failing with "field 'f' holds function, which is not callable"; `--json`
  dumping a stored function's syntax tree.
* **both** — JavaScript orders integer-like object keys ahead of string keys, so the native
  dictionary keeps that order as keys are inserted.

## Performance

Measured on this machine against `node main.js` (which includes Node's start-up):

| | native | node | |
|---|---|---|---|
| hello world | **1.6 ms** | 45 ms | 27× |
| `examples/sim.ax`, 20 000 frames | **25 ms** | 182 ms | 7× |
| 40 falling bodies with collisions, 3 000 frames | **11 ms** | 164 ms | 14× |
| 400 bodies (spheres, boxes, capsules), 600 frames | **0.43 s** | 1.1 s | 2.6× |
| `fib(27)` | **0.05 s** | 0.27 s | 5× |
| 3M-iteration `while` loop | **0.21 s** | 0.59 s | 2.8× |
| 200k dictionary updates | **0.06 s** | 0.21 s | 3.4× |

Both runtimes find collision pairs through a uniform grid when there are 24 or more bodies,
and produce exactly what the reference's all-pairs pass produced (see `step_collisions` in
`engine.c` for why that holds); before it, the 400-body row took 3.8 s native and 7.2 s in
Node.

## Design notes

* **Values** are 16 bytes: a tag plus a union. Numbers are doubles, as in the reference.
  Vectors, quaternions, poses and entities are refcounted heap objects, because the reference's
  are mutable objects: `pose.pos.y = 0` changes the vector every holder of it sees, and a port
  with value-semantics vectors would produce different simulations.
* **One evaluation context** (`AxCtx`: the running entity, `dt`, the event being handled, and
  whether this is a function body) decides how a bare name resolves and where an assignment
  lands — the one place the engine and the language meet. Function bodies get locals; entity
  blocks write to the entity, exactly the reference's rule.
* **Faults in a frame block are recorded, not fatal**: each block runs under its own error
  handler, and a fault ends that block, as in the reference.
* **Strings, identifiers and dictionary keys are interned**, so a scope lookup is a pointer hash.
* **Memory is reference counted.** Scopes also live on a stack that a throw unwinds, so a caught
  error releases the frames it jumped past (a loop catching 200 000 errors stays at 10 MB). A
  function declared at the top level refers to the scope that holds it; freeing a VM clears
  its globals first, which breaks that cycle. Other cycles (a closure stored in its own scope,
  an entity field pointing back at its entity) are not collected.
* **Control flow** is a return code; **errors** use `setjmp`/`longjmp`.

## Why C, and not C++ or assembly

**C11** — the runtime is a value model, a tree walker, a library and a physics loop, whose cost
is memory layout and branches, which C exposes directly; and it gives a stable ABI and a small
binary any host can embed. **Not C++** — nothing here wants templates, RAII or exceptions, and
the error path is `setjmp`/`longjmp` because that is what `^try` maps onto. **Not assembly** —
the compiler schedules a dispatch loop better than hand-written code, and the result would be
tied to one architecture; the place assembly could pay is a measured hot kernel (the
rasterizer's span loop), and nothing is there yet.

## Layout

| File | |
|---|---|
| `axiom.h` | the whole public surface: values, tokens, AST, VM, engine types |
| `value.c` | refcounting, strings, arrays, insertion-ordered dictionaries, scopes |
| `lexer.c` | indentation, sigils, f-strings, bracket continuation |
| `parser.c` | recursive descent into an arena-allocated AST, engine declarations included |
| `interp.c` | evaluation, calls, patterns, errors, the assignment rule |
| `stdlib.c` | the standard library and method dispatch |
| `engine.c` | entities, the frame loop, physics, collisions, events, tweens, actions, queries, `--json` |
| `host.c` | `&Pool`, `&Vec`, `&Map` fields |
| `infer.c` | `$` distributions, NavMesh3D, `!save`/`!load` |
| `jsmath.c` | V8's fdlibm, so `Math.*` agrees to the bit |
| `bigint.c` | `big(x)`: arbitrary-precision integers with JavaScript's BigInt rules |
| `glb.c` | binary glTF meshes (interpreter.js parseGLBMulti) |
| `check.c`, `checknames.h` | the static checker (checker.js); the tables are generated by `tools/gen_checknames.js` |
| `fmt.c` | f-string format specs |
| `repl.c` | `--repl` |
| `kbd.c` | live keyboard input for terminal mode |
| `render.c` | software rasterizer and PNG writer (new code) |
| `term.c` | terminal output, ported from terminal.js |
| `imports.c` | `^use` resolution |
| `axiom_api.h`, `api.c` | the embedding API |
| `main.c` | the `axiom` command |
| `wasm/` | WebAssembly: `sjlj.c` and `include/setjmp.h` (setjmp/longjmp on WebAssembly exceptions), `api_js.c` (the JavaScript entry points), `axiom.mjs` (the JavaScript API), `axiom-wasi.js` (runs the command build under Node), `index.html` (the playground) |
| `difftest.sh`, `simcmp.js`, `checkcmp.js` | differential tests against the JavaScript runtime |
| `tests/api/` | the C API's test host and the WebAssembly comparison |
