# AxiomScript — native runtime (C11)

A single dependency-free binary that runs AxiomScript programs. No Node, no packages, no
runtime to install: `cc`, libc, libm, 186 KB.

```
make
./axiom ../examples/calc.ax -- "2 * (3 + 4) - 10 / 5"
```

## Why C, and not C++ or assembly

**C11.** The runtime is a value model, a tree walker, and a standard library — code whose cost
is dominated by memory layout and branch behaviour, both of which C exposes directly. It also
gives the thing that matters most for a language meant to be embedded: a stable ABI, a 186 KB
static-ish binary, and a header any host language can bind to without a shim.

**Not C++.** Nothing here wants templates, RAII or exceptions. The value type is a tagged union
that must stay exactly 16 bytes; the error path is `setjmp`/`longjmp` because that is what an
interpreter's `^try` maps onto; ownership is one convention (a function that returns a value
returns a reference you own) applied uniformly, which a reader can check line by line. C++ would
add build complexity and a larger runtime in exchange for abstractions this design does not use.

**Not assembly.** A hand-written assembly interpreter would be slower than this one, not faster:
the compiler's register allocation and instruction scheduling beat hand-written code on a
branch-heavy dispatch loop, and the result would be tied to one architecture and unmaintainable
by anyone, model or human. The place where assembly could genuinely pay is a specific hot kernel
— a rasterizer span loop, a checksum — measured first, written as intrinsics second, and
assembly only if the intrinsics fall short. Nothing in this runtime is there yet.

## What it runs, and what it does not

| | |
|---|---|
| **Runs** | the whole *language*: `^main`, `^fn`/`^proc`, `^type` records, `~globals`, closures and lambdas, pattern matching with bindings and guards, `^try`/`^catch`/`^fin`/`^throw`, destructuring, comprehensions, pipelines, `in`, default parameters, f-strings, and ~190 standard-library functions (statistics, collections, dicts, strings, regex, JSON, time, files, process, seeded randomness, functional helpers) |
| **Does not run** | the *engine*: `@entities`, `&physics`/`&render`/`&tick`/`&on`, `^event`/`^mix`/`^mat`, `#resources`, queries (`?nearest`), distributions, pools, vectors and matrices, the rasterizer, navmesh and audio. Those stay in the JavaScript runtime (`node main.js world.ax`). |

A program that uses an engine construct is **refused by the parser** with a message naming the
JavaScript runtime — never half-run. The split is deliberate: the language is what programs are
written in, the engine is one host for them.

`^use "lib.ax"` imports work, with the reference's rules: resolved relative to the importing
file, `.ax` inferred, include-once (so diamonds and cycles terminate), and the importing file
winning a name clash.

Also absent: arbitrary-precision `big()`, and `sh()` argument escaping beyond what the shell
already does.

## Conformance

The JavaScript implementation is the specification while the port matures, so the whole
verification story is differential: run each program on **both** runtimes and require identical
stdout and identical exit codes.

```
./difftest.sh        # 10 conformance programs, imports, and 6 example invocations
make debug           # ASan + UBSan build; the corpus runs clean under both
```

`tests/*.ax` cover scope and recursion, closures, patterns, errors, collections, strings and
regex, numbers and seeded randomness, control flow, higher-order functions, files, and the edge
cases where two implementations usually drift (empty inputs, `null`, out-of-range indices,
negative indices, number formatting, key order).

Getting to byte-identical turned up real divergences, each fixed on whichever side was wrong:

* **the native side** — `0..5` lexed as a malformed number (the scanner swallowed both dots);
  `%g` reaching for exponent notation inside the plain-decimal range; f-string literal parts
  not decoding escapes; a call not skipping a same-named local that held data.
* **the JavaScript side** — `1e21` lexed as `1` with the unit `e21` and `1.5e-7` as `1.5 - e7`
  (both silently wrong); `[1] + [2]` producing `"12"`; `type({})` reporting `object` where
  STDLIB.md says `dict`; `print` rendering each argument as it evaluated it, so a later
  argument's side effect could change an earlier one's output.
* **both** — JavaScript orders integer-like object keys ahead of string keys, which is
  observable in anything that prints a `group_by` result, so the native dictionary keeps its
  entries in that order as they are inserted rather than sorting on iteration.

## Performance

Measured on this machine against `node main.js` (which includes Node's start-up):

| | native | node | |
|---|---|---|---|
| hello world | **1.6 ms** | 45 ms | 27× — the number that matters for a CLI |
| `fib(27)` | **0.11 s** | 0.23 s | 2.1× |
| 3M-iteration loop | **0.18 s** | 0.49 s | 2.7× |
| 200k dictionary updates | **0.10 s** | 0.18 s | 1.8× |

The compute ratios are modest because V8 JITs the reference interpreter well. The start-up ratio
is not modest, and for the thing this language is now used for — scripts, tools, pipelines —
start-up *is* the runtime.

Next, if more is wanted: resolve identifiers to (depth, slot) pairs at parse time instead of
hashing names, then compile the AST to a threaded-dispatch bytecode. Both are mechanical; both
cost readability; neither is justified by a measurement yet.

## Design notes

* **Values** are 16 bytes: a tag plus a union. Numbers are doubles, exactly as in the reference,
  so arithmetic agrees bit for bit rather than approximately.
* **Strings, identifiers and dictionary keys are interned**, so a scope lookup is a pointer hash
  and a pointer comparison.
* **Memory is reference counted**, with one convention: returning a value transfers a reference,
  taking one borrows it. Reference *cycles* — a closure capturing the scope that holds it — are
  not collected; a short-lived process exits long before that matters, and a tracing collector
  over C-stack roots would be a large amount of machinery for no benefit here. The AST lives in
  a bump arena freed in one call.
* **Control flow** (break/continue/return) is a return code because it is common; **errors** use
  `setjmp`/`longjmp` because they are not.
* **Loop frames are reused** when a body creates no closure, which is the same optimisation the
  reference makes, for the same reason.

## Layout

| File | |
|---|---|
| `axiom.h` | the whole public surface: values, tokens, AST, VM |
| `value.c` | refcounting, strings, arrays, insertion-ordered dictionaries, scopes |
| `lexer.c` | indentation, sigils, f-strings, bracket continuation |
| `parser.c` | recursive descent into an arena-allocated AST |
| `interp.c` | evaluation, calls, patterns, errors |
| `stdlib.c` | the standard library and method dispatch |
| `main.c` | the `axiom` command |
| `difftest.sh` | differential test against the JavaScript runtime |
