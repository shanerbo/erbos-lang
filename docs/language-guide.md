# Erbos Language Guide 🥔

## Entry Point

Every program starts with `spark`:

```
spark {
  yell("hello world")
}
```

## Variables

```
x is 10              // inferred type
x is int 10          // explicit type
nomut pi is 3        // immutable — cannot reassign
x be 20              // reassignment
```

All variables must be initialized. `x is int` without a value is a compile error.

## Types

| Type | Description |
|------|-------------|
| `int` | 64-bit signed integer |
| `str` | string |
| `bool` | `true` or `false` |

## Functions

```
// No return value
greet(name str) {
  yell("hello {name}")
}

// With return
add(a int, b int) int {
  give a + b
}

// Call
spark {
  result is add(3, 4)
  greet("erbos")
}
```

`give` returns a value. Bare `give` returns void (early exit).

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

- `?{` = if this condition is true
- Additional conditions after `}` = else-if
- `nah` = else

## Loops

### Range loop
```
through (i from 0 to 10 by 2) {
  yell(i)
}
```

### Collection loop
```
through (item in my_list) {
  yell(item)
}
```

### While loop
```
infi (x gt 0) {
  x be x - 1
}
```

### Infinite loop
```
infi {
  x eq 10 ?{
    stop
  }
}
```

### Loop control
- `stop` — break out of loop
- `skip` — skip to next iteration

## Strings

```
name is "world"
yell("hello {name}")          // interpolation
c is str_concat("a", "b")    // concatenation
```

## Structs

```
Point is {
  x int
  y int
}

spark {
  p is Point()
  p.x be 10
  p.y be 20
  yell(p.x + p.y)
}
```

## Lists

```
nums is [10, 20, 30]          // literal
yell(nums[0])                 // indexing (bounds-checked)

nums is list()                // dynamic list
nums.push(10)
nums.push(20)
last is nums.pop()
yell(nums.len())

through (n in nums) {         // iteration
  yell(n)
}
```

## Maps

```
m is map()
m.set("alice", 100)
m.set("bob", 85)

yell(m.get("alice"))          // 100
yell(m.len())                 // 2

// Iteration (ordered by insertion)
keys is m.keys()
through (k in keys) {
  yell(k)
  yell(m.get(k))
}
```

## Operators

### Arithmetic (symbols only)
| Symbol | Meaning |
|--------|---------|
| `+` | add |
| `-` | subtract |
| `*` | multiply |
| `/` | divide |

### Modulo (both work)
| Symbol | Word |
|--------|------|
| `%` | `mod` |

### Comparisons (both work)
| Symbol | Word | Meaning |
|--------|------|---------|
| `>` | `gt` | greater than |
| `<` | `lt` | less than |
| `>=` | `ge` | greater or equal |
| `<=` | `le` | less or equal |
| `==` | `eq` | equal |
| `!=` | `ne` | not equal |

### Logical (words only)
| Word | Meaning |
|------|---------|
| `and` | logical AND |
| `or` | logical OR |
| `not` | negation |

## Ownership & Memory

### RAII — automatic cleanup
Every heap allocation is freed when its scope ends:
```
spark {
  p is Point()
  p.x be 42
}                   // p auto-freed here
```

### Scoped blocks
Use bare `{ }` for short-lived allocations:
```
spark {
  {
    temp is Point()
    temp.x be 99
  }                 // temp freed here
  // temp doesn't exist here
}
```

### Move semantics
Transfer ownership — original is dead:
```
a is Point()
b is now a          // a is dead, b owns it
// yell(a.x)       // COMPILE ERROR: use of moved variable 'a'
```

### Clone
Deep copy — both live:
```
a is Point()
b is rep a          // b is a copy, both live
```

### give transfers ownership
```
make_point() int {
  p is Point()
  p.x be 42
  give p            // ownership moves to caller, no free here
}
```

### Ref params (mutable borrow)
```
reset(p ref int) {
  p.x be 0         // allowed — p is ref
}

read(p int) {
  yell(p.x)
  // p.x be 5     // COMPILE ERROR: p is not ref
}

spark {
  p is Point()
  reset(ref p)     // caller acknowledges mutation
}
```

## Immutability

```
nomut x is 10
x be 20             // COMPILE ERROR: cannot reassign nomut variable
```

## Recursion

```
fib(n int) int {
  n le 1 ?{
    give n
  }
  give fib(n - 1) + fib(n - 2)
}

spark {
  yell(fib(10))     // 55
}
```

## Bounds Checking

```
nums is [10, 20, 30]
yell(nums[5])       // RUNTIME PANIC: index out of bounds
```

## Concurrency (runtime)

```
spark {
  t is task()
  t.fire(worker())
  yell("done")
}

worker() {
  yell("working")
}
```

## Comments

```
// single line

/* multi
   line */
```

## File Extension

All source files use `.ptt` 🥔 extension.

## Build & Run

```bash
./erbos program.ptt    # compile to binary
./erbos run program.ptt # compile + run + cleanup 🥔
```
