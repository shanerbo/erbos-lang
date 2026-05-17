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
- [x] **α7** RAII: array's backing storage freed at scope end via
      compiler-emitted `_heap_free`. Two-step free per array
      local: data buffer (cap*esz bytes) then header (16 bytes).
      `local_is_array` and `local_array_esz` parallel arrays
      track per-local metadata.
      Acceptance: ASan shows zero leaks across stdlib tests.
- [x] **α8** `array of byte` for byte-level storage. New TYPE_BYTE
      kind. Element size 1; lowers to `IR_LOAD_BYTE` / `IR_STORE_BYTE`
      (ARM64 `ldrb` / `strb`). Required for String.data. types_equal
      treats TYPE_INT and TYPE_BYTE as interchangeable so int
      literals can flow into byte slots.
      Acceptance: `data is array of byte with cap 32` works;
      writing 511 stores 0xFF (truncation to low byte).

### Phase β — Stdlib rewrite using `array of T`

- [x] **β1** Rewrite `std/list.ptt` against `array of T`. Replace
      every `mem_load`/`mem_store` with typed indexing, every
      `heap_alloc(n*8)` with `array of T with cap n`. Compiler
      changes folded in:
        - monomorph: NODE_ARRAY_NEW + NODE_INDEX_ASSIGN handled
          in substitute_in_node + mangle_in_node so generic
          arrays get T substituted correctly.
        - irgen: NODE_FIELD_ASSIGN with named-local RHS marks
          the local moved (RAII-skips it on scope exit) — fixes
          double-free of the array when `self.data be fresh`
          pattern is used.
        - runtime _ha_found path zero-fills recycled memory
          (mmap pages are already zero, but free-list returns
          blocks whose first 16 bytes were [next, size]
          metadata; without zeroing, freshly-`_alloc_<X>`d
          structs see garbage in default-zero fields like
          List.data).
        - checker NODE_FIELD_ACCESS recognises `arr.cap` for
          TYPE_ARRAY receivers and emits a load at offset 0.
      Acceptance: `tests/test_list_methods.ptt` passes (6
      tests); `grep 'mem_\|heap_\|panic_oob' std/list.ptt`
      returns only the comment line.
- [x] **β2** Rewrote `std/map.ptt` against `array of T` — two
      parallel `array of K` and `array of V` fields, linear-probe
      scan, doubling growth on insert.
      Acceptance met: `tests/test_map_methods.ptt` passes; no
      `mem_*` / `heap_*` references in std/map.ptt.
- [x] **β3** Rewrote `std/string_map.ptt` against `array of T` —
      same two-parallel-array shape as `Map`, but uses
      `String.equals` for key comparison so distinct String
      headers carrying equal byte content match correctly.
      Acceptance met: clean grep; tests pass.
