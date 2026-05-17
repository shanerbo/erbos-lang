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
