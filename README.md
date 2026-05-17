# Potato 🥔

A systems programming language that reads like English, compiles to native code, and doesn't need a runtime.

**No garbage collector. No libc. No exceptions. Just raw performance with words you can actually read.**

Files use the `.ptt` extension — short for potato. 🥔

```
spark {
  name is "world"
  yell("hello {name}")

  through (i from 1 to 10 by 1) {
    i mod 3 eq 0 ?{
      yell("fizz")
    } nah {
      yell(i)
    }
  }
}
```

---

## Why Potato?

- **Reads like English** — `is`, `be`, `give`, `nah`, `through`, `infi`, `stop`, `skip`
- **Compiles to native ARM64** — no VM, no interpreter, no runtime
- **Zero dependencies** — no libc, just syscalls
- **Memory safe** — use-after-move detection, bounds checking
- **Type checked** — catches mismatches at compile time
- **Fast compilation** — instant builds, single-pass compiler

---

## Quick Start

```bash
# Build the compiler
make

# Compile and run in one shot 🥔
./erbos run hello.ptt

# Or compile to binary
./erbos hello.ptt
./hello

# Optimization levels — the IR pipeline accepts -O0 / -O1 / -O2 in any
# position relative to the subcommand. Default is -O1.
./erbos -O0 run hello.ptt   # skip iropt entirely
./erbos -O1 run hello.ptt   # default, every iropt pass runs
./erbos -O2 run hello.ptt   # reserved for tuning (currently same as -O1)

# Inspect generated assembly without assembling/linking:
./erbos ir hello.ptt        # writes hello.s
```

---

## Language Overview

### Variables
```
x is 10              // declare (type inferred)
x is int 10          // explicit type
nomut pi is 3        // immutable
x be 42              // reassign
```

### Functions
```
add(a int, b int) int {
  give a + b
}

greet(name String) {
  yell("hello {name}")
}
```

### Conditionals
```
x gt 10 ?{
  yell("big")
} x gt 5 ?{
  yell("mid")
} nah {
  yell("smol")
}
```

### Loops
```
through (i from 0 to 10 by 1) { }   // range
through (item in nums) { }           // collection
infi (x gt 0) { x be x - 1 }        // while
infi { stop }                        // infinite + break
```

### Structs
```
Point is {
  x int
  y int
}

p is Point()                       // zero-default: fields start at 0
p.x be 10
p.y be 20

q is Point(x is 10, y is 20)       // named-arg: atomic init, every field required
nomut origin is Point(x is 0, y is 0)
```

Two construction forms:

- `Point()` — zero-default. Cheap; mutate fields afterwards.
- `Point(field is value, ...)` — named-arg. Atomic. Every declared
  field must appear exactly once, order is free, types are checked.

Positional constructors (`Point(1, 2)`) are intentionally rejected —
field reorder would silently swap call-site semantics. Use named-arg
or define a factory method.

### Methods
```
Counter is { value int }

Counter.bump(self ref Counter) {
  self.value be self.value + 1
}

Counter.get(self Counter) int {
  give self.value
}

spark {
  c is Counter()
  c.bump()
  c.bump()
  yell(c.get())   // 2
}
```

### Generics
```
Box of T is { value T }

Box.set(self ref Box of T, v T) {
  self.value be v
}

Box.get(self Box of T) T {
  give self.value
}

spark {
  bi is Box of int
  bi.set(42)
  bs is Box of String
  bs.set("hello")
  yell(bi.get())   // 42
  yell(bs.get())   // hello
}
```

Word-style generics: `of` introduces one parameter, `of … to …`
introduces a key→value pair. There is no `<T>` syntax anywhere
in type position. Generic structs and methods are monomorphized
at compile time — each concrete instantiation gets its own
emitted code with a mangled symbol (`Box of int` → `_Box__int`,
`Map of String to int` → `_Map__String__int`). No v-tables, no
runtime cost. See [`docs/generics-syntax.md`](docs/generics-syntax.md)
for the full rules.

