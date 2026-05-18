# Potato Design Decisions 🥔

A running log of language-design discussions: what was proposed, what
was decided, and the first-principles reasoning. Append-only — when
a decision is revisited, add a new dated entry rather than editing
the old one. The point is that future-us can see the reasoning and
not relitigate the same trade-offs from scratch.

Format:

```
## <Title>
**Date:** YYYY-MM-DD
**Status:** decided | parked | reopened
**Decision:** <one-line summary>

<the discussion / rationale>
```

---

## Struct construction syntax (named-arg vs positional)
**Date:** 2026-05-16
**Status:** decided (shipped — commit `b18a4df`)
**Decision:** Two construction forms — `Point()` (zero-default) and
`Point(x is 1, y is 2)` (atomic named-arg). Positional struct
constructors `Point(1, 2)` are rejected.

### Pain that motivated the change

Before this work, the only way to construct a struct was
`p is Point()` followed by per-field `p.x be ...` assignments. That
left every value struct in a half-initialized state between
construction and the last `be`, made `nomut` a no-op for value
structs (you couldn't ever assign anything after the bind), and
silently survived field-reorder refactors. Positional struct
constructors `Foo(1, 2)` were also half-supported in `irgen.c`
with no checker enforcement — silent truncation on arity mismatch,
no type checking.

### Options considered

| Option | Surface | Verdict |
|---|---|---|
| A. Drop positional ctors entirely | `Point() ; p.x be 1 ; p.y be 2` | Doesn't fix the half-init window. |
| B. Keep positional but type-check it | `Point(1, 2)` | Brittle to field reorder — worst-case refactor surface, no syntactic warning when the reorder happens. |
| C. Add named-field literal | `Point{x: 1, y: 2}` (Rust-style) | The right shape, but `:` isn't a token anywhere else in the language. Inconsistent. |
| D. Reuse `is` inside `(...)` | `Point(x is 1, y is 2)` | Zero new tokens. Same shape as `is`/`be` everywhere else. Reads in English. **Picked.** |
| E. User-defined `Type.new(...)` only | `Point.new(1, 2)` | Already works — methods are just methods. Doesn't fix the half-init pain inside the body of `new`. Doesn't help `nomut`. |

### Why `nomut` got tightened at the same time

`nomut x is V` previously only blocked rebinding (`x be ...`); it
allowed `x.field be ...` to mutate fields of the binding. That
made it useless for value structs — without atomic init, you
couldn't construct + initialize a `nomut` value, and the binding
keyword was effectively decorative.

After this change, `nomut` blocks both rebinding and direct
field mutation. Method calls that take `ref self` are still
allowed because the type author opts into them via the receiver
declaration. The rule mirrors Rust's distinction between
binding-mut (`let mut x`) and method-controlled mutation (`x.push()`
on `&mut self`), spelled in Potato vocabulary.

---

## Capitalization of type names (primitives + stdlib)
**Date:** 2026-05-16
**Status:** parked — keep current rules (lowercase primitives, PascalCase user/stdlib)

### Two related questions discussed

1. Should primitives be capitalized? `Int` instead of `int`,
   `Bool` instead of `bool`?
2. Should stdlib types be lowercased? `list` / `map` / `queue`
   instead of `List` / `Map` / `Queue`, mirroring C++'s
   `std::string` / `std::vector`?

### Status quo

| Layer | Case | Rationale |
|---|---|---|
| Primitives (`int`, `bool`, `byte`, `void`) | lowercase | Reserved keywords. Compiler-builtin. Lowercase signals "the compiler knows this intrinsically." |
| User structs / enums (`Point`, `Counter`, `Result`) | PascalCase | Enforced by the checker (`tests/errors/lowercase_struct_name.ptt`). |
| Stdlib types (`String`, `List`, `Map`) | PascalCase | They *are* user-defined structs — greppable in `std/*.ptt`. The compiler treats them like any other user struct. |

### Why both questions were parked

1. **Case is a real signal, not noise.** `int` lowercase tells the
   reader "compiler intrinsic"; `String` PascalCase tells the reader
   "user-greppable struct, look in `std/string.ptt`." Flattening
   either direction throws away that distinction. The visible
   asymmetry in `Map of int to String` is the case rule *working*
   — it's telling you the container is a different kind of thing
   from the element type.

2. **The PascalCase rule is also what keeps the parser cheap.**
   Today `Foo()` is unambiguously a struct constructor and `foo()`
   is unambiguously a function call. That's a deliberate design
   choice (`docs/language-guide.md` Methods section). Lowercasing
   stdlib types would force a lookup table at parse time or defer
   disambiguation to the checker.

3. **The C++ analogy doesn't transfer.** `std::string` works in C++
   because every use site carries `std::` as the disambiguator.
   Potato has no namespace prefix at use sites — after
   `use std/list`, the user just writes `List of int`. C++ pays
   the lowercase cost with `std::`; we'd pay it with nothing.

4. **Industry split is uniform.** When stdlib types are lowercase,
   they always travel with a namespace prefix (C++, Go, Zig). When
   they're PascalCase, the prefix is optional or absent (Rust,
   Kotlin, Swift, Java). No mainstream language has lowercase
   stdlib types AND no use-site prefix. Potato is in the
   PascalCase-no-prefix group; that's the consistent point in
   the design space.

5. **Cost is real.** ~266 source-level edits for primitive
   capitalization (or ~150 for stdlib lowercasing), plus lexer
   keyword table swap, ~10 parser-side type-token cases, all docs,
   the VS Code extension, and every existing `.ptt` file in the
   wild. One-way door — once the spelling changes, every existing
   user file breaks until migrated.

### What was decided

Keep the current rules. The case difference is doing real work and
the cost of changing it is high.

### Conditions under which to revisit

- If we ever add namespace prefixes at use sites (`std.List of int`,
  `list::List`), the C++ argument for lowercase stdlib types
  becomes valid — there'd be a real disambiguator. Revisit then.
- If the parser's leading-case rule becomes a maintenance burden
  for some unrelated reason, the cost of dropping it goes down,
  which changes the trade.
