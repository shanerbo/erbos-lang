# Potato Built-in Functions 🥔

## Output

| Function | Description | Example |
|----------|-------------|---------|
| `yell(value)` | Print int or string with newline | `yell(42)`, `yell("hi")` |

## Universal

| Function | Description | Example |
|----------|-------------|---------|
| `len(value)` | Length of list, map, or string | `len(nums)`, `len("hi")` |

## Strings

| Function | Description | Example |
|----------|-------------|---------|
| `str_len(s)` | String length | `str_len("hello")` → 5 |
| `char_at(s, i)` | Character at index | `char_at("abc", 1)` → "b" |
| `str_concat(a, b)` | Concatenate strings | `str_concat("hi", " there")` |
| `int_to_str(n)` | Convert int to string | `int_to_str(42)` → "42" |
| `+` operator | String concat (when both str) | `"hi" + " there"` |

String interpolation: `"hello {name}"` works for both int and str variables.

## Lists

| Function/Method | Description | Example |
|----------|-------------|---------|
| `list of T` | Create typed list | `nums is list of int` |
| `.push(val)` | Append element | `nums.push(10)` |
| `.pop()` | Remove and return last | `nums.pop()` |
| `list_set(list, i, val)` | Set element at index | `list_set(nums, 0, 99)` |
| `[a, b, c]` | List literal | `nums is [1, 2, 3]` |

Lists are growable — no capacity limit.

## Maps (string keys)

| Function/Method | Description | Example |
|----------|-------------|---------|
| `map of K to V` | Create typed map | `m is map of str to int` |
| `.set(key, val)` | Insert/update entry | `m.set("x", 10)` |
| `.get(key)` | Get value (0 if missing) | `m.get("x")` |
| `.keys()` | Get list of keys | `m.keys()` |
| `["k" to v, ...]` | Map literal | `["a" to 1, "b" to 2]` |

Maps are growable — no capacity limit. Ordered by insertion.

## Maps (int keys)

| Function | Description | Example |
|----------|-------------|---------|
| `imap of K to V` | Create int-key map | `m is imap of int to int` |
| `imap_set(m, key, val)` | Set entry | `imap_set(m, 42, 100)` |
| `imap_get(m, key)` | Get value (0 if missing) | `imap_get(m, 42)` |
| `imap_len(m)` | Entry count | `imap_len(m)` |

## Structs

| Syntax | Description | Example |
|--------|-------------|---------|
| `StructName()` | Heap-allocate struct | `Point()` |

## Testing

| Function | Description | Example |
|----------|-------------|---------|
| `assert(cond)` | Pass if true, fail with line | `assert(x eq 5)` |

## Standard Library

Import with `use`:

```
use std/math    // min, max, abs, pow
use std/queue   // new, push, pop, size, empty
use std/stack   // new, push, pop, peek, size, empty
```
