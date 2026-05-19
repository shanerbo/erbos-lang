# Potato 🥔

A small, opinionated systems language that reads like English,
compiles to native ARM64 binaries on **macOS (Mach-O)** and
**Linux (ELF)**, and has no garbage collector, no runtime, no
libc dependency. Source files use the `.ptt` extension.

```
use std/string

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

Most systems languages force you to pick: cheap, fast, correct —
pick two. C/C++ are cheap and fast, correctness is on you. Rust
is fast and correct, the cheap went into your debugging time.
Go gives you cheap and (mostly) correct and pays for it with a
runtime. Potato is an experiment in being all three by
**picking a much smaller problem.**

It's a language for programs that are tree-shaped,
single-threaded, and freed when they go out of scope. The
features that aren't here aren't missing; they're **deliberately**
not here, and each one's absence is what makes the rest cohere.

The values:

1. **Reads like a notebook** — `is`, `be`, `give`, `through`. No symbol soup.
2. **Every decision is explicit** — no magic conversions, no hidden allocations, no implicit copies.
3. **The compiler is honest** — single backend, single optimizer, no preprocessor.
4. **Every feature pays rent** — see [`docs/design-decisions.md`](docs/design-decisions.md).

The bar isn't "competitor to Rust." It's "tries it for an
evening, ships something working, walks away with the mental
model intact."

---

## Memory & ownership

The most distinctive part of the language. Three rules:

1. **Every heap allocation has exactly one owner.** Freed when
   that owner goes out of scope. No GC, no refcount, no manual
   `free`.
2. **Aliasing is explicit.** `is now` to move, `is rep` to deep-clone,
   plain `q is p` for heap-shaped values is a compile error.
3. **Borrows are lexical.** A struct passed to a function is
   visible there as a pointer; when the function returns, the
   borrow is over. No `&T` bindings, no lifetime annotations.

```
Point is { x int, y int }

a is Point(x is 1, y is 2)            // construct
b is now a                             // move — `a` is dead
// yell(a.x)                          // COMPILE ERROR

read(p Point) { yell(p.x) }            // pointer; read-only
bump(p ref Point) { p.x be p.x + 1 }   // pointer; ref opts into mutation

bump(ref b)                            // call with explicit ref
```

A non-`ref` heap-shaped param is C++'s `const T&`: same data,
no copy, no mutation. There's no `nomut ref T` — absence of `ref`
already means read-only.

### Sharing data — the arena pattern

If you'd reach for `shared_ptr` in C++, the Potato idiom is
**arena + index**: one owner holds the data, consumers carry
integer handles.

```
use std/list

Image is { pixels int }
ImageStore is { images List of Image }

ImageStore.add(self ref ImageStore, img Image) int {
  self.images.push(img)
  give self.images.len() - 1
}

spark {
  store is ImageStore()
  icon_id is store.add(Image(pixels is 32000))

  // Many "consumers" reference the same image — by integer.
  window_icon is icon_id
  toolbar_icon is icon_id

  icon is store.images.get(window_icon)
  yell(icon.pixels)
}
```

The image exists once in memory. No refcount, no cycle leaks.
Lifetime is *structural* — the arena outlives its handles by
construction. See [`docs/language-guide.md`](docs/language-guide.md)
for `nomut`-protected construction, the `App` pattern for
multi-arena programs, and the full calling-convention table.

### What Potato deliberately doesn't have

- **No garbage collector.** Single ownership + RAII is enough.
- **No refcount (`Rc`/`Arc`).** Arena+index covers the case.
- **No `&T` long-lived borrows.** They'd require a borrow checker.
- **No module-level globals.** Use the `App` pattern.
- **No async/await.** A green-thread runtime exists in tree but isn't wired in yet.

These are decisions, not TODOs. Reasoning in
[`docs/design-decisions.md`](docs/design-decisions.md).

---

## Quick Start

```bash
make                              # build the compiler
./erbos run hello.ptt             # compile + execute + clean up
./erbos hello.ptt && ./hello      # build to binary
./erbos ir hello.ptt              # emit .s, don't assemble

