# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

NepLang is a compiled programming language for Nepali developers, mixing romanized Nepali
control-flow/keyword vocabulary with English technical terms (e.g. `vana` = print,
`yedi`/`natara` = if/else, `kaam` = function, `farkau` = return). The compiler/interpreter
is implemented in C (C11), built incrementally, milestone by milestone.

**Current status: all roadmap milestones are complete (M1–M14).** A program is a sequence of
newline-terminated statements: `vana(expr)` (print), `name = expr` (assignment, which
creates the variable on first use — no declaration keyword, per `docs/prompt.txt`),
`yedi cond { } natara yedi cond { } natara { }`, `jabasamma cond { }` (while),
`ko_lagi i in a..b { }` (for; end-exclusive, bounds evaluated once), `kaam name(a, b) { }`
(top-level only), `farkau [expr]`, call statements `name(args)`, and `a[i] = v`. Values are
integers, arrays (`[1, 2]`, `a[i]`, built-in `lambai(a)` for length, `ko_lagi x in a`) and
immutable strings (`"..."` with `\n \t \" \\` escapes; UTF-8 content allowed). `+` joins
strings, converting a number on either side to text; `s[i]`, `lambai(s)` and `ko_lagi c in s`
work on characters (code points), not bytes. Comparisons `== != < <= > >=` work on two
numbers or two strings (byte-wise order), yield 1/0; conditions must be numbers and any
non-zero number is truthy (there is no bool type yet).

Classes: `class Name { field = default ... kaam method(...) { } }` (top level only). Fields
must be declared with a starting value (no typed `string naam` declarations until the type
checker); assigning an undeclared field is an error. `Name(args)` constructs: field
defaults are re-evaluated per object in a globals-only context, then the method `banau` (the
constructor; not a lexer keyword, just a reserved method name) runs if present. `banau`
can't `farkau` a value or be called directly. `yo` (keyword) is the current object, only
valid in methods. Inside a method, bare names resolve locals/params → fields of `yo` →
globals, and bare assignment writes the field when the name is a field and not already a
local (so params shadow fields; `yo.x = x` is the idiom); bare `f()` calls a method of `yo`
before top-level functions. Members named `_x` are private: accessible only while a method
of the same class is running. Objects are ref-counted and shared like arrays; `==` on
objects is an error; no inheritance/interfaces yet. **Basics pass (post-M8):** logical operators `ra` (and), `wa` (or), `hoina` (not) with
short-circuiting, yielding 1/0 and binding looser than comparisons (`wa` < `ra` < `hoina` < compare;
implemented as `parse_comparison` → `parse_and` → `parse_not` → `parse_compare`); loop control `ruk`
(break) / `agadi` (continue), parsed only inside loops (`parser->loop_depth`) and unwound via
`interp->breaking`/`continuing` (`UNWINDING()` / `end_loop_iteration()`); builtins `lambai`,
`thapa(arr, v)` (push), `hata(arr)` (pop), `sankhya(str)` (string → number), `padha()` (read a
line from stdin); tests in `tests/basics/`. **Post-M14 additions:** string interpolation `"a ${expr} b"` (desugared in `parse_string` to `"a " + (expr) + " b"` via a sub-parser on a `Lexer` with `end` set; `\$` escapes; no `"` inside `${}`); expressions continue across lines after binary operators and inside `( )`/call args (`skip_newlines`); `sacho`/`jutho` lexed as `TOKEN_INT` 1/0 (superseded below).

