# AxiomScript Standard Library (v0.9.0)

Every name below is callable from any block, `^fn`, `^proc`, or `^main` — there is nothing to
import. Functions are total where they sensibly can be: an empty input returns a neutral value
(`0`, `[]`, `null`) rather than raising, so a generation mistake costs a wrong number rather
than a crashed run. The few that do raise (`check`, `check_eq`, `exit`, a sandbox denial, a
malformed regex) raise catchable errors — wrap them in `^try:` / `^catch e:`.

Conventions:

* a callback argument comes **last**, so a lambda closes the call: `sort_by(xs, \p: p.age)`
* anywhere a callback is taken, a **field-name string** works too: `sort_by(xs, "age")`
* predicates are `is_*`, conversions are `to_*` / `from_*`, two words are `snake_case`
* `xs` accepts an array, a string (characters), a range, or a dict (values)
* **any function is also a method** on its first argument (v0.9.3): where a value has no method
  of that name, `x.f(a)` is `f(x, a)` — `xs.sorted().uniq()`, `text.lines().len()`,
  `p.norm1()` for a `^fn norm1(p)`. Ranges take every array method: `(0..n).map(f)`

---

## Numbers and mathematics

| Call | Result |
|---|---|
| `abs(x)` `sign(x)` `floor(x)` `ceil(x)` `round(x)` `trunc(x)` | the usual rounding and sign primitives |
| `sqrt(x)` `cbrt(x)` `pow(b, e)` `exp(x)` | roots and powers (`**` is the operator form) |
| `log(x)` `ln(x)` `log2(x)` `log10(x)` `log1p(x)` | logarithms (`log` and `ln` are both natural) |
| `sin(x)` `cos(x)` `tan(x)` `asin(x)` `acos(x)` `atan(x)` `atan2(y, x)` | trigonometry, radians |
| `sinh(x)` `cosh(x)` `tanh(x)` | hyperbolic |
| `PI` `TAU` `E` `inf()` `nan()` | constants (`PI` and friends are called without parens) |
| `min(a, b, …)` `max(a, b, …)` | variadic; a single array argument is also accepted: `min(xs)` |
| `clamp(x, lo, hi)` `clamp01(x)` `wrap(x, lo, hi)` `fract(x)` | range handling |
| `lerp(a, b, t)` `inv_lerp(a, b, v)` `map_range(v, inLo, inHi, outLo, outHi)` `smoothstep(lo, hi, x)` | interpolation |
| `mod(a, b)` | mathematical modulo — always non-negative for a positive divisor (unlike `%`) |
| `divmod(a, b)` | `[quotient, remainder]`; pairs with `q, r = divmod(n, d)` |
| `gcd(a, b)` `lcm(a, b)` `isqrt(n)` | integer maths |
| `fact(n)` `comb(n, k)` `perm(n, k)` | combinatorics |
| `is_prime(n)` `primes(limit)` | primality and a sieve |
| `round_to(x, step)` `to_fixed(x, digits)` | quantising and fixed-point formatting (a string) |
| `hypot(a, b)` `dist(a, b)` | distance — `dist` also takes two vectors |
| `deg2rad(d)` `rad2deg(r)` | angle conversion |
| `num(v)` `int(s)` `float(s)` `parse_int(s, radix)` | conversion to number (`num` returns 0 on failure, `int`/`float` NaN — they are `parseInt`/`parseFloat` — and `parse_int` null) |
| `to_hex(n)` `to_bin(n)` `to_base(n, base)` | number → string |
| `band(a, b)` `bor(a, b)` `bxor(a, b)` `bnot(x)` `shl(a, n)` `shr(a, n)` | bitwise (the `&` sigil is reserved, so these are functions) |
| `big(v)` `is_big(v)` | arbitrary-precision integers (JavaScript's BigInt): `+ - * / % **` between two of them (`/` truncates), exact comparison with numbers; mixing one with a number in arithmetic, or passing one to a `Math` function, is an error — convert with `num(b)` / `big(n)` |
| `is_nan(x)` `is_int(x)` `is_finite(x)` | numeric predicates |

## Randomness

Seeded, so a run is reproducible: call `seed(n)` and the whole sequence repeats.

| Call | Result |
|---|---|
| `seed(n)` | reseed the generator; returns `n` |
| `random()` | float in `[0, 1)` |
| `random_range(lo, hi)` / `randomRange(lo, hi)` | float in `[lo, hi)` |
| `random_int(lo, hi)` / `randomInt(lo, hi)` | integer in `[lo, hi]`, inclusive |
| `gauss(mu, sigma)` | normally distributed sample |
| `pick(xs)` | one random element (null if empty) |
| `shuffle(xs)` | a shuffled copy |
| `uuid()` | a v4-shaped identifier string |

## Collections

| Call | Result |
|---|---|
| `len(v)` | length of an array, string, dict, record, range, pool, or buffer |
| `range(n)` `range(lo, hi)` `range(lo, hi, step)` | a lazy integer range; `step` may be negative |
| `sum(xs, key?)` `prod(xs, key?)` | reductions, optionally over a key |
| `mean(xs, key?)` `median(xs)` `mode(xs)` `variance(xs)` `stdev(xs)` | descriptive statistics |
| `sorted(xs, key_or_cmp?)` | sorted copy; a 1-argument callback is a key, a 2-argument one a comparator |
| `sort(xs, key_or_cmp?)` | the same as `sorted` (a copy; the input is not changed) |
| `sort_by(xs, key)` | sorted copy by key |
| `min_by(xs, key)` `max_by(xs, key)` | the element with the smallest/largest key |
| `group_by(xs, key)` | dict of key → array of members |
| `count_by(xs, key)` | dict of key → count |
| `count(xs, value_or_pred?)` | number of matches (or the length, with no second argument) |
| `partition(xs, pred)` | `[matching, rest]` |
| `uniq(xs, key?)` | duplicates removed, order preserved (values that display alike are duplicates: `1` and `"1"`) |
| `zip(a, b, …)` `unzip(pairs)` | transpose lists into tuples and back |
| `enumerate(xs)` | `[[0, x0], [1, x1], …]` — pairs with `*i, v in enumerate(xs):` |
| `chunk(xs, size)` `windows(xs, size)` | fixed-size blocks / sliding windows |
| `flatten(xs, depth?)` `reversed(xs)` | shape changes, non-mutating |
| `take(xs, n)` `drop(xs, n)` `first(xs, dflt?)` `last(xs, dflt?)` | slicing |
| `any(xs, pred?)` `all(xs, pred?)` | quantifiers |
| `find(xs, pred)` `find_index(xs, pred)` | search |
| `map(xs, fn)` `filter(xs, pred)` `reduce(xs, fn, init?)` `each(xs, fn)` | function forms of the array methods, for `\|>` chains |
| `union(a, b)` `intersect(a, b)` `difference(a, b)` | set algebra over plain arrays |
| `is_empty(v)` | true for null, `""`, `[]`, `{}` |
| `grid(rows, cols, fill_or_fn)` `transpose(m)` | 2D arrays for boards, tables, and DP |

## Dicts and records

| Call | Result |
|---|---|
| `keys(d)` `values(d)` `items(d)` | the three views; `items` yields `[key, value]` pairs |
| `dict(pairs)` | build a dict from `[key, value]` pairs (the inverse of `items`) |
| `merge(a, b, …)` | shallow merge into a new dict |
| `pick_keys(d, ks)` `omit_keys(d, ks)` | projections |
| `invert(d)` | values become keys |
| `has_key(d, k)` | membership by key (`k in d` is the operator form) |
| `clone(v)` `deep_eq(a, b)` | deep copy / deep comparison (as JSON text, so key order counts and `NaN` equals `NaN`) |
| `type(v)` | `"number"`, `"string"`, `"array"`, `"fn"`, `"atom"`, `"vec3"`, `"range"`, `"bigint"`, a `^type` name, … |
| `is_null(v)` `is_number(v)` `is_string(v)` `is_array(v)` `is_dict(v)` `is_bool(v)` `is_fn(v)` | type predicates |

## Strings, encoding, regular expressions

| Call | Result |
|---|---|
| `str(v)` | anything → string |
| `lines(s)` `words(s)` `chars(s)` | the three ways to split text |
| `capitalize(s)` `title(s)` `reverse_str(s)` | case and order |
| `ord(c)` `chr(n)` | character ↔ code point |
| `hash(s)` | stable 32-bit FNV-1a hash |
| `b64_encode(s)` `b64_decode(s)` | base64 |
| `to_json(v, pretty?)` `from_json(s)` | JSON (`json_stringify` / `json_parse` are the older names) |
| `re_test(s, pat, flags?)` | does the pattern match |
| `re_match(s, pat, flags?)` | `{match, index, groups}` or null |
| `re_all(s, pat, flags?)` | every match, as an array of the same records |
| `re_sub(s, pat, repl, flags?)` | replace all matches |
| `re_split(s, pat, flags?)` | split on a pattern |

String **methods** (called on a value) cover the rest: `.split(sep)` `.replace(a, b)`
`.replace_all(a, b)` `.trim()` `.trim_start()` `.trim_end()` `.upper()` `.lower()`
`.startsWith(p)` `.endsWith(p)` `.includes(p)` `.contains(p)` `.indexOf(p)` `.repeat(n)`
`.slice(a, b?)` `.padStart(n, c?)` `.padEnd(n, c?)` `.charAt(i)` `.charCodeAt(i)` `.count(sub)`
`.to_int(radix?)` `.to_num()` `.len()` `.reverse()` `.is_empty()` `.lines()` `.words()` `.chars()`.

## Time

| Call | Result |
|---|---|
| `now()` | milliseconds since the epoch |
| `time()` | seconds since the epoch, fractional |
| `clock()` | high-resolution seconds, for measuring durations |
| `date_iso(ms?)` | ISO-8601 string |
| `sleep(seconds)` | blocking pause — for scripts, never for a frame block |

## Files, process, environment

Gated in sandbox mode: reads need `--allow-read PATH`, writes need `--allow-write PATH`, and
`sh` needs `--allow-exec`. A denial raises `AX-SANDBOX-001`, which `^catch` can handle.

| Call | Result |
|---|---|
| `read(path, dflt?)` | file contents as text, or `dflt` (default null) |
| `read_lines(path)` `read_json(path, dflt?)` | convenience readers |
| `write(path, s)` `write_json(path, v, pretty?)` `append(path, s)` | writers; parent directories are created |
| `file_exists(path)` `is_dir(path)` `ls(dir)` `mkdir(dir)` `rm(path)` | filesystem inspection and management |
| `path_join(a, b, …)` | platform-correct path joining |
| `input(prompt?)` | one line from stdin (null at end of input) |
| `read_stdin()` | all of stdin as one string |
| `args()` | the program's arguments (everything after `--` on the command line) |
| `env(name, dflt?)` | environment variable |
| `print(…)` `eprint(…)` | write a line to stdout / stderr |
| `exit(code)` | stop the program with an exit code |
| `sh(cmd)` | run a shell command, returns `{out, code}` |

## Functions and testing

| Call | Result |
|---|---|
| `apply(fn, args_array)` | call with an argument list |
| `partial(fn, a, b, …)` | bind leading arguments |
| `compose(f, g, …)` | right-to-left composition |
| `memo(fn)` | cache results by argument — turns exponential recursion linear |
| `check(cond, msg?)` | raise unless the condition holds |
| `check_eq(a, b, msg?)` | raise unless the two values are equal |

## Engine intrinsics (3D / simulation)

Present in every program, meaningful when entities are used: `v2` `v3` `v3x` `v3y` `v3z`
`v3xz` `v2dir` `q` `euler` `m4` `persp` `ortho` `lookat` `aabb` `sphere` `box` `capsule` `bar`
`vision_cells` `cell_to_world`, and the easing atoms `linear` `in` `out` `inout` `bounce`
`elastic`. See `GRAMMAR.md` and `CHANGES.md` for the entity, block, and rendering model.
