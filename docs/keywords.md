# Potato Keywords 🥔

The reserved words. Anything not on this list is either an
identifier (variable, struct name, method name) or a stdlib
name resolved by `use`.

| Keyword | Purpose | Example |
|---------|---------|---------|
| `spark` | program entry point | `spark { }` |
| `is` | variable declaration | `x is 10` |
| `be` | reassignment | `x be 42` |
| `nomut` | immutable binding (no reassignment, no direct field mutation) | `nomut p is Point(x is 0, y is 0)` |
| `give` | return value | `give x + y` |
| `now` | move ownership | `b is now a` |
| `rep` | deep clone | `b is rep a` |
| `ref` | mutable borrow param | `func(p ref Point)` |
| `through` | range/collection loop | `through (i from 0 to 10 by 1) { }` |
| `from` / `to` / `by` | loop range | see `through` |
| `in` | collection iteration | `through (x in xs) { }` |
| `infi` | while/infinite loop | `infi (cond) { }` |
| `stop` | break | `stop` |
| `skip` | continue | `skip` |
| `nah` | else | `} nah { }` |
| `and` / `or` / `not` | logical (short-circuit) | `x gt 0 and x lt 10` |
| `eq` / `ne` / `gt` / `lt` / `ge` / `le` | comparisons | `x eq 5` |
| `mod` | modulo | `x mod 3` |
| `true` / `false` | booleans | `x is true` |
| `nil` | null pointer | `root eq nil` |
| `array` | typed-storage primitive (`array of T`, `array of byte`) — **contextual**, see note below | `xs is array of int with cap 8` |
| `with` / `cap` | array allocation parts — **contextual**, see note below | `array of int with cap 8` |
| `of` | generic type connective | `List of int`, `Map of String, int` |
| `match` | pattern match on enum | `match r { Ok(v) => ... }` |
| `use` | import module | `use std/math` |
| `as` | import alias | `use path as name` |
| `test` | define test block | `test "name" { }` |
| `task` | concurrency handle (compiled-mode placeholder; see note) | `t is task()` |

### Contextual words (`array`, `with`, `cap`)

These three are not reserved by the lexer — they tokenise as plain
identifiers — but the parser recognises them in array contexts
(after `is`, in type positions). That means a user *could* shadow
them with a local of the same name (`with is 7`), and the
compiler will not stop them. Editor tooling (the VS Code grammar)
does highlight them as keywords for clarity. Don't rely on the
contextual-only behaviour: future revisions may promote them to
true reserved words.

### Names that look like keywords but aren't

| Word | What it is |
|------|------------|
| `assert` | A stdlib function. `assert(cond)` parses as a regular call; the checker recognises the name and lowers to `_assert_fail`-on-false. Conceptually a `std/test` function. |
| `String` | The canonical text type, defined as a struct in `std/string.ptt`. String literals (`"..."`) are typed as `String` directly. |
| `List` / `Map` | Stdlib container structs from `std/list` / `std/map`. Used as `List of T` / `Map of K, V`. |
| `yell` | A compiler-known function. Resolves at compile time on the static type of its argument (int / bool / String / user struct). Users overload by defining `Type.yell(self Type)`. |

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

> Generics use word-style `of` only — type arguments are
> comma-separated. There is **no** `<T>` syntax in any type
> position, and `to` is not a generic separator (it remains
> reserved for `through (i from 0 to n by 1)` range loops and
> `["k" to v]` map literals). See the "Generics" section in
> [`language-guide.md`](language-guide.md).

> **Note on `task`:** the green-thread runtime in `compiler/runtime/`
> is exercised by `make test-runtime` (C-level harness) but is not
> yet integrated into compiled `.ptt` output. In compiled mode
> `_task_fire` and `_task_collapse` are no-ops, so `t.fire(worker())`
> runs `worker()` synchronously.
