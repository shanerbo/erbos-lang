# Language Law: Type Expressions And Value Formation

This document is the source of truth for the value-formation
grammar — type expressions, value formation, and enum factories.
The parser, checker, and tests enforce exactly what is written
here. If code and this document disagree, code wins; update the
document.

## Terms

- **Type expression**: names a type.
- **Value formation expression**: forms a value of a type.
- **Zero-value formation**: `TypeExpr()`.
- **Named-field formation**: `TypeExpr(field is value, ...)`.
- **Enum factory formation**: `none of T ()`, `some of T (v)`,
  `ok of T, E (v)`, `err of T, E (e)`.

Do not describe these as OO "constructors".

## Law

1. A type expression names a type only.
2. A bare type expression is never a value.
3. A value is formed only by:
   - `TypeExpr()`
   - `TypeExpr(field is value, ...)`
   - enum factory formation

## Type Expressions

These name types only:

```potato
Foo
int
bool
String
List of int
Map of String, int
Box of Foo
Result of User, String
```

They are valid in:

- variable type annotations
- function parameter types
- function return types
- struct field types
- method receiver types

## Zero-Value Formation

Always use `()`:

```potato
Foo()
List of int()
Map of String, int()
Box of Foo()
```

## Named-Field Formation

Always use `(field is value, ...)`:

```potato
Foo(x is 1, y is 2)
Box of int(value is 7)
Box of Foo(value is Foo(x is 1, y is 2))
```

Rules:

- every declared field must appear exactly once
- no positional field formation
- no mixed named and positional args

## Generic Rule

Generic types follow the same value-formation rule as non-generic types.

Type expressions:

```potato
List of Foo
Map of String, int
Box of Foo
```

Zero-value formation:

```potato
List of Foo()
Map of String, int()
Box of Foo()
```

Named-field formation:

```potato
Box of Foo(value is Foo(x is 1, y is 2))
```

## Enum Rule

Keep only enum factory formation:

```potato
none of int ()
some of int (7)
ok of int, String (1)
err of int, String ("bad")
```

Remove type-receiver enum value formation:

```potato
Option of int .None()
Option of int .Some(7)
Result of int, String .Ok(1)
Result of int, String .Err("bad")
```

Why both type args exist in `err of int, String ("bad")`:

- the formed value has type `Result of int, String`
- `int` is the `Ok` payload type
- `String` is the `Err` payload type
- `"bad"` is the actual `Err` payload

## Accepted Forms

These must compile:

```potato
Foo()
Foo(x is 1, y is 2)

List of int()
Map of String, int()
Box of Foo()
Box of Foo(value is Foo(x is 1, y is 2))

none of int ()
some of int (7)
ok of int, String (1)
err of int, String ("bad")
```

## Rejected Forms

These must be compile errors:

```potato
Foo
List of int
Map of String, int
Box of Foo

Foo(1, 2)
Box of int(7)

Wrap(item is Foo)
Wrap(items is List of Foo)

List of Foo(x is 1, y is 2)

Option of int .None()
Option of int .Some(7)
Result of int, String .Ok(1)
Result of int, String .Err("bad")
```

## Implementation Checklist

- [x] Remove all bare-type auto-value formation from the parser.
- [x] Remove generic auto-value formation from bare `Type of ...`.
- [x] Keep `TypeExpr()` parsing for zero-value formation.
- [x] Keep `TypeExpr(field is value, ...)` parsing for named-field formation.
- [x] Ensure type arguments after `of` are parsed only as types, never inner values.
- [x] Remove enum `Type.variant(...)` value formation.
  - Sub: parser keeps the AST shape so that the four stdlib
    factory bodies (`none`/`some`/`ok`/`err`) can still emit
    enum variant materialization. The checker rejects every
    other source position with a teaching diagnostic.
- [x] Treat bare type expressions in value position as checker errors.
- [x] Keep strict named-field validation:
  - [x] every field exactly once
  - [x] no positional args
  - [x] no mixed named/positional args
- [x] Reject nested bare type values like `Wrap(item is Foo)`.
- [x] Keep enum factories:
  - [x] `none of T ()`
  - [x] `some of T (v)`
  - [x] `ok of T, E (v)`
  - [x] `err of T, E (e)`
- [x] Add compile-pass tests for:
  - [x] `Foo()`
  - [x] `Foo(x is 1, y is 2)`
  - [x] `List of int()`
  - [x] `Map of String, int()`
  - [x] nested `Wrap(item is Foo())`
  - [x] enum factory formation
- [x] Add compile-fail tests for:
  - [x] bare `Foo`
  - [x] bare `List of int`
  - [x] bare `Map of String, int`
  - [x] `Wrap(item is Foo)`
  - [x] `Wrap(items is List of Foo)`
  - [x] `List of Foo(x is 1, y is 2)`
  - [x] enum `Type.variant(...)` forms
- [x] Update docs:
  - [x] `README.md`
  - [x] `docs/language-guide.md`
  - [x] stdlib comments/examples that describe old forms
  - [x] `std/STDLIB_CHECKLIST.md` (target API for unimplemented modules + construction-rules section)
  - [x] `std/queue.ptt` and `std/list.ptt` doc-comments
  - [x] historical-log header on `docs/design-decisions.md`;
        in-block examples that read as live guidance updated to the
        new grammar
- [x] Rewrite existing tests/examples/docs to the new grammar only.
- [x] Remove dual syntax; do not keep compatibility shims.
- [x] Do not claim completion until `make test` passes.

## Acceptance Notes For Claude

- Report exact files changed.
- Report syntax removed.
- Report syntax kept.
- Report new tests added.
- Report final `make test` result.
