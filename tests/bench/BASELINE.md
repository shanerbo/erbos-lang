# P6 Baseline — pre-rewrite Map<str, int> performance

These are the wall-clock timings for `tests/bench/map_bench.ptt` running
through the C-emitted `_map_set` / `_map_get` / `_map_keys` builtins,
captured immediately before the P6 rewrite begins. The `<= 1.2×`
acceptance gate compares the post-rewrite (pure-Potato `Map<str, int>`)
timings against these.

## Workload

- 512 distinct string keys (`"k0"` .. `"k511"`).
- Three phases: set, get-by-key, enumerate-and-lookup.
- Output shape (3 lines): `512 / 915712 / 915712`.
- Build: standard `make` (no debug, no sanitizers).

## Baseline (commit 083adb2, 2026-05-15, M-series Mac)

Wall-clock from `time ./erbos -O<n> run tests/bench/map_bench.ptt`,
median of 5 runs:

| Build | Wall (s) |
|-------|----------|
| `-O0` | 0.48     |
| `-O1` | 0.46     |

(Runs: -O0 = `[0.92, 0.49, 0.46, 0.48, 0.48]` median 0.48; -O1 =
`[0.45, 0.47, 0.47, 0.44, 0.46]` median 0.46. The first -O0 run shows
the typical filesystem-cache warm-up; we discard the outlier.)

The fixed cost (lex/parse/check/codegen/assemble/link/launch) dominates
the user-program time at this N, so the absolute number is mostly the
toolchain. Post-rewrite we expect the same toolchain cost to dominate;
the comparison metric stays valid.

## Methodology

```bash
for n in 0 1; do
  for r in 1 2 3 4 5; do
    /usr/bin/time -p ./erbos -O$n run tests/bench/map_bench.ptt > /dev/null
  done
done
```

Capture the `real` line; pick the median.

## What "1.2×" means in practice

The plan-doc gate is "User-Potato `Map<str, int>` runs within 1.2× of
`_map_*`". At this N the toolchain cost is the bulk of the timing, so
we'll re-measure with a larger N (10k+) once the rewrite lands and
tail-end timing matters more — but we keep this small-N baseline as
the safety net: any post-rewrite timing >2× over baseline at any -O
level fails the gate.
