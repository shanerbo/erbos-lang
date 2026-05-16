# Potato Language Guide 🥔

## Entry Point

```
spark {
  yell("hello world")
}
```

## Variables

```
x is 10              // inferred type
x is int 10          // explicit type
nomut pi is 3        // immutable
x be 42              // reassignment
```

All variables must be initialized.

## Types

| Type | Description |
|------|-------------|
| `int` | 64-bit signed integer |
| `str` | string |
| `bool` | `true` or `false` |
| `list of T` | typed dynamic list |
| `map of K to V` | typed ordered map (string keys) |
| `imap of K to V` | typed ordered map (int keys) |
| `nil` | null/empty pointer |

## Functions

```
add(a int, b int) int {
  give a + b
}

greet(name str) {
  yell("hello {name}")
}
```

`give` returns a value. Bare `give` returns void.

## Conditionals

```
x gt 10 ?{
  yell("big")
} x gt 5 ?{
  yell("mid")
} nah {
  yell("smol")
}
```

## Loops

```
// Range
through (i from 0 to 10 by 1) { }

// Collection
through (item in nums) { }

// While
infi (x gt 0) { x be x - 1 }

// Infinite + break
infi { stop }
```

`stop` = break, `skip` = continue.

## Strings

```
name is "world"
yell("hello {name}")          // interpolation (int and str vars)
c is "hi" + " there"          // concat
yell(len("hello"))            // 5
yell(char_at("abc", 1))       // "b"
```

## Structs

```
Point is {
  x int
  y int
}

p is Point()
p.x be 10
p.y be 20
```

## Methods

A method is a function attached to a user type. Define it with
`Type.name(self [ref] Type, ...)`:

```
Counter is { value int }

Counter.bump(self ref Counter) {
  self.value be self.value + 1
}

Counter.add(self ref Counter, n int) {
  self.value be self.value + n
}

Counter.get(self Counter) int {
  give self.value
}

spark {
  c is Counter()
  c.bump()
  c.add(40)
  yell(c.get())   // 41
}
```

Rules:
- The first parameter is the receiver. Use `ref` to allow mutation; without
  `ref`, the method cannot modify struct fields (same enforcement as
  ordinary `ref` parameters).
- The receiver's declared type must match the type the method is attached
  to. `Foo.bar(self Bar) { ... }` is a compile error.
- Methods on enum types work the same way:
  ```
  Result.tag(self Result) int { give 99 }
  ```
- Method dispatch is statically resolved. The compiler emits each method as
  `_<Type>_<name>` (for example `_Counter_bump`), so different types can
  share method names without collision.
- Built-in collection methods (`push`, `pop`, `set`, `get`, `keys`, `len`)
  on the literal `list` / `map` / `imap` / `task` types still lower to
  their built-in symbols. User methods on user types take precedence over
  any same-named free function.

## Generics

Structs and methods can be parametric. Type parameters appear after
`of` (one parameter) or `of … to …` (two parameters) immediately
after the type name in the declaration head:

```
Box of T is {
  value T
}

Box.set(self ref Box of T, v T) {
  self.value be v
}

Box.get(self Box of T) T {
  give self.value
}

Both of K to V is {
  key K
  value V
}

spark {
  // Two distinct instantiations of the same template.
  bi is Box of int ()
  bi.set(42)
  bs is Box of str ()
  bs.set("hello")
  yell(bi.get())   // 42
  yell(bs.get())   // hello

  p is Both of str to int ()
  // ...
}
```

Rules:

- Type parameters are bound by the surrounding declaration. Inside
  `Box.set`, the name `T` refers to whatever the caller instantiated
  the box with — the compiler reads it off the receiver's `of T`
  clause, so the method head doesn't repeat the parameter list.
- Constructors at use sites carry their type arguments explicitly:
  `Box of int ()`, `Both of str to int ()`, `Map of str to int ()`.
  The compiler does not currently infer type arguments from context.
- The compiler **monomorphizes**: each unique concrete form
  (`Box of int`, `Box of str`, `Both of str to int`, …) becomes its
  own emitted struct layout and its own emitted methods. There are
  no v-tables and no runtime type tags.
- Symbols are mangled positionally with `__` separators:
  `Box of int` → `_Box__int`, `Map of str to int` → `_Map__str__int`,
  `List of List of int` → `_List__List__int`.
- Nested generics are supported and parse right-associatively, so
  `Map of str to List of int` reads as `Map of str to (List of int)`.
  No commas, no parens, no `<>` in type position — anywhere.
- Up to two type parameters per generic (via `of T` or `of K to V`).
  Three-or-more-parameter generics are not yet supported; wrap
  multiple values in a named struct instead.
- Instantiating an undeclared template is a compile error
  (`cannot instantiate 'Foo<int>' — no generic type named 'Foo' is in scope`).

## Lists

```
nums is list of int           // typed, auto-constructed
nums.push(10)
nums.push(20)
yell(nums[0])                 // 10
yell(len(nums))               // 2
last is nums.pop()            // 20
list_set(nums, 0, 99)         // set by index

// Literal (type inferred)
vals is [1, 2, 3]

// Chained indexing
grid is list of list
grid.push([1, 2, 3])
yell(grid[0][1])              // 2
```

## Maps

```
// String-key map
scores is map of str to int
scores.set("alice", 95)
yell(scores.get("alice"))     // 95
yell(len(scores))             // 1

// Map literal
m is ["name" to 1, "age" to 2]

// Int-key map
memo is imap of int to int
imap_set(memo, 42, 100)
yell(imap_get(memo, 42))      // 100

// Iteration
keys is scores.keys()
through (k in keys) { yell(k) }
```

## Enums + Pattern Matching

```
Result is
  Ok(value int)
  | Err(message str)

r is Result.Ok(42)

match r {
  Ok(v) => yell(v)
  Err(msg) => yell(msg)
}
```

## Ownership & Memory

```
// RAII: auto-free at scope end
{ p is Point(); p.x be 42 }  // p freed here

// Move
b is now a                    // a is dead

// Clone (shallow)
c is rep b

// Ref params (mutable borrow)
reset(p ref Point) { p.x be 0 }
reset(ref pt)                 // caller acknowledges
```

> `ref` is enforced: mutating a non-ref struct parameter is a compile error.

## Imports

```
use std/math
use utils/helper as h

spark {
  yell(math.max(10, 20))
  h.do_thing()
}
```

Standard library: `std/math`, `std/queue`, `std/stack`

## Testing

```
test "addition" {
  assert(1 + 1 eq 2)
  assert(add(3, 4) eq 7)
}
```

Run: `erbos test file.ptt`

## Operators

| Symbol | Word | Meaning |
|--------|------|---------|
| `+` `-` `*` `/` | — | arithmetic |
| `%` | `mod` | modulo |
| `>` | `gt` | greater than |
| `<` | `lt` | less than |
| `>=` | `ge` | greater or equal |
| `<=` | `le` | less or equal |
| `==` | `eq` | equal |
| `!=` | `ne` | not equal |
| — | `and` `or` `not` | logical (short-circuit) |

## Build & Run

```bash
./erbos program.ptt          # compile to binary
./erbos run program.ptt      # compile + run + cleanup
./erbos test tests.ptt       # run test blocks
./erbos ir program.ptt       # emit assembly only (program.s), don't link
```

The `-O0` / `-O1` / `-O2` flags select an optimization level; `-O1` is
the default. They can appear anywhere relative to the subcommand:

```bash
./erbos -O0 run program.ptt  # skip iropt entirely
./erbos run -O1 program.ptt  # explicit default
./erbos -O2 program.ptt      # reserved for tuning; same as -O1 today
```
