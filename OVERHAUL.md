# Potato language overhaul — execution plan

**Status:** in progress. Started 2026-05-16 after the audit
conversation. This file is the persistent source of truth — if a
session dies mid-flight, the next session reads this file and
picks up at the first unchecked task.

## Goal

Eliminate every C-like primitive, free-function builtin, and
runtime magic-dispatch from user-visible Potato. After the
overhaul:

- No `mem_load` / `mem_store` / `heap_alloc` / `heap_free` /
  `write_bytes` / `panic_oob` callable from any `.ptt` file
  (user OR stdlib). They're compiler-internal symbols only.
- No `str_*` / `int_to_str` / `char_at` / `yell_str` / `len` /
  `list_*` / `map_*` / `imap_*` free-function builtins. Every
  operation is either a method on a stdlib type or a
  compile-time-resolved name like `yell` / `assert`.
- No runtime type-dispatch. `yell` and `len` resolve at compile
  time on the argument's static type.
- `list of T` / `map of K to V` / `imap of int to V` keyword
  forms are gone. The only spelling is `List of T` / `Map of K
  to V` / `Map of int to V` / `StringMap of V`, requiring the
  matching `use std/...`.
- `assert` is a stdlib function in `std/test`, not a reserved
  keyword.
- `spark` is a reserved keyword (it's the entry-point contract).
- Bundle import is `use std/basics`.
- Stdlib `.ptt` files read like ordinary Potato — no `mem_*` or
  `heap_*` calls visible. Allocation and indexing happen via the
  new `array of T` typed-storage primitive.
- The C runtime contains exactly the irreducible kernel-boundary
  symbols: `_heap_alloc`, `_heap_free`, `_yell_int`,
  `_String_yell`, `_write_bytes`, `_panic_oob`, `_panic_capacity`,
  `_assert_fail`, plus per-struct `_alloc_<X>` constructors.

## Locked design decisions

These were resolved during the audit and don't get reopened.

1. **Bundle name:** `std/basics` (not `std/prelude` / `std/all` /
   `std/core`). User opts in via one line: `use std/basics`.
2. **`yell` overloading:** `yell(x)` resolves at compile time on
   `x`'s static type. Compiler ships `yell(int)` (built-in)
   and `yell(String)` (via `_String_yell`). Users overload by
   defining `Type.yell(self Type) { ... }` and the same call
   `yell(c)` resolves to it.
3. **No auto-import:** every program that wants a stdlib type
   writes the matching `use std/...` line. No magic loading.
4. **No C-like primitives in `.ptt` files:** unless we're
   touching the kernel (and the kernel-touching code lives in
   the compiler runtime in raw asm, not in any `.ptt` file),
   there is no `mem_*` / `heap_*` / `write_bytes` / `ptr_of`
   visible anywhere.
5. **`str` is gone, full stop.** No backward compatibility, no
   sugaring. The `TOK_STR_TYPE` token is dropped from the lexer.
   The `TYPE_STR` kind is dropped from the checker. Every `s str`
   parameter / field / return type in every `.ptt` file is swept
   to `s String`. Programs that want strings write `use std/string`
   and use the `String` struct, exactly like `List` and `Map`.
6. **Method-style is the canonical surface:** `s.len()` /
   `xs.push(v)` / `m.set(k, v)`. No `len(s)` free function.
7. **`task` runtime stays as scaffolding:** the green-thread
   runtime is a separate effort. `TOK_TASK` and `_task_*` no-ops
   are not touched in this overhaul.
8. **Test framework:** `test "name" { ... }` blocks stay
   language-level (parallel to `spark { ... }`). `assert(cond)`
   moves to `std/test` as a function.

## Phase ordering

Each phase is independent enough that tests stay green at every
commit boundary. If a phase has multiple sub-tasks, they ship
as separate green commits.

```
α0 — reserve `spark` as a keyword
α  — typed arrays (`array of T` language primitive)
β  — stdlib rewrite using arrays
γ  — compiler-known names (`yell`, `assert`) + ban kernel layer
δ  — std/basics bundle
ε  — drop `list`/`map`/`imap` keyword forms
ζ  — delete dead C runtime
θ  — final verification
```

## Task checklist

Each entry: `[ ]` pending, `[~]` in progress, `[x]` done. Update
on completion. Don't redo a checked task.

### Phase α0 — Reserve `spark`

- [x] **α0.1** Add `TOK_SPARK` to lexer; map `spark` keyword to it.
- [x] **α0.2** Update parser entry-point detection to use `TOK_SPARK`
      instead of identifier matching.
- [x] **α0.3** Verify existing `spark { ... }` programs still work.
      Acceptance: every test passes; user code that defines a
      function named `spark` errors with "reserved keyword."

### Phase α — Typed arrays (`array of T`)

- [x] **α1** Parser: type-form `array of T`. Recognised in struct
      fields, function parameters, return types, var declarations.
      Internally encoded as `array<T>` (legacy mangled form).
      Monomorph skips `array<T>` (it's a built-in, not a user
      template).
      Acceptance: struct field `data array of int` parses.
- [x] **α2** Parser: constructor `array of T with cap N`. `N` is
      any integer expression. Parses to `NODE_ARRAY_NEW`.
      Acceptance: `xs is array of int with cap 8` parses.
- [x] **α3** Checker: `TYPE_ARRAY` with `elem_type` field. Type
      checking respects the element type. `parse_type_str` handles
      both `array<T>` (pre-monomorph) and `array__T` (post-monomorph)
      forms. types_equal compares element types.
      Acceptance: passing `array of int` where `array of str`
      expected errors.
- [x] **α4** Irgen: `array of T` constructor lowers to two heap
      allocations: 16-byte header (cap @ 0, data ptr @ 8) and
      cap*8 bytes of data. User Potato never names `_heap_alloc`.
      Acceptance: AST `NODE_ARRAY_NEW` produces correct IR.
- [x] **α5** Irgen: typed indexing `arr[i]` reads. Bounds-checked
      against `cap`. Layout selected via `is_array` flag set by
      checker (array: cap@0, data@8; legacy list: cap@0, count@8,
      data@16). Element-size offset compiler-synthesised.
      Acceptance: `arr[3]` reads element; `arr[-1]` and `arr[cap]`
      both panic.
- [x] **α6** Irgen: typed indexing `arr[i] be v` writes via new
      NODE_INDEX_ASSIGN AST node. Same bounds-check + layout
      selection as α5.
      Acceptance: write-then-read round-trips; OOB writes panic.
- [ ] **α7** RAII: array's backing storage freed at scope end via
      compiler-emitted `_heap_free`.
      Acceptance: ASan shows zero leaks across stdlib tests.
- [ ] **α8** `array of byte` for byte-level storage. Element size 1;
      lowers to `ldrb` / `strb`. Required for `String.data`.
      Acceptance: `data is array of byte with cap 32` works.

### Phase β — Stdlib rewrite using `array of T`

- [ ] **β1** Rewrite `std/list.ptt` against `array of T`. Replace
      every `mem_load`/`mem_store` with typed indexing, every
      `heap_alloc(n*8)` with `array of T with cap n`.
      Acceptance: `tests/test_list_methods.ptt` passes; `grep
      'mem_\|heap_' std/list.ptt` returns nothing.
- [ ] **β2** Rewrite `std/map.ptt` against `array of T`. Two
      parallel arrays (keys, values).
      Acceptance: `tests/test_map_methods.ptt` passes; clean grep.
- [ ] **β3** Rewrite `std/string_map.ptt` against `array of T`.
      Acceptance: clean grep; tests pass.
- [ ] **β4** Rewrite `std/string.ptt` against `array of byte`.
      `String.data` becomes `array of byte`. `String.equals`,
      `String.byte_at`, `String.char_at` use typed indexing.
      `String.concat` allocates via `array of byte with cap N`.
      `String.yell` waits on γ2.
      Acceptance: `tests/test_string_methods.ptt` passes; `grep
      'mem_\|heap_\|write_' std/string.ptt` returns only the
      `String.yell` line which γ fixes next.
- [ ] **β5** Drop `mem_load` / `mem_store` / `mem_load_byte` /
      `mem_store_byte` / `ptr_of` / `as_string` from
      `src/checker.c` and `src/irgen.c`. Compiler errors on user
      code calling them.
      Acceptance: `git grep mem_load src/checker.c` returns
      nothing in dispatch positions; tests green.

### Phase γ — Tier-2 compiler-known names + ban kernel layer

- [ ] **γ1** `yell` becomes a compiler-known name resolved at
      compile time on argument type. Drop runtime
      `_yell_dispatch` magic-number heuristic.
      `yell(int)` → `bl _yell_int`. `yell(String)` → `bl
      _String_yell`. `yell(c : Counter)` → `bl _Counter_yell`
      (user-defined). No match → error "no `yell` defined for
      type X."
      Acceptance: `nm` shows direct symbol calls; no
      `_yell_dispatch` symbol remains.
- [ ] **γ2** `_String_yell` becomes a compiler-emitted symbol
      (hand-rolled ARM64): reads count + data from String header,
      calls `_write_bytes`, writes newline. `String.yell` user
      method (in std/string.ptt) is the source of the symbol;
      compiler emits it via the normal method path.
      Acceptance: `s.yell()` and `yell(s)` both emit `bl
      _String_yell`.
- [ ] **γ3** `assert(cond)` moves from reserved keyword to stdlib
      function. Drop `TOK_ASSERT` from lexer; drop
      `NODE_ASSERT_STMT` from AST. `assert` becomes a
      compiler-known name (same machinery as `yell`); requires
      `use std/test`. Lowering: `cond ne true ?{
      _assert_fail(LINE) }`.
      Acceptance: every existing `tests/test_*.ptt` works after
      adding `use std/test`; `grep TOK_ASSERT src/` returns
      nothing.
- [ ] **γ4** Drop free-function builtins from checker special-case
      dispatch: `len`, `str_len`, `str_eq`, `str_concat`,
      `int_to_str`, `char_at`, `yell_str`. User code uses
      method-style: `s.len()`, `s.equals(t)`, `s + t` (operator),
      `n.to_string()`, `s.char_at(i)`, `s.yell()`.
      Sweep all `.ptt` files to use method form.
      Acceptance: `git grep '"len"\|"str_len"\|"str_eq"' src/
      checker.c` returns nothing; tests pass after sweep.
- [ ] **γ5** Ban kernel-layer names (`heap_alloc`, `heap_free`,
      `write_bytes`, `panic_oob`, `panic_capacity`) from being
      callable in any `.ptt` file. Drop them from checker's
      known-name dispatch. Compiler emits `bl _<name>` directly
      when lowering language constructs (`array of T` ctor →
      `bl _heap_alloc`; bounds-check fail → `bl _panic_oob`).
      Acceptance: `git grep '"heap_alloc"\|"write_bytes"\|"panic_
      oob"' src/checker.c src/irgen.c` returns nothing in
      dispatch positions; user/stdlib code calling those names
      errors with "undefined function."
- [ ] **γ6** Sweep all `s str` → `s String` in every `.ptt` file
      (parameters, fields, return types, var declarations). Add
      `use std/string` to every file that referenced `str`.
      Affected files: ~20 in `examples/`, `tests/`, `std/`.
      Acceptance: `grep -E '\\b(str)\\b' tests/ examples/ std/`
      returns nothing relevant; tests still pass.
- [ ] **γ7** Drop `TOK_STR_TYPE` from the lexer and `TYPE_STR`
      from the checker. The token and type kind are gone. String
      type is referenced exclusively as `String` (the struct from
      `std/string`).
      Acceptance: `grep -E 'TOK_STR_TYPE|TYPE_STR\\b' src/`
      returns nothing; tests pass; programs that say `s str`
      error with "unknown type 'str'; did you mean 'String' (and
      `use std/string`)?".

