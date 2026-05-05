# Erbos Examples

## Hello World
```
spark {
  yell("hello world")
}
```

## Fibonacci
```
fib(n int) int {
  n le 1 ?{
    give n
  }
  give fib(n - 1) + fib(n - 2)
}

spark {
  yell(fib(10))
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

## Linked List
```
Node is {
  value int
  next int
}

spark {
  n1 is Node()
  n1.value be 10
  n1.next be 0

  n2 is Node()
  n2.value be 20
  n2.next be n1

  yell(n2.value)
  yell(n1.value)
}
```

## Map Usage
```
spark {
  scores is map()
  scores.set("alice", 95)
  scores.set("bob", 87)
  scores.set("charlie", 92)

  keys is scores.keys()
  through (name in keys) {
    yell(name)
    yell(scores.get(name))
  }
}
```

## Dynamic List
```
spark {
  nums is list()
  nums.push(10)
  nums.push(20)
  nums.push(30)

  yell(nums.len())
  last is nums.pop()
  yell(last)

  through (n in nums) {
    yell(n)
  }
}
```

## Counter with While Loop
```
spark {
  count is 0
  infi (count lt 5) {
    yell(count)
    count be count + 1
  }
}
```

## Ownership & Move
```
Point is {
  x int
  y int
}

spark {
  a is Point()
  a.x be 10
  b is now a          // a is dead
  yell(b.x)          // 10
  // yell(a.x)       // COMPILE ERROR: use of moved variable
}
```

## Scoped Lifetime
```
Point is {
  x int
  y int
}

spark {
  x is 0
  {
    temp is Point()
    temp.x be 99
    x be temp.x
  }                   // temp freed here
  yell(x)            // 99
}
```

## Immutability
```
spark {
  nomut max is 100
  // max be 200      // COMPILE ERROR: cannot reassign nomut variable
  yell(max)
}
```