### Collections
```
use std/basics                // bundle: String + List + Map

xs is List of int
xs.push(10)
yell(xs.pop())

m is Map of String to int     // String-keyed
m.set("key", 42)
yell(m.get("key"))

mi is Map of int to int       // int-keyed
mi.set(42, 100)
yell(mi.get(42))
```

The `[1,2,3]` literal lowers to `List of int` when `use std/list`
is in scope. The `["k" to v]` literal lowers to
`Map of String to int` when `use std/map` is in scope.

### Ownership & Safety
```
a is Point()
b is now a          // move — a is dead
// yell(a.x)       // COMPILE ERROR: use of moved variable

c is rep b          // clone — shallow copy (pointer copy)

{
  temp is Point()
}                   // temp freed here (RAII)
```

> **Note:** RAII is real — heap allocations are freed when their scope ends.
> `rep` performs a shallow copy (pointer copy), not a deep clone.
> `ref` is enforced: non-ref struct params cannot be mutated. Caller must pass `ref` explicitly.

---

## Keywords

| Keyword | Purpose |
|---------|---------|
| `spark` | program entry point |
| `is` | variable declaration |
| `be` | reassignment |
| `nomut` | immutable variable |
| `give` | return value |
| `through` | range/collection loop |
| `from` / `to` / `by` | loop range |
| `in` | collection iteration |
| `infi` | while / infinite loop |
| `stop` | break |
| `skip` | continue |
| `nah` | else |
| `now` | move ownership |
| `rep` | clone |
| `ref` | mutable borrow |
| `and` / `or` / `not` | logical operators |
| `eq` / `ne` / `gt` / `lt` / `ge` / `le` | comparisons |
| `mod` | modulo |
| `true` / `false` | booleans |
| `nil` | null pointer |
| `array` | typed-storage primitive (`array of T`, `array of byte`) |
| `of` / `to` | word-style generic connectives (`Box of T`, `Map of K to V`) |
| `match` | pattern match on enum |
| `use` / `as` | import module / alias |
| `test` | built-in test block (`assert` is now a stdlib function, not a keyword) |
| `task` | concurrency handle (runtime not yet integrated) |

---

## Operators

| Symbol | Word | Meaning |
|--------|------|---------|
| `+` | — | add |
| `-` | — | subtract |
| `*` | — | multiply |
| `/` | — | divide |
| `%` | `mod` | modulo |
| `>` | `gt` | greater than |
| `<` | `lt` | less than |
| `>=` | `ge` | greater or equal |
| `<=` | `le` | less or equal |
| `==` | `eq` | equal |
| `!=` | `ne` | not equal |

Both symbol and word forms work for comparisons and modulo. Use whichever you prefer.

---

## Current Features