**Phase 2 (post-M14, "complete language"):** real value types `VAL_FLOAT` (`3.14`, `1e3`; `/` on two
ints stays integer division), `VAL_BOOL` (`sacho`/`jutho` are `TOKEN_BOOL`; comparisons/`ra`/`wa`/`hoina`
yield bools, printed `sacho`/`jutho`; bools are not numbers — `sacho + 1` is an error, `int(b)` converts;
numbers still work as conditions) and `VAL_NULL` (`khali`; functions without `farkau` return it; REPL echo
skips it). Written types (`TypeRef` in ast.h): `int float bool string array object any void` or a class /
interface name (`TYPE_CLASS`, implicitly nullable), `?` suffix = nullable. Declarations `TYPE name [= v]`
(detected by `at_typed_name()`: IDENT followed by IDENT or `?`, via `peek_token()` on a lexer copy; type
names are *not* keywords) and `const [TYPE] NAME = v` are `AST_ASSIGN` with `type`/`is_const`; untyped
declarations default via `ast_default_for`. Typed params `kaam f(int a) int {`, return type after `)`,
`void` (parser rejects `farkau value` there). Typed fields `string naam` (no value needed). Compound
`+= -= *= /= %=` desugar in `parse_assignment` to `x = x op e` using `ast_clone` (target evaluated twice).
Inheritance `class Kukur(Janawar, Bolne)` / `interface Bolne(Other) { kaam bolau() string }` (bodiless),
resolved by `src/classes.c` after modules and before the checker (layout documented on `class_decl` in
ast.h: inherited fields first and borrowed, own methods first then inherited borrowed ones,
`function.owner` = declaring class). `super.m(...)` / `super(...)` (only in `banau`) are `AST_CALL` with an
`AST_SUPER` receiver (`super(...)` keeps name token "super"). Standard library: all builtins live in the
`BUILTINS[]` table in rt.c (global ones, `module.fn` ones, and array/string methods `a.push(x)` =
`array.push(a, x)` via `rt_value_method`). `import math` (bare name of a stdlib module) is handled in
modules.c: no file, and `rewrite_module_uses()` turns `math.f(x)` into `AST_CALL` with `call.module` set
and `math.PI` into a 0-arg module call; quoted `import "math.np"` still loads a file. Types are enforced
at runtime by `rt_coerce` (int→float converts) in all backends (`IR_COERCE` with `IrProgram.types`), and
statically by the checker. CLI: `neplang` alone = REPL (`parser.repl` echoes bare top-level expressions;
`rt_recover` jmp_buf + `interpreter_recover`; functions may be redefined), `run`/`check`/`build`
subcommands, args after the file → `os.args()`. Not done: generics, maps, closures, concurrency,
http/json/crypto/db modules, `format`/`test` commands, `--lang=ne`.

**Later milestones (all done):**
- *Symbol table (M9)*: `Scope` in `interpreter.c` is a hashed table (dense `vars[]` + open-addressing `slots[]`, FNV-1a).
- *Static checker (M10)*: `src/checker.c`, run by `main.c` after parsing/import resolution and before any backend. Type *inference* only (`T_UNKNOWN/NUMBER/STRING/ARRAY/OBJECT`), no annotations. Variable types come from `collect()` + table-free `simple_type()`; unknown never errors, so it must never reject a valid program. Checks undefined variables/functions, arity (functions, methods, constructors, builtins), impossible operand types, bad conditions/indexes/fields.
- *Modules (M11)*: `import "file.np"` / `import name` (keyword `import`, `AST_IMPORT`, top level only). `src/modules.c` splices imported statements into the program root before checking (flat namespace, each path once, depth cap 32; lookup next to importer then `NEPLANG_PATH`). Module source buffers stay alive until `modules_free()`. `diagnostics.c` keeps a registry of source buffers so errors inside a module print that module's file/line.
- *IR (M13)*: `src/ir.c` — AST → stack-based IR per function (function 0 = top-level program; its variables are globals, loops use hidden local slots; functions get a slot for every assigned name, `IR_LOAD_NAME` falls back local→global like the interpreter). `ir_optimize` (constant folding, constant branches, jump-to-next, dead code), `ir_finalize` (label positions, max depth), `ir_dump`. Every instruction records its operand-stack `depth`.
- *Runtime (`src/rt.c`)*: shared value operations and error messages used by the VM and native programs (`VAL_UNSET` marks unassigned slots).
- *VM (M12)*: `src/vm.c`, explicit frame stack, call depth 1000. `neplang --vm`.
- *Native (M14)*: `src/cgen.c` turns IR into C (stack slots become variables `s0..`, labels become C labels) and runs the system C compiler against value.c/rt.c/diagnostics.c. `neplang --emit-c` / `--build prog.np -o out`. `DEFS` in the Makefile bakes in the runtime dir (`make install` rebuilds with the installed one).
- *Classes in the compiled backends*: each method is an `IrFunction` with `yo` in slot 0 (`param_count` includes it); `IrProgram.classes` maps class → AST_CLASS decl + `method_fn[]`. Ops `IR_NEW_OBJECT/INIT_FIELD/DUP/GET_FIELD/SET_FIELD/CALL_METHOD`; field/method lookup, privacy and arity errors are in `rt.c` (`rt_get_field`, `rt_find_method`), keyed by name at runtime with `a` = enclosing class for the privacy check. Bare field names in methods resolve statically (`is_bare_field`), field defaults are inlined at each construction site compiled in global-only mode. Native: `cgen.c` emits static `AstNode` class descriptions (`K0..`) so `value.c`/`rt.c` work unchanged. Known edge: a `ko_lagi` variable named like a field is always a local.
- *Phase 2 in the compiled backends*: only a class's *own* methods are compiled; `method_fn[]` entries for
  inherited methods point at the owner's function. Interfaces are in `IrProgram.classes` too (no methods
  compiled; cgen emits them so `rt_is_a` works). `super` calls resolve statically to `IR_CALL`. Typed
  globals/locals are pre-scanned (`declare_globals` / `declare_assigned`) so every store coerces. cgen
  always emits `FN[]`/`method_fn` (array/string method calls go through `IR_CALL_METHOD`) and needs `-lm`.
