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

greet(name str) {
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
through (item in list) { }           // collection
infi (x gt 0) { x be x - 1 }        // while
infi { stop }                        // infinite + break
```

### Structs
```
Point is {
  x int
  y int
}

p is Point()
p.x be 10
```

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
Box<T> is { value T }

Box<T>.set(self ref Box<T>, v T) {
  self.value be v
}

Box<T>.get(self Box<T>) T {
  give self.value
}

spark {
  bi is Box<int>()
  bi.set(42)
  bs is Box<str>()
  bs.set("hello")
  yell(bi.get())   // 42
  yell(bs.get())   // hello
}
```

Generic structs and methods (`Map<K, V>`, `Pair<K, V>`, etc.) are
monomorphized at compile time — each concrete instantiation gets its
own emitted code with a mangled symbol (`Box<int>` → `_Box__int`,
`Pair<str, int>` → `_Pair__str__int`). No v-tables, no runtime cost.

### Collections
```
nums is [1, 2, 3]         // list literal
m is ["name" to "alice", "score" to 100]
nums is list of int          // typed dynamic list
nums.push(4)
yell(nums.pop())

m is map of str to int       // typed ordered map
m.set("key", 42)
yell(m.get("key"))
```

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
| `list` / `map` / `imap` | collection types (`list of int`, `map of str to int`, `imap of int to int`) |
| `match` | pattern match on enum |
| `use` / `as` | import module / alias |
| `test` / `assert` | built-in test framework |
| `task` | concurrency handle (runtime not yet integrated) |
| `<T>` / `<K, V>` | generic type parameters on struct or method |

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
| Generics + monomorphization (`Box<T>`, `Map<K, V>`, …) | ✅ |
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
| Green thread runtime | Separate C library (`src/runtime.c`). Not integrated into compiled `.ptt` output. |
| Channels | Separate C library (`src/channel.c`). Not integrated into compiled output. |
| String comparison (eq/ne) | Uses `_str_eq` when checker detects both operands are str. |
| Experimental SSA IR backend (`erbos ir <file>`) | Stack-frame layout fixed, struct field access works across calls. Lists / maps / enums on this path still need targeted tests; cross-block regalloc is the next piece (see `docs/ir-pipeline.md` and `docs/native-stdlib-plan.md`). |

### Planned (v0.2+)
| Feature | Status |
|---------|--------|
| Deep clone for `rep` | Requires type-aware copy |
| Result/Option as built-in types | — |
| Traits / interfaces | — |
| Operator overloading | — |
| Optimization passes (inlining, SRA, escape analysis, BCE, LICM) | P5 |
| Pure-Potato `std/map` (replacing the C-emitted `_map_*` builtins) | P6 |
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
**Loses:** No goroutine integration in compiled output (the green-thread runtime in `src/runtime.c` is not yet wired into compiled binaries).

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
- [ ] Deep clone for `rep`
- [ ] Result/Option as built-in types
- [ ] Traits / interfaces
- [ ] Operator overloading
- [ ] Compile-time evaluation
- [ ] Built-in `fmt`
- [ ] Optimization passes (inlining, SRA, escape analysis, BCE, LICM)
- [ ] Pure-Potato `std/map`, `std/list` (replacing C-emitted builtins)
- [ ] Green-thread runtime integration in compiled output
- [ ] Self-hosting (compiler written in Potato)

The full multi-phase plan is in [`docs/native-stdlib-plan.md`](docs/native-stdlib-plan.md).

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
`make test` runs: passing examples, compile error tests, runtime panic tests, C runtime tests, output-validated leetcode tests, and framework tests.

---

## Imports

```
use std/math

spark {
  yell(math.max(10, 20))
}
```

Standard library: `std/math`, `std/queue`, `std/stack`

---

## Architecture

```
source.ptt 🥔 → [Lexer] → [Parser] → [Monomorph] → [Checker] → [Optimizer]
              → [IRGen] → [RegAlloc] → [IREmit] → ARM64 .s → [as + ld] → binary
```

Written in C11. ~7,300 lines across `src/`. No external dependencies.

The **Monomorph** pass (`src/monomorph.c`) instantiates every concrete generic form (`Box<int>`, `Map<str, int>`, …) before type checking, so the rest of the pipeline only ever sees fully-specialised types. Names are mangled inside-out: `List<Pair<str, int>>` becomes the symbol `_List__Pair__str__int`.

The **IR backend** is the only backend. The legacy AST-walking direct codegen was retired after the IR pipeline reached behavioural parity on every program in the test corpus (see `docs/ir-pipeline.md`). The IR uses cross-block, call-aware register allocation: vregs whose live range crosses a `bl` are placed in callee-save registers (x19..x28) with proper prologue saves, while shorter-lived values use the temporary range (x8, x11..x18; x9 and x10 are reserved as scratch for frame addressing).

`erbos ir <file.ptt>` emits the .s only, useful for inspecting generated assembly. Runtime helpers (yell, heap allocator, str/list/map/imap operations, panic and assert handlers) are emitted from `src/runtime_emit.c` as raw ARM64 assembly; they will progressively be replaced by pure-Potato implementations as the optimization passes (`docs/native-stdlib-plan.md`) make user-Potato performance competitive.

---

## Contributing

The language is in early development. If you want to help shape it, open an issue or PR.

---

## License

MIT
