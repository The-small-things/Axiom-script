# AxiomScript

**A general-purpose language written for language models, not for people.**

Version 0.9.0 · reference implementation in Node (no dependencies) · MIT

AxiomScript optimises for one measurable thing: **how many tokens it costs a model to write a
correct program**. Every syntactic decision follows from that. Sigils instead of keywords
(`^fn` not `function`), significant indentation instead of braces, an expression body instead
of a `return` statement, one operator per idea (`??`, `?:`, `|>`, `in`, `?*`), and a standard
library whose names can be guessed rather than looked up.

Through v0.8 it could only describe **game worlds**: entities, frame blocks, a 3D renderer.
v0.9 keeps all of that and makes it a language you can write **anything** in — a CLI tool, a
parser, a solver, a simulation, a data pipeline, a game — with the frame loop now opt-in.

```axiom
^fn fib(n) = n < 2 ? n : fib(n-1) + fib(n-2)

^main:
  print(fib(20))                       // 6765
  print([1,2,3,4] |> filter(\n: n % 2 == 0) |> sum)   // 6
```

```
$ node main.js hello.ax
```

---

## Why it looks like this

| Decision | Cost it removes |
|---|---|
| `^fn f(x) = expr` | no `function`, no `return`, no braces — ~6 tokens per function |
| `\x: x * 2` | an inline callback costs 2 tokens of overhead instead of a named declaration (~8) |
| `xs \|> map(f) \|> sum` | no nesting to balance, so no mismatched-paren retries |
| `?* value:` | a 6-way dispatch costs ~24 tokens instead of ~48 as an if/elif ladder |
| `a ?? b`, `x ??= v`, `x += 1`, `++x` | each replaces a 4–7 token conditional |
| `~NAME: value` globals | no config dict threaded through every call |
| a guessable stdlib (`sum`, `uniq`, `group_by`, `re_sub`, `read_json`) | reimplementation, and the bugs that come with it |
| `Add(l, r):` patterns | a test plus two field reads per branch, in every tree-shaped program |
| diagnostics that name the fix | a whole generate-run-fail-retry cycle, the most expensive thing a model can do |

The checker's messages are written **for the model**: every diagnostic carries a
`message_for_agent` explaining what to change, not just what went wrong. It catches, before
anything runs: calls with the wrong number of arguments, fields a record type does not declare,
mistyped names (which the language would otherwise turn silently into atoms), literals that
contradict a declared type, undefined functions, unreachable code, and missing event fields.

---

## Running a program

```
node main.js prog.ax                 # run it (script if it has ^main, simulation if it has entities)
node main.js prog.ax -- a b c        # everything after -- is available as args()
node main.js prog.ax --run           # force script mode: run ^main and exit
node main.js --help                  # every flag, with one line each
node main.js prog.ax --check         # parse and check only; exit 1 if anything blocks
node main.js prog.ax --json          # machine-readable result, log, and diagnostics
node main.js --eval '^main: print(1+1)'
node main.js --repl                  # a session: statements run as typed, expressions echo
node main.js world.ax --sim 120      # step a simulation 120 frames headless, no renderer
node main.js world.ax --terminal     # render a 3D world as Unicode blocks in the terminal
```

Sandboxing: `--sandbox` denies file writes and subprocesses; open holes explicitly with
`--allow-read PATH`, `--allow-write PATH`, `--allow-exec`, `--allow-net`.

