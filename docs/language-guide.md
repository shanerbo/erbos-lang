# Potato Language Guide 🥔

## Entry Point

```
spark {
  yell("hello world")
}
```

## Variables

```
x is 10              // inferred type
x is int 10          // explicit type
nomut pi is 3        // immutable
x be 42              // reassignment
```

All variables must be initialized.

## Types

### Primitives

These are compiler-builtin types — the names are reserved keywords
and the compiler knows their size and representation intrinsically.
Primitives are lowercase by convention; this is the visual signal
that distinguishes them from PascalCase user-defined or stdlib types.

| Type | Size | Range / Values | Notes |
|------|------|----------------|-------|
| `int` | 8 bytes | signed 64-bit (−2^63 .. 2^63−1) | The default integer type. Arithmetic operators (`+ - * /`), comparisons, and `mod` are intrinsic. |
| `bool` | 8 bytes | `true` or `false` | Stored as 0/1. Used in conditionals (`?{ }`), `infi`, and the boolean operators `and` / `or` / `not`. |
| `byte` | 1 byte | unsigned 8-bit (0..255) | Only valid as the element type of `array of byte`. Used by `std/string` to back UTF-8 storage. There is no bare `byte` variable type — promote to `int` or store inside `array of byte`. |
| `void` | — | no value | Function-return-type only. Functions without a return type implicitly return `void`. Cannot appear as a variable type. |

### Typed-storage primitive

`array of T` is the lowest-level container the compiler provides.
It's fixed-capacity, element-typed, and what the stdlib types are
built on top of.

| Form | Description |
|------|-------------|
| `array of T with cap N` | construct an `array of T` with fixed capacity `N` (any `int` expression) |
| `array of byte with cap N` | byte-element specialization (1-byte `ldrb`/`strb`); used by `std/string` |

Layout: 16-byte header (`cap` at offset 0, `data` pointer at offset 8),
plus the element storage at `data`. See `docs/builtins.md` for the
intrinsics (`heap_alloc`, `mem_load`, etc.) the runtime exposes.

### Stdlib types

These are PascalCase **because they are user-defined structs** —
you can grep for the definition in the `std/` directory. The
compiler treats them like any other user struct; there's no special
casing. `use std/...` brings them into scope.

| Type | Defined in | Notes |
|------|------------|-------|
| `String` | `std/string.ptt` | UTF-8 text, backed by `array of byte`. String literals (`"..."`) are values of this type. Requires `use std/string` (no auto-load). |
| `List of T` | `std/list.ptt` | Dynamic, growable list. `[1, 2, 3]` literals lower to `List of int` when `std/list` is in scope. |
| `Map of K, V` | `std/map.ptt` | Ordered key→value map. `K` may be `int`, pointer-shaped, or `String`. `["k" to v]` literals lower to `Map of String, V` when `std/map` is in scope. |

The bundle import `use std/basics` brings in `String + List + Map` together.

### Special values

| Value | Type | Notes |
|-------|------|-------|
| `true`, `false` | `bool` | Boolean literals. |
| `nil` | pointer-shaped | The null/empty pointer. Compares equal to an unset struct/array reference. Has no separate type — it takes whichever pointer type the context expects. |

## Functions

```
add(a int, b int) int {
  give a + b
}

greet(name String) {
  yell("hello {name}")
}
```

`give` returns a value. Bare `give` returns void.

### Calling convention

| Param type | What's passed | Aliasing | Mutation |
|------------|---------------|----------|----------|
| `int`, `bool`, `byte` | the value (8 bytes) | none — it's a copy | n/a |
| `Point`, `String`, `List of T`, etc. (no `ref`) | a pointer to the caller's heap block | yes — same data | **read-only** (compile error to mutate fields) |
| `ref T` (any heap-shaped T) | the same pointer | yes | callee may mutate fields |

A non-`ref` parameter of a heap-shaped type is the equivalent of
C++'s `const T&`: same data as the caller sees, no copy, no
mutation. There's no separate `nomut ref T` syntax — the absence
of `ref` already says read-only.

