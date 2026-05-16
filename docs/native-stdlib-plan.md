# Plan: native `std/map` (and the language work it requires)

## Status

| Phase | Description | Status |
|-------|-------------|--------|
| P0 | IR-backend stack-frame heap bug; methods spike | ✅ done |
| P1 | Methods on user types (full) | ✅ done |
| P2 | Per-struct field resolution | ✅ done |
| P3 | Generics + monomorphization | ✅ done |
| P4.1 | Fix IR heap-corruption bug (subsumed by P0) | ✅ done |
| P4.2 | Cross-block, call-aware register allocation | ✅ done |
| P4.3 | Switch IR to default backend; retire direct codegen | ✅ done (P4.3a–P4.3g) |
| P5.0 | `src/iropt.c` scaffold + `-O0`/`-O1`/`-O2` flag wiring | ✅ done |
| P5.1 | Aggressive inlining (loops, branches, locals) | ✅ done |
| P5.2 | Scalar replacement of aggregates (SRA) | ✅ done |
| P5.3 | Escape analysis + stack allocation | ✅ done |
| P5.4 | Bounds-check elimination (lower-bound; upper-bound deferred) | ✅ done |
| P5.5 | Loop-invariant code motion (LICM) | ✅ done |
| P6.0 | Raw memory primitives callable from Potato | ✅ done |
| P3.1 | Switch generics syntax from `<>` to `of`/`to` | ✅ done |
| P3.3a | Byte-level mem primitives + write_bytes | ✅ done |
| P3.4 | String literals lower to `String` struct value | ✅ done |
| P6.0b | Pure-Potato `std/string.ptt` (String + methods) | ✅ done |
| P6.1 | Pure-Potato `std/list.ptt` (`List of T`) | ✅ done |
| P6.2 | Pure-Potato `std/map.ptt` (`Map of int to V`) | ✅ done |
| P3.5 | Operator dispatch (`+`, `eq`) routes to user methods | deferred |
| P6.3 | Drop `list`/`map`/`imap` keywords; route literals to stdlib | deferred |
| P6.4 | Delete C-emitted list/map/imap builtins | deferred |
| P3.3b | Delete C string runtime | deferred |

The deferred items are coherent follow-ups that don't change
end-user capability — they only collapse the two parallel
surfaces (keyword form like `list of int` and stdlib form like
`use std/list; List of int`) into one. They're safe to land
incrementally in a future session.

After P3 the language can *express* `Map<K, V>` as pure Potato. P4 and P5
are the performance work that earns parity with the C-emitted built-in
collections; P6 is the payoff.

> **Note on the body below.** The phase descriptions starting at "Phase 1
> — Methods that dispatch to user code" reflect the *original* plan as
> written before any phase shipped, including references to file paths
> (`src/codegen.c:399-432`, etc.) that no longer exist after P4.3g
> retired the direct codegen. The body is preserved as a planning
> record; the status table above is the source of truth for what's
> currently shipped. Each P5.x commit adds an entry to the dispatch
> table in `src/iropt.c`.

## Goal

Replace the C-emitted `_map_*` builtins with a **pure Potato** implementation
in `std/map.ptt` that, after compilation, runs at performance parity (within
~1.2× — measured) with today's `_map_set` / `_map_get` / `_map_keys`. The
same machinery should let `std/list`, `std/imap`, `std/queue`, `std/stack`,
and future user collections be just-as-fast Potato code.

> **Rule:** no shortcuts that paper over missing language features. Every
> phase ends with the test suite green and no `// TODO: hack` comments left
> behind.

## Why this is hard right now

The existing direct codegen has hand-tuned register allocation per built-in
(`_map_set` keeps key/value/count/data-ptr in `x20`–`x23` across calls).
A user-Potato `Map.set` going through today's compiler would:

- box every local through a stack slot (no cross-block reg alloc),
- pay a `bl` for every method call (the inliner only handles single-give
  leaf functions),