- If the language ever picks up overload resolution / generic
  inference that makes `Map of int to String` rare in practice
  (because users don't have to spell out type args at call sites),
  the readability complaint shrinks on its own.

---

## `is ref` for variable bindings (e.g. `b is ref a`)
**Date:** 2026-05-16
**Status:** parked — `ref` stays parameter-only; document as intentional

### What was asked

Should `b is ref a` (and `c is ref a`, etc.) work as a way to
create local aliases? If so, what happens when `a` is dropped or
moved while `b`/`c`/`d` still hold references?

### What `ref` actually is today

A function-parameter modifier *only*. Two and only two positions:

1. In a declaration — `Counter.bump(self ref Counter)`,
   `out ref List of String`.
2. At a call site matching that parameter — `f(ref x)`.

There's no `ref` type, no `ref` binding, no `ref` variable.
`b is ref a` is a parse error in the current grammar. `ref` is a
*compile-time permission* on a parameter to mutate fields — no
runtime aliasing semantics, no borrow tracking, no reference
counting.

### Why parameter-only is the right line today

1. **Aliasing is lexical and controlled at call sites.** During a
   `f(ref x)` call, `x` is aliased only for the duration of `f`'s
   activation record. No way to outlive the caller. No way to
   stash. The bookkeeping the compiler has to do is zero.
2. **A `ref` *binding* needs a borrow checker.** The moment you
   write `b is ref a` you're asking: what if `a` is dropped while
   `b` is alive? what if `a is now x`? Solving that correctly
   means tracking lifetimes through control flow — Rust-grade
   work. Solving it incorrectly means use-after-free, like the
   `is rep` bug we just hit.
3. **Existing alternatives.** If you want `a` mutated by a callee:
   pass `ref a`. If you want a copy: `is rep`. If you want
   ownership transfer: `is now`. The space is covered.

### Recommended documentation

Document explicitly that `is ref` is intentionally not in the
grammar, and that aliasing is achieved by passing `ref` at call
sites. Today the user just gets a parse error, which is
confusing.

### Conditions under which to revisit

- If/when Potato grows lifetime tracking (a Rust-style borrow
  checker) — `is ref` becomes implementable safely.
- If user ergonomics demands surfaces a real use case that can't
  be expressed with `ref` parameters or `rep`.

---

## `is rep` shallow-copy + double-free (latent UAF)
**Date:** 2026-05-16
**Status:** ✅ FIXED 2026-05-17 — see "is rep deep clone" entry below.

### The bug

`b is rep a` is documented as a "shallow copy (pointer copy)" —
both `a` and `b` end up holding the same heap pointer. The irgen
path (`compiler/irgen.c` ~line 967) marks both as heap-owning
locals. At scope end (`emit_scope_cleanup`), both get a separate
`_heap_free` call on the same pointer. That's a double-free into
the bump+free-list allocator, which produces a cycle in the free
list and lets the next allocation reclaim a block that the other
alias still points at — a use-after-free.

Hidden at -O1+ by the stackify (escape-analysis) pass, which
elides the frees for non-escaping structs. Visible at -O0 today:

```
Point is { x int, y int }
spark {
  a is Point(x is 100, y is 200)
  { b is rep a }       // inner scope frees the (shared) block
  c is Point(x is 999, y is 888)  // c reclaims a's block
  yell(a.x)            // prints 999 — was 100
}
```

### Why it hasn't bitten yet

The escape-analysis pass at -O1+ stack-allocates small
non-escaping structs and skips both `_alloc_*` and `_heap_free`,
hiding the bug for the common case. As soon as `a` flows into a
function call (escapes), the bug resurfaces at every -O level.

### Fix options

| Option | What | Trade |
|---|---|---|
| A. Mark source as moved | After `b is rep a`, `a` is dead. | Defeats the point of `rep` — that's already what `is now` does. |
| B. Don't free the rep destination | `b` is a non-owning alias; only `a` frees. | Tactical band-aid. Falls apart if `a is now b` happens later (now no one frees). |
| C. Make `rep` actually deep-clone | Fresh `_heap_alloc` + memcpy of the struct fields (and recursively for nested structs / `array of T`). Each local owns an independent block. | Correct. The README already says deep-clone is on the v0.2+ roadmap; this just promotes it. |

Recommended: C. The current implementation is a bug pretending
to be a feature. The README already advertises `rep` as planned
to become deep-clone.

### Tracking

Roadmap in `README.md` lists "Deep clone for `rep`" under
Planned (v0.2+). When that ships, this UAF goes away by
construction.

---

## Object lifecycle / memory model — World A
**Date:** 2026-05-16
**Status:** decided
**Decision:** Single owner, explicit move/clone, no shared
ownership in the language. Shared lifetime via the arena+index
pattern.

### The three worlds we considered

| World | Adds | Cost |
|---|---|---|
| **A. Strict single owner** | `is now` (move), `is rep` (deep clone), `q is p` for heap-shaped values is an error. `ref T` for in-call mutation. No `&T`, no `Rc`, no borrow checker. | Smallest. Compiler stays simple. Users who want shared lifetime use arena+index. |
| **B. Single owner + opt-in shared** | Adds `Rc of T` (refcounted wrapper) as a stdlib type. | Medium. Refcount runtime cost; cycle leaks unless `Weak` is added. |
| **C. Borrow checker** | Adds `&T` / `&mut T` with compile-time lifetime checking. | Largest. Multi-year language design effort. Brings all of Rust's lifetime/async/self-ref pain. |

We picked A.

### Why A

1. **Most data is tree-shaped.** Heap structs in Potato are
   already pointers under the hood; the existing `is now` /
   `is rep` / `ref` machinery covers the common cases.
2. **Shared-ownership use cases are rarer than C++ habit
   suggests.** Modern compiler/ECS/database design uses arena +
   integer indices, not `shared_ptr`. We saw no Potato program
   in the tree that demands `Rc`.
3. **The borrow checker is a separate, much larger language.**
   World C isn't a feature; it's a multi-year effort. KISS
   forbids it for now.
4. **Single-owner is enforceable cheaply.** Add the rejection of
   plain `q is p` for heap-shaped values, finish deep-clone for
   `is rep`, and the compiler proves single ownership for every
   heap allocation across every program.

### The three rules of World A

1. Every heap allocation has exactly one owner at every point in
   the program.
2. `q is now p` moves (source dead afterwards). `q is rep p`
   deep-clones (independent block, both alive). Plain `q is p`
   for heap-shaped values is a compile error: the user must say
   which they meant.
3. `f(p T)` passes a pointer with read-only permission;
   `f(p ref T)` passes the same pointer with write permission.
   Borrows are *lexical* to the call. No long-lived borrows;
   no `&T` binding form (`is ref` parked separately).

### Escape hatch — arena + index, not Rc

For data that needs to be referenced from many places without
copying:

```
// The arena owns the data
images is List of Image
img_id is images.len()
images.push(load("foo.png"))

// Consumers hold an int (the index), not a pointer
window.icon_id be img_id
toolbar.icon_id be img_id

// Access through the arena
icon is images.get(window.icon_id)
```

Memory profile is identical to `shared_ptr`: one copy of the
data, many lightweight handles. Lifetime is *structural* (until
the arena dies) instead of *dynamic* (until the last user dies).
No refcount, no cycles, no `Weak`, no atomic ops.

This pattern is not a feature; it's a coding discipline. We
document it in `docs/language-guide.md` and the front-page
README so users coming from C++ know it exists.

### Conditions under which to revisit

- A real Potato program demonstrates a use case that genuinely
  cannot be expressed via arena+index, owned moves, or `is rep`.
  At that point we can consider adding `Rc of T` as a stdlib
  type (World B), opt-in only, never the default.
- We never go to World C without an explicit, scoped multi-year
  design effort. The borrow checker is its own language project.

---

## `nomut ref T` — rejected as redundant
**Date:** 2026-05-16
**Status:** decided
**Decision:** No `nomut ref T` syntax. The default param form
`T` (no `ref`) is *already* read-only; `nomut ref T` would be
the same thing spelled twice.

### What's already true

| Param syntax | Read fields | Write fields | C++ analog |
|---|---|---|---|
| `f(p Point)` | yes | **no** (compile error) | `const Point& p` |
| `f(p ref Point)` | yes | yes | `Point& p` |

The checker enforces this in NODE_FIELD_ASSIGN: a non-`ref`
struct param cannot have its fields mutated.

### Why C++ needs `const T&` but Potato doesn't

In C++, the default for a parameter is *copy* (`T p`). To get a
no-copy non-mutating reference you need `const T&` — three
tokens that *together* mean "pointer + read-only." Potato's
default for any heap-shaped param is *already* "pointer +
read-only"; one token does the work of three.

If we added `nomut ref T` it would either:
- mean exactly the same as `T` (redundant, confusing), or
- mean some new third thing (granular mutation control) — which
  we explicitly don't want, see "every feature pays rent."

### The `nomut` keyword's two roles

It's worth being clear that `nomut` already wears two hats:

- `nomut x is V` — block reassigning x AND block mutating x's
  fields.
- (parameter form, implicit when no `ref`) — block mutating the
  parameter's fields.

Same idea (no-mutation), two surfaces. The parameter form
doesn't need an explicit keyword because the absence of `ref`
already says it.

---

## Calling convention — explicit
**Date:** 2026-05-16
**Status:** documented (no code change)
**Decision:** Document the actual calling convention: heap-shaped
types are passed as pointers; primitives are passed by value.

### What actually happens (verified in the IR backend)

| Source param | Carried in | What it is | Aliasing | Mutation |
|---|---|---|---|---|
| `n int` | x0..x7 | the 8-byte value | no — it's a copy | n/a |
| `b bool` | x0..x7 | 0 or 1 | no — copy | n/a |
| `p Point` (no `ref`) | x0..x7 | pointer to caller's heap struct | yes — points at caller's data | **NO**, checker-blocked |
| `p ref Point` | x0..x7 | same pointer | yes | yes |
| `xs List of int` (no `ref`) | x0..x7 | pointer to list header | yes | NO |
| `xs ref List of int` | x0..x7 | pointer to list header | yes | yes |
| `s String` | x0..x7 | pointer to String header | yes | NO (String fields are immutable from outside via methods) |

Verified: `compiler/regalloc.c:170-177` (params come in via
x0..x7), `compiler/iremit.c:428-438` (call site moves args to
x0..x7), and the actual emitted asm for a struct param shows
a single 8-byte pointer load.

### Why this is enough without a borrow checker

Three properties make pointer-passing-without-lifetimes safe:

1. **Aliasing is lexical.** `f(p)` aliases `p` only for the
   duration of `f`'s activation. The caller can't observe a
   write until `f` returns; after, the alias is gone.
2. **The owner outlives the alias by construction.** A local
   can't be dropped while it's an argument; no way to
   prematurely free what the callee is reading.
3. **No `&` escape.** There's no syntax to capture `p` into a
   long-lived location inside `f`. Even `g(p)` from inside `f`
   just continues the lexical chain.

This is the "borrow ends when the function returns" subset of
Rust's borrow checker, enforced for free by the grammar (no way
to express anything else). It's what makes World A
implementable without a real borrow checker.

