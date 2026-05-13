# Potato Built-in Functions 🥔

## Output

| Function | Description | Example |
|----------|-------------|---------|
| `yell(value)` | Print int or string to stdout with newline | `yell(42)`, `yell("hi")` |

## Universal

| Function | Description | Example |
|----------|-------------|---------|
| `len(value)` | Get length of list, map, or string | `len(nums)`, `len(m)`, `len("hi")` |

## Strings

| Function | Description | Example |
|----------|-------------|---------|
| `str_concat(a, b)` | Concatenate two strings | `str_concat("hello", " world")` |
| `str_len(s)` | Get string length | `str_len("hello")` → 5 |
| `char_at(s, i)` | Get character at index | `char_at("hello", 0)` → "h" |
| `int_to_str(n)` | Convert int to string | `int_to_str(42)` → "42" |

String interpolation is built into the language:
```
name is "erbos"
yell("hello {name}")    // prints: hello erbos
```

## Lists

| Method | Description | Example |
|--------|-------------|---------|
| `list of T` | Create typed list | `nums is list of int` |
| `.push(val)` | Append value | `nums.push(10)` |
| `.pop()` | Remove and return last element | `last is nums.pop()` |
| `.len()` | Get number of elements | `yell(nums.len())` |
| `list_set(list, i, val)` | Set element at index | `list_set(nums, 0, 99)` |

List literals also work:
```
nums is [1, 2, 3]
yell(nums[0])           // bounds-checked
```

## Maps

| Method | Description | Example |
|--------|-------------|---------|
| `map of K to V` | Create typed map | `m is map of str to int` |
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

RAII is implemented — heap allocations are automatically freed when their scope ends. Move semantics (`is now`) transfer ownership. Clone (`is rep`) creates shallow copies (pointer copy). Deep clone is not yet implemented.

## Int-Key Maps (imap)

| Function | Description | Example |
|----------|-------------|---------|
| `imap of K to V` | Create typed int-key map | `m is imap of int to int` |
| `imap_set(m, key, val)` | Set entry (int key) | `imap_set(m, 42, 100)` |
| `imap_get(m, key)` | Get value (0 if missing) | `imap_get(m, 42)` |
| `imap_len(m)` | Get entry count | `imap_len(m)` |
