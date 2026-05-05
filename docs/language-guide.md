# Erbos Language Guide

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
x be 20               // reassignment
```

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

`give` returns a value. Last expression is NOT implicit — you must use `give`.

## Conditionals

```
x > 10 ?{
  yell("big")
} x > 5 ?{
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
infi (x > 0) {
  x be x - 1
}
```

### Infinite loop
```
infi {
  // runs forever
  x == 10 ?{
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
  p is alloc_Point()
  p.x be 10
  p.y be 20
  yell(p.x + p.y)
}
```

## Lists

```
nums is [10, 20, 30]
yell(nums[0])                 // indexing
yell(nums[2])

through (n in nums) {         // iteration
  yell(n)
}
```

## Maps

```
m is map_new()
map_set(m, "alice", 100)
map_set(m, "bob", 85)

yell(map_get(m, "alice"))     // 100
yell(map_len(m))              // 2

// Iteration (ordered by insertion)
keys is map_keys(m)
through (k in keys) {
  yell(k)
  yell(map_get(m, k))
}
```

## Operators

| Operator | Meaning |
|----------|---------|
| `+` `-` `*` `/` | arithmetic |
| `==` `!=` | equality |
| `<` `>` `<=` `>=` | comparison |
| `and` `or` `not` | logical |

## Immutability

```
nomut x is 10
x be 20              // COMPILE ERROR: cannot reassign nomut variable
```

## Recursion

```
fib(n int) int {
  n <= 1 ?{
    give n
  }
  give fib(n - 1) + fib(n - 2)
}

spark {
  yell(fib(10))     // 55
}
```

## Concurrency (runtime)

```
spark {
  t is task
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

All source files use `.erbos` extension.

## Build & Run

```
./erbos program.erbos
./program
```