- *Tests*: `make test` = interpreter suites + `tests/run_backends.sh vm` (re-runs every program through the VM, plus IR checks); `make test-native` does the same via native executables; `make test-all` both.

Milestone order follows `docs/prompt.txt` §33 where it and the README disagreed (strings
came before structs/classes); the README roadmap now reflects that.

## Commands

```bash
make          # build bin/neplang
make test-all # interpreter + VM + native backends
make debug    # build with -fsanitize=address,undefined (use this while developing)
make test     # build, then run tests/run_tests.sh
make clean    # remove bin/

./bin/neplang examples/hello_calc.np   # run a program
./bin/neplang path/to/file.np          # run any .np source file
```

Run a single test case manually (bypassing the harness):
```bash
./bin/neplang tests/calculator/basic.np
```

There is no test framework beyond `tests/run_tests.sh`, a plain bash script:
- Every subdirectory of `tests/` is a suite (`calculator/`, `variables/`, ...).
- Any `tests/SUITE/NAME.np` with a matching `NAME.expected` is run and its stdout diffed
  against the `.expected` file.
- Any `tests/SUITE/NAME.error.np` is expected to exit non-zero (syntax/runtime errors);
  there is no matching `.expected` file for these.
- To add a test: drop a new `.np`/`.expected` pair (or `.error.np` for a failure case) into
  a suite directory; a new milestone gets a new directory — no registration needed.

## Architecture

```
source → Lexer → Parser → AST → modules_resolve → classes_resolve_program → check_program ─┬→ interpret
                                                                         └→ ir_compile → ir_optimize → vm_run / cgen
```

- `src/value.h`/`value.c` — runtime `Value` (tagged union: `VAL_INT`, `VAL_ARRAY`,
  `VAL_STRING` or `VAL_OBJECT`). An `Object` points at its `AST_CLASS` declaration
  (`klass`, which outlives all objects) and holds `fields[]` in declaration order; this is
  why `value.c` includes `ast.h` (for class/field names when printing `Name(f=1, g="x")`). Arrays are heap-allocated and **reference-counted** with shared (Python/Java-style)
  semantics. Strings are ref-counted and immutable (`String` with a flexible `chars[]`,
  NUL-terminated, storing both byte `length` and character `count`; `string_char_at()` is
  O(1) when the string is all-ASCII, else a UTF-8 scan). `value_type_name()` includes the
  article ("a string") for use in error messages. Strings nested in a printed array are quoted. Ownership rule used throughout the interpreter: a function returning a `Value`
  hands over one reference; whoever stores or drops it must `value_release()` it
  (`value_retain()` to take an extra one). Self-containing arrays leak (no cycle collector).
- `src/token.h` — `Token`/`TokenType`. Tokens hold `start`/`length` slices into the original
  source buffer rather than copied strings, to avoid per-token allocation.
