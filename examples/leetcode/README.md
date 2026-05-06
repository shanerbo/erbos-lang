# Leetcode Examples Guidelines

## Test Case Requirements

Every leetcode solution must include **at least 20 test cases** covering:

### Easy (5+ cases)
- Minimal input (empty, single element, single char)
- Simple happy path with obvious answer

### Medium (10+ cases)
- Multiple valid inputs of moderate size
- Edge cases: duplicates, boundaries, all-same values
- Cases where answer is at start/end/middle

### Hard (5+ cases)
- Large inputs (stress the algorithm)
- Worst-case scenarios
- Tricky edge cases specific to the problem

## File Structure

```
examples/leetcode/
  problem_name.ptt              # solution code
  problem_name.ptt.expected     # expected output (one value per line)
```

## Solution Format

```
// Leetcode N: Problem Title
// Approach: brief description
// Time: O(?)  Space: O(?)

// ... solution functions ...

spark {
  // Easy
  yell(solve(...))   // case 1
  yell(solve(...))   // case 2
  ...

  // Medium
  yell(solve(...))   // case 6
  ...

  // Hard
  yell(solve(...))   // case 16
  ...
}
```

## Rules

1. Write solution in pure Potato lang — no workarounds for compiler bugs
2. If the compiler can't handle it, fix the compiler first
3. Both brute force and optimal solutions when possible
4. All test cases must pass via `make test` (output validated against .expected)
5. Do not modify solution code to work around compiler issues

## Validating Output

Run all leetcode tests with output validation:
```bash
make test
```

Run a single solution and compare manually:
```bash
./erbos run examples/leetcode/two_sum.ptt > /tmp/actual.txt
diff /tmp/actual.txt examples/leetcode/two_sum.ptt.expected
```

Regenerate `.expected` after confirming correctness:
```bash
./erbos run examples/leetcode/two_sum.ptt > examples/leetcode/two_sum.ptt.expected
```
