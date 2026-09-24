# AxiomScript — native runtime (C11)

A single dependency-free binary that runs AxiomScript programs — scripts and simulations alike.
No Node, no packages, no runtime to install: `cc`, libc, libm, ~400 KB.

```
make
./axiom ../examples/calc.ax -- "2 * (3 + 4) - 10 / 5"    # a script
./axiom ../examples/sim.ax --sim 400 --json              # a simulation, stepped headless
./axiom ../examples/scene.ax                             # a scene, drawn in the terminal
./axiom ../examples/scene.ax --headless 60               # … or to PNG frames in screenshots/
```

## What it runs

Everything the JavaScript runtime runs, with the same results:

| | |
|---|---|
| **The language** | `^main`, `^fn`/`^proc`, `^type` records, `~globals`, closures and lambdas, pattern matching with bindings and guards, `^try`/`^catch`/`^fin`/`^throw`, destructuring, comprehensions, pipelines, `^use` imports, and ~190 standard-library functions |
| **The engine** | `@entities` with `~fields`, `^mix` mixins, one-line blocks and `at` poses; `&physics` (60 Hz fixed step), `&tick(Nhz)`, `&render`, `&on(Event)`; `^event` broadcasts — global, `to #Tag`, `within(r)`, deferred when sent from `&tick`; `&Body3D` gravity, drag, `!force`/`!impulse`; the ground plane; sphere/box/capsule collisions with layers, masks and `Collide` events; timers (`~cd: 0.5s`), tweens and easing, transition chains; `#Tag` reads and cross-entity writes; `?nearest`/`?exists`/`?dist`, the `?>` raycast; `!spawn`/`!despawn`/`!move`; `&Pool`/`&Vec`/`&Map` fields; `$` grid distributions with `~=` and `~>`; NavMesh3D `?path` (single- and multi-layer A*), `?raycast` and dynamic obstacles; `!save`/`!load` with the schema check; `^emit` channels; Vec2/Vec3/Quat/Mat4/Transform values |
| **Output** | `--sim N [--json]` prints exactly main.js's state dump; `--terminal` draws each frame with the same half-block/ASCII encoder as terminal.js — live, steered from the keyboard, when run at a terminal; `--headless N` writes PNGs |
| **Tools** | `--repl` (the same session as `node main.js --repl`); `--json` for scripts, faults and `exit()` included |

### What differs, and why

* **Rendering is new code, not a port.** main.js draws through `render3d.js`, which is not part
  of this code base, so there was nothing to port. `render.c` is a small software rasterizer
  written to the same contract — `!mesh(#Res, color: …)` commands from `&render` blocks, seen
  through the first `&Camera` (`~target`, `~fov`) and lit by the first `&Light` (`~dir`,
  `~intensity`, `~ambient`). Meshes are procedural (a `#Mesh3D` path naming a sphere, a plane,
  or anything else → a box); `.glb` geometry, textures, skinning and animation clips are not
  drawn. The terminal encoder *is* a port: `term.c` produces the same bytes as terminal.js.
* **Audio is not played.** `!play`/`!music` are recorded as in the reference, but the native
  build does not spawn a player process.
* **Particle-filter distributions** draw from the program's seeded generator (`seed(n)`) rather
  than Math.random, so they are reproducible here and random there. `infer: exact`
  distributions use no randomness and match exactly.
* `--json` combined with `--terminal` prints the `--sim` state format.

## Conformance

The JavaScript implementation is the specification, so verification is differential: run each
program on **both** runtimes and require the same result.

```
./difftest.sh        # 39 checks: language, imports, examples, 17 engine programs, math, terminal
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
* **Terminal output** — 60 pixel buffers × option combinations through terminal.js and
  `term.c`, byte-identical.

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
| 60 bouncing bodies, all-pairs collisions, 2 000 frames | **0.31 s** | 0.53 s | 1.7× |
| `fib(27)` | **0.11 s** | 0.23 s | 2.1× |
| 3M-iteration loop | **0.18 s** | 0.49 s | 2.7× |

Collision detection is all-pairs, as in the reference; with many bodies that — not the
interpreter — is the cost, which is why the last simulation row gains least.

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
* **Memory is reference counted.** Cycles (a closure capturing its own scope, an entity field
  pointing at an entity) are not collected; a process exits long before that matters.
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
| `render.c` | software rasterizer and PNG writer (new code) |
| `term.c` | terminal output, ported from terminal.js |
| `main.c` | the `axiom` command |
| `difftest.sh`, `simcmp.js` | differential tests against the JavaScript runtime |
