# Erbos Built-in Functions

## Output

| Function | Description | Example |
|----------|-------------|---------|
| `yell(value)` | Print int or string to stdout with newline | `yell(42)`, `yell("hi")` |

## Strings

| Function | Description | Example |
|----------|-------------|---------|
| `str_concat(a, b)` | Concatenate two strings | `str_concat("hello", " world")` |

String interpolation is built into the language:
```
name is "erbos"
yell("hello {name}")    // prints: hello erbos
```

## Maps

| Function | Description | Example |
|----------|-------------|---------|
| `map_new()` | Create empty ordered map | `m is map_new()` |
| `map_set(m, key, val)` | Insert or update entry | `map_set(m, "age", 25)` |
| `map_get(m, key)` | Get value by key (0 if missing) | `map_get(m, "age")` |
| `map_len(m)` | Get number of entries | `map_len(m)` |
| `map_keys(m)` | Get list of keys (insertion order) | `map_keys(m)` |

## Structs

| Function | Description | Example |
|----------|-------------|---------|
| `alloc_StructName()` | Heap-allocate a struct instance | `alloc_Point()` |

Automatically generated for every struct definition.

## Memory

All allocations are heap-backed via `mmap`. No libc dependency.
No garbage collector — memory is freed when the process exits (RAII planned).