### Phase δ — `std/basics` bundle

- [ ] **δ1** Create `std/basics.ptt`. Re-exports: `use std/io`,
      `use std/string`, `use std/list`, `use std/map`, `use std/
      string_map`, `use std/test`.
      Acceptance: `use std/basics` makes String / List / Map /
      StringMap / yell / assert all available without other
      `use` lines.
- [ ] **δ2** Verify recursive imports propagate correctly through
      `std/basics`. (Already implemented in main.c since commit
      1e987e0.)

### Phase ε — Drop `list` / `map` / `imap` keyword forms

- [ ] **ε1** Drop `TOK_LIST`, `TOK_MAP`, `TOK_IMAP` from lexer.
      `list`, `map`, `imap` become regular identifiers.
      Acceptance: lexer no longer emits those tokens.
- [ ] **ε2** Sweep all `.ptt` files. `xs is list of int` →
      `xs is List of int` (with `use std/list`). `m is map of
      str to int` → `m is StringMap of int` (with `use
      std/string_map`). `m is imap of int to V` → `m is Map of
      int to V` (with `use std/map`).
      Affected files: ~18 in `examples/`, `tests/`, `std/`.
      Acceptance: every existing test passes; `grep -E 'list of
      |map of|imap of' tests/ examples/ std/` returns nothing
      (only `array of` matches remain).