- `src/lexer.c` — hand-written scanner, one token at a time via `lexer_next_token()`. Tracks
  line/col for diagnostics (columns count UTF-8 characters, not bytes). `#` starts a line
  comment. String literals become `TOKEN_STRING` (slice includes the quotes); the lexer
  validates escapes and UTF-8 and, for those errors, sets `lexer->error_message`, which the
  parser's `advance()` reports instead of its generic "Unexpected character". The parser
  (`parse_string`) decodes escapes into the `AST_STRING` node, which owns its `chars`.
  Newlines are significant: `\n` is emitted as `TOKEN_NEWLINE` (statement terminator), so an
  expression cannot currently span lines. Keywords (`vana`, `yedi`, `natara`, `jabasamma`,
  `ko_lagi`, `in`, `kaam`, `farkau`, `class`, `yo`) get their own token types via the `keywords[]` table in `lexer.c` — add new
  keywords there.
- `src/parser.c` — recursive descent. Operator precedence is encoded by the *call chain*, not
  a precedence table: `parse_comparison` (non-associative `== != < <= > >=`; the entry point
  for full expressions) calls `parse_expr` (handles `+ -`) calls `parse_term` (handles `* / %`)
  calls `parse_factor` (unary sign, then `parse_postfix(parse_primary)`: numbers, strings, variables,
  calls, `[...]` array literals, parens, followed by any number of `[index]`, `.field` or `.method(args)` in `parse_postfix`). When adding new operators,
  insert a new precedence level as a new function in this chain rather than adding a table.
  `ko_lagi` range bounds use `parse_expr`, not `parse_comparison`; `ko_lagi x in EXPR {` with
  no `..` is a for-each over an array or string. Newlines are skipped inside `[ ]` array literals only.
  Statements: `parse_statements(parser, block, end)` is shared by the top level (`end` =
  `TOKEN_EOF`) and `{ }` blocks (`end` = `TOKEN_RBRACE`); a statement must be followed by a
  newline or the block's closing `}`. `natara` must follow `}` on the same line, since the
  parser has a single token of lookahead and newlines are tokens. `reject_keyword_as_name()`
  gives the "reserved keyword" error for things like `yedi = 5`. Only the first error is
  reported (`had_error` stops parsing). `advance()` reports `TOKEN_ERROR` (bad/non-ASCII
  characters) immediately. `parser->block_depth` rejects `kaam` inside any block;
  `parser->in_function` rejects `farkau` outside a function. An identifier followed by `(`
  is a call (`parse_call`). Statements starting with a name or `yo` go through
  `parse_assignment`, which parses a full postfix expression and then requires either `=`
  (target must be `AST_VARIABLE`/`AST_INDEX`/`AST_FIELD`) or that it was a call.
  `parse_class` parses the class body itself (fields and `kaam` methods; not via
  `parse_statements`), setting `parser->in_method` (allows `yo`) and, for `banau`,
  `parser->in_constructor`.
