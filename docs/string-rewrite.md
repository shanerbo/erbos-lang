# Rewriting `str` as pure-Potato `String`

The plan documented here replaces the C-emitted string runtime with
a `String` type defined in `std/string.ptt`. After this rewrite,
`int`, `bool`, and `nil` are the only primitive types in the
language; `String` is a struct, just like `List of T` and
`Map of K to V` will be.

## Motivation

`std/*` is the right place for collections, and the same logic
applies to strings: if it can be expressed in Potato, it should be.
Performance gaps that surface get closed via the existing iropt
passes (P5.1–P5.5). The 1.2× target the std/map plan documented
applies here too.

## Surface — what changes for users

| Today | After |
|---|---|
| `s is "hello"` (type str) | `s is "hello"` (type String) |
| `len(s)` | `s.len()` |
| `str_len(s)` | `s.len()` |
| `char_at(s, i)` | `s.char_at(i)` (or `s.byte_at(i)`) |
| `s1 + s2` | unchanged — `+` desugars to `String.concat` |
| `s1 eq s2` | unchanged — `eq` desugars to `String.eq` |
| `"hi {name}"` interpolation | unchanged — desugars to `.concat` chain |
| `yell(s)` | unchanged — `yell` builtin handles String |

Source code outside `std/string.ptt` looks almost identical. The
generic `len()` builtin disappears across the language.

## Storage layout

```
String is {
  cap   int     // bytes allocated at `data` (>= count, may be 0 for borrowed)
  count int     // bytes used (the length)
  data  int     // raw pointer (held in int) to byte storage
  owned int     // 1 if heap-allocated and owns `data`; 0 if borrowed (rodata)
}
```

The `owned` flag is what lets string literals share rodata bytes:
the literal's struct points at the rodata copy with `owned=0`, so
its destructor knows not to call `heap_free`. Mutation operations
(append, in-place concat) check `owned`; if 0, they allocate fresh
bytes, copy the borrowed content over, set `owned=1`, then proceed.

## Literal lowering (P3.4)

Source `"hello"` produces this in the emitted assembly:

```asm
_strlit_42_bytes: .asciz "hello"
.p2align 3
_strlit_42_obj:
  .quad 5                  ; cap
  .quad 5                  ; count
  .quad _strlit_42_bytes   ; data
  .quad 0                  ; owned (borrowed)
```

Every use of `"hello"` in source emits a load of `_strlit_42_obj`'s
address. That address IS a `String` value — methods called on it
load the count/data fields directly.

The codegen needs to know `String`'s field layout (4 fields, 32
bytes, in this order) to emit the `_strlit_*_obj` correctly. We
read the layout from the parsed `String` struct definition. This
means `std/string.ptt` must be parsed before any user code that
contains a string literal — which the existing `use std/string`
mechanism handles, with auto-import as a fallback so users don't
have to write `use` for the string type.

## Operators (P3.5)

The checker today handles `+` and `eq` as TYPE_STR-specific
fast paths. After the rewrite, they become single-type-dispatch
on the struct named `"String"`:

- `s1 + s2` where both have type `String` → `String.concat(s1, s2)`
- `s1 eq s2` where both have type `String` → `String.eq(s1, s2)`

This is NOT general operator overloading. It's a hardcoded rule:
the type named `String` (defined in stdlib) is the canonical string
type and `+` / `eq` on its values dispatch to its methods. Same
shape as `K.eq` for `Map of K to V`.

Interpolation `"hi {name}"` already lowers to a chain of `+`
operations in the parser; once `+` dispatches to `String.concat`,
interpolation is free.

## Runtime trim (P3.3)

Symbols that survive in the C runtime (`src/runtime_emit.c`):

| Symbol | Purpose |
|---|---|
| `_heap_alloc(size)` | mmap-backed bump allocator |
| `_heap_free(ptr, size)` | free-list insert |
| `_yell_int(x)` | int → decimal → write syscall |
| `write_bytes(ptr, len)` | NEW: write raw byte buffer via syscall |
| `_panic_oob`, `_panic_capacity`, `_assert_fail` | abort handlers |
| `_alloc_<Struct>` | per-struct constructor (existing) |

Symbols deleted from the runtime:

| Deleted | Replacement |
|---|---|
| `_str_eq` | `String.eq` user method |
| `_str_concat` | `String.concat` user method |
| `_str_len` | `String.len` user method |
| `_yell_str` | `String.yell` calling `write_bytes` |
| `_yell_dispatch` | obsolete — every `yell(x)` now resolves at compile time |
| `_int_to_str` | `int.to_string` user method (returns String) |
| `_char_at` | `String.char_at` / `String.byte_at` user methods |

The `yell` builtin keeps its identity — it dispatches on argument
type at compile time: `yell(x: int)` → `bl _yell_int`,
`yell(x: String)` → call `String.yell` (which calls `write_bytes`).

