# Erbos Keywords

| Keyword | Purpose | Example |
|---------|---------|---------|
| `spark` | program entry point | `spark { }` |
| `is` | variable declaration | `x is 10` |
| `nomut` | immutable variable | `nomut x is 10` |
| `give` | return value from function | `give x + y` |
| `through` | range/collection loop | `through (i from 0 to 10 by 1) { }` |
| `from` | loop range start | see `through` |
| `to` | loop range end | see `through` |
| `by` | loop range step | see `through` |
| `in` | collection iteration | `through (x in list) { }` |
| `infi` | while/infinite loop | `infi (cond) { }` or `infi { }` |
| `stop` | break out of loop | `stop` |
| `skip` | continue to next iteration | `skip` |
| `nah` | else block | `} nah { }` |
| `and` | logical AND | `x > 0 and x < 10` |
| `or` | logical OR | `x == 0 or x == 1` |
| `not` | logical NOT | `not done` |
| `true` | boolean true | `x is true` |
| `false` | boolean false | `x is false` |
| `list` | list type annotation | `list of int` |
| `map` | map type annotation | `map of str to int` |
| `of` | type parameter | see `list`/`map` |
| `task` | concurrency handle | `t is task` |

## Symbols

| Symbol | Meaning |
|--------|---------|
| `?{` | if (condition before `?`, block after `{`) |
| `=` | reassignment |
| `{ }` | block / scope |
| `( )` | function params, loop header |
| `[ ]` | list literal, index access |
| `.` | field access, method call |
| `//` | single-line comment |
| `/* */` | multi-line comment |
