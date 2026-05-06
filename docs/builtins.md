# Potato Built-in Functions 🥔

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

## Lists

| Method | Description | Example |
|--------|-------------|---------|
| `list()` | Create empty dynamic list | `nums is list()` |
| `.push(val)` | Append value | `nums.push(10)` |
| `.pop()` | Remove and return last element | `last is nums.pop()` |
| `.len()` | Get number of elements | `yell(nums.len())` |

List literals also work:
```
nums is [1, 2, 3]
yell(nums[0])           // bounds-checked
```

## Maps

| Method | Description | Example |
|--------|-------------|---------|
| `map()` | Create empty ordered map | `m is map()` |
| `.set(key, val)` | Insert or update entry | `m.set("age", 25)` |
| `.get(key)` | Get value by key (0 if missing) | `m.get("age")` |
| `.len()` | Get number of entries | `m.len()` |
| `.keys()` | Get list of keys (insertion order) | `m.keys()` |

## Structs

| Syntax | Description | Example |
|--------|-------------|---------|
| `StructName()` | Heap-allocate a struct instance | `Point()` |

Automatically generated for every struct definition:
```
Point is {
  x int
  y int
}

p is Point()
p.x be 10
```

## Memory

All allocations are heap-backed via `mmap` syscall. No libc dependency.

RAII is implemented — heap allocations are automatically freed when their scope ends. Move semantics (`is now`) transfer ownership. Clone (`is rep`) creates deep copies.