### Why no `ref int`

Primitives are 8 bytes — same size as a pointer. Pass-by-value
is as cheap as pass-by-pointer. There's no performance reason
for `ref int`, and we don't allow primitive parameter mutation
anyway. If a user genuinely needs an out-int (rare), they wrap
it in a one-field struct: `Counter is { value int }` and
`Counter.bump(self ref Counter)`.

---

## Every feature must pay rent
**Date:** 2026-05-16
**Status:** standing principle
**Decision:** Every proposed language feature must demonstrably
make the user's first hour better, or actively prevent a class
of bugs in their first thousand lines. Otherwise it doesn't ship.

### The principle

> If a feature isn't actively making the user's first hour
> better, or actively preventing a class of bugs in their first
> thousand lines, it's not paying rent. Cut it or defer it.

### Features that pay rent (existing or near-term)

- Better error messages with caret + context (top priority for
  user feel).
- File I/O (without it the language is a curiosity, not a tool).
- `Result of T to E` for fallible operations (immediately useful
  once I/O exists).
- A "Potato in 10 minutes" tutorial (zero compiler cost,
  enormous adoption value).
- Default `_<Type>_yell` per struct at monomorphize time (debug
  printing without per-type boilerplate).

### Features that DO NOT pay rent — defer or never

- **Operator overloading.** Permanent complexity tax (every `+`
  becomes a method-resolution question), small benefit. Go and
  Zig don't have it and are fine. Defer indefinitely.
- **Traits / interfaces.** Tempting siren song. Most code that
  thinks it needs traits actually needs free functions or
  monomorphization (which we have). Defer until concrete.
- **Async / await.** Don't wire the green-thread runtime into
  compiled output until there's a real use case. Every language
  that added async early regretted the syntax. Defer.
- **Macros.** Hard no, indefinitely. C, Lisp, and Rust all
  regret macros. Even Rust's `macro_rules!` is a pain point.
- **Self-hosting the compiler.** Bragging point that costs months
  for benefits the user never sees. Defer until the language is
  stable enough that the rewrite isn't going to be obsolete.

### The forcing function

When someone proposes feature X, ask: "Does this make the user's
first hour better, OR does this prevent a class of bugs?" If the
answer is no, the feature gets logged here as deferred and the
discussion ends.

If the answer is yes, the feature must still survive the other
language principles (no magic, no `<T>`, KISS, every decision
serves cohesion over completeness).

---