`--json` always prints one object, `{main_result, log, diagnostics, exit_code}` — also when
`^main` faults (the diagnostic names the statement's line) or calls `exit(n)`. Without it, an
uncaught error prints `prog.ax:LINE: runtime error [CODE]: message`. `exit(n)` ends the program
wherever it is called and cannot be caught.

The REPL reads a program a chunk at a time, from a terminal or a pipe: a complete line runs at
once (so a harness can write a line and read the answer), a block (a line ending in `:`) ends at
a blank line, a bare expression prints its value (strings quoted), declarations (`^fn`, `^type`,
`~G:`, `@Entity`, `^use`) join the session, and `:step N` advances the simulation N frames.
Errors print `error [CODE]: message` to stderr and the session continues.

```
$ printf 'x = 6\n^fn f(n) = n * x\nf(7)\nf"{f(7):>6.1f}"\n' | node main.js --repl
42
"  42.0"
```

---

## The language in one page

### Entry point and structure

```axiom
~MAX: 100                  // program global (constant or shared table)
^use "lib.ax"              // import every declaration of another file (include-once)

^type Point: x, y          // a record shape — `Point(1, 2)` or `Point(x: 1, y: 2)`
^fn area(w, h = 1) = w * h // function, expression body, default parameter
^proc log_it(msg):         // procedure: statements, no implied return
  print(msg)

^main(argv):               // the entry point; `^return n` sets the exit code
  print(area(3))
```

### Values

```axiom
n = 42          f = 3.5        s = "text"      b = true       nothing = null
xs = [1, 2, 3]                 d = {a: 1, "k-2": 2, [key]: 3}
p = Point(1, 2)                atom_value = idle        // bare name = symbol
big_n = big("9007199254740993")                          // arbitrary precision
v = v3(1, 2, 3)                two = 2v                  // engine vector types
```

Arrays, dicts, and records compare **structurally** (`[1,2] == [1,2]` is true).

### Control flow

```axiom
?hp <= 0:                  // if        (`if hp <= 0:` also works)
  state = dead
?!shielded:                // if not
  hp -= 10
?!:                        // else
  hp -= 1

*i in 0..10:               // for over a range
*k, v in items(d):         // for with destructuring
*hp > 0:                   // while
  ~break                   // ~continue likewise

?* cmd:                    // match: arms are values, `_` is the default
  "add", "plus": r = a + b
  "neg": r = -a
  _: ^throw f"unknown {cmd}"

?* node:                   // patterns take a value apart and name the pieces
  Add(l, r): v = ev(l) + ev(r)      // record pattern — binds l and r
  Num(v) if v > 0: acc += v         // guard, sees the bindings
  [x, y]: v = x + y                 // array pattern
  {name, age}: greet(name, age)     // dict pattern — binds by key
  n if n > 10: v = n                // guarded capture
  _: ^throw "?"
```

### Functions and data flow

```axiom
double = \x: x * 2              // lambda (`\x => x * 2` is the same)
pairs = xs.map(\x: [x, double(x)])
adults = people.filter(\p: p.age >= 18).sort_by("age")
total  = orders |> map(\o: o.price) |> sum
q, r   = divmod(17, 5)          // destructuring assignment
apply_twice = \f, v: f(f(v))    // functions are values: pass, store, return
top3 = scores.sorted().reverse().take(3)   // any function is a method on its first argument
```

### Errors

```axiom
^try:
  cfg = from_json(read("config.json", "{}"))
  check(has_key(cfg, "port"), "config needs a port")
^catch e:
  eprint(f"bad config: {e.msg} ({e.code})")
  cfg = {port: 8080}
^fin:
  log_it("config loaded")
```

`^throw` takes any value; `^catch e` binds `{msg, code, value}`. Runtime faults (bad index,
unknown method, sandbox denial) are catchable through the same handler.

### Strings

```axiom
name = "world"
greet = f"hello {name}, {1 + 1} exclamations"   // f-string ($"..." is the same)
row = f"{item:<12}{qty:>4}{price:>9.2f}{share:>7.1%}"  // format specs: [[fill]align][sign][0][width][,][.prec][type]
block = """
  multi-line text, dedented
"""
```

### Simulation (the game half, unchanged)

```axiom
@Player &Body3D
  ~hp: 100
  ~speed: 6
  &physics:                       // 60 Hz, allocation-free
    pose.pos += input.move * speed * dt
  &tick(10hz):                    // slow logic
    ?hp <= 0: ^Died(who: self)
  &on(Died):                      // event handler
    !play("die.wav")
```

`^main` and entities can coexist: `^main` runs once as setup, then the frame loop starts.

---

## What changed in v0.9 (and why it mattered)

| Before | Now |
|---|---|
| every program needed an entity and a frame loop | `^main:` runs a program, no world required |
| assignments wrote to the entity, so recursion clobbered its own locals | real lexical scope; recursion, shadowing, and entity-free functions work |
| callbacks had to be declared `^fn`s — anything else made `.map` silently return a copy | lambdas and closures; every higher-order call takes any callable |
| a failure aborted the rest of the block | `^try` / `^catch` / `^fin` / `^throw` |
| one file per program | `^use "lib.ax"` |
| loops capped at 10 000 iterations everywhere | uncapped in functions and scripts; capped only in frame blocks |
| 30-odd helpers | 200+ standard-library functions: statistics, regex, files, JSON, time, encoding, seeded RNG |
| the runtime always loaded the 3D renderer | the renderer loads only when a program declares visual resources |
| a multi-line array or dict was a syntax error | brackets continue across lines |

Full detail in [`CHANGES.md`](CHANGES.md).

---

## Two runtimes

| | `node main.js` (JavaScript) | `native/axiom` (C11) |
|---|---|---|
| The language | complete | complete |
| The engine — entities, blocks, events, physics, collisions, navmesh, save/load | complete | complete, same numbers to the bit |
| Rendering | needs `render3d.js` (not in this repository) | built-in software rasterizer → terminal or PNG |
| Start-up | ~45 ms | ~1.6 ms |
| Simulations (`--sim`) | baseline | 1.7–14× faster |
| Compute (fib, loops, dictionaries) | baseline | 1.8–2.7× faster |
| Install | Node 18+ | `make`; one ~400 KB binary, libc and libm only |

```
cd native && make
./axiom ../examples/calc.ax -- "2 * (3 + 4) - 10 / 5"
./axiom ../examples/sim.ax --sim 400 --json      # a simulation, stepped headless
./axiom ../examples/scene.ax                     # drawn in the terminal
```

The two are kept identical by differential testing: `native/difftest.sh` runs every conformance
program, example and engine test on both runtimes and compares stdout, exit codes and the final
simulation state field by field. Both compute `Math.*` with the same fdlibm algorithms, so a
simulation's numbers agree to the last bit, not approximately. See
[`native/README.md`](native/README.md) for the details and the divergences testing turned up.

## Documentation

* [`STDLIB.md`](STDLIB.md) — every standard-library function, grouped
* [`GRAMMAR.md`](GRAMMAR.md) — the complete EBNF grammar and sigil disambiguation rules
* [`CHANGES.md`](CHANGES.md) — version history
* [`examples/`](examples) — worked programs, each runnable and covered by the test suite:
  `fizzbuzz.ax`, `stats.ax`, `wordcount.ax`, `life.ax`, `calc.ax` (a complete expression
  interpreter written in AxiomScript), `sim.ax` (entities + script in one program)

## Tests

```
npm run test:lang             # both suites below
node test_v090_general.js     # v0.9.0 language, library, examples, documentation coverage
node test_v0817_features.js   # v0.8.17 regression suite
```

`npm test` runs the repository's full runner (`run_all_tests.js`), which picks up
`test_v090_general.js` along with the rest of the `test_*.js` suites.

## Implementation

| File | Role |
|---|---|
| `lexer.js` | indentation- and sigil-aware tokenizer |
| `parser.js` | recursive-descent parser with error recovery (reports every error in one pass) |
| `checker.js` | static diagnostics and module resolution |
| `interpreter.js` | evaluator, scopes, closures, entity/frame runtime |
| `stdlib.js` | the standard library |
| `main.js` | CLI: script mode, headless simulation, terminal and window rendering |
| `render3d.js`, `terminal.js`, `input_gamepad.js` | the optional rendering and input backends |
| `native/` | the C11 runtime: same language, no Node, ~1.6 ms start-up |

The renderer is optional: with `render3d.js` absent, scripts, functions, checks, and `--sim`
all still run.
