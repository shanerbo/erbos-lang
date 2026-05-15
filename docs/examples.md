# Potato Examples 🥔

## Hello World
```
spark {
  yell("hello world")
}
```

## FizzBuzz
```
spark {
  through (i from 1 to 100 by 1) {
    i mod 15 eq 0 ?{
      yell("fizzbuzz")
    } i mod 3 eq 0 ?{
      yell("fizz")
    } i mod 5 eq 0 ?{
      yell("buzz")
    } nah {
      yell(i)
    }
  }
}
```

## Fibonacci
```
fib(n int) int {
  n le 1 ?{ give n }
  give fib(n - 1) + fib(n - 2)
}

spark {
  yell(fib(10))
}
```

## Structs
```
Point is {
  x int
  y int
}

spark {
  p is Point()
  p.x be 3
  p.y be 4
  yell(p.x + p.y)
}
```

## Collections
```
spark {
  // Typed list
  nums is list of int
  nums.push(10)
  nums.push(20)
  yell(nums[0])

  // Map literal
  scores is ["alice" to 95, "bob" to 87]
  yell(scores.get("alice"))

  // Int-key map
  memo is imap of int to int
  imap_set(memo, 1, 100)
  yell(imap_get(memo, 1))
}
```

## Enums + Error Handling
```
Result is
  Ok(value int)
  | Err(message str)

divide(a int, b int) Result {
  b eq 0 ?{ give Result.Err("division by zero") }
  give Result.Ok(a / b)
}

spark {
  match divide(10, 2) {
    Ok(v) => yell(v)
    Err(msg) => yell(msg)
  }
}
```

## Methods
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
  c.bump()
  yell(c.get())   // 3
}
```

## Generics
```
Box<T> is {
  value T
}

Box<T>.set(self ref Box<T>, v T) {
  self.value be v
}

Box<T>.get(self Box<T>) T {
  give self.value
}

Pair<K, V> is {
  key K
  value V
}

Pair<K, V>.set_key(self ref Pair<K, V>, k K) {
  self.key be k
}

Pair<K, V>.set_value(self ref Pair<K, V>, v V) {
  self.value be v
}

spark {
  // Each instantiation produces its own emitted code:
  //   Box<int>      -> _Box__int
  //   Box<str>      -> _Box__str
  //   Pair<str,int> -> _Pair__str__int
  bi is Box<int>()
  bi.set(42)
  yell(bi.get())   // 42

  bs is Box<str>()
  bs.set("hello")
  yell(bs.get())   // hello

  p is Pair<str, int>()
  p.set_key("alice")
  p.set_value(95)
}
```

## BST (Binary Search Tree)
```
TreeNode is {
  value int
  left int
  right int
}

new_node(val int) TreeNode {
  n is TreeNode()
  n.value be val
  n.left be nil
  n.right be nil
  give n
}

insert(root int, val int) int {
  root eq nil ?{ give new_node(val) }
  val lt root.value ?{ root.left be insert(root.left, val) }
  val gt root.value ?{ root.right be insert(root.right, val) }
  give root
}
```

## Imports
```
use std/math

spark {
  yell(math.max(10, 20))
  yell(math.pow(2, 8))
}
```

## Testing
```
add(a int, b int) int { give a + b }

test "addition" {
  assert(add(1, 2) eq 3)
  assert(add(-1, 1) eq 0)
}
```

Run: `erbos test file.ptt`

## Ownership
```
Point is { x int, y int }

spark {
  a is Point()
  a.x be 10
  b is now a        // move: a is dead
  yell(b.x)        // 10

  {
    tmp is Point()  // scoped
    tmp.x be 42
  }                 // tmp freed here (RAII)
}
```