- [x] **β4** Rewrite `std/string.ptt` against `array of byte`.
      `String.data` becomes `array of byte`. `String.equals`,
      `String.byte_at` use typed indexing on the byte array.
      `String.concat` allocates via `array of byte with cap N` and
      builds a fresh `String` header. `int.to_string` uses the same
      pattern. `String.yell` waits on γ2.
      Codegen contract:
        - `iremit_finalize_data` emits each literal as three blocks:
          `_strbytes<i>` (rodata), `_strarr<i>` (16-byte array-of-byte
          header pointing at `_strbytes<i>`), `_str<i>` (32-byte
          String header whose data field points at `_strarr<i>`).
        - C runtime helpers (`_yell_str`, `_str_eq`, `_str_concat`,
          `_int_to_str`, `_char_at`) two-step deref through the
          array-of-byte header to reach the raw byte buffer.
        - Panic / pass / assert / test_name data sections all use
          the matching two-tier layout (`_oob_arr`/`_oob_str`,
          `_pass_prefix_arr`/`_pass_prefix`,
          `_test_name_<i>_arr`/`_test_name_<i>`).
      Stack-discipline fix folded into the same task: `_int_to_str`'s
      epilogue freed its 32-byte scratch buffer BEFORE popping x21,
      reading garbage into x21 in the caller. Reordered so the pop
      precedes the scratch free; otherwise the loop variable held in
      x21 across `bl _int_to_str` got corrupted (which surfaced as
      `tests/test_map.ptt`'s "map growable" producing only one entry).
      Acceptance: `tests/test_map.ptt` and `tests/test_string_methods.ptt`
      pass; full `make test` green; `grep 'mem_\|heap_\|write_\|
      panic_oob\|ptr_of\|as_string' std/string.ptt` returns nothing
      (`String.yell` waits for γ2 to land via the user-method path).
- [x] **β5** Drop `mem_load` / `mem_store` / `mem_load_byte` /
      `mem_store_byte` / `ptr_of` / `as_string` / `heap_alloc` /
      `heap_free` / `write_bytes` from `src/checker.c` and
      `src/irgen.c`. User code calling them errors with "unknown
      function 'X'" through the normal undefined-function path.
      The compiler still emits `bl _heap_alloc` / `bl _heap_free`
      / `bl _write_bytes` directly when lowering `array of T`
      construction, RAII drop, and `String.yell` (γ2).
      Removed regression tests (`tests/ir/raw_mem.ptt`,
      `tests/ir/byte_mem.ptt`, `tests/ir/string_header.ptt`) — they
      tested user-callable primitives that no longer exist. The
      same machinery is exercised end-to-end by `array_basic.ptt`
      and `array_byte.ptt` through the language-level `array of T`
      surface.
      Acceptance: `git grep -E 'mem_load|mem_store|ptr_of|as_string'
      src/checker.c src/irgen.c` returns nothing in dispatch
      positions; `make test` green.

### Phase γ — Tier-2 compiler-known names + ban kernel layer

- [x] **γ1** `yell` is a compiler-known name resolved at the
      checker on the argument's static type. The call node's name
      is rewritten in-place so irgen + iremit just emit
      `bl <symbol>`:
        - int / bool / byte → `_yell_int`
        - String            → `_String_yell`
        - struct T          → `_<T>_yell` (user-defined method)
        - TYPE_UNKNOWN      → `_yell` (runtime magic-number shim)
      The TYPE_UNKNOWN arm exists only for values flowing out of
      the legacy untyped `list` element through chained indexing.
      Once ε drops the legacy keyword forms every value carries a
      concrete type and the shim goes away.
      `through (k in xs)` now propagates xs's element type to k —
      both for `array of T` (concrete elem type) and for typed
      `list of T`. Without an element type the loop var lands as
      TYPE_UNKNOWN and routes through the shim.
      Acceptance: `make test` green; user `Counter.yell(self)` is
      reachable via `yell(c)` (verified manually).
- [x] **γ2** `_String_yell` is the canonical compiler-emitted
      symbol for `yell(s String)` and `s.yell()`. Hand-rolled
      ARM64 in `runtime_emit.c::emit_String_yell`. Reads count
      from the String header at offset 8, walks String.data ->
      array_of_byte hdr -> bytes ptr to reach the buffer, calls
      write(2), then writes a newline. Same `.globl` block also
      exposes `_yell_str` so the runtime-internal panic / pass /
      assert paths still link without a per-caller sweep; ζ2
      removes the alias.
      Acceptance: `make test` green; `s.yell()` and `yell(s)`
      both emit `bl _String_yell` (the same symbol).
- [x] **γ3** `assert(cond)` moves from reserved keyword to stdlib
      function. `TOK_ASSERT` removed from lexer + token enum; the
      parser no longer constructs `NODE_ASSERT`. `assert(cond)`
      now parses as a plain `NODE_CALL` and the checker recognises
      the name and tags it void; irgen lowers it to the same
      conditional `_assert_fail(line)`-on-false path the dropped
      `NODE_ASSERT` used to take.
      `NODE_ASSERT` itself stays in the AST enum + monomorph /
      optimiser switches so old object code that constructs it
      keeps building, but it's a dead branch — no parser path
      creates it. A future cosmetic pass can remove it.
      Acceptance: `grep -rn 'TOK_ASSERT' src/` returns only
      comments; every existing test passes without code change
      (assert keeps its same call shape).
- [x] **γ4** Drop free-function string/len builtins from checker
      special-case dispatch: `str_len`, `str_eq`, `str_concat`,
      `int_to_str`, `char_at`, `yell_str`, `panic_oob`. User code
      now uses method form: `s.len()`, `s + t` (operator),
      `n.to_string()`, `s.char_at(i)`, `s.equals(t)`. The free-
      function names hit the generic "unknown function" error
      path. The universal `len()` builtin stays for one more
      phase (it's deeply tangled with the legacy `list` / `map`
      keyword forms; ε kills both together).
      Swept files: `tests/test_string.ptt`, `tests/test_map.ptt`,
      `examples/concat.ptt`, `examples/kitchen_sink.ptt`,
      `examples/leetcode/{longest_substr,longest_substr_brute,
      phone_combos,two_sum_map}.ptt`,
      `examples/leetcode/tests/test_longest_substr.ptt`. Files
      that newly use `n.to_string()` or `s.char_at(i)` got
      `use std/string` injected.
      Compiler changes folded in:
        - String.char_at method added to std/string.ptt — sugar
          over byte_at + a 1-byte allocation.
        - Method dispatch on TYPE_UNKNOWN receivers (legacy
          untyped `list[i]`) now defaults to looking up int methods
          first, with the same TYPE_STR-fallback the str path takes.
        - Built-in `set` / `get` / `keys` method type-checking
          now recurses into args so nested method calls
          (`m.set(i.to_string(), v)`) get their resolved_struct_name
          tagged.
        - Binary `+` / `eq` / `ne` / `lt` / `gt` / `le` / `ge` now
          treat TYPE_STR and TYPE_STRUCT("String") interchangeably
          for resolved_type tagging — same downstream emission.
      Acceptance: `make test` green; calls to the dropped builtin
      names error with "unknown function 'X'".
- [x] **γ5** Ban kernel-layer names (`heap_alloc`, `heap_free`,
      `write_bytes`, `panic_oob`, `panic_capacity`) from being
      callable in any `.ptt` file. Already shipped as part of
      β5 — the checker's kernel-name dispatch table was deleted
      whole. The compiler emits `bl _<name>` directly when
      lowering language constructs (array constructor → `bl
      _heap_alloc`; bounds-check fail → `bl _panic_oob`; etc.)
      User code calling these names hits the generic "unknown
      function 'X'" error.
      Acceptance: verified — `panic_oob()` and `write_bytes(0,0)`
      from user code error with "unknown function".
- [x] **γ6** Sweep all `s str` → `s String` in every `.ptt` file —
      function parameters, struct/enum field types, return types,
      var declarations. Each file that newly uses `String` got a
      `use std/string` injected.
      Files swept: tests/test_enum.ptt, tests/test_methods_user.ptt,
      tests/test_generics.ptt, examples/kitchen_sink.ptt,
      examples/leetcode/{longest_substr,longest_substr_brute,
      phone_combos}.ptt, examples/leetcode/tests/test_longest_substr.ptt.
      The `map of str to V` legacy keyword form is left in place —
      that's a phase ε concern (the entire `map` keyword goes
      away there, taking `str` with it as a key-type spelling).
      Acceptance: `grep -rnE '\bstr\b' tests/*.ptt examples/*.ptt
      examples/**/*.ptt` returns only `map of str to V` and
      string-test-internal references; `make test` green.