- `src/ast.h`/`ast.c` — every node has a `token` (source location for runtime errors; zeroed
  for nodes without one, e.g. `ast_new_placeholder()` used after parse errors). Tagged-union
  AST nodes (`AST_NUMBER`, `AST_STRING`, `AST_BINOP`, `AST_UNARY`,
  `AST_VARIABLE`, `AST_ASSIGN`, `AST_PRINT`, `AST_IF`, `AST_WHILE`, `AST_FOR`,
  `AST_FOR_EACH`, `AST_ARRAY`, `AST_INDEX`, `AST_INDEX_ASSIGN` (target is an `AST_INDEX`),
  `AST_FUNCTION`, `AST_CALL` (`receiver` non-NULL for `obj.m(args)`), `AST_RETURN` (value may
  be NULL), `AST_CLASS` (`AstField` fields + method `AST_FUNCTION`s), `AST_FIELD`,
  `AST_FIELD_ASSIGN` (target is an `AST_FIELD`), `AST_SELF` (`yo`), and `AST_BLOCK` — a growable statement array used both for `{ }` bodies and as the program
  root). `binop.op` is a `TokenType` (two-char operators don't fit a `char`); `unary.op` is
  still a `char`. Growable arrays (block statements, params, call args) share `grow_array()`
  in `ast.c`. An `else_branch` is either a block or a nested `AST_IF` (for `natara yedi`). Variable/assign nodes keep their name `Token` for error locations. `ast_free` recursively frees a tree; every node allocated by `ast_new_*` must
  eventually pass through it.
- `src/interpreter.c` — (Phase 2: all value operations, field/method lookup, builtins and runtime
  errors now go through `rt.c`, same as the VM/native backends; `Variable` carries `type`/`is_const`/`decl`;
  `interp->self_class` is the running method's declaring class, used for privacy and `super`; public API
  `interpreter_new/run/recover/free` for the REPL. The rest of this paragraph is the original design.)
  Tree-walking evaluator: `execute` runs statements, `eval` evaluates
  expressions, directly on the AST (no IR). Variables live in flat, linearly-searched
  `Scope`s (names are source slices) — the hashed symbol table is its own later milestone.
  `interp->locals` is the current call's scope (NULL at top level): reads check locals then
  globals; assignment inside a function always writes a local (spec §12 — functions never
  overwrite globals). No block scope: `ko_lagi` loop variables persist after the loop.
  Functions are registered in a pre-pass over top-level statements before execution (so
  call-before-define and mutual recursion work; duplicate names are reported there).
  Functions and variables are separate namespaces. `farkau` sets `interp->returning`, which
  makes blocks/loops stop until `call_function` clears it; no `farkau` → returns 0. Call
  depth is capped at `MAX_CALL_DEPTH` with a friendly error. `eval` returns an owned `Value`;
  `eval_int` / `eval_condition` enforce "must be a number". `eval_binop` sends any operation
  with a string operand to `eval_string_binop`; `AST_INDEX` / for-each branch on array vs
  string; `a[i] = v` on a string is a runtime error. Built-ins (`lambai`) are dispatched
  in `call_function` via `is_builtin()` and can't be redefined. Classes are registered in the
  same pre-pass as functions (`check_new_global_name` keeps functions, classes and builtins
  in one namespace). `interp->self` is the running method's object (borrowed; the caller
  holds the reference). `call_function` dispatches: receiver → `call_on_receiver`; builtin;
  method of `self`; class → `construct`; function. All of them run bodies via `invoke()`,
  which saves/restores `locals` and `self`. Bare-name reads/writes check
  `self_field_index()` (see Classes above). All runtime errors go through
  `runtime_error(interp, node->token, msg)` (→ `report_error` with caret + `exit(1)`).
- `src/diagnostics.c` — single shared `report_error()` used by the parser (and meant to be
  reused by future phases) to print beginner-friendly errors: file/line/column, the offending
  source line, and a `^` caret under the exact column, per the project's error-message
  philosophy (clear, located, non-cryptic — see also `--lang=ne`/`--lang=en` as a planned
  future CLI option for localized error text).
- `src/main.c` — wires the pipeline together: read file → init lexer → init parser → parse →
  bail out non-zero if `parser.had_error` → interpret → free.

`vana(...)` is still special-cased as a statement keyword in `parse_print()`, not a builtin
function — it can't be used inside an expression or redefined.

## Development approach for this project

This codebase is being built as a strict milestone sequence (see `README.md` roadmap:
variables → conditionals → loops → functions → arrays → structs/classes → symbol table →
type checker → modules → bytecode VM → IR → native codegen). When extending it:

- Implement one milestone/subsystem at a time; do not add features from a later milestone
  early (e.g. don't add floats or general function calls while working on variables).
- Every milestone must compile cleanly (`-Wall -Wextra -Wpedantic`, zero warnings) and pass
  `make test` before moving on. Add test cases under `tests/` for every new feature, including
  at least one `.error.np` case where relevant.
- Build with `make debug` (ASan/UBSan) while developing; the interpreter does manual
  `malloc`/`free` (see `ast_free`), so sanitizer runs are the main safety net.
- Keep new language keywords consistent with the project's Nepali/English mix — Nepali for
  control flow and everyday verbs (`yedi`, `natara`, `jabasamma`, `ko_lagi`, `kaam`, `farkau`,
  `banau`), English for more technical/structural concepts (`class`, `interface`, `import`).
  Don't blindly translate every keyword to Nepali.