## Struct field auto-init for struct-typed fields
**Date:** 2026-05-17
**Status:** decided (shipped — bug #114 fix)
**Decision:** When a struct's field has a type that is itself a
struct (user-defined OR stdlib generic like `List of T`,
`Map of K to V`, `String`), the parent's `_alloc_<X>`
constructor recursively calls `_alloc_<FieldType>` for that
field and stores the resulting pointer.

### The bug

Without this, `Store is { items List of Item }` followed by
`s is Store(); s.items.push(...)` segfaults at `-O0` (visible
on macOS as exit code 139). At `-O1+` the optimizer's stackify
pass *partially* hides the crash by stack-allocating the Store,
but the underlying issue remains — and the same bug bites for
any nested user struct (`Outer { inner Inner }; o.inner.v` →
null deref).

The root cause is the layout: every struct field is an 8-byte
*pointer* slot, never an inlined struct body. After
`_alloc_Store` zero-fills the block, `s.items` is a null
pointer. Calling `List.push` dereferences it and crashes.

### Why auto-init (not "user must initialize explicitly")

We considered three options:

| Option | What | Trade |
|---|---|---|
| A. Auto-init | `_alloc_<X>` recursively allocates struct-typed fields. | Hidden allocation per construction. Matches user expectation: "every field starts at its type's zero value" extends to "an empty List for List fields, an empty String for String fields, ...". |
| B. Require explicit init | User must use named-arg form `Store(items is List of Item)` for any struct with struct-typed fields. | No hidden allocation. But makes zero-default useless for any struct that contains a stdlib collection — every Store-like type forces verbose construction. |
| C. Runtime null-check | Insert null-deref guards on every method call. | Per-access overhead. Doesn't fix the actual problem, just turns a segfault into a panic. |

Picked A. The principle "no hidden allocations" was meant to
mean "no allocations the user can't see in source," not "an
int field allocates nothing, and a struct field also allocates
nothing." A struct-typed field is a struct in the type system's
view; allocating it as part of the parent's construction is
consistent with how every other heap allocation in the language
works (`p is Point()` allocates).

### Implementation

`compiler/main.c`'s `EMIT_IR_TO_FILE` macro generates two
shapes for `_alloc_<X>`:

- **No struct-typed fields** → simple `_heap_alloc(size)` + zero-fill.
- **At least one struct-typed field** → 32-byte frame (preserves
  x29/x30 + x19/x20 across recursive `bl _alloc_<FieldType>`
  calls). Allocate parent, zero-fill, then per-field call
  `_alloc_<FieldType>` and store the pointer at the field offset.

`compiler/iropt.c` was updated for both relevant passes:

- **SRA** (`alloc_call_field_count`) — refuses to eliminate
  any `_alloc_<X>` whose struct has a struct-typed field. The
  alloc has side effects beyond just the heap_alloc.
- **Stackify** (`alloc_has_field_init`) — refuses to promote
  such allocs to stack slots. Same reason.

Both passes consult a module-level `g_iropt_program` set by
`iropt_run`, which now takes the parsed AST as a third argument.

### Tracking

- `tests/test_struct_field_init.ptt` — 5 framework tests
  (user struct field, stdlib generic field, two-struct
  independence, deep nesting, full arena pattern).
- `tests/ir/struct_field_auto_init.ptt` — 2 IR regression tests
  at -O0/-O1/-O2.
- All tests pass clean at -O0/-O1/-O2; ASan + UBSan clean.

---

## No module-level globals
**Date:** 2026-05-17
**Status:** decided
**Decision:** Potato has no top-level / file-scope / module-level
variable bindings. Every `is` lives inside `spark { }`,
`test "..." { }`, or a function body. The convention for
long-lived shared state is the `App` pattern (one top-level
struct that owns every arena, passed by `ref`).

### Why no globals

Pushing back on a comparison reflex: most languages have globals
(C++ file-scope `static`, Python module-level `let`, Java
class-level `static`), so users coming from those languages
reach for them first. The case for *not* adding them:

1. **Function signatures stay complete descriptions of what the
   function touches.** `f(p Point)` reads `p` and nothing else.
   No "this function might mutate the global cache somewhere"
   surprise. Test that function in isolation by constructing a
   fresh `Point`; it can't reach for anything you didn't give it.

2. **Initialization order is impossible to get wrong.** No
   "static initializer A depends on static initializer B in
   another translation unit." `spark` runs top to bottom;
   whatever's bound by line N is what's available on line N+1.

3. **The compiler doesn't need a notion of static lifetime.**
   Globals would need a separate memory region (BSS / DATA), a
   separate cleanup story (do they get freed? when?), and a
   separate const-vs-mut distinction. Locals + heap + RAII
   covers everything.

4. **Concurrency stays honest.** When the green-thread runtime
   gets wired into compiled output, there are no shared globals
   to synchronize. Each task has its own data; sharing is
   explicit through channels. The hardest C++/Java concurrency
   problem — "what does this global thread-safely look like?" —
   doesn't exist by construction.

### What you give up

| Cost | Severity | Workaround |
|---|---|---|
| Function signatures can grow when many arenas are involved | low–medium | The `App` pattern: pass one `ref App` instead of N arenas. |
| No singletons (one logger, one config) | medium | Logger lives in `App`. Functions that log take `ref App` (or just the logger). |
| No module-init code | low | Build everything at the top of `spark` and pass it in. |

### The `App` pattern (canonical)

For programs that need long-lived shared state, define a single
top-level struct that owns every arena, instantiate it once at
the top of `spark`, and pass `ref App` (or just the specific
arena) to functions that need access. See
`docs/language-guide.md` § "Larger programs — the `App`
pattern" for the full worked example.

### What this is NOT

- Not "no constants." Compile-time constants (`nomut x is 10`)
  inside a function or block work today. We could add file-scope
  `nomut` for true constants without touching any of the above
  reasoning. That's a smaller, separate question; not addressed
  here.
- Not "no module state ever." If we eventually add a stdlib
  logger or similar, it could be a struct that the user
  instantiates in `App`. The convention does the work, not the
  language.

### Conditions under which to revisit

- A real Potato program demonstrates a use case that genuinely
  cannot be expressed via the `App` pattern, AND can't be
  factored differently. We haven't hit one.
- File-scope `nomut` (compile-time constants only, no mutable
  globals) is a separate proposal that could land cheaply if
  there's demand. Not on the roadmap today.

---

## Import system — `use` resolution + `potato.toml`
**Date:** 2026-05-17
**Status:** decided (shipped — commits 38a0b14 / f49abed / db16ce2 / 95604ec / f034bf7)
**Decision:** Three-tier resolution (sibling, project-root,
compiler-bundled stdlib). Project root identified by `potato.toml`
walk-up. Stdlib resolved relative to the compiler binary, not
cwd. No auto-loaded modules; every `use` is explicit. `as`
aliasing kept for free-function namespacing.

### What was broken before

Five separate problems in the old resolver:

1. **Stdlib resolution was cwd-anchored.** Running `erbos run
   /tmp/x.ptt` with `use std/list` failed unless cwd had a
   sibling `std/`. Made Potato unusable outside its own repo.
2. **`std/string` was implicitly auto-loaded.** A ~95-line
   special case in main.c hardcoded the auto-load. Singled out
   one stdlib module from all the others; violated the stated
   "no magic" rule.
3. **`examples/` was a third search root.** Hack to make
   leetcode tests find shared libraries. Conflated "browsable
   demo programs" with "shared library code."
4. **No project-root concept.** Every non-stdlib import had to
   be a sibling or hit cwd. Real projects (multi-directory,
   `lib/` + `src/` shape) couldn't be expressed.
5. **Generic error messages.** `error: cannot find module 'x'`
   didn't tell the user what to do — typo? broken install?
   missing project root? Each requires a different fix.

### What ships

Resolver tries three roots, in order:

```
1. <dir-of-source-file>/<path>.ptt       — sibling
2. <project-root>/<path>.ptt             — via potato.toml walk-up
3. <compiler-binary-dir>/<path>.ptt      — bundled stdlib
```

Three tiers, three honest roles. No special cases for any one
module.

**`potato.toml`** marks a project root. The file can be empty;
the format is reserved for future build/dependency metadata.
Walk-up from the source file finds the first ancestor with the
marker. Programs that only use stdlib + sibling imports don't
need a marker.

**Stdlib is bundled with the compiler binary.** Found via
`_NSGetExecutablePath` on macOS. `use std/list` works regardless
of cwd or project structure. The Potato repo's own `std/`
directory IS the bundled stdlib (the binary lives at the same
level), so dev workflow is unchanged.

**No auto-loaded modules.** Every `use std/string` (or any
other import) is explicit. The runtime built-ins (`_String_yell`,
`_String_concat`, etc.) remain — those are compiler intrinsics,
not stdlib methods. So bare `yell("hi")` still works, but
`s.len()` requires `use std/string` because it dispatches to
the user-Potato `String.len` method.

**`as` aliasing kept.** Audit found two real uses
(`tests/leetcode/test_next_perm_v2.ptt`,
`tests/leetcode/test_longest_substr.ptt`) — both legitimate:
two libraries with similar long prefixes get aliased to short
names (`v2`, `brute`). The keyword stays. `as` only affects
the free-function namespace; types are global and methods
dispatch by receiver type, so the alias has no effect on
either.

### What's deliberately NOT in the import system

| Feature | Status | Why |
|---|---|---|
| `..` paths | rejected at parse time | Forward-only by grammar. Sibling-of-parent code goes in a real location, or in `std/`. |
| Absolute paths (`use /abs/path`) | rejected | Same reason. |
| Quoted paths (`use "x"`) | rejected | Identifier segments only. |
| Selective imports (`use std/math.{max}`) | not added | Doesn't pay rent. Bring in the whole module. |
| Wildcard imports (`use std/math.*`) | rejected | Footgun. Same as Python's `from x import *`. |
| Third-party packages | not yet | When we have a package manager, deps go in potato.toml. |
| Cross-project linking | not yet | Same. |

### Conditions under which to revisit

- A real third-party library ecosystem develops. At that point
  potato.toml gets `[dependencies]` and the resolver learns to
  consult a vendor / cache directory.
- Cross-platform install layout differs from "binary + std/
  side-by-side." On Linux/Windows we may want
  `<compiler-dir>/../share/potato/std/` or an environment
  variable override.
- Selective imports become important enough that wildcard-
  module-loads cause real friction. Today they don't.

### Tracking

- `compiler/main.c` — `compiler_dir()` and `find_project_root()`
  helpers; resolver loop unified.
- `tests/lib/leetcode/` — moved from `examples/leetcode/` since
  the latter was the legacy fixture root.
- `potato.toml` at repo root — empty marker so the walk-up rule
  works for `make test`.
- 48 .ptt files updated with explicit `use std/string`.
- Helpful error messages cover the three failure modes (stdlib
  typo, missing project root, missing file in present project).

---

## `is rep` deep clone (UAF fixed)
**Date:** 2026-05-17
**Status:** decided (shipped)
**Decision:** `b is rep a` allocates a fresh heap block of the
same size as `a`, recursively deep-clones every struct-typed
field, and inline-copies every `array__*` field's header + data
buffer. The two locals own independent blocks; mutating one
doesn't affect the other.

### What ships

For every struct registered in the program (user-defined or
post-monomorphisation stdlib like `List__int`,
`Map__String__int`, `String`), the compiler emits a
`_clone_<X>(src) -> dst` symbol alongside the existing
`_alloc_<X>`. The body:

1. Allocates `size = field_count * 8` bytes; zero-fills.
2. Per field, three cases:

   | Field type | Action |
   |---|---|
   | Primitive (`int`, `bool`, `byte`, anything not in struct registry and not `array__*`) | 8-byte copy `[dst+off] := [src+off]` |
   | Struct in this program (user struct or monomorphised stdlib) | Recurse: `bl _clone_<FieldType>` after null-guard |
   | `array__<elem>` | Inline copy: alloc fresh 16-byte header, alloc fresh `cap*esz` data buffer, byte-loop memcpy data, store new header pointer in dst (skipped on null source) |

   `esz` is 1 for `array__byte`, 8 for everything else (the only
   two element sizes the language supports today).

3. Returns the new block in x0.

Frame: 48 bytes preserving x29/x30 + x19 (src) + x20 (dst) +
x21/x22 (scratch saved across `_heap_alloc` calls inside the
array-field path).

The checker stashes the source's struct name into
`var_decl.type_name` when the source has a known struct type;
irgen reads it and emits `IR_CALL clone_<TypeName>` instead of
the old shallow rebind.

### Why this implementation

We considered three options earlier:

| Option | What | Why rejected |
|---|---|---|
| A. Mark source as moved | After `b is rep a`, `a` is dead. | Defeats the point of `rep` — that's already `is now`. |
| B. Don't free the rep destination | `b` is a non-owning alias; only `a` frees. | Tactical band-aid. Falls apart if `a is now b` later (no owner). |
| C. Real deep-clone | Per-struct `_clone_<X>` recursing through fields. | **Picked.** The README already advertised `rep` as deep clone. The bug was the implementation lying. |

### Tradeoffs accepted

- **Code-size cost.** Every struct now emits both `_alloc_<X>`
  AND `_clone_<X>`. For programs that never use `is rep`, this
  is dead code in the binary. Acceptable — the alternative is
  emitting clone lazily, which complicates symbol resolution.
  Linker dead-strip eventually picks it up.
- **No deep-clone for raw `array of T` locals.** Only structs
  get a clone symbol. A bare `array of int` local cloned via
  `is rep` would fall into the legacy shallow path. We don't
  have a use case for that today (`array of T` is always wrapped
  in `List`/`Map`/`String` in user code). If a use case appears,
  we can either generate a `_clone_array__<T>` symbol or refuse
  the clone with a checker error.
- **Cyclic struct graphs would infinite-loop.** Today no struct
  in the codebase contains a field of its own type, so cycles
  can't be constructed. If/when we permit recursive types, deep
  clone needs cycle detection. Not a problem today.

### Tracking

- `compiler/main.c` — `_clone_<X>` emission alongside `_alloc_<X>`.
- `compiler/checker.c` — stashes source struct name into
  `var_decl.type_name` for `is_rep` decls.
- `compiler/irgen.c` — `is_rep` + `type_name` → emit
  `IR_CALL clone_<TypeName>` instead of shallow rebind.
- `tests/test_rep_deep_clone.ptt` — 6 framework tests
  (primitive struct, nested struct, list with array storage,
  4-way independence, source-survives-rep'd-destruction, String).
- `tests/ir/rep_deep_clone.ptt` — 3 IR regression tests at
  -O0/-O1/-O2.
- ASan + UBSan clean on every test.

---

## Plain `q is p` for heap-shaped values is now a compile error
**Date:** 2026-05-17
**Status:** decided (shipped)
**Decision:** When the right-hand side is a bare identifier (a
local variable reference) AND its type is heap-shaped (struct,
list, map, array, String), plain `q is p` produces a compile
error. The user must say `is now` (move) or `is rep` (deep clone).

### What was wrong before

Plain `q is p` for a heap-shaped source produced **silent
untracked aliasing**: irgen rebound `q` to `p`'s pointer, the
checker didn't mark `p` as moved, and neither was registered as
"this is an alias" anywhere. When `p` and `q` were in different
scopes — one going out before the other — the surviving binding
held a dangling pointer. Demoed earlier in this same log
(see "is rep shallow-copy + double-free" entry — same
underlying bug, different surface).

The READMEs and language guide already *claimed* this was
disallowed (since the World A entry shipped), but it wasn't
actually enforced. Today's commit closes the gap.

### Implementation

One block in compiler/checker.c NODE_VAR_DECL: if not `is_move`,
not `is_rep`, source is `NODE_IDENT`, and the resolved type is
`STRUCT | LIST | MAP | ARRAY | STR`, error out with a teaching
message that suggests both `is now` and `is rep`.

```
error:10: ambiguous alias: `b is a` for a heap-shaped value
  help: use `b is now a` to move ownership (source becomes inaccessible)
  help: use `b is rep a` to deep-clone (independent copy)
```

Primitives (`int`, `bool`, `byte`) still allow plain `is`
because copying an 8-byte primitive value is unambiguous and
makes two independent values automatically.

### What still works

| Form | Heap-shaped | Primitive |
|---|---|---|
| `b is a` | **error** | OK (copies value) |
| `b is now a` | OK (move) | OK (functionally same as copy) |
| `b is rep a` | OK (deep clone) | OK (no clone needed; copies value) |

### Codebase audit before shipping

Grepped every `IDENT is IDENT` in the source tree (9 matches).
All sources were primitive (`int` or `bool`); none were
heap-shaped. The change broke zero existing files. Future code
that tries the alias form will hit the new error.

### Tests

- `tests/errors/alias_struct.ptt` — struct alias rejected.
- `tests/errors/alias_list.ptt` — List alias rejected.
- `tests/errors/alias_string.ptt` — String alias rejected.
- `tests/errors/alias_array.ptt` — `array of T` alias rejected.
- `tests/test_alias_rules.ptt` — 6 framework tests verifying
  primitive plain `is` still copies, `is now` transfers, and
  `is rep` deep-clones (for both struct and List sources).

---

## Spudlock-driven fixes (real-project test pass)
**Date:** 2026-05-17
**Status:** decided (shipped — commits `0acd587`, `c0cfb1e`,
`deabcc9`, `1782054`)
**Decision:** Drove the compiler against a substantial real
project (Spudlock — a dependency resolver with versions,
constraints, conflicts, cycles, build planning) and shipped
fixes for every reproducible bug it surfaced.

### Why this matters

Synthetic tests verified individual features. Spudlock
exercises *combinations*: `through (x in struct.list_of_T)`
flowing into a `match` arm whose binding's method is called.
Bugs in that combined path were invisible to feature-isolated
tests. Real-project testing is the only way to find them.

### What was wrong

Six classes of bug, all real, all fixed:

1. **Parser oversights** — `--help` not recognized; multiline
   named-arg constructors rejected; `field be now src`
   rejected; `module.func(ref x, ...)` rejected at call site;
   `field is List of T` (no parens) rejected as a value.

2. **`erbos ir` artifacts** — output `.s` written next to source.
   Now writes to cwd (build mode unchanged).

3. **Type inference for stdlib generics as struct fields** —
   `parse_type_str("List__Item")` was lossy (val_type=NULL),
   so `through (x in h.items)` left x as TYPE_UNKNOWN and
   field accesses on x were miscategorized as int. Symptom:
   `x.name + "..."` failed with "String + int."

4. **Field access on UNKNOWN-typed receivers** always returned
   TYPE_INT instead of looking up the actual declared field
   type. Compounded with #3 to turn String fields into ints.

5. **NODE_MATCH was never visited by the checker.** Match-arm
   binding parameters had no symbol-table entries, so method
   dispatch on them (e.g. `count.to_string()` for `Success(count
   int)`) hit the unknown-receiver fallback and emitted
   `bl _to_string` (undefined). This was the root cause of
   spudlock's "program stops after first failure result" and
   "second test in file skipped" symptoms.

6. **`field be now obj.field2`** was overzealously rejected
   (required IDENT RHS). Spudlock's
   `out.order be now ctx.order` is the canonical drain pattern
   at end of a function. Relaxed: pointer transfer works; we
   just can't mark a struct field as moved (move tracking is
   at symbol level only).

### What's interesting about the bug clustering

Issues 4, 6, 7, 10, 11, 12 in the user's original report all
turned out to be downstream of (3), (4), and (5). The match-
binding bug in particular was masquerading as multiple
unrelated symptoms (tests skipping, programs exiting early,
linker errors for `_to_string`) because a missing type leads
to wrong symbol resolution leads to undefined references, in
varying patterns depending on which method was called.

### Tests

- `tests/test_spudlock_fixes.ptt` — 3 framework tests for
  multiline named-args, `field be now`, `field be rep`.
- `tests/test_spudlock_module_ref.ptt` — 2 tests for
  module-qualified call-site `ref`.
- `tests/test_through_field_typing.ptt` — 2 tests for type
  inference through generic-stdlib struct fields.
- `tests/test_match_binding_types.ptt` — 3 tests for
  match-arm binding parameters typing correctly so methods
  dispatch right.

### What this teaches us

Real-project testing surfaces bugs that minimal repros
can't. The cost-benefit of building one substantial example
program early in a language's life is large. We should keep
spudlock-style testing in the loop — when a major feature
ships, run it through spudlock before declaring it done.

---

## Codex review feedback on the audit-fix sweep
**Date:** 2026-05-17
**Status:** decided (shipped — three follow-up commits)
**Decision:** Three P0 ownership-semantics holes from the
initial audit-fix sweep, plus a P1 framing correction. All
addressed. Audit-fix work was claimed "complete" prematurely;
this entry documents the gaps and what tightened them.

### What I missed in the initial sweep

1. **Method receiver ref enforcement (a.k.a. "P0-2 incomplete").**
   The fix preserved call-site `ref` for ordinary args but
   never extended the same rule to the implicit `self` argument
   of method calls. Repro:

       Counter.bump(self ref Counter) { ... }
       touch(c Counter) { c.bump() }    // mutates caller's c
                                        // through a non-ref param

   The checker happily accepted this — and an inline comment in
   NODE_FIELD_ASSIGN (line 1417 pre-fix) explicitly justified
   it as "Method calls that take `ref self` are still allowed."
   That stance contradicted the language guide's calling
   convention, which says non-`ref` params are read-only.

   Fix: when a user method's `self` is `ref`, the receiver must
   be a local (`is_ref == -1`) or a `ref` parameter (`is_ref == 1`).
   A non-`ref` parameter receiver is rejected with a teaching
   error suggesting `ref T`.

2. **Heap replacement leaks the previous owner ("P0-7 incomplete").**
   The alias ban shipped — `q is p` for heap values now requires
   `now`/`rep` — but the `now`/`rep` paths themselves overwrote
   the destination's slot via `set_local` without dropping the
   prior owned value. Same for `obj.field be now src`. Stress
   test would show:

       a is List of int; a.push(1)
       through (i from 0 to 1000 by 1) {
         fresh is List of int
         fresh.push(i)
         a be now fresh   // a's previous List header + array leaks here
       }

   ASan flagged the leak. Fix: emit a drop call for the old
   slot value before overwriting. Helper
   `emit_drop_local_slot` in irgen.c handles both struct and
   array shapes; field-assign emits the same logic inline
   based on the field's declared type. Skipped when the slot
   was never heap-marked or has been moved.

3. **Recursive drop skipped self-type fields ("P0-8 incomplete").**
   The drop loop's `if (sj == si) continue;` filter — copied
   from `_alloc_<X>`'s init loop where it's correct (avoids
   infinite recursion at construction time) — was wrong in
   `_drop_<X>` and `_clone_<X>`. Self-type fields like
   `Node.next Node` form chains that terminate via nil; both
   drop and clone need to walk the chain. The repo even
   shipped `examples/linked.ptt` exercising exactly this
   shape. Fix: removed the self-skip in `_drop_<X>` and
   `_clone_<X>`. Kept in `_alloc_<X>` (recursive auto-init
   would loop forever at first construction).

4. **P1-13 was partial, framed as complete.** Quoting paths
   with single-quote wrappers fixes the spaces-in-paths case
   but not paths containing `'`. The follow-up should be
   posix_spawn with argv (no shell at all). Re-framed the
   inline comment to be precise: "spaces-in-paths fixed;
   pathological characters still don't."

### What this teaches

The same lesson as the spudlock thread: **claim of "fixed"
must match what's actually fixed.** The audit's evidence-based
review caught three real holes in code I'd just landed and
declared green. Three remediations:

- After landing each P0 fix, list the specific surface the fix
  *doesn't* cover. The `_drop_<X>` initial commit message
  even acknowledged "replacing an owned heap field via
  `field be now src` doesn't drop the previous field value"
  as a known limitation — but didn't elevate that to a
  follow-up task. The right move was to ship the follow-up
  before declaring P0-8 done.
- Stress-test ownership invariants under ASan over loops
  with many iterations. A single replacement leaks
  invisibly; 200 of them leaks visibly. The test
  `tests/test_replace_drops_old.ptt` runs 200/100 iters
  for exactly this reason.
- Treat reviewer findings as ground truth even when I think
  the spec says otherwise. The "comment that explicitly
  justified ref-self mutation through non-ref params" was
  load-bearing for *not* fixing P0-2 properly. A reviewer
  pointing at the code is a stronger signal than my prior
  reading of the comment.

### Tracking

- `tests/test_replace_drops_old.ptt` — 2 framework tests:
  200-iter local reassign + 100-iter field reassign. ASan
  catches the leak pre-fix.
- `tests/test_self_type_drop.ptt` — Node chain drop test.
- `examples/linked.ptt` continues to work.
- Spudlock end-to-end clean under ASan + UBSan.
- All previous regression tests still green.

---

## Codex review round 2: ref-self walker for nested receivers
**Date:** 2026-05-17
**Status:** decided (shipped — single follow-up commit)
**Decision:** Walk the receiver expression to its root identifier
when checking ref-self method calls. Reject when the root is a
non-ref parameter, regardless of how many `.field` /
`[index]` steps sit between the root and the method call.

### What round 1 missed

The previous fix only checked when the method-call object was
a bare `NODE_IDENT`. Codex found a real escape:

    Counter is { value int }
    Holder is { counter Counter }

    touch(h Holder) {
      h.counter.bump()    // h is non-ref; bump takes ref self
    }

The receiver `h.counter` is a `NODE_FIELD_ACCESS`, not a
`NODE_IDENT`. The check skipped it; the call went through.
Caller's data mutated through a parameter the docs call
read-only.

### Fix

Walk the receiver expression's leftmost child until we hit
`NODE_IDENT` (root) or a non-rootable expression
(`NODE_CALL` / `NODE_METHOD_CALL` — transient values).
Steps recognised:

- `NODE_FIELD_ACCESS` → step into `.object`
- `NODE_INDEX` → step into `.object`

If the root is a `NODE_IDENT` whose `is_ref == 0` (non-ref
parameter) and the method is `ref self`, reject. Locals
(`is_ref == -1`) and ref params (`is_ref == 1`) pass.

Tests:
- `tests/errors/ref_self_via_field_access.ptt` — depth-1
  reject (`h.counter.bump()`).
- `tests/errors/ref_self_via_nested_field.ptt` — depth-2
  reject (`o.inner.counter.bump()`).
- `tests/test_ref_self_walk.ptt` — 3 positive tests:
  ref-param root allowed, local root allowed, bare local
  control.

### What this still does NOT cover

Receivers rooted at a method-call return value alias caller
storage but are treated as transient by the walker. Concretely:

    touch(xs List of Counter) {
      xs.get(0).bump()   // mutates xs's element 0
    }                     // xs is non-ref but the call is allowed

The walker bottoms out at `xs.get(0)` (a `NODE_METHOD_CALL`),
treats it as transient, and lets the call through. Empirically
verified that `xs.get(0).bump()` does mutate the underlying
list element — so this is a real escape, just narrower than
what Codex specifically flagged.

Two reasons not to fix in this commit:
- The minimal fix (reject any ref-self chain through a method
  call) is overly conservative — it would reject legitimate
  patterns like `make_counter().bump()` where the method
  returns a fresh transient.
- The proper fix needs a function-attribute (each method
  declares whether its return aliases storage reachable from
  arguments). That's a non-trivial language feature.

Tracked as task #142. The current shipped scope: rejection
through `field`/`index` chains; method-call returns remain a
known gap that requires separate design.

### What this teaches (round 2)

The first-round fix was scoped to "the exact program in the
audit's repro" rather than "the class of bugs that program
exemplifies." Codex's round-2 finding was the same class of
bug, one syntactic level deeper. Pattern: when a fix has the
shape "if A: reject," ask whether the same logic applies to
"if A nested under B," "under B and C," etc. The receiver
walker generalises from the original NODE_IDENT-only check;
the alias-through-method-return is the next nested case I
chose not to address yet.

## 2026-05-17 — Borrow-aware drop/clone for `String` literals

`_drop_<X>` and `_clone_<X>` are synthesised per-struct in
`compiler/main.c` and walk the struct's heap-shaped fields
recursively. After P0-8 made `_drop_<X>` recurse into
struct-typed fields, a latent bug surfaced through `_drop_String`:

    Package(name is "foo", version is 7)

lowers to:
1. `_alloc_Package` zero-fills then auto-inits the `name` field
   via `_alloc_String` (a heap-allocated empty String).
2. The named-arg constructor stores the rodata literal pointer
   `_str0` over the auto-init'd `name` field.
3. End of test scope: `_drop_Package` calls
   `_drop_String(_str0)` which walks the rodata header,
   `_heap_free`s the rodata `array of byte` header (which
   isn't on the heap), and corrupts the heap free list.
4. The next allocation (in a later test) crashes with SIGSEGV.

`tests/test_spudlock_fixes.ptt` exhibited this: tests 0 and 1
printed pass, test 2 silently aborted (or exited 139,
depending on what the corrupted free list pointed at).

### Fix

Generalise the borrow convention encoded in `String`:
**a struct with an `int owned` field whose value is `0`
points at rodata and must not be freed**. This mirrors the
contract `iremit.c` already uses when emitting literal
`String` headers (`.quad 0` for the `owned` slot).

`_drop_<X>` now starts with:

    cbz x0, _drop_X_done
    ldr x9, [x0, #owned_off]
    cbz x9, _drop_X_done    // borrowed → no-op
    ; ... existing field-by-field free path ...

`_clone_<X>` now starts with:

    cbz x0, _clone_X_borrowed
    ldr x9, [x0, #owned_off]
    cbnz x9, _clone_X_owned
    _clone_X_borrowed: ret  // borrowed → return same pointer
    _clone_X_owned: ; ... existing deep-copy path ...

The check is gated on a static struct-shape match: a field
literally named `owned` whose declared type is `int`. Any
struct that doesn't follow the convention emits the original
unconditional drop/clone.

### Why a struct-shape convention rather than a special case for `"String"`

Naming the struct in the codegen would tie the runtime to a
single stdlib type. The shape-based detection lets any future
type that wraps borrow-able rodata (e.g. a `ByteSlice` or a
`StaticView of T`) opt in by mirroring the layout. The
convention is documented inline at the emit site and at the
String literal layout in `iremit.c::iremit_finalize_data`.

### Secondary leak (not fixed in this commit)

The named-arg constructor still leaks the auto-init'd empty
String when overwritten by the literal pointer. With the
borrow fix landed, this is a *leak*, not a crash — the
auto-init'd String becomes orphaned heap memory that lives
until the program exits.

A clean fix needs the constructor lowering to either
(a) skip auto-init for fields that the named-arg list
explicitly initialises, or (b) emit a drop on each
named-arg-initialised field before the literal pointer
store. Both are local to the named-arg path in `irgen.c`
and worth doing, but are out of scope for the immediate
crash fix.

Tracked as a separate follow-up. ASan confirms the leak is
deterministic and bounded (one auto-init'd String per
named-arg `String` field per construction).

### What this teaches

The earlier P0-8 commit message acknowledged that
`_drop_<X>` would walk struct-shaped fields, but didn't
think through the case where the field's value at drop
time was rodata rather than heap. The crash was latent
behind two preconditions:

1. A struct field of type `String`.
2. A named-arg constructor that stores a literal into
   that field (so the field holds rodata, not the
   auto-init'd heap String).

`tests/test_spudlock_fixes.ptt` happened to exercise
exactly this combination — a field of type `String`
initialised via a named-arg literal — which is why the
regression was scoped to that file. Other test programs
that use `String` literals in stack-local variables or
construct String fields via method calls (which return
heap-owned Strings with `owned == 1`) didn't hit the
path.

Lesson: when adding a recursive operation over struct
fields, classify each field by *runtime ownership* not just
*static type*. Heap and rodata both have type `String`;
they need different treatment.

## 2026-05-17 — Import resolution (P1-11) and duplicate-alias rejection (P1-12)

### P1-11 — transitive imports resolve from the *importing* file's directory

The `use <path>` resolver had three roots:
1. sibling to the source file
2. project root (walks up to `potato.toml`)
3. compiler binary dir (stdlib)

But "sibling" was implemented as `<dir-of-input>/<path>.ptt`,
where `input` is the top-level file the user invoked the
compiler on. So when `main.ptt` imported `lib/a.ptt` and
`a.ptt` did `use helper`, the resolver looked for
`<main's dir>/helper.ptt` instead of `<lib>/helper.ptt`. Real
projects with internal lib modules failed.

Fix: track the originating directory per `use` in a parallel
array (`use_origin_dirs`) maintained only at codegen time.
Top-level `use`s record the input file's dir. When the
import loop absorbs an imported file's `use`s into the parent
program, each absorbed entry records the *imported file's*
directory. Resolution then uses `use_origin_dirs[ui]` as the
sibling base instead of the global `dir`.

Why a side-array rather than adding `use_origin_dirs` to the
Program AST: this is purely a codegen-time concern. The AST
already exposes paths and aliases for the checker; origin
dirs only matter for file lookup. Keeping the side-table in
main.c localises the change.

Project root and compiler binary dir tiers are unchanged;
they were always absolute and never relative to the wrong
file.

### P1-12 — same module imported under two aliases

Pre-fix, dedupe was path-based:

    use lib as a
    use lib as b

would load `lib.ptt` once (under whichever alias's `use`
ran first), and the other alias bound to nothing. Calls
through the second alias surfaced as either a checker error
("module 'b' has no function 'greet'") with the source
location wrong, or — on older revisions before P0-1 — as a
linker error for an undefined `_b_greet`.

Two reasonable resolutions:
- (a) reject the duplicate path at use-resolution time.
- (b) load the module twice and prefix free functions with
  every alias.

(a) is simpler and forces the user to pick a single name for
the module. (b) implies that two aliases imply two
dispatch tables; for free functions that's just symbol
duplication, but for any future module-level state it's
genuinely two modules. The audit's expected direction
explicitly preferred "an early checker error unless multiple
aliases are a deliberate language feature." They're not, so
we reject.

Implementation: a quadratic scan over `use_paths` /
`use_aliases` before the load loop. If two entries share a
path but differ in alias, emit:

    error: module 'lib' imported twice under different
           aliases ('a' and 'b')
      note: each module loads once; only the first alias is
            bound to its free functions.
      help: pick a single alias for `lib`

Same path + same alias remains a no-op (path-based dedupe
catches it).

### Tests

- `tests/test_transitive_imports.ptt` (positive) — top-level
  imports `lib/transitive/inner`; `inner.ptt` does
  `use helper` (sibling to itself). Pre-P1-11 this failed
  to compile.
- `tests/lib/transitive/inner.ptt`, `helper.ptt` — the
  fixture pair under `tests/lib/`.
- `tests/errors/dup_alias.ptt` — `use std/math as a` and
  `use std/math as b`; expected to fail compilation with
  the new diagnostic.

### P1-11 round 2: dedupe by *resolved* path, not raw `use` text

Codex's review of the first P1-11 fix surfaced a closely
related but distinct bug: the load-loop dedupe and the
absorption-time dedupe both keyed on the raw `use_paths[ui]`
string. That's wrong as soon as two transitive `use`s share
the same use-path text but resolve to different files:

    main.ptt
      use lib1/a
      use lib2/b
    lib1/a.ptt -> use helper -> lib1/helper.ptt: val_one() => 1
    lib2/b.ptt -> use helper -> lib2/helper.ptt: val_two() => 2

After absorption, parent.use_paths contains two `helper`
entries (one with origin `lib1/`, one with `lib2/`). The
old load-loop saw the second `use helper` as "already loaded
because the string matches" and skipped it; the second
helper's symbols never made it into the program.

Fix: the load loop now (a) resolves each `use` to a concrete
file path *first*, then (b) dedupes by that resolved path.
Resolution is factored into `resolve_use_path()` which walks
the same three tiers (importer-sibling, project-root,
compiler-binary-dir). Cycles still terminate because
re-loading the same concrete file is still a no-op.

Absorption-time dedupe is loosened in the corresponding way:
it only skips entries that already exist under the *same*
(use_path, alias, origin_dir) triple. Distinct origin dirs
mean distinct queue entries even when the use-path text
matches; the load-loop's resolved-path dedupe handles
genuine duplicates downstream.

Test: `tests/test_transitive_imports_dup_filenames.ptt`
plus its fixture pair under `tests/lib/transitive_dup1/`
and `tests/lib/transitive_dup2/`.

What this teaches: the round-1 fix scoped "transitive
imports work for one chain" but didn't think through "two
chains with overlapping helper names." The dedupe key choice
(raw text vs resolved file) is the root invariant, not the
origin-dir tracking — origin-dir is a downstream consequence.
Same lesson as the round-1 ref-self enforcement: when a fix
has the shape "store extra metadata about each entry," ask
whether *every* place that compares entries needs to use that
metadata too. The load-loop and absorption-time dedupe were
two such places.

### P1-11 round 3: symbol prefix must follow resolved-module identity

Round 2 fixed the load/dedupe layer so distinct transitive
helpers were both queued and loaded. But the compiler still
emitted free-function symbols as `_<alias>_<func>` — using the
*alias text* the user wrote. With both lib1/a.ptt and
lib2/b.ptt declaring `use helper as h`, both emitted
`_h_<func>` symbols. Helper functions with overlapping names
(e.g. both define `answer()`) collided at the assembler:

    main.s:523:1: error: symbol '_h_answer' is already defined

This is not a theoretical case; Codex's repro program is six
files with the shared name `helper`. The shipped round-2
regression test happened to use distinct names (`val_one` vs
`val_two`), so it didn't exercise the symbol-collision path.

The semantic the user wrote is clear: `h` in lib1/a.ptt means
"the helper.ptt sibling to lib1/a.ptt", and `h` in lib2/b.ptt
means "the helper.ptt sibling to lib2/b.ptt". The alias is
*per-file lexical*; the compiler had been treating it as
*global identity*.

Fix: at import time, every `use X as A` (top-level or
transitive) gets a canonical synthetic alias derived from the
resolved file path. A small global map (`g_canon` in main.c)
keeps `resolved_path → canonical_alias` stable across the
program; the canonical strings are sequential `m0`, `m1`, ...

For the importing file's body, the user-written alias `A` is
mechanically rewritten to the canonical synthetic on every
NODE_METHOD_CALL whose receiver is a bare NODE_IDENT matching
`A`. This happens before checker / monomorph see the AST. The
existing `<alias>_<func>` symbol-prefix scheme then naturally
emits `<canonical>_<func>` symbols — one per resolved file.

User-facing diagnostics keep the user's alias by stashing it
in `method_call.alias_display` during the rewrite. The checker's
"function 'math.max' expects 2 args" still says `math`, not
`m1`.

Why a synthetic name and not the resolved path itself: the
canonical needs to be a valid C identifier suffix, short
enough to keep symbol names readable, and stable per
compilation. Sequential `m<N>` covers all three.

Why mechanical rewrite at import time, not at check time: the
existing checker logic for module-call dispatch already keys
on `<alias>_<func>` symbol lookup. Rewriting at parse time
keeps the checker untouched (modulo the diagnostic-display
fallback), reuses every downstream pass without per-pass
alias-scope plumbing.

Test: `tests/test_transitive_imports_dup_filenames.ptt` now
covers both the original "distinct symbols" case and the new
"overlapping symbol names" case via two `answer()` functions
in the sibling fixtures. ASan + UBSan clean across the full
suite at -O0/-O1/-O2.

What this teaches: when the compiler synthesises symbol
names, the synthesis must use *module identity* (resolved
file), not *user-supplied lexical names* (alias). Aliases
exist for ergonomics; they're not unique. Resolved file
paths are. Same lesson as P1-11 round 2 about dedupe keys —
both rounds were the same root issue (one layer apart): "what
identifies a module?" — and the right answer is always
"resolved path," never "user text."

### P1-11 round 4: alias rewrite must respect local scope

Codex round-4 review found that the round-3 rewriter walked
function bodies before the checker had any scope information
and unconditionally rewrote every NODE_METHOD_CALL receiver
matching an import alias. That mis-handles a local variable
that shadows the alias:

    use std/math
    MathBox is { value int }
    MathBox.max(self MathBox) int { give self.value }
    spark {
      math is MathBox(value is 7)
      yell(math.max())
    }

The user-visible name `math` here refers to the local — the
method call should dispatch on `MathBox.max()` and yield 7.
Pre-fix, the rewriter saw `math` matching the import alias
`math` and rewrote the receiver to the canonical synthetic.
The checker then complained "function 'math.max' expects 2
arguments, got 0" — pointing at the wrong language feature.

Fix: scope-aware rewrite. Maintain a flat `ScopeStack` of
names introduced by:

- function parameters (seeded at body entry)
- NODE_VAR_DECL (`x is ...`) — pushed *after* walking the
  initializer, so `x is foo()` reads `foo` in the outer scope
- NODE_THROUGH_RANGE / NODE_THROUGH_IN loop vars (scoped to
  the loop body)
- NODE_MATCH arm bindings (scoped to the arm body)
- NODE_BLOCK push/pop boundary

A receiver IDENT is rewritten only if no entry in the stack
matches its name. Locals that shadow aliases now resolve
through the checker's normal struct-method dispatch, exactly
as users expect.

The aliases-and-local-coexist case still works: in a function
where some lines use `math.max(a, b)` (alias call) and later
lines introduce a local `math`, the loop visits in order. Each
call site's rewrite decision uses the scope at that point —
pre-shadow calls get rewritten, post-shadow calls don't.

Test: `tests/test_alias_shadowing.ptt` covers four cases:
local var, function param, alias-and-local-coexisting in the
same scope, and match arm binding.

What this teaches: pre-checker AST rewrites that depend on
*name resolution* are dangerous unless they replicate the
checker's scope rules exactly. The earlier rewrite was a
"matches alias text? rewrite" pattern that ignored every
shadowing case the language actually supports. Round 4
threads a small scope tracker through the walker so the
rewrite has the same name-binding view the checker eventually
will. Cleaner long-term option: do the rewrite at the
checker, where the scope already exists. Kept at parse time
in this commit because the existing module-call dispatch
infrastructure (find_func with `<alias>_<func>` symbol names)
fits the rewrite shape; restructuring the checker is a bigger
sweep.

## 2026-05-17 — Retire the legacy `str` type (P2-16)

CLAUDE.md, the language guide, and the keywords reference all
say there is no `str → String` sugar. The lexer still
recognised `str` as a keyword (TOK_STR_TYPE) and the parser
silently accepted it in type positions and on method-def
heads, transparently treating it as `String`. The audit's
expected direction is removal — the design-consistent
behaviour is that programs migrate to `String` and add
`use std/string`.

Implementation:

- `parse_type_name` (parser entry for type positions) errors
  on TOK_STR_TYPE with a teaching message and `help:` line.
- The variable-annotation path (`x is str`) errors before
  consuming the TOK_STR_TYPE.
- The method-def head (`str.foo(self str)`) errors before
  recording the receiver type, suggesting the user move the
  method to `String.foo(self String)`.
- The lexer keeps producing TOK_STR_TYPE so the parser can
  surface a single unique error rather than fall back to
  generic "unknown identifier."

The `TYPE_STR` enum value and the dead-branch comparisons in
the checker are retained as transitional fossil — nothing in
the source path produces TYPE_STR anymore, but the existing
"is this a string?" predicate cells still test it as a
fallback. A future cleanup can delete those when the
str-rewrite phase is fully retired in `docs/string-rewrite.md`.

Test: `tests/errors/legacy_str_type.ptt` — a function
parameter typed `str`, expected to error at the parser with
the teaching diagnostic.

Lesson: deprecated transitional features should fail loudly,
not silently. Keeping `str` accepted "for backwards
compatibility" was a low-grade UX bug — every user reading
documentation would conclude `str` was retired and write
`String`, but copy-pasting old examples or older agent output
would silently work, masking the migration. A clean error
makes the deprecation visible and surfaces the migration step
(add `use std/string`) at the right moment.