Borrows are *lexical to the call*: an argument is only aliased
for the duration of that function's activation. There's no
syntax for a long-lived borrow (no `&p`, no `is ref`), which is
what lets the compiler stay simple while remaining safe.

## Conditionals

```
x gt 10 ?{
  yell("big")
} x gt 5 ?{
  yell("mid")
} nah {
  yell("smol")
}
```

## Loops

```
// Range
through (i from 0 to 10 by 1) { }

// Collection
through (item in nums) { }

// While
infi (x gt 0) { x be x - 1 }

// Infinite + break
infi { stop }
```

`stop` = break, `skip` = continue.

## Strings

`String` is a stdlib struct backed by a UTF-8 byte buffer. `use
std/string` is the explicit import; `use std/basics` brings it in
along with the other stdlib types.

```
use std/string

name is "world"
yell("hello {name}")          // interpolation (int and String vars)
c is "hi" + " there"          // concat returns a fresh String
yell("hello".len())           // 5
yell("abc".char_at(1))        // "b" (1-byte String)
yell(42.to_string())          // "42"
```

The methods come from `std/string.ptt`: `len`, `empty`, `equals`,
`byte_at`, `char_at`, `concat` (called via `+`), and `int.to_string`
on int receivers.

## Structs

```
Point is { x int, y int }

// Or, equivalently, multiline (newlines also separate fields):
Point is {
  x int
  y int
}
```

Fields can be separated by commas, newlines, or both. Use
whichever fits — short structs read fine on one line; longer
ones get the multiline form.

There are two ways to construct a struct value:

**Zero-default** — every field starts at the type's zero value:

```
p is Point()
p.x be 10
p.y be 20
```

**Named-arg** — atomic init, every declared field set in one go:

```
p is Point(x is 10, y is 20)
```

Rules for named-arg construction:

- Every declared field must appear exactly once.
- Order in the call is free; values land in declared field order.
- Each value's type must match the field's declared type.
- Mixing named and positional args (`Point(1, y is 2)`) is a
  compile error.
- Positional struct constructors (`Point(1, 2)`) are not allowed —
  they tie call sites to declaration order with no syntactic
  warning. Use named-arg or define a factory method.

Use whichever form fits. Zero-default is cheaper to write when
you'll mutate fields anyway. Named-arg is required when you want
the binding to be `nomut` (see below) or when you want refactor-
safe construction that breaks loudly if a new field is added.

### `nomut` and structs

`nomut x is V` means: the binding `x` cannot be reassigned, and
its fields cannot be directly mutated. Method calls that take
`ref self` are still allowed — they're an explicit opt-in by the
type author.

```
nomut origin is Point(x is 0, y is 0)   // ok: atomic init
// origin.x be 1                        // error: nomut blocks field mutation
// origin be Point(x is 9, y is 9)      // error: nomut blocks reassignment

nomut xs is List of int                 // ok: zero-default of stdlib container
xs.push(10)                              // ok: List.push takes `ref self`
```

`nomut p is Point()` parses but is generally a dead end — without
field mutation you can never set `p.x` / `p.y` to anything other
than zero. Prefer `nomut p is Point(x is ..., y is ...)` so the
value is fully determined at the bind site.

## Methods

A method is a function attached to a user type. Define it with
`Type.name(self [ref] Type, ...)`:

```
Counter is { value int }

Counter.bump(self ref Counter) {
  self.value be self.value + 1
}

Counter.add(self ref Counter, n int) {
  self.value be self.value + n
}

Counter.get(self Counter) int {
  give self.value
}

spark {
  c is Counter()
  c.bump()
  c.add(40)
  yell(c.get())   // 41
}
```

Rules:
- The first parameter is the receiver. By convention it's named
  `self` (matching Rust / Python / Swift). Use `ref` to allow
  mutation; without `ref`, the method cannot modify struct fields
  (same enforcement as ordinary `ref` parameters).
- The receiver's declared type must match the type the method is attached
  to. `Foo.bar(self Bar) { ... }` is a compile error.
