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