### Implemented
| Feature | Status |
|---------|--------|
| Integer arithmetic (+, -, *, /, mod) | ✅ |
| Strings + interpolation (`"hello {name}"`) | ✅ |
| String concat with `+` operator | ✅ |
| Functions + recursion | ✅ |
| Function arg count validation | ✅ |
| Unknown function/type detection | ✅ |
| Structs (heap-allocated) | ✅ |
| Dynamic lists (push/pop/len, growable) | ✅ |
| Ordered maps — string keys (set/get/keys/len) | ✅ |
| Ordered maps — int keys (`imap`) | ✅ |
| Conditionals (?{ / nah) | ✅ |
| Loops (through range, through in, infi) | ✅ |
| Move semantics (`is now`) | ✅ |
| Use-after-move detection (heap vars) | ✅ |
| Type inference + type mismatch errors | ✅ |
| Bounds checking (panic on OOB index) | ✅ |
| nomut enforcement | ✅ |
| Negative numbers | ✅ |
| `nil` for null pointers | ✅ |
| Method syntax (obj.method()) | ✅ |
| User-defined methods on structs and enums (`Type.name(self ...)`) | ✅ |
| Per-struct field resolution (typed receivers; ambiguity is a compile error) | ✅ |
| Generics + monomorphization (`Box of T`, `Map of K to V`, …) | ✅ |
| Scoped blocks ({} for lifetimes) | ✅ |
| RAII (heap freed at scope end) | ✅ |
| Enums + `match` pattern matching | ✅ |
| Multi-file imports (`use std/math`) | ✅ |
| Built-in test framework (`test`/`assert`) | ✅ |
| Optimizer pass (constant folding, DCE, inlining) | ✅ |
| `erbos run` (compile + execute + cleanup) | ✅ |

### Partial / Experimental
| Feature | Status |
|---------|--------|
| Clone (`is rep`) | Shallow copy (pointer copy). Deep clone not implemented. |
| `ref` enforcement | Non-ref struct params cannot be mutated (compile error). |
| Green thread runtime | Separate C library in `compiler/runtime/`. Not integrated into compiled `.ptt` output. |
| Channels | Separate C library in `compiler/runtime/`. Not integrated into compiled output. |
| Pure-Potato stdlib (`std/string`, `std/list`, `std/map`) | The only collection / String surface. Backed by `array of T`. List / map literals lower through these types automatically. |
| `array of T` / `array of byte` | Typed-storage primitives — what the stdlib types are built on. |

### Planned (v0.2+)
| Feature | Status |
|---------|--------|
| Deep clone for `rep` | Requires type-aware copy |
| Result/Option as built-in types | — |
| Traits / interfaces | — |
| Operator overloading | — |
| Self-hosting | — |

---

## Comparison

| | Potato | Rust | C++ | Go | Python |
|--|-------|------|-----|-----|--------|
| **Syntax** | English words | Symbol-heavy | Verbose | Clean | Clean |
| **Compilation** | Instant | Slow | Slow | Fast | Interpreted |
| **Memory** | Scope tracking + move | Borrow checker | Manual/RAII | GC | GC |
| **Safety** | Move detection, bounds/capacity panics | Full borrow checker | Opt-in | Nil panics | Runtime errors |
| **Dependencies** | Zero (no libc) | Minimal | Heavy (STL) | Runtime | Interpreter |
| **Learning curve** | Low | High | High | Low | Low |
| **Performance** | Native ARM64 | Native | Native | Native + GC | Slow |
| **Concurrency** | Green threads | async/await | threads | Goroutines | GIL |
| **Error handling** | Planned (Result types) | Result<T,E> | Exceptions | error return | Exceptions |
| **Generics** | Monomorphized | Monomorphized | Templates | Type params | Duck typing |

### Potato vs Rust
**Wins:** Simpler syntax, no lifetime annotations, faster to learn, instant compilation.
**Loses:** No borrow checker, no ecosystem.

### Potato vs C++
**Wins:** No headers, no UB, no preprocessor, readable syntax, compile-time move checking, monomorphized generics without template metaprogramming.
**Loses:** No operator overloading, less control.

### Potato vs Go
**Wins:** No GC, move semantics prevent some leaks, word-based syntax.
**Loses:** No goroutine integration in compiled output yet (the green-thread runtime in `compiler/runtime/` is not wired into compiled binaries).

### Potato vs Python
**Wins:** 100x+ faster, compiled, type safe at compile time, no runtime needed.
**Loses:** No libraries, no REPL, less forgiving, early stage.

---

## Roadmap

- [x] Enums with data (algebraic types)
- [x] Pattern matching (`match`)
- [x] Multi-file imports (`use`)
- [x] Built-in test runner (`erbos test`)
- [x] User-defined methods on structs and enums
- [x] Per-struct type-aware field resolution
- [x] Generics + monomorphization
- [x] Cross-block / call-aware register allocation in the IR backend
- [x] Switch IR to default backend; retire the direct codegen
- [x] iropt scaffold + `-O0`/`-O1`/`-O2` CLI flags
- [x] Optimization passes: inlining, SRA, escape analysis, BCE, LICM
- [x] Pure-Potato `std/list`, `std/map`, `std/string_map`, `std/string` (against `array of T`)
- [x] `spark` reserved keyword; `assert` demoted to stdlib function
- [x] Compile-time `yell` overload (resolves on static argument type)
- [x] Drop legacy `list` / `map` / `imap` keyword forms (phase ε)
- [x] Delete dead C runtime collection helpers (phase ζ1)
- [ ] Deep clone for `rep`
- [ ] Result/Option as built-in types
- [ ] Traits / interfaces
- [ ] Operator overloading
- [ ] Compile-time evaluation
- [ ] Built-in `fmt`
- [ ] Green-thread runtime integration in compiled output
- [ ] Self-hosting (compiler written in Potato)

---

## Testing

### Built-in test framework
```
test "my feature" {
  assert(1 + 1 eq 2)
  assert(add(3, 4) eq 7)
}
```

Run: `erbos test file.ptt`

### Compiler test suite
`make test` runs: passing examples, leetcode library compile checks, expected compile-error tests, expected runtime panics, C-runtime tests, framework tests across the whole `tests/` tree, and the IR backend regression matrix at `-O0` / `-O1` / `-O2`.

---

## Imports

```
use std/math

spark {
  yell(math.max(10, 20))
}
```

Standard library:
- `std/basics` — bundle (`String` + `List` + `Map`)
- `std/string`, `std/list`, `std/map`
- `std/math`, `std/queue`, `std/stack`

---

## Architecture

```
source.ptt 🥔 → [Lexer] → [Parser] → [Monomorph] → [Checker] → [Optimizer]
              → [IRGen] → [IROpt] → [RegAlloc] → [IREmit] → ARM64 .s → [as + ld] → binary
```

Written in C11. The compiler frontend lives in `compiler/`; the
green-thread runtime + channels live in `compiler/runtime/`. No
external dependencies.

The **IROpt** stage (`compiler/iropt.c`) runs the five optimization
passes — inlining, scalar replacement of aggregates, escape
analysis (stackify), bounds-check elimination, and loop-invariant
code motion — when called at `-O1` or `-O2`. `-O0` skips them all.

The **Monomorph** pass (`compiler/monomorph.c`) instantiates every
concrete generic form (`Box of int`, `Map of String to int`, …)
before type checking, so the rest of the pipeline only sees fully-
specialised types. Names are mangled inside-out:
`List of Map of String to int` becomes the symbol
`_List__Map__String__int`.

The **IR backend** is the only backend. It uses cross-block,
call-aware register allocation: vregs whose live range crosses a
`bl` are placed in callee-save registers (x19..x28) with proper
prologue saves; shorter-lived values use x8/x11..x18 (x9 and x10
are reserved as iremit scratch for large-offset frame addressing).
See [`docs/ir-pipeline.md`](docs/ir-pipeline.md).

`erbos ir <file.ptt>` emits the .s only, useful for inspecting
generated assembly. The runtime helpers in
`compiler/runtime_emit.c` are down to the irreducible kernel-
boundary set: `_heap_alloc` / `_heap_free`, `_yell_int` /
`_String_yell`, `_write_bytes`, `_panic_oob` / `_panic_capacity` /
`_assert_fail`, per-struct `_alloc_<X>` constructors, plus a small
residual (`_str_eq` / `_str_concat` / `_int_to_str` / `_yell`
shim) used by the operator + interpolation paths. The pure-Potato
stdlib (`std/list`, `std/map`, `std/string`) provides every
collection and String operation through monomorphized user
methods. See [`docs/runtime.md`](docs/runtime.md) for the full
breakdown.

---

## Contributing

The language is in early development. If you want to help shape it, open an issue or PR.

---

## License

MIT
