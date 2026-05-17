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
**Status:** known bug — fix on the roadmap (deep clone for `rep`)

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
