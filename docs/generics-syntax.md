# Generics syntax

The Potato language uses **word-style** generics, matching the rest of
its grammar (`is`, `be`, `gt`, `give`, `through`, `infi`). There is
no `<T>` syntax anywhere in type position.

## Surface

```
// Single type parameter
Box of T is {
  value T
}

// Two type parameters
Map of K, V is {
  count int
  keys  array of K
  vals  array of V
}

// Methods — receiver type binds the parameters
Box.set(self ref Box of T, v T) {
  self.value be v
}

Box.get(self Box of T) T {
  give self.value
}

Map.set(self ref Map of K, V, k K, v V) {
  // ...
}

// Use sites — auto-construct (no parens)
xs is List of int
m is Map of String, int
b is Box of int
b.value be 42

// Nested — no parens needed; right-associative
ll is List of List of int
mml is Map of String, List of int
deep is Map of String, Map of int, String
```

## Rules

1. **One connective:** `of` introduces the type-argument list.
   Additional type arguments are separated with commas:
   `Type of A`, `Type of A, B`, `Type of A, B, C`.
2. **Commas are the only multi-argument separator.** `to` is not valid in
   type position. It remains valid for map literals (`["k" to 1]`) and
   range loops (`through (i from 0 to n by 1)`).
3. **No `<>` anywhere.** Hard cut, no transition.
4. **No parens around type arguments.** Nested generic arguments are
   parsed through the `of` chains, so `List of List of int` parses
   exactly one way.
5. **Auto-construct on typed declaration.** `x is List of int` with
   no initializer auto-emits the constructor call.
6. **Method head infers parameters from the receiver.** Write
   `Box.set(self ref Box of T, v T)` — not `Box of T.set(...)`.
   The compiler reads the receiver's `of T` clause and binds `T`
   for the rest of the signature and body.

## Mangling

- `Box of int` → `_Box__int`
- `Box of String` → `_Box__String`
- `Map of String, int` → `_Map__String__int`
- `List of List of int` → `_List__List__int`
- `Map of String, List of int` → `_Map__String__List__int`

`to` and `of` both lower to `__` in the mangled name. The mangling
is positional, so `Map of int, String` and `Map of String, int`
produce different symbols.

## Why commas

Earlier Potato used `Map of K, V` for two-parameter types. That read
nicely for maps, but it did not generalize to other two-parameter types
such as `Result`, `Pair`, or user-defined structs. Potato now uses one
generic spelling everywhere:

```
Map of K, V
Result of T, E
Pair of A, B
```

The type's name carries the semantics. `Map` means key/value, `Result`
means success/error, and `Pair` means first/second.

## Why no parens

Same reasoning: zero parens in type position means zero precedence
rules to memorise. The right-associative `of` chaining reads naturally:

```
List of Map of int, List of str
//   parses as:
List of (Map of int, (List of str))
```

The parser consumes comma-separated type arguments after `of` while the
next token still looks like a type argument.

## No Pair / tuple types

`Map of K, V` stores its entries as two parallel `array of K` and
`array of V` fields, not as an array of `Pair<K, V>`. Users who
want a 2-tuple define their own named struct.
