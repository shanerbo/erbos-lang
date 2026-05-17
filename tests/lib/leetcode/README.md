# Leetcode — Library Files

Each `.ptt` here is a **library** that exposes the algorithm
(`solve()` and any helpers) — no `spark { }`, no entry point. The
matching framework tests in `tests/leetcode/test_<name>.ptt`
import the algorithm and exercise it via assertions.

## Importing

The compiler's import resolver finds these via project-root
walk-up (`potato.toml` lives at the repo root). Test files
write:

```
use tests/lib/leetcode/two_sum

test "basic" {
  assert(two_sum.solve([2, 7], 2, 9) eq 1)
}
```

The path is the file's location relative to the project root.
Same rule for any other shared library elsewhere in the tree.

## File Structure

```
tests/lib/leetcode/
  problem_name.ptt              # library: defines solve() + helpers, no spark
tests/leetcode/
  test_problem_name.ptt         # framework tests; uses `use tests/lib/leetcode/problem_name`
```

## Solution Format

```
// Leetcode N: Problem Title
// Approach: brief description
// Time: O(?)  Space: O(?)

use std/...                     // whatever the algorithm needs

solve(args...) ReturnType {
  // ... algorithm ...
}

helper(...) {
  // ... internal helper if needed ...
}
```

The solution must be a function that **returns** the answer (or
mutates a `ref` argument), not one that prints it. Tests assert
on the return value or the post-call state.

## Test Coverage

Each test file under `tests/leetcode/` should cover:

- **Easy** (3+ cases): minimal / single-element / obvious-happy-path.
- **Medium** (3+ cases): edge cases, duplicates, boundaries.
- **Hard** (1+ case): worst-case, large inputs.
- **Cross-impl** (when applicable): if there's a brute-force
  *and* an optimal version, assert they agree on shared inputs
  (where the answer is unique — note inputs with multiple valid
  answers if both implementations are valid).

## Rules

1. Solution code is pure Potato — no workarounds for compiler bugs.
2. If the compiler can't handle it, fix the compiler first.
3. Both brute force and optimal solutions are welcome when
   they exist (e.g. `two_sum.ptt` + `two_sum_map.ptt`).
4. Tests use `assert(...)`. The framework runner reports
   pass/fail; no stdout-snapshot files.