- Struct and enum names must start with an uppercase letter
  (PascalCase). The grammar uses the leading-case rule to
  disambiguate `Foo()` (struct constructor) from `foo()`
  (function call) at parse time. The checker errors with a
  suggested capitalized form on any lowercase-leading struct
  or enum.
- Methods on enum types work the same way:
  ```
  Result.tag(self Result) int { give 99 }
  ```
- Method dispatch is statically resolved. The compiler emits each method as
  `_<Type>_<name>` (for example `_Counter_bump`), so different types can
  share method names without collision.
- Built-in `task` methods (`fire`, `collapse`) lower to runtime
  stubs. User methods on user types take precedence over any
  same-named free function.

## Generics

Structs and methods can be parametric. Type parameters appear after
`of`; additional type parameters use commas:

```
Box of T is {
  value T
}

Box.set(self ref Box of T, v T) {
  self.value be v
}

Box.get(self Box of T) T {
  give self.value
}

Both of K, V is {
  key K
  value V
}

spark {
  // Two distinct instantiations of the same template. With no
  // value expression after `is`, the compiler auto-constructs:
  bi is Box of int
  bi.set(42)
  bs is Box of String
  bs.set("hello")
  yell(bi.get())   // 42
  yell(bs.get())   // hello

  p is Both of String, int
  // ...
}
```

Auto-construct also works for an explicit constructor call —
`bi is Box of int ()` is equivalent. The trailing `()` is a
nullary call on the type expression. The no-parens form is
preferred — `xs is List of int` reads as "xs is a list of int"
rather than "xs is a list-of-int call result".

Rules:

- Type parameters are bound by the surrounding declaration. Inside
  `Box.set`, the name `T` refers to whatever the caller instantiated
  the box with — the compiler reads it off the receiver's `of T`
  clause, so the method head doesn't repeat the parameter list.
- Constructors at use sites carry their type arguments explicitly:
  `Box of int ()`, `Both of String, int ()`, `Map of String, int ()`.
  The compiler does not currently infer type arguments from context.
- The compiler **monomorphizes**: each unique concrete form
  (`Box of int`, `Box of String`, `Both of String, int`, …) becomes
  its own emitted struct layout and its own emitted methods. There
  are no v-tables and no runtime type tags.
- Symbols are mangled positionally with `__` separators:
  `Box of int` → `_Box__int`, `Map of String, int` → `_Map__String__int`,
  `List of List of int` → `_List__List__int`.
- Nested generics are supported and parse right-associatively, so
  `Map of String, List of int` reads as `Map of String, (List of int)`.
  There is no `to` generic separator, no parens, and no `<>` in type
  position.
- Multi-parameter generics use the same comma form for every type:
  `Map of K, V`, `Result of T, E`, `Pair of A, B`.
- Instantiating an undeclared template is a compile error
  ("cannot instantiate 'Foo<int>' — no generic type named 'Foo' is in scope").

## Lists

```
use std/list

nums is List of int           // auto-constructed
nums.push(10)
nums.push(20)
yell(nums[0])                 // 10
yell(nums.len())              // 2
last is nums.pop()            // 20

// Literal — lowers to `List of int` because std/list is in scope:
vals is [1, 2, 3]
yell(vals[0])                 // 1

// Chained indexing on List of List of int:
grid is List of List of int
inner is List of int
inner.push(1)
inner.push(2)
inner.push(3)
grid.push(inner)
yell(grid[0][1])              // 2

// Iteration:
through (x in nums) {
  yell(x)
}
```

## Maps

```
use std/map

// String-keyed
scores is Map of String, int
scores.set("alice", 95)
yell(scores.get("alice"))     // 95
yell(scores.len())            // 1

// Int-keyed
memo is Map of int, int
memo.set(42, 100)
yell(memo.get(42))            // 100

// Map literal — lowers to `Map of String, int` because
// std/map is in scope:
m is ["name" to 1, "age" to 2]
yell(m.get("name"))           // 1

// Iteration via .keys() returns a List of K:
keys is scores.keys()
through (k in keys) {
  yell(k)
  yell(scores.get(k))
}
```

