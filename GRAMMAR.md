# AxiomScript v0.9.0 — Formal Grammar (EBNF)

> **v0.9.0 additions** (all marked `v0.9.0` below): the `^main` entry point, `^use` imports,
> top-level `~NAME: value` globals, lambdas (`\x: expr`), the pipeline operator `|>`,
> `^try`/`^catch`/`^fin`/`^throw`, the `?*` match statement, destructuring assignment,
> multi-variable loops, default parameters, the `in` membership operator, calling any callable
> expression, richer dict-literal keys, single-line `^type`/`^event` field lists, and
> bracket-continued multi-line literals.
>
> This grammar covers ALL syntax in AxiomScript, including every addition from Phases 1–2
> (f-strings, comprehensions, expression-body functions, inline blocks, prefix ternary,
> multi-field declarations, implicit-self actions, numeric shortcuts, comma-mixins, semicolons
> in entity headers) and the alias forms from Phase 3 (`if`/`elif`/`else`).
>
> **Validation**: every example in `AXIOM_REF.md` is derivable from this grammar. Mismatches
> are spec bugs — report them.

## Notation

- `(* ... *)` — comment
- `[ ... ]` — optional (0 or 1 occurrence)
- `{ ... }` — repetition (0 or more)
- `( ... | ... )` — alternation
- `'...'` — literal token
- `IDENT`, `NUMBER`, `STRING`, etc. — token classes from the lexer (see §Tokens)

## Tokens

