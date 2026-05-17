# Potato Keywords 🥔

> **Status:** mid-overhaul. `assert` was demoted from reserved to
> a stdlib function (γ3); `spark` was promoted from identifier to
> reserved keyword (α0). The legacy `list` / `map` / `imap` keyword
> forms are still accepted but scheduled for removal in phase ε
> of [`OVERHAUL.md`](../OVERHAUL.md).

| Keyword | Purpose | Example |
|---------|---------|---------|
| `spark` | program entry point | `spark { }` |
| `is` | variable declaration | `x is 10` |
| `be` | reassignment | `x be 42` |
| `nomut` | immutable variable | `nomut x is 10` |
| `give` | return value | `give x + y` |
| `now` | move ownership | `b is now a` |
| `rep` | clone (shallow) | `b is rep a` |
| `ref` | mutable borrow param | `func(p ref Point)` |
| `through` | range/collection loop | `through (i from 0 to 10 by 1) { }` |
| `from` / `to` / `by` | loop range | see `through` |
| `in` | collection iteration | `through (x in list) { }` |
| `infi` | while/infinite loop | `infi (cond) { }` |
| `stop` | break | `stop` |
| `skip` | continue | `skip` |
| `nah` | else | `} nah { }` |
| `and` / `or` / `not` | logical (short-circuit) | `x gt 0 and x lt 10` |
| `eq` / `ne` / `gt` / `lt` / `ge` / `le` | comparisons | `x eq 5` |
| `mod` | modulo | `x mod 3` |
| `true` / `false` | booleans | `x is true` |
| `nil` | null pointer | `root eq nil` |
| `array` | typed-storage primitive (`array of T`, `array of byte`) | `xs is array of int with cap 8` |
| `with` / `cap` | array constructor parts | `array of int with cap 8` |
| `of` / `to` | generic type connectives | `List of int`, `Map of str to int` |
| `match` | pattern match on enum | `match r { Ok(v) => ... }` |
| `use` | import module | `use std/math` |
| `as` | import alias | `use path as name` |
| `test` | define test block | `test "name" { }` |
| `task` | concurrency handle (compiled-mode placeholder; see note) | `t is task()` |

### Scheduled for removal (phase ε)

| Keyword | Replacement |
|---------|-------------|
| `list` (in `list of T` and the implicit `[a,b,c]` literal type) | `List of T` from `std/list` |
| `map` (in `map of K to V` and `["k" to v]` literals) | `StringMap of V` from `std/string_map` |
| `imap` | `Map of int to V` from `std/map` |

### No longer reserved

| Word | Why |
|------|-----|
| `assert` | Now a stdlib function (γ3). `assert(cond)` parses as a regular call; the checker recognises the name and lowers to `_assert_fail`-on-false. |

## Symbols

| Symbol | Meaning |
|--------|---------|
| `?{` | if (condition before `?`) |
| `=>` | match arm arrow |
| `\|` | enum variant separator |
| `{ }` | block / scope |
| `( )` | params, loop header |
| `[ ]` | list/map literal, index |
| `.` | field access, method call, module access; method-def head `Type.name(self ...)` |
| `+` `-` `*` `/` `%` | arithmetic |
| `>` `<` `>=` `<=` `==` `!=` | comparisons |
| `//` | single-line comment |
| `/* */` | multi-line comment |

> Generics use word-style `of` and `to` only — there is **no** `<T>`
> syntax in any type position. The angle-bracket form was retired
> in phase P3.1; see [`generics-syntax.md`](generics-syntax.md).

> **Note on `task`:** the green-thread runtime in `src/runtime.c`
> is exercised by `make test-runtime` (C-level harness) but is not
> yet integrated into compiled `.ptt` output. In compiled mode
> `_task_fire` and `_task_collapse` are no-ops, so `t.fire(worker())`
> runs `worker()` synchronously.
