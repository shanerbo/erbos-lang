# Potato Built-in Functions 🥔

> **Status:** post-overhaul. Free-function string builtins
> (`str_len`, `str_eq`, `str_concat`, `int_to_str`, `char_at`),
> kernel-layer names (`heap_alloc`, `mem_load`, etc.), and the
> legacy collection keyword forms (`list of T`, `map of K to V`,
> `imap of int to V`) are all gone. User code uses the pure-Potato
> stdlib types (`List of T`, `Map of K to V`, `StringMap of V`)
> from std/list / std/map / std/string_map. Programs that want
> the comfortable defaults import `std/basics` for the bundle.

## Output

| Function | Description | Example |
|----------|-------------|---------|
| `yell(value)` | Print a value with newline. Resolves at compile time on the static type — int / bool / byte → `_yell_int`, String → `_String_yell`, struct T → `_<T>_yell` (user-defined). | `yell(42)`, `yell("hi")`, `yell(c)` (where `Counter.yell(self Counter)` is defined) |

Users overload `yell` on their own types by defining
`Type.yell(self Type) { ... }`; subsequent `yell(value : Type)`
calls resolve to it at compile time.

## Universal

| Function | Description | Example |
|----------|-------------|---------|
| `len(value)` | Length of a legacy `list`, `map`, or `imap`. | `len(nums)` |

`len()` on strings is gone — use `s.len()` (which dispatches to
`String.len`). The free-function `len` will be retired in phase ε
once the legacy collection keyword forms go.

## Strings

`String` is a stdlib struct (in `std/string.ptt`); programs that
manipulate strings explicitly write `use std/string` (or the
bundle `use std/basics`).

| Method / Operator | Description | Example |
|---|---|---|
| `s.len()` | byte length | `"hello".len()` → 5 |
| `s.empty()` | true if zero-length | `s.empty()` |
| `s.equals(t)` | byte-equality compare | `s.equals(t)` |
| `s.byte_at(i)` | byte at index, as int | `"abc".byte_at(0)` → 97 |
| `s.char_at(i)` | 1-byte String at index | `"abc".char_at(0)` eq `"a"` |
| `s + t` | concatenation, returns a fresh String | `"hi" + " there"` |
| `s eq t`, `s ne t` | structural equality / inequality | `s eq "hello"` |
| `n.to_string()` | int → decimal String | `42.to_string()` |
| `s.yell()`, `yell(s)` | print + newline | both emit `bl _String_yell` |

String literals (`"..."`) lower to a 32-byte `String` header
backed by an `array of byte`; the rodata bytes live behind a
two-tier indirection (String → array of byte → bytes). The
compiler knows the layout; user code only sees `String`.

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

## Maps (`std/string_map`, `std/map`)

`StringMap of V` is the string-keyed map; `Map of K to V` is for
int / pointer-shaped keys (uses `==` directly). Map literals
`["k" to v]` lower to `StringMap of int` automatically when
`use std/string_map` is in scope.

| Method / Form | Description |
|---|---|
| `m is StringMap of V` | construct an empty string-keyed map |
| `m is Map of K to V` | construct an empty int / pointer-keyed map |
| `m.set(key, val)` | insert / update |
| `m.get(key)` | get value (0 if absent) |
| `m.keys()` | List of keys (StringMap only) |
| `m.len()` | entry count |
| `["k" to v, ...]` | string-keyed map literal |

## Structs

| Syntax | Description | Example |
|--------|-------------|---------|
| `StructName()` | heap-allocate struct | `Point()` |
| `Type.method(self [ref] Type, ...)` | define a method on a struct or enum | `Counter.bump(self ref Counter) { ... }` |
| `StructName of T()` | constructor for a generic instantiation | `Box of int ()`, `Map of str to int ()` |
| `Type of T.method(self [ref] Type of T, ...)` | method on a generic type | `Box of T.set(self ref Box of T, v T) { ... }` |

## Tasks (concurrency placeholder)

| Syntax | Description | Example |
|--------|-------------|---------|
| `t is task()` | create a task handle | `t is task()` |
| `t.fire(fn(...))` | schedule a call (synchronous in compiled output today) | `t.fire(worker())` |

> The green-thread runtime in `src/runtime.c` runs in
> `make test-runtime` only; it is not yet wired into compiled
> binaries.

## Testing

| Function | Description | Example |
|----------|-------------|---------|
| `assert(cond)` | pass if true; on false print line + " assertion failed", exit 1 | `assert(x eq 5)` |

`assert` is no longer a reserved keyword — it parses as a regular
call but the checker recognises the name and emits the same
`_assert_fail`-on-false path. Conceptually a `std/test` function.

## Standard Library

```
use std/basics       // bundle: String + List + Map + StringMap
use std/string       // String, String.* methods, int.to_string
use std/list         // List of T
use std/map          // Map of K to V (int / pointer-shaped K)
use std/string_map   // StringMap of V (String keys with .equals)
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
