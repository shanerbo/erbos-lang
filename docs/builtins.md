# Potato Built-in Functions 🥔

Reference for the names the Potato compiler treats specially:
`yell`, `assert`, `len`, plus string / collection methods that
come from the stdlib (`use std/...`).

## Output

| Function | Description | Example |
|----------|-------------|---------|
| `yell(value)` | Print a value with a trailing newline. Resolves at compile time on the static type — int / bool / byte → `_yell_int`, String → `_String_yell`, struct T → `_<T>_yell` (user-defined). | `yell(42)`, `yell("hi")`, `yell(c)` (where `Counter.yell(self Counter)` is defined) |

Users overload `yell` on their own types by defining
`Type.yell(self Type) { ... }`; subsequent `yell(value : Type)`
calls resolve to it at compile time.

## Strings

`String` is a stdlib struct (`std/string.ptt`). Every program
that uses `String` must include `use std/string` (or the bundle
`use std/basics`). String literals (`"..."`) compile to a String
header even without the import — the compiler-runtime built-in
`_String_yell` makes bare `yell("hi")` work — but any *method*
call (`s.len()`, `s.equals(t)`, `n.to_string()`) needs the
import to resolve to the user-defined `String.*` methods.

| Method / Operator | Description | Example |
|---|---|---|
| `s.len()` | byte length | `"hello".len()` → 5 |
| `s.empty()` | true if zero-length | `s.empty()` |
| `s.equals(t)` | byte-equality compare | `s.equals(t)` |
| `s.byte_at(i)` | byte at index, as int | `"abc".byte_at(0)` → 97 |
| `s.char_at(i)` | 1-byte String at index | `"abc".char_at(0)` eq `"a"` |
| `s + t` | concatenation, returns a fresh String | `"hi" + " there"` |
| `s eq t`, `s ne t` | structural (byte-by-byte) equality | `s eq "hello"` |
| `n.to_string()` | int → decimal String | `42.to_string()` |
| `s.yell()`, `yell(s)` | print + newline | both emit `bl _String_yell` |

String literals lower to a 32-byte `String` header backed by an
`array of byte`; the rodata bytes live behind a two-tier
indirection (String → array of byte → bytes). The compiler knows
the layout; user code only sees `String`.

String interpolation `"hi {name}"` desugars to a chain of `+`
operations — same `String.concat` path as explicit concatenation.

## Lists (`std/list`)

`use std/list` brings `List of T` into scope. Literals
`[a, b, c]` lower to `List of int` automatically when the
import is present.

| Method / Form | Description | Example |
|---|---|---|
| `xs is List of T` | construct an empty list | `nums is List of int` |
| `xs.push(v)` | append | `nums.push(10)` |
| `xs.pop()` | remove and return last | `last is nums.pop()` |
| `xs.get(i)` / `xs[i]` | element at index | `nums[0]` |
| `xs.set(i, v)` / `xs[i] be v` | write at index | `nums[0] be 99` |
| `xs.len()` | count | `nums.len()` |
| `[a, b, c]` | list literal | `nums is [1, 2, 3]` |
| `through (x in xs) { }` | iterate | dispatches to `.len` + `.get` |

## Maps (`std/map`)

`Map of K, V` handles every key type: int, pointer-shaped
struct, or `String`. The binary `eq` operator on String operands
routes through `_str_eq` (byte-by-byte compare) inside generic
code, so two `"foo"` literals from different rodata addresses
match correctly.

Map literals `["k" to v]` lower to `Map of String, int`
automatically when `use std/map` is in scope.

| Method / Form | Description |
|---|---|
| `m is Map of K, V` | construct an empty map |
| `m.set(key, val)` | insert / update |
| `m.get(key)` | get value (0 if absent) |
| `m.keys()` | `List of K` of keys in insertion order |
| `m.len()` | entry count |
| `["k" to v, ...]` | string-keyed map literal |

## Structs

| Syntax | Description | Example |
|--------|-------------|---------|
| `StructName()` | heap-allocate struct | `Point()` |
| `Type.method(self [ref] Type, ...)` | define a method on a struct or enum | `Counter.bump(self ref Counter) { ... }` |
| `StructName of T` (no parens) | auto-construct a generic instantiation | `Box of int`, `Map of String, int` |
| `Type.method(self [ref] Type of T, ...)` | method on a generic type | `Box.set(self ref Box of T, v T) { ... }` |

Struct and enum names must start with an uppercase letter
(PascalCase). The grammar uses leading-case to disambiguate
`Foo()` (constructor) from `foo()` (call) at parse time.

## Tasks (concurrency placeholder)

| Syntax | Description | Example |
|--------|-------------|---------|
| `t is task()` | create a task handle | `t is task()` |
| `t.fire(fn(...))` | schedule a call (synchronous in compiled output today) | `t.fire(worker())` |

> The green-thread runtime in `compiler/runtime/` runs in
> `make test-runtime` only; it is not yet wired into compiled
> binaries.

## Testing

| Function | Description | Example |
|----------|-------------|---------|
| `assert(cond)` | pass if true; on false print line + " assertion failed", exit 1 | `assert(x eq 5)` |

`assert` is a regular function call (not a reserved keyword) —
the checker recognises the name and emits a conditional
`_assert_fail`-on-false path. Programs at most have a single
`spark { }` block; the test runner synthesises its own entry
point that calls each `_test_<i>` function in turn.

## Standard Library

```
use std/basics       // bundle: String + List + Map
use std/string       // String, String.* methods, int.to_string
use std/list         // List of T
use std/map          // Map of K, V (any K, including String)
use std/math         // min, max, abs, pow
use std/queue        // queue.new, push, pop, size, empty
use std/stack        // stack.new, push, pop, peek, size, empty
```

## Banned from user code

These names are compiler-internal; user code calling them errors
with "unknown function":

`heap_alloc`, `heap_free`, `mem_load`, `mem_store`,
`mem_load_byte`, `mem_store_byte`, `write_bytes`, `panic_oob`,
`panic_capacity`, `ptr_of`, `as_string`, `str_len`, `str_eq`,
`str_concat`, `int_to_str`, `char_at`, `yell_str`.

The compiler still emits direct `bl _<name>` to the runtime
symbols when lowering language constructs — they're reachable
from compiled output, just not from source.