./erbos -O0 run hello.ptt         # skip iropt
./erbos -O1 run hello.ptt         # default
./erbos -O2 run hello.ptt         # reserved, currently == -O1
```

By default the compiler targets the host. Pass `--target=<t>`
to switch backends:

```bash
./erbos --target=darwin-arm64 run hello.ptt   # macOS / Mach-O (default on Mac)
./erbos --target=linux-arm64  ir  hello.ptt   # Linux / ELF — emit .s
```

For projects bigger than one file, drop a `potato.toml` (empty is
fine) at the project root. The compiler walks up to find it.
Stdlib (`use std/list`, `use std/string`, etc.) is bundled with
the compiler binary — no copying needed.

---

## Platforms

Two native ARM64 backends ship in-tree, both validated end-to-end:

| Target | Object format | Syscall ABI | Toolchain | Status |
|---|---|---|---|---|
| `darwin-arm64` | Mach-O | x16 + `svc #0x80` | Apple `as` + `ld` (`xcrun --show-sdk-path`, `-lSystem`) | default; full repo test suite passes |
| `linux-arm64` | ELF | x8 + `svc #0` | GNU `as` + `ld` (`-nostdlib -static`) | 182 program runs validated identical to Darwin (examples + framework + leetcode + IR matrix at `-O0/-O1/-O2` + runtime panics) |

Linux output was validated by cross-assembling on macOS (clang
`--target=aarch64-linux-gnu` + `ld.lld -static`) and executing
inside two independent Linux/aarch64 runtimes on Apple Silicon:
Apple's native `container` CLI (Alpine, kernel 6.18.15) and
Docker Desktop (Debian trixie, linuxkit kernel 6.12). Both
runtimes produced identical results to Darwin across all 182
program runs. Bare-metal `erbos --target=linux-arm64 run …`
requires GNU `as` and `ld` on the host PATH; on a real
Linux/aarch64 host the existing `./erbos --target=linux-arm64
run hello.ptt` path is the same as Darwin's.

Linux backend implementation: `compiler/target_linux_arm64.c`;
every Linux-specific constant (`__NR_write=64`, `__NR_exit=93`,
`__NR_mmap=222`, `MAP_PRIVATE|MAP_ANONYMOUS=0x22`, `:lo12:`
relocations) is cited inline to its source-of-truth document.
Validation playbook + acceptance-gate evidence:
[`docs/linux-arm64-backend-plan.md`](docs/linux-arm64-backend-plan.md).

The host machine the compiler itself builds on is currently macOS
only (`compiler/main.c` uses `_NSGetExecutablePath` for stdlib
resolution); a Linux host port is straightforward but not
shipped.

---

## What it looks like

```
// Variables, conditions, loops
x is 10
nomut pi is 3
through (i from 0 to 10 by 1) { yell(i) }
x gt 5 ?{ yell("big") } nah { yell("smol") }

// Functions
add(a int, b int) int { give a + b }

// Structs
Point is { x int, y int }
p is Point(x is 1, y is 2)

// Methods (with `ref self` for mutation)
Counter is { value int }
Counter.bump(self ref Counter) { self.value be self.value + 1 }

// Generics — word-style, no <T>
Box of T is { value T }
b is Box of int()
```

```
// Collections
use std/basics                  // bundle: String + List + Map

xs is List of int()             // also: [1, 2, 3]
xs.push(10)

m is Map of String, int()       // also: ["k" to 1]
m.set("key", 42)

// Enums — values formed only via the stdlib factories
use std/result
r is ok of int, String (42)
match r {
  Ok(v) => yell(v)
  Err(e) => yell(e)
}

// Built-in tests
test "addition" {
  assert(1 + 1 eq 2)
}
```

