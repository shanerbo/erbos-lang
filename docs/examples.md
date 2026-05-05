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
  n <= 1 ?{
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
    i % 15 == 0 ?{
      yell("fizzbuzz")
    } i % 3 == 0 ?{
      yell("fizz")
    } i % 5 == 0 ?{
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
  n1 is alloc_Node()
  n1.value = 10
  n1.next = 0

  n2 is alloc_Node()
  n2.value = 20
  n2.next = n1

  yell(n2.value)
  yell(n1.value)
}
```

## Map Usage
```
spark {
  scores is map_new()
  map_set(scores, "alice", 95)
  map_set(scores, "bob", 87)
  map_set(scores, "charlie", 92)

  keys is map_keys(scores)
  through (name in keys) {
    yell("{name} scored {map_get(scores, name)}")
  }
}
```

## Counter with While Loop
```
spark {
  count is 0
  infi (count < 5) {
    yell(count)
    count = count + 1
  }
}
```

## Immutability
```
spark {
  nomut max is 100
  // max = 200  ← compile error!
  yell(max)
}
```
