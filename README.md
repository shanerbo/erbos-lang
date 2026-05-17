# Potato 🥔

A small, opinionated systems language that reads like English,
compiles to native code, and has no garbage collector, no
runtime, no libc dependency.

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

## Why Potato

Most systems languages force you to pick: cheap, fast,
correct — pick two. C and C++ are cheap and fast and the
correctness is on you. Rust is fast and correct and the cheap
went into your debugging time. Go gives you cheap and (mostly)
correct and pays for it with a runtime. Potato is an
experiment in being all three by **picking a much smaller
problem.**

It's a language for writing programs that are tree-shaped,
single-threaded, and freed when they go out of scope.
Concurrency, garbage collection, dynamic dispatch, lifetime
annotations, async — none of that. The features that aren't
here aren't missing; they're **deliberately** not here, and
each one's absence is what makes the rest cohere.

The values, in priority order:

1. **Reads like a notebook.** `is`, `be`, `give`, `through`,
   `infi`, `nah`. The vocabulary is small enough to hold in your
   head. There's no symbol soup.
2. **Every decision is explicit.** No magic conversions, no
   hidden allocations, no implicit copies. If two
   spellings would have different meanings, the language picks
   one and rejects the other.
3. **The compiler is honest.** Single backend, single optimizer
   pipeline, no preprocessor. What you write is what runs.
4. **Every feature pays rent.** A feature ships if it makes the
   first hour better or prevents a class of bugs. Otherwise it
   doesn't ship — even if every other language has it. See
   [`docs/design-decisions.md`](docs/design-decisions.md).

The result isn't a Rust competitor or a C++ competitor. It's a
language that someone tries for an evening, ships something
working, and walks away with their mental model intact. That's
the bar.

---

## Memory & ownership at a glance

This is the part of the language that's most worth understanding
first, because every other design decision flows from it.

### The three rules

1. **Every heap allocation has exactly one owner.** When that
   owner goes out of scope, the allocation is freed. There is
   no garbage collector, no refcount, no manual `free`.
2. **Aliasing is explicit.** To create a second binding for the
   same data, you say what you mean: `is now` to move ownership
   (source becomes inaccessible), or `is rep` to deep-clone
   (independent copy). Plain `q is p` for heap-shaped values is
   a compile error — the language refuses to guess.
3. **Borrows are lexical to a function call.** Pass a struct to
   a function and the function sees a pointer to your data. When
   the function returns, the borrow is over. There are no
   long-lived references, no `&T` bindings, no lifetime
   annotations.

### Construction

```
Point is {
  x int
  y int
}

p is Point()                    // zero-default — every field starts at 0
p.x be 10
p.y be 20

q is Point(x is 10, y is 20)    // named-arg — atomic init, every field required
nomut origin is Point(x is 0, y is 0)
```

Two forms because they serve different needs. Zero-default is
cheap; named-arg is what you reach for when you want
`nomut`-protected immutability or refactor-safe construction.
Positional `Point(1, 2)` is intentionally rejected — field
reorder would silently swap call-site semantics.

### Move and clone

```
a is Point(x is 1, y is 2)

b is now a                      // ownership moves to b; `a` is dead
// yell(a.x)                    // COMPILE ERROR: use of moved variable

c is rep b                      // c gets an independent copy (deep clone)
                                // both b and c are alive, each owns its block
                                // (currently shallow — see "Status" below)
```

After the scope ends, each remaining heap binding is freed
exactly once. RAII is real — no leaks, no double-free.

### Calling convention

| Param | What's passed | Mutation |
|-------|---------------|----------|
| `n int`, `b bool` | the value (8 bytes, copied) | n/a |
| `p Point` (no `ref`) | a pointer to the caller's heap struct | **read-only** (compile error to mutate fields) |
| `p ref Point` | a pointer to the same struct | callee may mutate |

A non-`ref` heap-shaped param is C++'s `const T&`: same data
the caller sees, no copy, no mutation. There's no `nomut ref T`
— absence of `ref` already means read-only.

```
read_only(p Point) {
  yell(p.x)            // ok, reads through the pointer
  // p.x be 99         // COMPILE ERROR: parameter is not ref
}

bump(p ref Point) {
  p.x be p.x + 1       // ok, ref opted into mutation
}
```

This is the "borrow ends when the function returns" subset of
Rust's borrow checker, enforced for free by the grammar — there
is no syntax to express anything else.

### Sharing data — the arena pattern

If you're coming from C++ where you'd reach for `shared_ptr`,
the Potato idiom is **arena + index**. The arena owns the data
once; consumers hold integer handles into it.

```
use std/list

Image is {
  pixels int
}

ImageStore is {
  images List of Image
}

ImageStore.add(self ref ImageStore, img Image) int {
  self.images.push(img)
  give self.images.len() - 1
}

spark {
  store is ImageStore()
  icon_id is store.add(Image(pixels is 32000))

  // Many "consumers" all reference the same image — by integer.
  window_icon is icon_id
  toolbar_icon is icon_id
  about_icon is icon_id

  // Look up via the arena when you need the actual data.
  icon is store.images.get(window_icon)
  yell(icon.pixels)        // 32000

  // The image exists ONCE in memory; window_icon, toolbar_icon,
  // and about_icon are just int handles into the arena.
}
```