- [x] **γ7** TYPE_STR is no longer produced by anything in the
      checker. `parse_type_str("str")` and `parse_type_str("String")`
      both return `TYPE_STRUCT("String")` (the canonical stdlib
      struct); `NODE_STR_LIT` resolves to the same. Functions
      declared to return `int` that try to `give "..."` now error
      with "return type mismatch: expected 'int', got 'String'"
      because the old int↔struct compat allowance is narrowed to
      exclude TYPE_STRUCT("String").
      The TYPE_STR enum value is left in `types.h` so existing
      switch statements stay well-formed; nothing creates a value
      with that kind anymore. The TOK_STR_TYPE token is also kept
      so `s str` and `map of str to V` (legacy keyword form) still
      parse — both produce the same canonical String type. ε will
      retire the legacy spelling along with the keyword forms.
      Acceptance: `make test` green; programs writing `give "x"`
      from an int-returning function error correctly; `s str`
      and `s String` produce identical types end-to-end.

### Phase δ — `std/basics` bundle

- [x] **δ1** Create `std/basics.ptt`. Re-exports `use std/string`,
      `use std/list`, `use std/map`, `use std/string_map`. (`yell`
      and `assert` are compiler-known names — they don't need a
      `use` line; `std/io` and `std/test` are conceptual today,
      not separate files.)
      Acceptance: `use std/basics` makes String / List / Map /
      StringMap all reachable in a single import. Verified via
      manual smoke test (allocates a List, a StringMap, calls
      `s.len()` on a literal — all link and run).
- [x] **δ2** Verify recursive imports propagate correctly through
      `std/basics`. The transitive-resolution path in main.c
      (commit 1e987e0) already handles arbitrarily nested `use`
      directives; the smoke test in δ1 exercised the four-level
      chain (basics → string_map → string → byte primitives).