`Map` handles String keys via byte-equality compare automatically:
the `eq` operator on String operands routes through `_str_eq`
inside generic code, so two `"foo"` literals from different
rodata addresses still match.

## Enums + Pattern Matching

```
Result is
  Ok(value int)
  | Err(message String)

r is Result.Ok(42)

match r {
  Ok(v) => yell(v)
  Err(msg) => yell(msg)
}
```

## Ownership & Memory

```
// RAII: auto-free at scope end
{ p is Point(); p.x be 42 }  // p freed here

// Move — `a` is inaccessible after this; reads error
b is now a

// Deep clone — fresh independent block; both `b` and `c` alive
c is rep b

// Plain alias for heap values is rejected — be explicit:
//   d is c          // COMPILE ERROR for struct/list/map/string/array
//   d is now c      // OK — moves
//   d is rep c      // OK — clones

// `field be now src` / `field be rep src` for the same on
// struct fields:
out.items be now ctx.items   // transfer ctx's list into out
out.config be rep cfg        // independent copy of cfg

// Ref params (mutable borrow)
reset(p ref Point) { p.x be 0 }
reset(ref pt)                 // caller acknowledges
```

> `ref` is enforced: mutating a non-ref struct parameter is a compile error.

Primitives (`int`, `bool`, `byte`) copy by value, so plain
`b is a` is fine for them. The explicit-aliasing rule applies
only to heap-shaped values (struct, List, Map, array, String).

## Larger programs — the `App` pattern

Potato has no module-level / global variables. Every binding
lives inside a `spark { }` block, a `test "..." { }` block, or
a function body. This is deliberate (see
[`docs/design-decisions.md`](design-decisions.md)) — function
signatures stay honest about what data they touch, and
initialization-order bugs become impossible.