```ebnf
(* Token classes produced by the lexer (lexer.js). The lexer is indentation-sensitive:
   physical lines are joined into logical lines, INDENT/DEDENT tokens are emitted based on
   the indent stack, and NEWLINE tokens terminate each logical line. SEMICOLON is treated
   as an inline separator equivalent to NEWLINE in most parser positions. *)

token = AT | TILDE | DOLLAR | AMP | BANG | BANGBANG
      | QUESTION | CARET | HASH | QMARKEQ | STAR | PLUS
      | ARROW | TILDEEQ | TILDEGT | COLONCOLON | QMARKGT | DOTDOT
      | COLON | LPAREN | RPAREN | LBRACKET | RBRACKET
      | COMMA | DOT | LBRACE | RBRACE | SEMICOLON
      | PLUS | MINUS | STAR | SLASH | PERCENT
      | PLUSEQ | MINUSEQ | STAREQ | SLASHEQ | PERCEQ    (* v0.8.15: += -= *= /= %= *)
      | PLUSPLUS | MINUSMINUS | STARSTAR                        (* v0.8.15: ++ -- *)
      | NULLCOAL | ANDAND | OROR           (* v0.8.8: ?? ; v0.8.15: && || *)
      | NULLCOALEQ                              (* v0.8.17: ??= null-coalescing assign *)
      | GT | LT | GE | LE | EQEQ | NE | ASSIGN | PIPE
      | MIDDOT | CROSS | COMPOSE
      | BACKSLASH | PIPEGT | FATARROW          (* v0.9.0: lambda, pipeline, lambda-body alias *)
      | NUMBER | HEXNUM | IDENT | STRING | FSTRING
      | NEWLINE | INDENT | DEDENT | EOF ;

(* v0.9.0 LEXER CHANGES
   - BACKSLASH `\` introduces a lambda; PIPEGT `|>` is the pipeline operator; FATARROW `=>`
     is an accepted alias for the `:` in a lambda body.
   - `?!` lexes as QMARKEQ (the else sigil) ONLY when immediately followed by `:`. Elsewhere
     it is QUESTION + BANG, so `?!ready:` reads as "if not ready".
   - LINE CONTINUATION: a physical line that leaves a `(`, `[`, or `{` open (counted outside
     string literals) is joined with the lines that follow until the bracket closes. Multi-line
     array, dict, and argument lists are therefore legal. *)

(* NUMBER carries an optional `unit` suffix (e.g. `3s`, `10hz`, `3f`, `3v`).
   FSTRING is the f"..." or $"..." interpolated string (single token, raw payload kept).
   HEXNUM is 0x... hex literal.
   STRING is a plain "..." or '...' string literal.
   v0.8.15: STRING also covers triple-quoted strings ("""...""" or '''...''') — the lexer
   produces a single STRING token for both forms. Triple-quoted strings can span multiple
   physical lines; the lexer's line-joining pass merges them into one logical line.
   v0.8.15: NEW token types — PLUSEQ, MINUSEQ, STAREQ, SLASHEQ, PERCEQ (compound assign),
   PLUSPLUS, MINUSMINUS (incr/decr), NULLCOAL (??), PERCENT (modulo). *)
```

## Top-level Program

```ebnf
program = [ version_pragma ]
          { top_level_decl } ;

version_pragma = 'axiom' IDENT '.' IDENT ;  (* e.g. "axiom 0.8" — recorded, not enforced *)

top_level_decl = entity_decl
               | resource_decl
               | event_decl
               | type_decl
               | fn_decl
               | proc_decl
               | mixin_decl
               | material_decl
               | main_decl        (* v0.9.0 *)
               | use_decl         (* v0.9.0 *)
               | global_decl ;    (* v0.9.0 *)

(* v0.9.0: the program entry point. A file with a ^main runs as a script — the body executes
   once, with no entities and no frame loop. A file with BOTH a ^main and entities runs ^main
   as setup and then starts the loop. At most one ^main per program. *)
main_decl = '^' 'main' [ '(' IDENT ')' ]
            ( '=' expr NEWLINE
            | ':' stmt NEWLINE                                  (* single-line body *)
            | ':' NEWLINE INDENT { stmt } DEDENT ) ;

(* v0.9.0: textual import, include-once by resolved absolute path (cycles terminate). Paths
   resolve relative to the importing file; '.ax' is appended if the literal path is absent.
   All of the imported file's declarations enter one flat namespace; a name the importing file
   also declares wins, and the shadowing is reported (AX-USE-002). *)
use_decl = '^' 'use' STRING { ',' STRING } NEWLINE ;

(* v0.9.0: program global — evaluated once, in declaration order, into the root scope. Visible
   to every function, so constants and lookup tables need not be threaded through calls. *)
global_decl = '~' IDENT { ',' IDENT } ':' expr NEWLINE ;

(* v0.8.15: @input: block REMOVED (Phase 9 Option B). It was parsed but had no runtime effect.
   Now it's a fatal parse error. Input is fixed: input.move (Vec2), input.jump (bool),
   input.fire (bool), input.aim (Vec2). Read these names directly in blocks — no declaration
   needed. *)
```

## Resources

```ebnf
resource_decl = '#' IDENT IDENT ':' resource_source NEWLINE ;
(* e.g. "#Mesh3D Hero: \"hero.glb\"" or "#Texture Sky: \"sky.png\"" *)

resource_source = STRING                                      (* file path *)
                | 'base64' '(' STRING ')'                     (* inline single-line *)
                | 'glb' ':' NEWLINE INDENT { STRING NEWLINE } DEDENT ;  (* heredoc *)
```

## Events & Types

```ebnf
event_decl = '^' 'event' IDENT schema_fields ;
type_decl  = '^' 'type'  IDENT schema_fields ;

(* v0.9.0: fields may be written on one line, comma separated, and the type annotation is
   optional (the runtime is dynamically typed — the annotation documents intent). *)
schema_fields = ':' schema_field { ',' schema_field } NEWLINE
              | ':' NEWLINE INDENT { schema_field { ',' schema_field } NEWLINE } DEDENT ;
schema_field = IDENT [ '::' type_ref ] [ '?' ] ;  (* '?' marks the field optional *)

(* v0.9.0: a ^type name is also a CONSTRUCTOR. `P(1, 2)` fills the fields in declaration order;
   `P(y: 2)` names them; omitted fields are null. The result is a dict carrying `__type`, which
   `type(v)` reports and a `?*` match arm can dispatch on. *)
type_ref = '#' IDENT | IDENT ;  (* "#Entity" or "number", "v3", etc. *)
```

## Functions & Procedures

```ebnf
(* v0.8.7: expression-body shorthand — `^fn name(args) = expr` *)
fn_decl = '^' 'fn' IDENT [ param_list ] [ ret_type ]
          ( '=' expr NEWLINE                                  (* expression-body *)
          | ':' NEWLINE INDENT { stmt NEWLINE } DEDENT ) ;    (* block body *)

proc_decl = '^' 'proc' IDENT [ param_list ] ':' NEWLINE INDENT { stmt NEWLINE } DEDENT ;

(* v0.9.0: a parameter may carry a default. A caller that omits the argument (or passes null)
   gets the default, which removes the `?x == null: x = d` prologue optional arguments needed. *)
param_list = '(' param { ',' param } [ '->' type_ref ] ')' ;
param = IDENT [ '=' expr ] ;
ret_type = '->' type_ref ;

(* Inside every ^fn / ^proc / ^main / lambda body, `args` is bound to the full argument list. *)
```

## Mixins & Materials

```ebnf
mixin_decl = '^' 'mix' IDENT ':'
             ( NEWLINE INDENT { mixin_member } DEDENT         (* multi-line *)
             | mixin_member_inline ) ;                         (* single-line *)

mixin_member = field_decl_group | block ;
mixin_member_inline = field_decl_group ;

material_decl = '^' 'mat' IDENT [ ':' ]
                ( NEWLINE INDENT { mat_prop NEWLINE } DEDENT   (* multi-line *)
                | mat_prop { ',' mat_prop } NEWLINE ) ;        (* single-line *)
mat_prop = IDENT ':' expr ;
```

## Entities

```ebnf
(* v0.8.7: entity header is a loop accepting `;` as a separator between header elements.
   Order is flexible — fields, mixins, base, blocks, and `at` pose can all appear on the
   same line, separated by `;` or by NEWLINE (which exits the header loop). *)

entity_decl = '@' IDENT { header_element } [ NEWLINE ] [ entity_body ] ;

header_element = ';'
               | base_type            (* &Base — only when not followed by ( or :non-newline *)
               | mixin_include        (* +M1, M2, M3 *)
               | field_decl_group     (* ~field: value, ~a,b,c: value *)
               | at_pose              (* at v3(...) *)
               | ':'                  (* optional header colon — no-op *)
               | one_liner_block ;    (* &block: stmt — when & is followed by ( or :non-newline *)

base_type = '&' IDENT ;  (* only if NOT followed by '(' or ':' non-newline — see disambiguation *)
mixin_include = '+' IDENT { ',' IDENT } ;  (* v0.8.7: comma-separated mixins *)
at_pose = 'at' expr ;

(* Disambiguation: `&` in header position is a BASE TYPE unless:
   - it's followed by IDENT and '(' (block with frequency: &tick(10hz): ...)
   - it's followed by IDENT and ':' and a non-NEWLINE/non-`;` token (one-liner block: &physics: stmt)
   In those cases, it's a one-liner BLOCK. *)

one_liner_block = block ;  (* parsed via parseBlock — single-line form *)

entity_body = INDENT { member_line } DEDENT ;
member_line = entity_decl              (* nested entity — rare *)
            | block                    (* &physics: ... *)
            | field_decl_group ;       (* ~field: value, ~a,b,c: value *)
```

## Field Declarations

```ebnf
(* v0.8.7: multi-field shorthand — `~a, b, c: value` declares three fields all set to `value`.
   parseField returns either one FieldDecl or an array; callers normalize via [].concat(). *)

field_decl_group = tilde_field { ',' field_continuation } ;

tilde_field = '~' IDENT [ '!' ] [ ':' field_value ] ;  (* `~name`, `~name!: val`, `~name: val` *)
                                                (* '!' marks intentional mixin override (noWarn) *)

(* v0.8.7: multi-name form — after `~name`, if `,` follows (before `:` or `!`), collect more names *)
multi_name_field = '~' IDENT { ',' IDENT } [ ':' field_value ] ;  (* `~a, b, c: 100` *)

field_continuation = IDENT [ '!' ] [ ':' field_value ]   (* sigil-less continuation after comma *)
                   | tilde_field                          (* new field with explicit sigil *)
                   | dollar_field ;

dollar_field = '$' IDENT ':' dist_expr [ 'infer' ':' call_like ] ;
field_value = pool_type | expr ;
pool_type = '&' ( 'Pool' | 'Vec' ) '(' IDENT ',' NUMBER ')'
          | '&' 'Map' '(' IDENT ',' IDENT ',' NUMBER ')' ;
```

## Blocks

```ebnf
(* Blocks are the executable units inside entities. Only 4 names are executed by the runtime:
   physics (60Hz), render (60Hz after physics), tick(Nhz), on(Event). Other names parse
   but are dead code (AX-BLOCK-001 advisory). *)

block = '&' IDENT [ '(' ( NUMBER | IDENT ) ')' ] ':' block_body ;

(* block_body:
   - Multi-line: NEWLINE INDENT { stmt } DEDENT
   - Single-line (v0.8.1): one stmt on the same line as the header
   - Single-line multi-stmt (v0.8.7): stmts separated by ';' on the same line *)

block_body = NEWLINE INDENT { stmt } DEDENT
           | stmt { ';' stmt } ;  (* v0.8.7: ';' separated — token-stream mutation makes stmt
                                     parsers see NEWLINE instead of ';' *)
```

## Statements

```ebnf
stmt = assert_stmt
     | action_stmt
     | emit_stmt
     | return_stmt
     | broadcast_stmt
     | cond_block
     | loop
     | break_stmt
     | continue_stmt
     | transition_chain
     | assign_stmt
     | expr_stmt
     | cross_entity_assign
     | match_stmt            (* v0.9.0 *)
     | try_stmt              (* v0.9.0 *)
     | throw_stmt            (* v0.9.0 *)
     | destructure_assign ;  (* v0.9.0 *)

(* v0.9.0: multiway match. Arm patterns are ordinary expressions compared by value equality
   (which is STRUCTURAL for arrays and dicts, so a tuple works as a pattern); `_` is the
   default arm and must come last. A bare identifier naming a ^type matches any record of that
   type. A match is a statement, not an expression — assign inside the arms. *)
match_stmt = '?' '*' expr ':' NEWLINE INDENT { match_arm } DEDENT ;
match_arm  = ( pattern { ',' pattern } | '_' ) [ 'if' expr ] ':' inline_or_block_body ;

(* v0.9.1: PATTERNS. A pattern is parsed with the expression grammar and interpreted by these
   rules, so no new syntax is needed:

     IDENT '(' pattern-args ')'   a record of that ^type; arguments match its fields in
                                  declaration order, or by name (`P(y: b)`)
     '[' pattern { ',' pattern } ']'   an array of exactly that length, element-wise
     '{' key ':' pattern … '}'    a dict containing at least those keys (extra keys ignored);
                                  `{x, y}` is the shorthand that binds those keys
     IDENT                        INSIDE a pattern: binds whatever is in that position
                                  AT THE TOP LEVEL of an arm: compares (so `idle:` still tests
                                  the atom `idle`) — unless the arm has a guard, where a bare
                                  name captures the subject (`n if n > 10:`)
     '_'                          matches anything and binds nothing
     IDENT (a declared ^type)     matches any record of that type, binding nothing
     any other expression         evaluated and compared by value (structurally, for arrays
                                  and dicts)

   Bindings become the arm's scope frame, so the guard and the body see them and a failed arm
   leaves nothing behind. *)

(* v0.9.0: structured error handling. `^catch` may bind a variable, which receives
   {msg, code, value}; engine faults (bad index, unknown method, sandbox denial) are catchable
   through the same handler. At least one of ^catch / ^fin must be present. *)
try_stmt   = '^' 'try' ':' inline_or_block_body
             [ '^' 'catch' [ IDENT ] ':' inline_or_block_body ]
             [ '^' 'fin' ':' inline_or_block_body ] ;
throw_stmt = '^' 'throw' [ expr ] ( NEWLINE | ';' ) ;

(* v0.9.0: unpack an array, a [key, value] pair, or a record (by field name). *)
destructure_assign = IDENT { ',' IDENT } '=' expr ( NEWLINE | ';' ) ;

inline_or_block_body = stmt | NEWLINE INDENT { stmt } DEDENT ;

assert_stmt = ( '!!' | BANG BANG ) expr ( NEWLINE | ';' ) ;
action_stmt = '!' IDENT [ '(' args ')' ] ( NEWLINE | ';' ) ;  (* v0.8.7: implicit-self *)
emit_stmt = '^' 'emit' dotted_path '(' args ')' ( NEWLINE | ';' ) ;
return_stmt = '^' 'return' expr ( NEWLINE | ';' ) ;
broadcast_stmt = '^' IDENT '(' args ')' [ addressing ] ( NEWLINE | ';' ) ;
addressing = 'to' [ '(' ] '#' IDENT [ ')' ]
           | 'within' '(' expr [ ',' 'origin' ':' expr ] ')' ;

(* Conditional block — v0.8.7: `if`/`elif`/`else` are aliases for `?`/`elif`/`?!:`.
   Both forms are first-class and produce the same CondBlock AST. *)
cond_block = ( '?' | 'if' ) infer_expr ':'
             ( NEWLINE INDENT { stmt } DEDENT          (* multi-line body *)
             | stmt                                    (* single-line body *)
             | expr NEWLINE INDENT { stmt } DEDENT )   (* guard syntax: ?cond: guard\n body *)
             [ else_clause ] ;

else_clause = ( '?!:' | '?' '!' ':' | 'else' ':' ) NEWLINE INDENT { stmt } DEDENT
            | 'elif' cond_block ;  (* v0.8.7: chained else-if — desugars to else:[CondBlock] *)

(* v0.9.0: a for loop may bind SEVERAL variables, destructuring each element — `*k, v in
   items(d):`, `*i, x in enumerate(xs):`, `*a, b in zip(p, q):`. Bodies may be inline.
   Loop variables live in a scope frame, so they shadow rather than overwrite, nest safely,
   and work with no entity present.
   Iteration budget: loops inside &physics/&render/&tick/&on are capped (AX-LOOP-002) so one
   frame cannot hang; loops inside ^fn/^proc/^main are uncapped. *)
loop = for_loop | while_loop ;
for_loop = '*' IDENT { ',' IDENT } 'in' expr ':' ( stmt | NEWLINE INDENT { stmt } DEDENT ) ;
while_loop = '*' expr ':' ( stmt | NEWLINE INDENT { stmt } DEDENT ) ;

break_stmt = '~' 'break' ( NEWLINE | ';' ) ;
continue_stmt = '~' 'continue' ( NEWLINE | ';' ) ;

transition_chain = IDENT { '->' IDENT [ '(' args ')' ] [ 'if' expr ] } ( NEWLINE | ';' ) ;

(* v0.8.15: compound assignment operators. `hp -= 10` desugars to `hp = hp - 10`.
   The compoundOp field on the Assign AST tells the interpreter to read the current value,
   apply the op, and write back. Works on plain vars, member paths, and array indices.
   v0.8.17: `??=` (NULLCOALEQ) is null-coalescing assignment — `~x ??= v` writes v only
   if x is currently null/undefined; otherwise it's a no-op. Saves 5-7 tokens vs
   `?x == null: x = v`. compoundOp is '??' — the interpreter short-circuits instead of
   calling binaryOp (since ?? is not a binaryOp). *)
assign_op = '=' | '+=' | '-=' | '*=' | '/=' | '%=' | '~=' | '??=' ;

assign_stmt = [ '$' ] IDENT assign_op expr ( NEWLINE | ';' )                    (* simple/compound *)
            | IDENT { '.' IDENT } assign_op expr ( NEWLINE | ';' )               (* member assign/compound *)
            | IDENT '[' expr ']' assign_op expr ( NEWLINE | ';' )                (* index assign/compound *)
            | '~' IDENT ':' expr ( NEWLINE | ';' ) ;                             (* ~field: expr reassign *)

(* v0.8.15: prefix increment/decrement. `++x` desugars to `x = x + 1` (statement, not expression).
   Prefix form only — `x++` (postfix) is not supported. *)
incr_stmt = ( '++' | '--' ) ( IDENT | IDENT { '.' IDENT } | IDENT '[' expr ']' ) ( NEWLINE | ';' ) ;

cross_entity_assign = '#' IDENT { '.' IDENT } '=' expr ( NEWLINE | ';' ) ;  (* #Tag.field = expr *)

expr_stmt = expr ( NEWLINE | ';' ) ;
```

## Expressions

```ebnf
(* Precedence (lowest to highest): ternary → infer → comparison → additive → multiplicative →
   unary → postfix → primary. *)

(* v0.8.15: null-coalescing `??` at lowest precedence (below ternary). `a ?? b ?? c` chains. *)
(* v0.9.0: `|>` (pipeline) is the LOWEST precedence, below the ternary.
     x |> f              → f(x)
     x |> f(a)           → f(x, a)        (the piped value becomes the FIRST argument)
     x |> obj.m(a)       → obj.m(x, a)
     x |> \v: v + 1      → (\v: v + 1)(x)
   Chaining reads left to right with no nesting to balance. *)
expr = pipeline ;

pipeline = null_coalesce { '|>' null_coalesce } ;

null_coalesce = ternary { '??' ternary } ;

(* v0.8.7: BOTH ternary forms are first-class.
   - Suffix: `cond ? then : else` (existing, C/Python/JS-like)
   - Prefix: `? cond : then : else` (new — leading `?` makes ternary intent visible at start)
   Disambiguation from query: `? IDENT (` is a query primary; `? <anything else>` is prefix ternary. *)
ternary = infer [ '?' ternary_then ':' ternary_else ]              (* suffix form *)
        | '?' infer ':' ternary_then ':' ternary_else ;            (* prefix form — v0.8.7 *)

ternary_then = expr ;
ternary_else = expr ;

infer = comparison [ '~>' IDENT ] ;  (* e.g. `$belief ~> argmax` *)

(* v0.9.0: `in` / `!in` — membership over arrays, strings, dict keys, ranges, and buffers,
   at comparison precedence. No ambiguity with the loop/comprehension `in`: those parse their
   variable list and consume `in` before any expression parsing begins. *)
comparison = additive { ( '>' | '<' | '>=' | '<=' | '==' | '!=' | '?>' | 'in' | '!' 'in' ) additive } ;

(* v0.9.0: `==` / `!=` are STRUCTURAL for arrays and plain dicts/records (compared element by
   element, to a depth of 32) and identity-based for everything else (entities, pools, shapes). *)

additive = multiplicative { ( '+' | '-' | '..' ) multiplicative } ;

(* v0.8.15: `%` (modulo) at multiplicative precedence (same as `*`/`/`). *)
multiplicative = unary { ( '*' | '/' | '%' | '·' | '×' | '∘' ) unary } ;

unary = ( '!' | '-' ) unary | postfix ;

(* v0.9.0: ANY expression that evaluates to a callable may be called — `fns[i](x)`,
   `(\x: x * 2)(4)`, `table.get(k)(arg)`. A dict field holding a function is a method:
   `ops.dbl(3)` calls the closure stored at `ops.dbl`. *)
postfix = primary { postfix_op } ;
postfix_op = '.' IDENT [ '(' args ')' ]     (* member access, method call, or stored-function call *)
           | '[' expr ']'                   (* index *)
           | '(' args ')' ;                 (* call — of an IDENT, or of any callable result *)

primary = NUMBER [ unit_suffix ]            (* v0.8.7: 3f, 3v *)
        | HEXNUM
        | STRING
        | FSTRING                            (* v0.8.7: f"..." or $"..." *)
        | '#' IDENT { '.' IDENT }            (* TagRef *)
        | '$' IDENT                          (* $-sigil ident *)
        | '[' [ comprehension | array_elems ] ']'  (* array literal or comprehension *)
        | '{' dict_elems '}'                 (* dict literal *)
        | '(' expr ')'                       (* grouping *)
        | '?' IDENT '(' args ')'             (* receiverless query *)
        | '?!#' IDENT { '.' IDENT }          (* v0.8.15: ?!#Tag shorthand for ?exists(#Tag) *)
        | lambda                             (* v0.9.0 *)
        | IDENT ;                            (* bare identifier — a declared ^fn name used
                                                without parens is a FUNCTION VALUE; any other
                                                unbound name is an atom *)

(* v0.9.0: lambda. The body is ONE expression and ends where the enclosing expression ends, so
   `xs.map(\x: x * 2)` needs no closing delimiter of its own. Closures capture the scope, the
   entity, and the world they were created in. `=>` is an accepted alias for the `:`. *)
lambda = '\' [ IDENT { ',' IDENT } ] ( ':' | '=>' ) expr ;

(* v0.8.7: numeric literal suffixes — attached by the lexer as `unit`. *)
unit_suffix = 'f' | 'v' | 's' | 'ms' | 'hz' | IDENT ;  (* 'f'=float, 'v'=uniform Vec3, others passthrough *)

(* v0.8.15: v3 with 2 args = horizontal plane (x, 0, z). 1-arg = uniform (v,v,v). 3-arg = full. *)
(* The v3 intrinsic handles all three forms by checking args.length at runtime. *)

(* v0.8.7: array comprehension — `[expr for var in iter (if cond)?]` *)
comprehension = expr 'for' IDENT { ',' IDENT } 'in' expr [ 'if' expr ] ;

(* v0.9.0: trailing commas are tolerated in arrays, dicts, and argument lists. *)
array_elems = expr { ',' expr } [ ',' ] ;

(* v0.9.0: dict keys may be identifiers, string literals, or computed; `{x}` is shorthand for
   `{x: x}`. Keys are strings — a computed key is coerced the way an f-string would render it. *)
dict_elems = dict_entry { ',' dict_entry } [ ',' ] ;
dict_entry = IDENT ':' expr
           | STRING ':' expr
           | '[' expr ']' ':' expr
           | IDENT ;                         (* shorthand: {x} → {x: x} *)
args = '(' [ arg { ',' arg } ] ')' ;
arg = IDENT ':' expr                (* named arg *)
    | ( '>' | '<' | '>=' | '<=' | '==' | '!=' ) expr   (* predicate arg *)
    | expr ;                        (* positional arg *)

call_like = IDENT [ '(' args ')' ] ;
dist_expr = call_like [ '~' call_like ] ;
dotted_path = IDENT { '.' IDENT } ;
```

## F-String Interpolation (v0.8.7)

```ebnf
(* The lexer produces FSTRING as a single token. The parser splits the raw payload on `{`/`}`
   (with `{{`/`}}` escapes) and recursively parses each `{expr}` placeholder. *)

fstring = ( 'f' | '$' ) '"' fstring_body '"' ;
fstring_body = { fstring_part } ;
fstring_part = literal_text              (* verbatim text, including {{ → { and }} → } *)
             | '{' expr '}' ;            (* placeholder — full expression grammar available *)
```

## Sigil Disambiguation Rules (v0.8.7)

The 9 sigils (`@ ~ & ! ^ ? * # +`) have context-dependent meanings. The rules:

| Sigil | Position | Meaning | Example |
|---|---|---|---|
| `@` | top-level / indent | Entity declaration | `@Player &Body3D` |
| `~` | entity header / body line | Field declaration | `~hp: 100` |
| `~` | block body, before `break`/`continue` | Loop control | `~break` |
| `~` | block body, before IDENT `:` | Reassignment | `~field: new_value` |
| `~` | after `$belief` | Infer operator (`~>`) | `$belief ~> argmax` |
| `&` | entity header, before Capitalized IDENT (no `:`/`(`) | Base type | `&Body3D` |
| `&` | entity header, before IDENT `(` or IDENT `:` non-NEWLINE | One-liner block | `&physics: stmt` |
| `&` | entity body (member position) | Block (always) | `&tick(10hz):` |
| `&` | after `~field:` | Pool/Vec/Map type | `~bullets: &Pool(Bullet, 64)` |
| `!` | block body, before IDENT | Action (implicit-self) | `!play("hit.wav")` |
| `!` | after `!!` | Assert | `!!hp > 0` |
| `!` | after field name in `~` decl | Override marker (noWarn) | `~hp!: 60` |
| `^` | top-level | Declaration keyword | `^event`, `^fn`, `^mix`, `^mat` |
| `^` | block body, before `emit`/`return` | Control flow | `^return x`, `^emit Chan(x)` |
| `^` | block body, before IDENT | Broadcast | `^Hit(damage: 25)` |
| `?` | statement start | Conditional block | `?hp <= 0:`, `if hp <= 0:` |
| `?` | expression position, before IDENT `(` | Query primary | `?path(from, to)` |
| `?` | expression position, before non-`(` | Prefix ternary (v0.8.7) | `? cond : a : b` |
| `?` | after expr (in ternary) | Suffix ternary | `cond ? a : b` |
| `?!` | statement start (after cond block) | Else clause | `?!:`, `else:` |
| `*` | statement start | Loop | `*i in 0..10:`, `*hp > 0:` |
| `#` | anywhere | Resource/entity reference | `#Mesh3D Sphere: "..."`, `#Player.pos` |
| `+` | entity header | Mixin include | `+Stats`, `+M1, M2, M3` |
| `+` | in expression | Addition operator | `a + b` |
| `^` | top-level (v0.9.0) | `^main` entry point, `^use` import | `^main:`, `^use "lib.ax"` |
| `^` | block body (v0.9.0) | `^try` / `^catch` / `^fin` / `^throw` | `^try:`, `^throw "x"` |
| `~` | top level (v0.9.0) | Program global | `~MAX: 100` |
| `?*` | statement start (v0.9.0) | Match | `?* cmd:` |
| `?!` | statement start, NOT followed by `:` (v0.9.0) | Negated condition | `?!ready:` |
| `\` | expression position (v0.9.0) | Lambda | `\x: x * 2` |
| `\|>` | between expressions (v0.9.0) | Pipeline | `xs \|> sum` |
| `in` | between expressions (v0.9.0) | Membership | `x in xs` |

### Concrete disambiguation examples

```axiom
// & disambiguation:
@Enemy &Body3D              // &Base (Capitalized, no : or ()
@Enemy &physics: stmt       // &block (lowercase, : followed by non-newline)
@Enemy &tick(10hz):         // &block (has ()
~bullets: &Pool(Bullet, 64) // &Pool type (after ~field:)

// ? disambiguation:
?hp <= 0:                    // conditional block (statement start)
x = ?path(from, to)          // query primary (? before IDENT ()
x = ? hp > 0 : "yes" : "no" // prefix ternary (? before non-()
x = hp > 0 ? "yes" : "no"   // suffix ternary (? after expr)
?!:                          // else clause (statement start, after cond block)

// ~ disambiguation:
~hp: 100                     // field declaration (entity header/body line)
~break                       // loop control (in block body, before break/continue)
~field: new_value            // reassignment (in block body, before IDENT :)
$belief ~> argmax            // infer operator (~> after $-field)

// ! disambiguation:
!play("hit.wav")             // action (block body, before IDENT)
!!hp > 0                     // assert (double-bang)
~hp!: 60                     // override marker (! after field name in ~ decl)

// v0.9.0 — ?! disambiguation:
?!:                          // else clause      (`?!` followed by `:`)
?!ready:                     // if NOT ready     (`?!` followed by anything else)
?!#Boss:                     // if #Boss exists  (the ?exists shorthand)

// v0.9.0 — \ and |> :
xs.map(\x: x * 2)            // lambda; body ends with the enclosing expression
xs |> filter(\x: x > 0) |> len   // pipeline: filter(xs, ...) then len(...)

// v0.9.0 — `in`:
*i in 0..10:                 // loop keyword (parsed before any expression)
?x in [1, 2, 3]:             // membership operator
[v for v in xs if v in ok]   // both, in one comprehension
```

## Scope and assignment (v0.9.0)

AxiomScript is lexically scoped. Each `^fn` / `^proc` / `^main` call and each loop, match arm,
and `^try` body gets a scope frame; a name resolves to the innermost frame that binds it, then
to the running entity's fields, then to the program globals.

Assignment (`x = v`) resolves in this order:

1. a name already bound in an enclosing scope frame → written there (parameters, locals);
2. an existing field of the entity running the statement → written to the field (this is the
   pre-0.9 behaviour, so every existing entity program is unaffected);
3. otherwise → declared in the current function frame, or, inside an entity block, on the
   entity (again matching pre-0.9 behaviour for frame-to-frame scratch state).

Inside a function body, `~x: v` forces the scope branch, which is how a local deliberately
shadows an outer name. A function never sees its caller's locals.

Consequences: recursion is correct (each call has its own frame), a parameter can be assigned,
a lambda returned from a function keeps working (closures capture their defining scope), and a
`^fn` can run with no entity at all — which is what makes script mode possible.

Recursion depth is bounded by the host stack. The CLI raises it at start-up (see
`--no-restack`); exceeding it raises the catchable `AX-DEPTH-001` rather than crashing.

## Comments

```ebnf
(* Comments start with `//` and run to end of line. Stripped by the lexer BEFORE tokenization.
   Respected inside string literals — `"http://..."` is NOT a comment. *)
comment = '//' { any_char_except_newline } ;
```

## Reference Types

```ebnf
(* Reference types are used in schema_field declarations (^event/^type) and material props. *)
type_ref = '#' IDENT     (* entity reference: #Entity, #Player, etc. *)
         | IDENT ;        (* primitive: number, string, v3, v2, quat, bool, etc. *)
```

## Newline & Semicolon Handling

```ebnf
(* The lexer emits NEWLINE at the end of each logical line and INDENT/DEDENT based on the
   indent stack. SEMICOLON is a v0.8.7 addition — treated as an inline NEWLINE in most
   parser positions (entity header separator, single-line block stmt separator).
   
   In single-line block bodies, the parser mutates SEMICOLON tokens to NEWLINE in-place
   (in the token stream) so that existing stmt parsers — which expect NEWLINE as a terminator —
   work unchanged. This means `&physics: a = 1; b = 2` is parsed as two stmts in one block body. *)
```