### Phase ε — Drop `list` / `map` / `imap` keyword forms

- [ ] **ε1** Drop `TOK_LIST`, `TOK_MAP`, `TOK_IMAP` from lexer.
      `list`, `map`, `imap` become regular identifiers.
      Acceptance: lexer no longer emits those tokens.
- [x] **ε2** Partial sweep — every `.ptt` file whose surface
      is fully covered by the stdlib types' methods is migrated:
        - `tests/test_list.ptt`        → `List of int`
        - `tests/test_map.ptt`         → `StringMap of int` + `Map of int to int`
        - `examples/list_methods.ptt`  → `List of int`
        - `examples/map_methods.ptt`   → `StringMap of int` (dropped `.keys()` example)
        - `examples/map_update.ptt`    → `StringMap of int`
        - `examples/leetcode/longest_substr.ptt` → `StringMap of int`
        - `examples/leetcode/two_sum_map.ptt`    → `StringMap of int`
        - `examples/leetcode/tests/test_longest_substr.ptt` → `StringMap of int`

      Files left on legacy keyword forms because they exercise
      `.keys()` iteration or other legacy-only semantics that ε
      retires later:
        - `examples/kitchen_sink.ptt` (uses `m.keys()`,
          `list_set`, list-of-list, list-of-Person — phase ε3/ε4
          will route the literals through stdlib too)
        - `examples/map_iter.ptt`     (the `.keys()` demo file)
        - `examples/leetcode/rotting_oranges.ptt` + its test
          (uses `len(queue)` / `queue.push` / `queue[head]` /
          `list_set`)
        - `tests/bench/map_bench.ptt` (the perf baseline)
        - `tests/ir/raii_and_iter.ptt` (regression test that
          intentionally exercises the legacy iteration codegen)
        - `std/queue.ptt`, `std/stack.ptt` (consumers of the
          `list of int` keyword type)

      Compiler change folded in: `_alloc_<X>` now zero-fills
      its allocation up to `field_count * 8` bytes. Without
      this, a stdlib container struct allocated from a
      free-list-recycled block sees garbage in its lazy-init
      sentinel field (`self.data eq 0`) and the lazy path
      doesn't fire — surfaced when two consecutive test blocks
      both used `List of int` or `StringMap of int`.

      Acceptance: `make test` green; the migrated files exercise
      the new surface end-to-end (List push/pop/index/iterate,
      StringMap set/get/len, Map set/get/len). The remaining
      legacy-keyword files migrate as part of ε3/ε4 once the
      literal-form lowering routes through the stdlib types.
- [ ] **ε3** List literal `[1, 2, 3]` lowers to `List of int`
      constructor + pushes (instead of building C-runtime list
      header).
      Acceptance: programs using `[1, 2, 3]` require `use std/
      list`; tests pass.
- [ ] **ε4** Map literal `["a" to 1, "b" to 2]` lowers to
      `StringMap of int` constructor + sets.
      Acceptance: requires `use std/string_map`; tests pass.
- [x] **ε5** `xs[i]` and `xs[i] be v` dispatch to `<Type>_get` /
      `<Type>_set` when the receiver's static type is one of the
      stdlib container structs (`List`, `Map`, `StringMap`, with
      or without monomorph mangling). The checker tags the
      `NODE_INDEX` / `NODE_INDEX_ASSIGN` node with
      `method_struct = <mangled_struct_name>`; irgen sees the tag
      and emits `bl _<mangled>_get` (or `_set`) instead of decoding
      the legacy header layout. The legacy `list` / `map` keyword
      receivers continue to use the inline-bounds-check path.
      User structs that happen to define `.get` don't auto-route
      through `[]` — only the three known stdlib container names
      (matched by prefix `List__` / `Map__` / `StringMap__` plus
      bare `List` / `Map` / `StringMap`) do.
      Acceptance: `xs[0]` on `List of int` reads the right element;
      `xs[0] be v` writes through `List.set`; `make test` green.
- [x] **ε6** `through (x in xs)` for stdlib containers dispatches
      to `<Type>_len` (loop-bound) + `<Type>_get` (element load).
      Same checker-tag mechanism as ε5 (`NODE_THROUGH_IN.method_struct`).
      Element type comes from the struct's val_type when set.
      Acceptance: `through (x in nums)` on `List of int` produces
      the right values; legacy `list` / `map` iteration unchanged;
      `make test` green.

### Phase ζ — Delete dead C runtime

