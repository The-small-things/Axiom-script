# AxiomScript v0.8.15 — Formal Grammar (EBNF)

> This grammar covers ALL syntax in AxiomScript v0.8.7, including every addition from Phases 1–2
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
      | NUMBER | HEXNUM | IDENT | STRING | FSTRING
      | NEWLINE | INDENT | DEDENT | EOF ;

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
               | material_decl ;

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
event_decl = '^' 'event' IDENT ':' NEWLINE INDENT { schema_field NEWLINE } DEDENT ;
type_decl  = '^' 'type'  IDENT ':' NEWLINE INDENT { schema_field NEWLINE } DEDENT ;
schema_field = IDENT '::' type_ref [ '?' ] ;  (* '?' marks the field optional *)
type_ref = '#' IDENT | IDENT ;  (* "#Entity" or "number", "v3", etc. *)
```

## Functions & Procedures

```ebnf
(* v0.8.7: expression-body shorthand — `^fn name(args) = expr` *)
fn_decl = '^' 'fn' IDENT [ param_list ] [ ret_type ]
          ( '=' expr NEWLINE                                  (* expression-body *)
          | ':' NEWLINE INDENT { stmt NEWLINE } DEDENT ) ;    (* block body *)

proc_decl = '^' 'proc' IDENT [ param_list ] ':' NEWLINE INDENT { stmt NEWLINE } DEDENT ;

param_list = '(' IDENT { ',' IDENT } [ '->' type_ref ] ')' ;
ret_type = '->' type_ref ;
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
     | cross_entity_assign ;

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

loop = for_loop | while_loop ;
for_loop = '*' IDENT 'in' expr ':' NEWLINE INDENT { stmt } DEDENT ;
while_loop = '*' expr ':' NEWLINE INDENT { stmt } DEDENT ;

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
expr = null_coalesce ;

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

comparison = additive { ( '>' | '<' | '>=' | '<=' | '==' | '!=' | '?>' ) additive } ;

additive = multiplicative { ( '+' | '-' | '..' ) multiplicative } ;

(* v0.8.15: `%` (modulo) at multiplicative precedence (same as `*`/`/`). *)
multiplicative = unary { ( '*' | '/' | '%' | '·' | '×' | '∘' ) unary } ;

unary = ( '!' | '-' ) unary | postfix ;

postfix = primary { postfix_op } ;
postfix_op = '.' IDENT [ '(' args ')' ]     (* member access or method call *)
           | '[' expr ']'                   (* index *)
           | '(' args ')' ;                 (* call (only if primary is IDENT) *)

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
        | IDENT ;                            (* bare identifier *)

(* v0.8.7: numeric literal suffixes — attached by the lexer as `unit`. *)
unit_suffix = 'f' | 'v' | 's' | 'ms' | 'hz' | IDENT ;  (* 'f'=float, 'v'=uniform Vec3, others passthrough *)

(* v0.8.15: v3 with 2 args = horizontal plane (x, 0, z). 1-arg = uniform (v,v,v). 3-arg = full. *)
(* The v3 intrinsic handles all three forms by checking args.length at runtime. *)

(* v0.8.7: array comprehension — `[expr for var in iter (if cond)?]` *)
comprehension = expr 'for' IDENT { ',' IDENT } 'in' expr [ 'if' expr ] ;

array_elems = expr { ',' expr } ;
dict_elems = { IDENT ':' expr } ;
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
```

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