## std/string.ptt (P6.0b)

```
String is {
  cap   int
  count int
  data  int
  owned int
}

String.len(self String) int { give self.count }

String.eq(self String, other String) bool {
  self.count ne other.count ?{ give false }
  // byte-by-byte compare via mem_load
  through (i from 0 to self.count by 1) {
    a is mem_load(self.data, i)
    b is mem_load(other.data, i)
    a ne b ?{ give false }
  }
  give true
}

String.concat(self String, other String) String {
  // Allocate count_a + count_b bytes
  total is self.count + other.count
  buf is heap_alloc(total)
  through (i from 0 to self.count by 1) {
    mem_store(buf, i, mem_load(self.data, i))
  }
  through (i from 0 to other.count by 1) {
    mem_store(buf, self.count + i, mem_load(other.data, i))
  }
  out is String()
  out.cap be total
  out.count be total
  out.data be buf
  out.owned be 1
  give out
}

String.byte_at(self String, i int) int {
  i lt 0 or i ge self.count ?{
    panic_oob()      // (or however we expose panic to Potato)
  }
  give mem_load(self.data, i)
}

String.yell(self String) {
  write_bytes(self.data, self.count)
  // Also write a newline?
}

int.to_string(self int) String {
  // base-10 conversion via repeated div/mod, build digits
  // bottom-up, then reverse. Returns String with owned=1.
  // ...
}
```

`mem_load` reads 64 bits but we only want a single byte for
`byte_at`. Either:
- Add `mem_load_byte(p, off) int` that does `ldrb` instead of `ldr` (one more raw primitive in P6.0).
- Read 8 bytes and shift+mask. Slower, no new primitive.

I'd add `mem_load_byte` and `mem_store_byte` — they're trivial extensions of P6.0.

## Migration (P6.0c)

Files that use strings (essentially all examples and tests):

- `len(s)` → `s.len()`
- `str_len(s)` → `s.len()`
- `char_at(s, i)` → `s.byte_at(i)` or `s.char_at(i)` per chosen API
- Everything else (concat, eq, interpolation) is unchanged at the source level

Approximately 30 files affected. Mechanical sweep.

## Performance

Today's `_str_eq` is a tight 8-instruction loop. Pure-Potato
`String.eq` after P5.1 inlining + P5.5 LICM should be within 2×.
The 1.2× target requires further specialization (small-string
optimization, SIMD byte comparison) — out of scope for the initial
rewrite, but the optimization passes are already shipped to make
the gap survivable.

`_str_concat` similarly — pure-Potato concat allocates once and
copies in a loop. Inlining + LICM should make it competitive.

`_int_to_str` is the trickiest because the digit conversion logic
is nontrivial. The existing C version is also nontrivial. A
faithful port should be near-parity.

## Bootstrap order

1. **P3.3** — trim runtime. Build will break (linker errors for
   deleted symbols). Existing tests are temporarily broken.
2. **P3.4** — literal lowering produces String objects. Depends on
   String type being declared somewhere; we hardcode the layout
   in codegen for now (4 quads in the order cap, count, data,
   owned) and validate against the parsed std/string.ptt at build
   time.
3. **P6.0b** — write `std/string.ptt` with all the methods. The
   stdlib file uses `mem_load_byte` / `mem_store_byte` /
   `heap_alloc` / `heap_free` / `write_bytes` — all primitives.
   String literals inside `std/string.ptt` itself work because
   String is fully declared at the top of the file before any
   method body runs.
4. **P3.5** — wire `+` and `eq` on String to user methods.
5. **P6.0c** — migrate every `.ptt` file. Verify test suite.

This order means there will be intermediate commits where the
build works but specific tests are broken (e.g., between P3.3 and
P3.4 nothing using strings will run). That's acceptable; the
rewrite lands as a sequence of commits with the suite green only
after P6.0c.

## Open questions

1. **byte_at vs char_at API.** Does `s.char_at(i)` return a
   1-character String (allocates per call), or does it return a
   byte as int? Bytes-as-int is more honest but loses ergonomics.
   Recommend: ship both. `byte_at` is the fast path; `char_at`
   wraps it into a 1-byte String for ergonomic uses.

2. **Auto-import std/string.** Does every file implicitly `use
   std/string`, or must users opt in? Implicit auto-import is
   friendlier (literals just work) but couples the language to
   the stdlib path. Recommend: implicit, with an escape valve
   for the rare program that wants no stdlib.

3. **`yell` newline.** Today `_yell_str` writes the bytes and
   appends a newline. With `String.yell` calling `write_bytes`
   directly, we can decide: append newline in the method, or have
   the user write `yell(s.append("\n"))`. Recommend: keep newline
   for consistency with current behavior; users who want
   no-newline write to `write_bytes` directly.