- run a second `bl` for key equality (only `_str_eq` / int-cmp are
  built in; user code can't define `eq`),
- never specialize on `K` and `V` (no generics — `list of int` and `map
  of str to int` are hard-coded keywords).

So "make it as fast as the built-in" is really "build the compiler the
built-in deserves." Roughly six pieces of work, in order. Skipping any
one of them turns into duct tape later.

## Phase ordering (each phase ships behind a green `make test`)

```
P1 methods on user types          ──▶ unblocks Map.set syntactically
P2 per-struct field resolution    ──▶ correctness prerequisite for everything
P3 generics + monomorphization    ──▶ Map of K to V can be defined
P4 fix IR backend + cross-block regalloc ──▶ removes stack-slot tax
P5 inlining + escape analysis + BCE
                                  ──▶ erases call/alloc/check overhead
P6 rewrite std/map in Potato + benchmark
```

P4 and P5 are the real performance work. P1–P3 are the language work that
gates *being able to write* the implementation. P6 is the payoff.

---

## Phase 1 — Methods that dispatch to user code

### Problem
Today every `obj.method(args)` on a recognised method name (`push`, `pop`,
`set`, `get`, `keys`, `len`, `fire`, `collapse`) is intercepted in
`src/codegen.c:399-432` and lowered to a fixed `_list_*` / `_map_*` /
`_task_*` symbol. A user-defined `set` on their own type is shadowed.

### What ships
- A method definition syntax. Smallest viable form: a function whose first
  parameter is `self ref Type` (or `self Type`) registers it as a method on
  `Type`.
  ```
  Map.set(self ref Map, key str, value int) {
    self.count be self.count + 1
  }
  ```
  Driver: `parser.c` — the `IDENT.IDENT(...)` opener already parses; thread
  the receiver type onto `func_def`.
- New `func_def` field: `char *receiver_type` (NULL for free functions).
- Checker: `find_func` → `find_method(receiver_type, name)`. Method
  resolution: prefer user method on the static type of the receiver; fall
  back to free function; built-ins lose their unconditional shortcut.
- Codegen: dispatch order in `NODE_METHOD_CALL` becomes:
  1. enum constructor (existing),
  2. user method on the static struct type,
  3. import-alias call (existing),
  4. built-in (`_list_push`, `_map_set`, …) **only if** the receiver is the
     literal built-in type (`list`, `map`, `imap`, `task`).
- Mangling: `_Type_method`. Today's `_method` collisions go away.

### Acceptance
- A `tests/test_methods_user.ptt` file defines a `Counter` struct with
  `bump(self ref Counter)` and `value(self Counter) int`. `make test`
  passes.
- The existing built-in `m.set(k, v)` continues to lower to `_map_set` (no
  user method shadowing) — kept by `Map`/`List` etc. being reserved type
  names until P3.

### Files touched
`src/ast.h`, `src/parser.c`, `src/checker.c`, `src/codegen.c`,
`docs/language-guide.md`, `docs/keywords.md`.

---

## Phase 2 — Per-struct field resolution (existing item A1)

### Problem
`src/codegen.c:464-495` (field load) and `src/codegen.c:632-642` (field
assign) do a global "first struct with a field of this name" search when
the receiver's struct name isn't statically known. With two structs sharing
a field name at different offsets, the wrong field is silently loaded.

The checker already tags `field_access.struct_name` when it can prove the
type — codegen just isn't strict.

### What ships
- Promote the global-name fallback to a **compile error**: "ambiguous field
  `count` on receiver of type `int` (could be `Map.count` or
  `List.count`); annotate the variable with an explicit type".
- Strengthen the checker so that whenever a variable's type is known
  through any path (var-decl with explicit type, function-param type,
  return type, generic-binding), the resolved struct is propagated to
  every `NODE_FIELD_ACCESS` and `NODE_FIELD_ASSIGN` reachable from it.
- BST/linked-list-style "store struct pointer as int" patterns continue to
  work behind explicit `nil` plus a typed return: those are already handled
  by `commit 831900e: allow BST/linked list patterns`.

### Acceptance
- `tests/errors/ambiguous_field.ptt` — two structs both having `count`,
  used through an untyped variable, errors with a helpful message.
- All existing tests still pass.

### Files touched
`src/checker.c`, `src/codegen.c`, `tests/errors/`, docs.

---

## Phase 3 — Generics and monomorphization

> **The big one.** This is the project's biggest single piece of work. It
> deserves its own design doc; the sketch below is the shape, not the
> finished design.

### Problem
`list of T`, `map of K to V`, `imap of int to V` are **keywords** today,
not type constructors. There's no syntax to define a generic struct or a
generic function. To replace the built-in `_map_*` we need:
1. user-defined parametric structs (`Map<K, V> is { ... }`),
2. user-defined parametric functions / methods (`Map<K,V>.set(self ref Map<K,V>, k K, v V)`),
3. monomorphization at codegen time: every concrete instantiation
   (`Map<str, int>`, `Map<int, Point>`) becomes its own emitted struct
   layout and its own method symbols.

### Surface choices
Pick one to keep parser simple. Recommendation: **angle-bracket generics
with explicit instantiation** because Potato already uses `<` and `>` only
as comparison operators, and only one-token lookahead is needed to
disambiguate after an identifier:

```
Map<K, V> is {
  cap int
  count int
  data list of Pair<K, V>
}

Pair<K, V> is { key K, value V }

Map<K,V>.set(self ref Map<K,V>, key K, value V) {
  // ...
}

spark {
  m is Map<str, int>()
  m.set("alice", 95)
}
```

Alternative, more in keeping with the language's word style: `Map of K, V
is { ... }`. Both work. Pick one and stick with it.

### What ships
- AST: `NODE_STRUCT_DEF` and `NODE_FUNC_DEF` carry an optional
  `type_params: char**` array.
- Type representation: `Type` gains `Type **type_args; int type_arg_count`,
  used by `TYPE_STRUCT` and a new `TYPE_GENERIC_VAR`.
- Parser: parse type-parameter lists; parse instantiated types in field
  declarations, parameter lists, return types, and `is`-declarations.
- Checker: per-function generic environment; substitute type variables on
  use; resolve method calls against the receiver's instantiated type.
- A monomorphization pass between checker and codegen:
  - Walk every reachable concrete instantiation (`Map<str, int>`,
    `Pair<str, int>`, `Map<str, int>.set@<str,int>`, …).
  - For each, clone the AST subtree of the generic def, substitute type
    variables, mangle the name (`_Map__str__int_set`).
  - Emit one specialised copy. The original generic AST is **not** lowered.
- Built-in `list of T` / `map of K to V` keep working — they desugar to
  instantiations of stdlib `List<T>` / `Map<K, V>` once those exist
  (P6). Until then they remain primitive.

### Mangling
A simple, stable scheme: `_<TypeName>__<arg1>__<arg2>__<method>`. Nested
generics flatten with `_` separators; structs inside structs use the
struct name only (the contained generics are part of *that* mangling).

### Acceptance
- `tests/test_generics.ptt`: a `Pair<K, V>` struct used with two distinct
  instantiations in the same program, both compile, behave correctly.
- A `Box<int>` struct + `Box<int>.set` method works with no fall-through to
  built-ins.
- Compiler emits one `_Box__int_set` symbol regardless of how many call
  sites use it; emits a *different* symbol for `_Box__str_set`.

### Files touched
`src/ast.h`, `src/parser.c`, `src/checker.c`, plus a **new file**
`src/monomorph.c` / `src/monomorph.h` invoked from `src/main.c` between
`checker_run` and `optimizer_run`. `src/codegen.c` only changes mangling.

### Risks
- AST cloning + re-checking instantiations costs compile time. Cache
  instantiations by (template, type-args) tuple.
- Recursive generics (`List<List<int>>`) are normal; *infinitely recursive*
  generics (`List<T> contains List<List<T>>`) need a depth bound and an
  error message.
- This phase is the biggest source of compiler bugs. Budget ~3× the time
  of any other phase. Land in a feature branch with extensive tests.

---

## Phase 4 — Make the IR backend correct, then make it good

### Problem (from `docs/ir-pipeline.md`)
- Heap memory the program just stored to is zeroed after any `bl` call —
  documented blocker. Suspected cause: `_heap_alloc`'s loose size
  accounting on the free list (see A2 in the earlier review).
- Register allocator is single-basic-block. Locals always go through stack
  slots. That's the exact tax that makes user-Potato code slow vs. the
  hand-written built-ins.
- IR has no optimization passes.

### What ships, in order

**P4.1 — Fix the heap bug.**
- Add a debug build of `_heap_alloc` / `_heap_free` that records every
  allocation in a side log.
- Run the minimal repro from `docs/ir-pipeline.md`. Confirm whether the
  corruption is from (a) free-list size mismatch reusing a live block,
  (b) `_yell_int`'s syscall buffer, or (c) something else.
- Land the fix; remove the debug build.
- Acceptance: the repro from `docs/ir-pipeline.md` lines 8–20 prints
  `20 / 20`, and the existing IR pipeline can run all `tests/test_*.ptt`
  cases that touch structs.

**P4.2 — Cross-block register allocation in the IR.**
- Replace the current local-always-spilled scheme with a real linear-scan
  allocator over global live ranges.
- Add PHI-node-equivalent handling at block joins. Most lowerings already
  produce SSA-shaped IR; finish that.
- Acceptance: a microbenchmark counts `ldr`/`str` instructions in the
  emitted code for a tight loop. Goal: ≤2× the count of the direct codegen
  for the same loop. Not parity yet — that comes from P5.

**P4.3 — Switch the default backend to IR.**
- Once IR passes every test the direct codegen passes, flip the default
  in `src/main.c` so `./erbos run` uses the IR pipeline. Keep
  `--legacy-codegen` for one release as an escape hatch.
- Delete the direct codegen after one clean release cycle. Two backends
  is ongoing tax.

### Files touched
`src/iremit.c`, `src/regalloc.c`, `src/irgen.c`, `src/codegen.c` (heap
allocator), `docs/ir-pipeline.md`.

---

## Phase 5 — Optimization passes that earn parity

These all run on the IR (post-P4). Order matters: each enables the next.

### P5.1 — Aggressive inlining
Today's inliner only handles one-line leaf functions. Replace with:
- A size budget (e.g. caller grows by ≤ 64 IR instructions).
- A recursion guard.
- Substitution that handles full bodies: locals, branches, loops.
- Devirtualization point: when a method call has a statically-known
  receiver type (post-monomorphization, almost always), inlining is
  trivial — no v-table. This is the single largest perf lever for
  user-Potato `Map.set`.

### P5.2 — Scalar replacement of aggregates
After inlining, fields of a non-escaping struct become plain virtual
registers. `Map`'s internal `count`, `cap`, `data_ptr` should live in regs
across the body of a hot method.

### P5.3 — Escape analysis + stack allocation
A `Pair<K,V>()` constructed inside `Map.set` and never stored anywhere
visible should be stack-allocated, not heap-allocated. Combined with SRA
above, the inner allocations vanish.

### P5.4 — Bounds-check elimination
`list_set` / `list_push` always emit a bounds compare. If the index is
provably in range (e.g. `i` is a loop variable bounded by the list's
count, established at the start of the loop), drop the compare. Standard
range analysis pass.

### P5.5 — Loop-invariant code motion
Specifically: hoist the `data_ptr` reload that today's `_map_set` does
once per loop iteration in user-Potato code (every `self.data` read goes
through a load).

### Acceptance for the whole phase
- Dedicated micro-benchmark suite: 1M `set`+`get` pairs on each map
  variant. User-Potato `Map<str, int>` runs within 1.2× of `_map_*`.
- Emitted assembly for `Map<str,int>.set` after monomorph + inline + SRA
  resembles `_map_set` in shape: data ptr in a register across the inner
  loop, no per-iteration field reload, no per-iteration `bl _str_eq` call
  (it'll be inlined).

### Files touched
`src/optimizer.c` is the wrong home for these — they run on IR, not AST.
Add `src/iropt.c` / `src/iropt.h` as a sibling of `src/iremit.c`.

### Risks
- These passes interact. Build them in the order above; keep each behind
  a flag (`-O0` / `-O1` / `-O2` style) so regressions are bisectable.
- IR-level testing is mandatory: every pass should have IR-in / IR-out
  golden tests in addition to the end-to-end `make test`.

---

## Phase 6 — Rewrite `std/map.ptt`, retire built-ins

### What ships
- `std/map.ptt` containing `Map<K, V>` with `set`, `get`, `len`, `keys`,
  `delete`. Uses an internal `Pair<K, V>` and a `List<Pair<K,V>>`.
- A trait-or-equivalent for key equality. Two viable shapes:
  1. **Special method name**: `Map<K,V>` requires `K.eq(K) bool` to exist;
     compiler errors at instantiation if it doesn't. Simple, no traits
     needed yet.
  2. **Inline the comparison through a generic function `eq<K>(a K, b K)
     bool`** with built-in specializations for primitive types and
     `_str_eq` for `str`. User types provide their own `eq` overload.
  Recommendation: shape #1; it's cheaper to implement and adequate for a
  Map. Operator overloading / traits arrive in a later release.
- `std/imap.ptt` (delete; just `Map<int, V>` after this), `std/list.ptt`.
- `make` keyword `map`/`imap`/`list` desugar to stdlib instantiations
  during parsing. The C-emitted `_map_*` / `_imap_*` / `_list_*` builtins
  in `src/codegen.c` are removed.
- The `task` builtin stays as-is (its replacement is the green-thread
  runtime work, separate effort).

### Acceptance
- All existing `.ptt` tests pass unchanged.
- The microbenchmark from P5 stays within 1.2× of the (now deleted)
  built-in baseline measured at the start of P6.
- `src/codegen.c` shrinks by ~600 LOC (the `emit_map_builtins`,
  `emit_imap_builtins`, `emit_list_builtins` blocks).

---

## Cross-cutting hygiene picked up along the way

Best resolved opportunistically inside the phases above so they don't
become a separate cleanup commit:

| Item | Phase to fold into | Rationale |
|---|---|---|
| **D5** dedupe `_map_*` ↔ `_imap_*` | P6 (they're being deleted anyway) | wasted effort to dedupe code that's about to be removed |
| **D3** dedupe `codegen()` ↔ `codegen_emit_builtins()` | P4.3 (when direct codegen is removed) | second backend goes away |
| **D2** dedupe import realloc loops | P3 (touching `main.c` for monomorph hooks) | natural neighbour |
| **M6** parallel `Gen` arrays → struct | P4.3 (Gen disappears with direct codegen) | Same — about to be removed |
| **A2** heap allocator size mismatch | P4.1 | identical investigation |
| **B5** read_file error checking, AST leak | P4 (touching main.c) | small |

---

## Effort estimate (rough)

| Phase | Estimate (focused dev-days) | Risk |
|---|---|---|
| P1 methods | 2–3 | low |
| P2 field resolution | 1–2 | low |
| P3 generics + monomorph | **15–25** | high |
| P4.1 heap fix | 2–4 | medium |
| P4.2 cross-block regalloc | 8–12 | medium |
| P4.3 backend switch + cleanup | 2–3 | low |
| P5 optimization passes | 15–25 | medium |
| P6 std/map rewrite | 3–5 | low |
| **Total** | **~50–80 days** | |

This is a real compiler project, not a sprint. The right cadence is one
phase per branch, each with its own test additions, merged behind a green
suite.

## What we are NOT doing

- No interpreter / bytecode VM. Native ARM64 only.
- No FFI to libc. Syscalls only.
- No traits or operator overloading in this plan — Map's key-equality
  contract uses Phase 6 shape #1 (a required `K.eq(K) bool` method).
  Traits arrive after this plan if needed.
- No multi-platform port (x86-64, RISC-V) — that's its own effort.
- No incremental compilation. The compiler stays single-pass-per-file.

## Verification — global

Once all phases ship, the answer to "is `std/map` truly native?" is:

1. `git grep -E '_map_(set|get|new|len|keys)' src/codegen.c` returns
   nothing.
2. `nm` on a compiled `.ptt` binary shows `_Map__str__int_set` (or
   equivalent), not `_map_set`.
3. The microbenchmark suite shows `Map<str, int>` within 1.2× of the
   baseline captured before P6.
4. All existing `.ptt` examples and tests pass unchanged.