- [ ] **ε3** List literal `[1, 2, 3]` lowers to `List of int`
      constructor + pushes (instead of building C-runtime list
      header).
      Acceptance: programs using `[1, 2, 3]` require `use std/
      list`; tests pass.
- [ ] **ε4** Map literal `["a" to 1, "b" to 2]` lowers to
      `StringMap of int` constructor + sets.
      Acceptance: requires `use std/string_map`; tests pass.
- [ ] **ε5** `xs[i]` and `xs[i] be v` dispatch to `List.get` /
      `List.set` (or `Map.get` / `Map.set` / `StringMap.get` /
      `StringMap.set`). Drop hardcoded `_list_get` /
      `_list_set` lowering.
      Acceptance: existing indexing callsites work.
- [ ] **ε6** `through (x in xs)` dispatches to `List.len` +
      `List.get`. Drop hardcoded `_list_len` call in iteration
      lowering.
      Acceptance: existing iteration tests pass.

### Phase ζ — Delete dead C runtime

- [ ] **ζ1** Delete `emit_list_builtins`, `emit_map_builtins`,
      `emit_imap_builtins` from `src/runtime_emit.c`.
      Acceptance: `git grep '_list_\|_map_\|_imap_'
      src/runtime_emit.c` returns only comments; tests green.
