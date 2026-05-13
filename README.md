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
| Ordered maps (set/get/keys/len, growable) | ✅ |
| Conditionals (?{ / nah) | ✅ |
| Loops (through range, through in, infi) | ✅ |
| Move semantics (`is now`) | ✅ |
| Use-after-move detection (heap vars) | ✅ |
| Type inference + type mismatch errors | ✅ |
| Bounds checking (panic on OOB index) | ✅ |
| nomut enforcement | ✅ |
| Negative numbers | ✅ |
| Method syntax (obj.method()) | ✅ |
| Scoped blocks ({} for lifetimes) | ✅ |
| RAII (heap freed at scope end) | ✅ |
| `erbos run` (compile + execute + cleanup) | ✅ |

### Partial / Experimental
| Feature | Status |
|---------|--------|
| Clone (`is rep`) | Shallow copy (pointer copy). Deep clone not implemented. |
| `ref` enforcement | Non-ref struct params cannot be mutated (compile error). |
| Struct field access | Resolves field by name across all structs, not per-type. Works if field names are unique. |
| Green thread runtime | Separate C library (`src/runtime.c`). Not integrated into compiled `.ptt` output. |
| Channels | Separate C library (`src/channel.c`). Not integrated into compiled output. |
| String comparison (eq/ne) | Uses `_str_eq` when checker detects both operands are str. |

### Planned (v0.2+)
| Feature | Status |
|---------|--------|
| Deep clone for `rep` | Requires type-aware copy |
| Per-struct field resolution | Requires type tracking through variables |
| Enums with data (algebraic types) | — |
| Pattern matching | — |
| Result/Option types | — |
| Traits / interfaces | — |
| Generics (monomorphization) | — |
| Multi-file imports | — |
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
| **Generics** | Planned | Monomorphized | Templates | Type params | Duck typing |

### Potato vs Rust
**Wins:** Simpler syntax, no lifetime annotations, faster to learn, instant compilation.
**Loses:** No borrow checker, no ecosystem.

### Potato vs C++
**Wins:** No headers, no UB, no preprocessor, readable syntax, compile-time move checking.
**Loses:** No templates, no operator overloading, less control.

### Potato vs Go
**Wins:** No GC, move semantics prevent some leaks, word-based syntax.
**Loses:** No goroutine integration in compiled output, single-file only.

### Potato vs Python
**Wins:** 100x+ faster, compiled, type safe at compile time, no runtime needed.
**Loses:** No libraries, no REPL, less forgiving, early stage.

---

## Roadmap

- [ ] Deep clone for `rep`
- [ ] Per-struct type-aware field resolution
- [ ] Enums with data (algebraic types)
- [ ] Pattern matching
- [ ] Result/Option types
- [ ] Traits / interfaces
- [ ] Generics (monomorphization)
- [ ] Multi-file imports
- [ ] Operator overloading
- [ ] Compile-time evaluation
- [ ] Built-in tooling (fmt, test, build)
- [ ] Self-hosting (compiler written in Potato)

---

## Testing

`make test` runs: passing examples, compile error tests, runtime panic tests, C runtime tests, and output-validated leetcode tests.

---

## Architecture

```
source.ptt 🥔 → [Lexer] → [Parser] → [Type Checker] → [Codegen] → ARM64 .s → [as + ld] → binary
```

Written in C. ~2500 lines. No dependencies.

---

## Contributing

The language is in early development. If you want to help shape it, open an issue or PR.

---

## License

MIT
