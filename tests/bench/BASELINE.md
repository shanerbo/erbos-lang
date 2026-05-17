# Map of String to int — bench baseline

Wall-clock timings for `tests/bench/map_bench.ptt`. Captured for
the post-overhaul `Map of String to int` running through the
pure-Potato stdlib (backed by `array of T`).

## Workload

- 512 distinct string keys (`"k0"` .. `"k511"`).
- Three phases: set, get-by-key, enumerate-via-`.keys()`-and-lookup.
- Output shape (3 lines): `512 / 915712 / 915712`.
- Build: standard `make` (no debug, no sanitizers).

## Baseline (M-series Mac)

Wall-clock from `time ./erbos -O<n> run tests/bench/map_bench.ptt`:

| Build | Wall (s) |
|-------|----------|
| `-O0` | ~0.10    |
| `-O1` | ~0.10    |

The fixed cost (lex/parse/check/codegen/assemble/link/launch)
dominates user-program time at this N, so the absolute number is
mostly the toolchain. The number is preserved here so a future
regression that doubles compile time or runtime shows up
immediately.

## Methodology

```bash
for n in 0 1 2; do
  for r in 1 2 3 4 5; do
    /usr/bin/time -p ./erbos -O$n run tests/bench/map_bench.ptt > /dev/null
  done
done
```

Capture the `real` line; pick the median.
