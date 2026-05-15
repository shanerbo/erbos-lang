# Potato Keywords 🥔

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
| `list` | list type | `nums is list of int` |
| `map` | string-key map type | `m is map of str to int` |
| `imap` | int-key map type | `m is imap of int to int` |
| `of` / `to` | type parameters | `list of int`, `map of str to int` |
| `match` | pattern match on enum | `match r { Ok(v) => ... }` |
| `use` | import module | `use std/math` |
| `as` | import alias | `use path as name` |
| `test` | define test block | `test "name" { }` |
| `assert` | test assertion | `assert(x eq 5)` |
| `task` | concurrency handle (compiled-mode placeholder; see note) | `t is task()` |

## Symbols

| Symbol | Meaning |
|--------|---------|
| `?{` | if (condition before `?`) |
| `=>` | match arm arrow |
| `\|` | enum variant separator |
| `{ }` | block / scope |
| `( )` | params, loop header |
| `[ ]` | list/map literal, index |
| `.` | field access, method call, module access |
| `+` `-` `*` `/` `%` | arithmetic |
| `>` `<` `>=` `<=` `==` `!=` | comparisons |
| `//` | single-line comment |
| `/* */` | multi-line comment |

> **Note on `task`:** the green-thread runtime in `src/runtime.c` is exercised by `make test-runtime` (C-level harness) but is not yet integrated into compiled `.ptt` output. In compiled mode `_task_fire` and `_task_collapse` are no-ops, so `t.fire(worker())` runs `worker()` synchronously.