Full reference: [`docs/language-guide.md`](docs/language-guide.md).
Keyword + operator tables: [`docs/keywords.md`](docs/keywords.md).

---

## Status

**Implemented.** Integer/bool/byte arithmetic. Strings + interpolation.
Functions, recursion, type inference. Structs (zero-value +
named-field formation; struct-typed fields auto-init).
Lists, maps (String- or int-keyed), arrays. Conditionals,
range/collection/while loops. Move semantics with use-after-move
detection. **Deep clone via `is rep`.** Plain `q is p` for heap-shaped
values rejected (must use `now` or `rep` explicitly). `nomut`
(rebind + field-mut). Methods (with `ref self` for mutation).
Generics + monomorphization. Enums + `match` with typed bindings;
`Option of T` and `Result of T, E` formed via factories
(`none` / `some` / `ok` / `err`). Multi-file imports with
`potato.toml` project root. Stdlib (`std/string`, `std/list`,
`std/map`, `std/option`, `std/result`, `std/stack`,
`std/queue`, `std/deque`, `std/ring_buffer`, `std/arena`,
`std/byte_buffer`, `std/string_builder`, `std/math`,
`std/algo`, `std/basics`) bundled with the compiler binary.
Built-in test framework. Five-pass IR optimizer (inlining, SRA,
escape analysis, BCE, LICM). Bounds-checked array/list/map access.
Helpful import-error messages (suggests `potato.toml`, etc.).
**Two native backends: `darwin-arm64` (Mach-O) and `linux-arm64`
(ELF), selectable via `--target=<t>`.**

**In flight.**
- Green-thread runtime exists in `compiler/runtime/` but isn't
  wired into compiled output.

**Roadmap.** Hash `Map`. `Set of T`. `Path` / `File` / `Reader` /
`Writer`. `Pool of T`. Caret + source-context error messages.

**Explicitly NOT on the roadmap** (see design-decisions log):
operator overloading, traits/interfaces, async, macros,
self-hosting.

---

## Project layout

```
~/myproject/
  potato.toml          # marker — empty is fine
  src/
    spark.ptt          # use std/list, use lib/...
  lib/
    helper/
      str_ops.ptt
```

Run `erbos run src/spark.ptt` from anywhere. The compiler walks
up from the source file to find `potato.toml`, anchors all
non-stdlib `use` paths to that root. Stdlib (`std/list`,
`std/string`, etc.) is bundled with the compiler binary.

Resolver order: sibling → project-root → bundled-stdlib. Full
rules in [`docs/language-guide.md`](docs/language-guide.md).

---

## Architecture

```
source.ptt → Lexer → Parser → Monomorph → Checker → Optimizer
          → IRGen → IROpt → RegAlloc → IREmit → ARM64 .s → as + ld → binary
                                          │
                                          ▼
                    target_{darwin,linux}_arm64.c
                    (sections, syscall ABI, address-load relocations,
                     entry-point emission, toolchain driver)
```

C11. Compiler frontend in `compiler/`; green-thread runtime in
`compiler/runtime/`. No external dependencies. The IR backend is
the only backend; it does cross-block call-aware register
allocation, and lowers to ARM64 assembly that's parameterised by
a small `Target` interface (`compiler/target.h`) so a single
emitter serves both Mach-O+Darwin and ELF+Linux. See
[`docs/ir-pipeline.md`](docs/ir-pipeline.md) for the IR side and
[`docs/linux-arm64-backend-plan.md`](docs/linux-arm64-backend-plan.md)
for the per-target split.

---

## Contributing

Early-stage language. Open an issue or PR to help shape it.

Before proposing a language change, read
[`docs/design-decisions.md`](docs/design-decisions.md) — the
running log of what's decided, what's parked, and the
first-principles reasoning. The standing rule: **every feature
must pay rent** (improve the user's first hour, or prevent a
class of bugs in their first thousand lines).

---

## License

MIT
