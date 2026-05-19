# stdlib bench baselines

Wall-clock timings for the bench programs in `tests/bench/`. Each
file is a stand-alone `spark` program; the suite is run manually
(no `make` target) to avoid coupling regressions to compile-time
fixed cost.

## `map_bench.ptt` — `Map of String, int`

- 512 distinct string keys (`"k0"` .. `"k511"`).
- Three phases: set, get-by-key, enumerate-via-`.keys()`-and-lookup.
- Output shape (3 lines): `512 / 915712 / 915712`.
- Build: standard `make` (no debug, no sanitizers).

| Build | Wall (s) |
|-------|----------|
| `-O0` | ~0.10    |
| `-O1` | ~0.10    |

## `map_keys_values_bench.ptt` — `Map.keys` / `Map.values` materialization

F-006 reference workload. 4096-entry `Map of int, int`; phases
materialize the key list and value list and sum each. Pre-fix the
result `List of K` / `List of V` grew through ~9 cap doublings;
post-fix the materializer reserves `self.count` and the path is
one allocation + one loop.

- Output shape (3 lines): `4096 / 8386560 / 58705920`.

## `set_materializers_bench.ptt` — `Set.values` / `Set.intersect` / `Set.difference`

F-006 reference workload. Two 2048-element `Set of int`s with
`[1024..2048)` overlap. Phases enumerate `a.values()`, then
`a.intersect(b).values()`, then `a.difference(b).values()`,
summing each. Pre-fix the intersect / difference output Sets
rebuild-and-rehash through every 70% load doubling.

- Output shape (7 lines): `2048 / 2048 / 2096128 / 1024 / 1572352 / 1024 / 523776`.

## `string_builder_push_int_bench.ptt` — `StringBuilder.push_int` direct emit

F-008 reference workload. Append `0..10000` to a
`StringBuilder` two ways: (1) direct `push_int` (post-fix
direct-digit emit), (2) `push_string(i.to_string())` (legacy
intermediate-String path). Pre-fix both phases produced the
same allocation profile (one heap `String` per call); post-fix
phase 1 has zero per-call heap allocations beyond the builder's
own amortised grow. The bench yells the length of each phase's
output plus an `identical` / `MISMATCH` line so any silent
divergence in formatting shape surfaces immediately.

- Output shape (3 lines): `38890 / 38890 / identical`.

## `string_search_bench.ptt` — index_of / split / replace

F-007 reference workload. Builds a 5000-field comma-separated
record (~28890 bytes), then exercises:
  1. an `index_of` of a 6-byte needle in a 3006-byte
     repeated-prefix haystack (forces the general path through
     `str_find`);
  2. a single-byte `split` on the comma separator (4999 hits
     through the fast path), summing field lengths;
  3. a `replace` of every `,` with `|` followed by another
     single-byte `split` on `|`. Both replace's count pass and
     replace's copy pass route through `str_find`'s single-byte
     fast path; the post-split sum must match phase 2's sum.

- Output shape (6 lines): `28889 / 3000 / 5000 / 23890 / 5000 / 23890`.

## `hash_probe_bench.ptt` — Map probe quality on adversarial keys

F-005 reference workload. Stresses `Map of int, int` with 2048
adversarial keys (multiples of 8) — the F-005 worst case under
the pre-fix low-bit modulo. Phases:
  1. set: insert N=2048 multiples of 8.
  2. get-hit: read every key back; sum values.
  3. get-miss: try_get of N non-member keys (odd offsets).
  4. tombstone-reuse: remove half the keys, re-insert a
     different adversarial set, lookup-hit verify.

Pre-fix, sequentially-related keys that share low bits all
collided on the same bucket regardless of cap, turning every
lookup into a near-full-table linear scan. Post-fix the
bucket index folds the high-bit window of `h` into the low
bits, restoring full-table distribution.

- Output shape (6 lines): `2048 / 14672896 / 2048 / 3072 / 49 / 0`.

## `hash_churn_bench.ptt` — Map sustained churn

F-010 reference workload. 10 cycles of (insert 1024 + remove
1024) against a `Map of int, int` that already holds 1024
unrelated baseline entries. Pre-F-010 each cycle accumulated
~1024 tombstones in the table; over 10 cycles the cluster of
tombstones could reach ~10000, and every probe in subsequent
phases had to walk past them until the next resize/clear.
Post-F-010 backshift deletion heals the chain in place — the
post-cycle table is byte-equivalent to one that never held the
churned keys, so probe quality stays steady regardless of
churn volume.

- Output shape (3 lines): `1024 / 1024 / 8904192`.

## `hash_string_bench.ptt` — int and String Map hasher quality

F-010 reference workload. Two parallel workloads — `Map of
int, int` and `Map of String, int` — each running fill / hit /
miss phases at N=1024, plus a 4096-element resize-stress for
the int side. Pre-F-010 `int.hash` was a 32-bit Knuth
multiplier (weak high-bit mixing on 64-bit inputs) and
`String.hash` was plain djb2 (no avalanche). Post-F-010 both
hashers carry a 64-bit Knuth-multiplicative finalizer, so the
output bits are spread across all 64 positions; combined with
the F-005 high-bit-into-low fold in `map_probe_index`, the
effective avalanche is two-stage. The bench prints lengths and
sums for both sides so hasher-quality changes are measurable
rather than assumed.

- Output shape (7 lines): `1024 / 523776 / 1024 / 5120 / 1024 / 523776 / 1024`.

## Baseline (M-series Mac)

The fixed cost (lex/parse/check/codegen/assemble/link/launch)
dominates user-program time at these N, so the absolute number
is mostly the toolchain. Numbers are preserved as-of-landing so a
future regression that doubles compile time or runtime shows up
immediately.

## Methodology

```bash
for f in tests/bench/*.ptt; do
  for n in 0 1 2; do
    for r in 1 2 3 4 5; do
      /usr/bin/time -p ./erbos -O$n run "$f" > /dev/null
    done
  done
done
```

Capture the `real` line; pick the median per (file, -O level).