- [ ] **ζ1** Delete `emit_list_builtins`, `emit_map_builtins`,
      `emit_imap_builtins` from `src/runtime_emit.c`.
      Acceptance: `git grep '_list_\|_map_\|_imap_'
      src/runtime_emit.c` returns only comments; tests green.
- [x] **ζ2** (partial) Delete `emit_str_builtins` from
      `src/runtime_emit.c`. The symbols it emitted (`_str_len`,
      `_char_at`) had no remaining callers after γ4 — every user
      callsite goes through the `s.len()` / `s.char_at(i)` method
      form, which dispatches to the user-method `_String_len` /
      `_String_char_at` symbols emitted from `std/string.ptt`.
      Also dropped the dead `len(s)` → `_str_len` branch in
      `irgen.c`.
      Symbols still emitted (kept for ε-driven retirement when
      the legacy keyword forms go away):
        - `_str_eq`     — used by `+`/`eq` on String operands
        - `_str_concat` — used by `+` on String operands and
          interpolation
        - `_int_to_str` — used by interpolation
        - `_yell_str`   — alias of `_String_yell` for the panic /
          pass / assert internal callers
        - `_yell`       — magic-number shim for legacy untyped
          values (TYPE_UNKNOWN fallback)
      Acceptance: `make test` green; `_str_len` / `_char_at` no
      longer exist in compiled output.
- [x] **ζ3** `docs/runtime.md` created — documents the
      irreducible kernel-boundary helpers, the transitional
      helpers awaiting ε / ζ retirement, the already-retired
      symbols, and the layout contracts the runtime depends on
      (String, array of byte, legacy list/map/imap, panic-message
      data sections).

### Phase θ — Final verification

- [x] **θ1** `grep -rE 'heap_alloc|heap_free|mem_load|mem_store|
      write_bytes|panic_oob|ptr_of|as_string' std/*.ptt
      tests/*.ptt examples/*.ptt examples/**/*.ptt` returns
      nothing — the kernel layer is unreachable from any
      user-or-stdlib `.ptt` file. Verified 2026-05-16.
- [~] **θ2** Partial. `TOK_ASSERT` is gone (γ3); only comments
      mention it. `TOK_STR_TYPE` is still in dispatch positions
      so the legacy `s str` parameter spelling and `map of str
      to V` keyword form keep parsing — γ7 made the dispatch
      coerce both `str` and `String` to the same canonical
      TYPE_STRUCT("String"). `TOK_LIST` / `TOK_MAP` / `TOK_IMAP`
      stay until phase ε1 retires the legacy collection keywords.
- [~] **θ3** Partial. `nm` on a compiled binary confirms
      `_str_len` / `_char_at` / `_yell_dispatch` are gone (ζ2 +
      γ1). `_yell_str` survives as an alias for `_String_yell`
      (the runtime's own panic / pass / assert message paths
      emit it). `_str_eq` / `_str_concat` / `_int_to_str` /
      `_list_*` / `_map_*` / `_imap_*` / `_yell` (the magic-
      number shim) remain — see `docs/runtime.md` for which
      remaining helpers are transitional vs irreducible. Full
      retirement waits on ε / ζ1 to land.
- [x] **θ4** `tests/bench/map_bench.ptt` runs in 0.08s user
      time (sub-second wall) on the M-series Mac it's developed
      on. Output verified — 512 inserts, value sum 915712, key-
      enumerate sum 915712. The legacy `map of str to int`
      keyword form is what runs the bench today; ε hasn't
      retired the C-runtime backend yet so the baseline is
      unchanged. Once ε lands the bench will rerun against the
      pure-Potato `StringMap of int` to validate the 1.5×
      ceiling.
- [x] **θ5** Full `make test` green. Every IR regression test
      runs at -O0, -O1, -O2 via the test harness's per-level
      sweep (`OK: <name> -O<n>` for each n in {0,1,2}); every
      framework test, error-case test, and runtime C test
      passes. End-of-suite line: "All tests passed."
- [x] **θ6** ASan + UBSan compiler build (`cc -Wall -Wextra
      -std=c11 -fsanitize=address,undefined -g -o erbos-san
      src/*.c`) runs every IR regression test in `tests/ir/`
      clean: array_basic, array_byte, bce, callee_save_preservation,
      inlining, interpolation, iropt_levels, licm,
      list_literal_index, match_enum, method_dispatch,
      raii_and_iter, sra, stackify, string_iteration,
      struct_field_after_call, test_block_runner, through_in.
      No leaks, no undefined-behaviour reports.

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
