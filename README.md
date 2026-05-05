# Erbos

A systems programming language that reads like English, compiles to native code, and doesn't need a runtime.

**No garbage collector. No libc. No exceptions. Just raw performance with words you can actually read.**

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

## Why Erbos?

- **Reads like English** — `is`, `be`, `give`, `nah`, `through`, `infi`, `stop`, `skip`
- **Compiles to native ARM64** — no VM, no interpreter, no runtime
- **Zero dependencies** — no libc, just syscalls
- **Memory safe** — RAII, ownership, move semantics, use-after-move detection
- **Type checked** — catches mismatches at compile time
- **Fast compilation** — instant builds, single-pass compiler

---

## Quick Start

```bash
# Build the compiler
make

# Compile and run in one shot
./erbos run hello.erbos

# Or compile to binary
./erbos hello.erbos
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

### Collections
```
nums is [1, 2, 3]         // list literal
nums is list()             // dynamic list
nums.push(4)
yell(nums.pop())

m is map()                 // ordered map
m.set("key", 42)
yell(m.get("key"))
```

### Ownership & Safety
```
a is Point()
b is now a          // move — a is dead
// yell(a.x)       // COMPILE ERROR: use of moved variable

c is rep b          // clone — both live

{
  temp is Point()
}                   // temp auto-freed here (RAII)
```

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

| Feature | Status |
|---------|--------|
| Integer arithmetic | ✅ |
| Strings + interpolation | ✅ |
| Functions + recursion | ✅ |
| Structs (heap-allocated) | ✅ |
| Dynamic lists (push/pop/len) | ✅ |
| Ordered maps (set/get/keys/len) | ✅ |
| Conditionals (?{ / nah) | ✅ |
| Loops (through, infi) | ✅ |
| RAII (auto-free at scope end) | ✅ |
| Move semantics (is now) | ✅ |
| Clone (is rep) | ✅ |
| Use-after-move detection | ✅ |
| Type inference + checking | ✅ |
| Bounds checking (panic on OOB) | ✅ |
| nomut enforcement | ✅ |
| String interpolation | ✅ |
| Negative numbers | ✅ |
| Method syntax (obj.method()) | ✅ |
| Scoped blocks ({} for lifetimes) | ✅ |
| Green thread runtime | ✅ (separate) |
| Channels | ✅ (separate) |
| Multi-core work-stealing | ✅ (separate) |

---

## Comparison

| | Erbos | Rust | C++ | Go | Python |
|--|-------|------|-----|-----|--------|
| **Syntax** | English words | Symbol-heavy | Verbose | Clean | Clean |
| **Compilation** | Instant | Slow | Slow | Fast | Interpreted |
| **Memory** | RAII + ownership | Borrow checker | Manual/RAII | GC | GC |
| **Safety** | Move semantics, bounds checks | Full borrow checker | Opt-in | Nil panics | Runtime errors |
| **Dependencies** | Zero (no libc) | Minimal | Heavy (STL) | Runtime | Interpreter |
| **Learning curve** | Low | High | High | Low | Low |
| **Performance** | Native ARM64 | Native | Native | Native + GC | Slow |
| **Concurrency** | Green threads | async/await | threads | Goroutines | GIL |
| **Error handling** | Planned (Result types) | Result<T,E> | Exceptions | error return | Exceptions |
| **Generics** | Planned | Monomorphized | Templates | Type params | Duck typing |

### Erbos vs Rust
**Wins:** Simpler syntax, no lifetime annotations, faster to learn, instant compilation.
**Loses:** Less mature, no borrow checker (yet), smaller ecosystem.

### Erbos vs C++
**Wins:** No headers, no UB, no preprocessor, readable syntax, memory safe by default.
**Loses:** No templates (yet), no operator overloading (yet), less control over memory layout.

### Erbos vs Go
**Wins:** No GC pauses, ownership model prevents leaks, word-based syntax.
**Loses:** No goroutine integration in compiled output yet, single-file only.

### Erbos vs Python
**Wins:** 100x+ faster, compiled, type safe, no runtime needed.
**Loses:** Less libraries, no REPL (yet), less forgiving.

---

## Roadmap

- [ ] Enums with data (algebraic types)
- [ ] Pattern matching
- [ ] Result/Option types
- [ ] Traits / interfaces
- [ ] Generics (monomorphization)
- [ ] Multi-file imports
- [ ] Operator overloading
- [ ] Compile-time evaluation
- [ ] Built-in tooling (fmt, test, build)
- [ ] Self-hosting (compiler written in Erbos)

---

## Architecture

```
source.erbos → [Lexer] → [Parser] → [Type Checker] → [Codegen] → ARM64 .s → [as + ld] → binary
```

Written in C. ~2500 lines. No dependencies.

---

## Contributing

The language is in early development. If you want to help shape it, open an issue or PR.

---

## License

MIT
