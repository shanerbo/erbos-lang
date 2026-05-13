# Potato Keywords 🥔

| Keyword | Purpose | Example |
|---------|---------|---------|
| `spark` | program entry point | `spark { }` |
| `is` | variable declaration | `x is 10` |
| `be` | reassignment | `x be 42` |
| `nomut` | immutable variable | `nomut x is 10` |
| `give` | return value from function | `give x + y` |
| `now` | move ownership | `b is now a` |
| `rep` | clone (shallow copy) | `b is rep a` |
| `ref` | mutable borrow (param) | `func(p ref int) { }` |
| `through` | range/collection loop | `through (i from 0 to 10 by 1) { }` |
| `from` | loop range start | see `through` |
| `to` | loop range end | see `through` |
| `by` | loop range step | see `through` |
| `in` | collection iteration | `through (x in list) { }` |
| `infi` | while/infinite loop | `infi (cond) { }` or `infi { }` |
| `stop` | break out of loop | `stop` |
| `skip` | continue to next iteration | `skip` |
| `nah` | else block | `} nah { }` |
| `and` | logical AND | `x gt 0 and x lt 10` |
| `or` | logical OR | `x eq 0 or x eq 1` |
| `not` | logical NOT | `not done` |
| `eq` | equal | `x eq 5` |
| `ne` | not equal | `x ne 0` |
| `gt` | greater than | `x gt 10` |
| `lt` | less than | `x lt 5` |
| `ge` | greater or equal | `x ge 0` |
| `le` | less or equal | `x le 100` |
| `mod` | modulo | `x mod 3` |
| `true` / `false` | booleans | `x is true` |
| `list` | list type/constructor | `nums is list of int` |
| `map` | map type/constructor | `m is map of str to int` |
| `of` | type parameter | `list of int` |
| `task` | concurrency handle | `t is task()` |

## Symbols

| Symbol | Meaning |
|--------|---------|
| `?{` | if (condition before `?`, block after `{`) |
| `{ }` | block / scope / RAII lifetime |
| `( )` | function params, loop header |
| `[ ]` | list literal, index access |
| `.` | field access, method call |
| `+` `-` `*` `/` | arithmetic |
| `%` | modulo (same as `mod`) |
| `>` `<` `>=` `<=` `==` `!=` | comparisons (same as word forms) |
| `//` | single-line comment |
| `/* */` | multi-line comment |

| `match` | pattern match on enum | `match r { Ok(v) => ... }` |
| `imap` | int-key map type | `m is imap of int to int` |