The convention for programs that need long-lived shared state is
to wrap every arena in a single top-level `App` struct, owned
by `spark`. Functions that need an arena take `ref App` (or
just the specific arena, when that's all they touch).

```
use std/list

Image is { pixels int }
ImageStore is { images List of Image }

ImageStore.add(self ref ImageStore, img Image) int {
  self.images.push(img)
  give self.images.len() - 1
}

User is { id int }
UserStore is { users List of User }

UserStore.add(self ref UserStore, u User) int {
  self.users.push(u)
  give self.users.len() - 1
}

// One App holds every long-lived arena.
App is {
  images ImageStore
  users  UserStore
}

// Functions reach data through `ref App` (or a specific arena).
show_icon(app ref App, icon_id int) {
  icon is app.images.images.get(icon_id)
  yell(icon.pixels)
}

count_users(users UserStore) int {
  give users.users.len()
}

spark {
  app is App()

  // Populate. Field auto-init means app.images and app.users
  // are already empty arenas — see "struct field auto-init"
  // in design-decisions.md.
  app.images.add(Image(pixels is 32000))
  app.users.add(User(id is 1))
  app.users.add(User(id is 2))

  // Use.
  show_icon(ref app, 0)        // 32000
  yell(count_users(app.users)) // 2
}
```

This is structurally identical to "one global `App` instance"
in C++/Python — same data accessible from the same set of
functions — but the path is *visible in every signature*. A
function that doesn't take `ref App` can't reach into the App,
even by accident. Testability follows: pass a fresh `App` to
test a function in isolation.

When a function only needs one arena, take just that arena
(`count_users(users UserStore)`), not the whole `App`. Smaller
signatures are clearer about what the function depends on.

## Imports

```
use std/math
use lib/helper/str_ops
use lib/helper/parse_helpers as parse

spark {
  yell(math.max(10, 20))
  yell(str_ops.upper("hello"))
  parse.run()
}
```

### Resolution rules

A `use <path>` statement is resolved against three roots in order.
First match wins.

| # | Root | What it's for |
|---|------|---------------|
| 1 | `<dir-of-source-file>/<path>.ptt` | Sibling files. Two files in the same directory can `use` each other by basename. |
| 2 | `<project-root>/<path>.ptt` | Anywhere under your project. Project root is found by walking up from the source file looking for `potato.toml`. |
| 3 | `<compiler-binary-dir>/<path>.ptt` | Bundled stdlib. `use std/list` always works regardless of cwd or project structure. |

The path is **a sequence of identifier-shaped segments separated
by `/`**, with `.ptt` stripped. There are no quoted paths
(`use "foo"` is a parse error), no upward-relative paths
(`use ../foo` is a parse error), no absolute paths.

### `potato.toml` — the project root marker

Any directory containing a `potato.toml` file is treated as a
project root. The file can be empty for now; the format is
reserved for future build/dependency metadata.

```
my-project/
  potato.toml          ← marker — empty file is fine
  lib/
    helper/
      str_ops.ptt
  src/
    main.ptt
```

In `src/main.ptt`, `use lib/helper/str_ops` resolves to
`my-project/lib/helper/str_ops.ptt`. Run `erbos run src/main.ptt`
from anywhere on the filesystem — the walk-up finds the marker.

Programs that only use stdlib + sibling imports don't need a
`potato.toml`. As soon as you write `use lib/...` or any other
non-stdlib path, the marker is required.

### `as` aliasing — for free functions only

`use lib/foo as f` makes `f` the alias prefix for free functions
defined in `lib/foo.ptt`. So instead of `lib_foo.bar(x)` you write
`f.bar(x)`.

Aliases only rename the **free-function namespace**. They have no
effect on:
- **Types** — types are global; `Counter` is `Counter` regardless
  of which file defined it. There's no `lib_foo.Counter`.
- **Methods** — dispatched by receiver type, not by importer.
  `c.bump()` works the same whether the file imported `lib/foo`
  or `lib/foo as f`.

If your import path is short and readable already, skip the alias.
Use `as` when two imported files have similar prefixes
(`use tests/lib/leetcode/longest_substr_brute as brute`).

### What the language deliberately doesn't have

- **No `..` in paths.** Forward-only, by grammar.
- **No selective imports** (`use std/math.{max,min}`). Adds
  complexity without paying rent. Bring in the whole module.
- **No wildcard imports** (`use std/math.*`). Footgun.
- **No third-party library system** (no Cargo / go get
  equivalent yet). When we have one, it'll go in `potato.toml`.
- **No module-level globals.** See "Larger programs — the App
  pattern" above.

### Standard library

- `std/string` — `String`, `String.*`, `int.to_string`
- `std/list` — `List of T`
- `std/map` — `Map of K, V` (K = int, pointer-shaped, or String)
- `std/math` — `min`, `max`, `abs`, `pow`
- `std/queue`, `std/stack` — bounded fixed-cap queues / stacks
- `std/basics` — bundle (re-exports String + List + Map)

`use std/string` is required whenever your program uses `String`
in any form (literal `"..."`, type name, method call). The
auto-load that used to make this implicit is gone — every import
is now explicit.

## Testing

```
test "addition" {
  assert(1 + 1 eq 2)
  assert(add(3, 4) eq 7)
}
```

Run: `erbos test file.ptt`

## Operators

| Symbol | Word | Meaning |
|--------|------|---------|
| `+` `-` `*` `/` | — | arithmetic |
| `%` | `mod` | modulo |
| `>` | `gt` | greater than |
| `<` | `lt` | less than |
| `>=` | `ge` | greater or equal |
| `<=` | `le` | less or equal |
| `==` | `eq` | equal |
| `!=` | `ne` | not equal |
| — | `and` `or` `not` | logical (short-circuit) |

## Build & Run

```bash
./erbos program.ptt          # compile to binary
./erbos run program.ptt      # compile + run + cleanup
./erbos test tests.ptt       # run test blocks
./erbos ir program.ptt       # emit assembly only (program.s), don't link
```

The `-O0` / `-O1` / `-O2` flags select an optimization level; `-O1` is
the default. They can appear anywhere relative to the subcommand:

```bash
./erbos -O0 run program.ptt  # skip iropt entirely
./erbos run -O1 program.ptt  # explicit default
./erbos -O2 program.ptt      # reserved for tuning; same as -O1 today
```