- [ ] **ζ2** Delete `emit_str_eq`, `emit_str_concat`,
      `emit_str_builtins`, `emit_int_to_str`, `emit_yell_str`,
      `emit_yell_dispatch` from `src/runtime_emit.c`.
      Acceptance: `git grep '_str_\|_yell_str\|_yell_dispatch
      \|_int_to_str' src/runtime_emit.c` returns nothing; tests
      green.
- [ ] **ζ3** Update `docs/runtime.md` (create if missing) to
      document the final irreducible runtime list:
      `_heap_alloc`, `_heap_free`, `_yell_int`, `_String_yell`,
      `_write_bytes`, `_panic_oob`, `_panic_capacity`,
      `_assert_fail`, plus per-struct `_alloc_<X>`.

### Phase θ — Final verification

- [ ] **θ1** `grep -rE 'heap_alloc|heap_free|mem_load|mem_store
      |write_bytes|panic_oob|ptr_of|as_string' std/*.ptt
      tests/*.ptt examples/**/*.ptt` returns nothing.
- [ ] **θ2** `grep -E 'TOK_LIST|TOK_MAP|TOK_IMAP|TOK_ASSERT'
      src/parser.c src/checker.c` returns nothing in dispatch
      positions.
- [ ] **θ3** `nm` on a compiled binary shows only the irreducible
      runtime set + user/stdlib symbols. No `_str_*` / `_list_*`
      / `_map_*` / `_imap_*` / `_yell_str` / `_yell_dispatch` /
      `_int_to_str` / `_char_at`.
- [ ] **θ4** `tests/bench/map_bench.ptt` runs within 1.5× of the
      pre-rewrite baseline (commit `c0e8c09`).
- [ ] **θ5** Full `make test` green at -O0, -O1, -O2.
- [ ] **θ6** ASan + UBSan build runs every IR test clean.

## How to resume after a crash

1. Read this file top-to-bottom. The locked decisions section
   tells you what NOT to redesign. The audit conclusions are
   final.
2. Find the first `[ ]` task (or `[~]` in-progress). That's
   where work resumes.
3. Run `make test` first to confirm the build is green at the
   resume point. If not, the previous session left a partial
   commit; check `git log --oneline -5` and reconcile.
4. Do the next task in isolation. Tests must be green after
   each task before moving on.
5. Mark the task `[x]` and commit the change to this file as
   part of the same commit that lands the work. That way `git
   log` and the checklist stay in lockstep.
6. **Don't redo `[x]` tasks.** They shipped. If they look
   wrong, the right move is a new task, not an undo.

## Resume-critical files

If something looks wrong, these are the files most likely
involved per phase:

| Phase | Files |
|---|---|
| α0 | `src/lexer.c`, `src/lexer.h`, `src/parser.c` |
| α  | `src/parser.c`, `src/ast.h`, `src/checker.c`, `src/irgen.c`, `src/iremit.c`, `src/ir.h` |
| β  | `std/list.ptt`, `std/map.ptt`, `std/string_map.ptt`, `std/string.ptt`, `src/checker.c`, `src/irgen.c` |
| γ  | `src/checker.c`, `src/irgen.c`, `src/runtime_emit.c`, `src/lexer.c`, `src/parser.c`, every `.ptt` file (sweep) |
| δ  | `std/basics.ptt` (new), `src/main.c` (recursion already in place) |
| ε  | `src/lexer.c`, `src/parser.c`, `src/irgen.c`, every `.ptt` file (sweep) |
| ζ  | `src/runtime_emit.c`, `docs/runtime.md` (new) |

## Open trade-off note

If a task hits a real blocker (e.g. typed arrays force a
non-trivial regalloc change), document the blocker on the task
line itself rather than skipping. Example:

```
- [~] α5 Irgen: typed indexing `arr[i]` reads.
      BLOCKER: regalloc doesn't handle the new IR_ARR_LOAD
      opcode's spill semantics. See src/regalloc.c:182.
      Next step: add IR_ARR_LOAD to compute_liveness's special-
      case list.
```

The block-list survives the session and the next session sees
the actual problem instead of starting cold.
