# Generics syntax

The Potato language uses **word-style** generics, matching the rest of
its grammar (`is`, `be`, `gt`, `give`, `through`, `infi`). The original
P3 implementation used angle brackets (`Box<T>`); P3.1 replaced them
with `of` and `to`.

## Surface

```
// Single type parameter
Box of T is {
  value T
}

// Two type parameters
Map of K to V is {
  cap int
  count int
  entries int
}

// Methods — receiver type binds the parameters
Box.set(self ref Box of T, v T) {
  self.value be v
}

Box.get(self Box of T) T {
  give self.value
}

Map.set(self ref Map of K to V, k K, v V) {
  // ...
}

// Use sites — auto-construct (no parens)
xs is List of int
m is Map of str to int
b is Box of int
b.value be 42

// Nested — no parens needed; right-associative
ll is List of List of int
mml is Map of str to List of int
deep is Map of str to Map of int to str
```

## Rules

1. **Two connectives:** `of` introduces a type parameter; `of … to …`
   introduces a key→value pair. There is no `,` in type position
   anywhere in the language.
2. **Maximum two parameters per generic.** `T of A` (1 param) or
   `T of A to B` (2 params). If a 3-param generic is ever needed,
   we'll add a third connective at that point — until then, users
   wrap multiple values in named structs.
3. **No `<>` anywhere.** Hard cut, no transition.
4. **No parens around type arguments.** The grammar is unambiguous
   without them — `of` and `to` are right-associative and only
   chain through the connectives, so `List of List of int` parses
   exactly one way.
5. **Auto-construct on typed declaration.** `x is List of int` with
   no initializer auto-emits the constructor call; same as the
   pre-P3.1 keyword-form `x is list of int` did.
6. **Method head infers parameters from the receiver.** Write
   `Box.set(self ref Box of T, v T)` — not `Box of T.set(...)`.
   The compiler reads the receiver's `of T` clause and binds `T`
   for the rest of the signature and body.

## Mangling (unchanged from P3)

- `Box of int` → `_Box__int`
- `Box of str` → `_Box__str`
- `Map of str to int` → `_Map__str__int`
- `List of List of int` → `_List__List__int`
- `Map of str to List of int` → `_Map__str__List__int`

`to` and `of` both lower to `__` in the mangled name. The mangling
is positional, so `Map of int to str` and `Map of str to int`
produce different symbols (`_Map__int__str` vs `_Map__str__int`).

## Why no commas

A comma in type position introduces real ambiguity at call sites:

```
// Hypothetical — NOT supported
f(Map of K, V, x)
```

Is the third arg `V` or is `V` part of the map type and `x` is the
third? Without commas the question never arises. We pay a small cost
(only 1 and 2 parameter generics) for a major grammar simplification.

## Why no parens

Same reasoning: zero parens in type position means zero precedence
rules to memorise. The right-associative `of` / `to` chaining is
strictly LL(1) parseable and reads naturally:

```
List of Map of int to List of str
//   parses as:
List of (Map of int to (List of str))
```

The parser greedily consumes after `of` / `to` until it hits
something that can't be a type continuation (newline, `is`, `=`,
`(`, `)`, etc).

## What this replaces

`docs/native-stdlib-plan.md` previously used `Pair<K, V>` as the
internal entry type for `Map<K, V>`. P3.1 drops `Pair` from the
plan: `Map of K to V` stores its entries via raw memory layout
(`mem_load` / `mem_store`), not via a separate `Pair` struct.
Users who want a 2-tuple define their own struct.