**The image exists once in memory.** Just like `shared_ptr`. But
there's no refcount overhead, no atomic ops, no cycle-leak risk,
and lifetime is *structural* — the arena outlives its handles by
construction. This is the pattern modern compilers (LLVM,
rust-analyzer), ECS game engines, and databases all converged
on. It works without language changes.

### What Potato deliberately doesn't have

- **No garbage collector.** Single ownership + RAII is enough.
- **No refcount (`Rc`/`Arc`).** Arena+index covers the use case.
- **No `&T` long-lived borrows.** They'd require a borrow
  checker, which is a separate multi-year language design.
- **No async/await.** A green-thread runtime exists in the
  source tree but isn't wired into compiled output yet.

These aren't TODOs. They're decisions. If a real Potato program
ever genuinely can't be expressed with the existing tools, we
revisit. Until then, every feature on this list is **a tax we
chose not to pay**.

The full reasoning for each is in
[`docs/design-decisions.md`](docs/design-decisions.md).

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

# Optimization levels — accepted in any position relative to the subcommand.
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
nomut pi is 3        // immutable (no rebind, no field-mut)
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

`give` returns a value. Bare `give` returns void.

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
runtime cost. See [`docs/generics-syntax.md`](docs/generics-syntax.md).

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

---

## Keywords

| Keyword | Purpose |
|---------|---------|
| `spark` | program entry point |
| `is` | variable declaration |
| `be` | reassignment |
| `nomut` | immutable binding (no rebind, no field-mut) |
| `give` | return value |
| `through` | range/collection loop |
| `from` / `to` / `by` | loop range |
| `in` | collection iteration |
| `infi` | while / infinite loop |
| `stop` | break |
| `skip` | continue |
| `nah` | else |
| `now` | move ownership |
| `rep` | deep clone |
| `ref` | mutable parameter |
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

## Status

### Implemented
Integer + bool + byte arithmetic. Strings + interpolation. String
concat with `+`. Functions, recursion, type inference. Structs
(zero-default + named-arg construction). Lists, maps (String- or
int-keyed), arrays. Conditionals, range/collection/while loops.
Move semantics with use-after-move detection. `nomut`
enforcement (rebind + field-mut). Methods (with `ref self` for
mutation). Generics + monomorphization. Enums + `match`.
Multi-file imports. Built-in test framework. Five-pass IR
optimizer (inlining, SRA, escape analysis, BCE, LICM).
Bounds-checked array/list/map access.

### Partial
- `is rep` is currently shallow copy; deep clone is in flight.
  See `docs/design-decisions.md` for the latent UAF and the fix.
- Green-thread runtime exists in `compiler/runtime/` but isn't
  wired into compiled `.ptt` output.

### Roadmap
- Deep clone for `rep` (correctness fix)
- `Result of T to E` for fallible operations
- Better error messages with caret + source context
- File I/O
- Default `_<Type>_yell` for debugging structs without
  per-type boilerplate

Explicitly **not** on the roadmap (see design-decisions log):
operator overloading, traits/interfaces, async, macros,
self-hosting (deferred until language is stable).

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
`make test` runs: passing examples, leetcode library compile
checks, expected compile-error tests, expected runtime panics,
C-runtime tests, framework tests across the whole `tests/`
tree, and the IR backend regression matrix at `-O0` / `-O1` /
`-O2`. Must end with `All tests passed.`

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

The **IROpt** stage runs five passes — inlining, scalar
replacement of aggregates, escape analysis (stackify), bounds-
check elimination, and loop-invariant code motion — at `-O1`
and `-O2`. `-O0` skips them all.

The **Monomorph** pass instantiates every concrete generic
form (`Box of int`, `Map of String to int`, …) before type
checking, so the rest of the pipeline only sees fully-
specialised types. Names are mangled inside-out:
`List of Map of String to int` becomes `_List__Map__String__int`.

The **IR backend** is the only backend. Cross-block, call-aware
register allocation: vregs whose live range crosses a `bl` are
placed in callee-save registers (x19..x28) with proper prologue
saves; shorter-lived values use x8/x11..x18 (x9 and x10 are
reserved as iremit scratch for large-offset frame addressing).
See [`docs/ir-pipeline.md`](docs/ir-pipeline.md).

`erbos ir <file.ptt>` emits the .s only, useful for inspecting
generated assembly.

---

## Contributing

The language is in early development. If you want to help shape
it, open an issue or PR.

Before proposing a language change, read
[`docs/design-decisions.md`](docs/design-decisions.md) — the
running log of what's been decided, what's parked, and the
first-principles reasoning. The standing rule: **every feature
must pay rent** — improve the user's first hour, or prevent a
class of bugs in their first thousand lines.

---

## License

MIT
