# Findings Claims

This file is the implementer-to-auditor handoff for findings that are
ready to be re-checked.

## Ownership

- `Claude` is the only writer of this file.
- `Codex` may read this file at any time, but must not edit it.
- `codex/findings.md` remains the source of truth for actual finding
  state. This file only carries fix claims and audit requests.

## Contract

### Claim state

- `IDLE`: no new audit request is pending.
- `READY_FOR_AUDIT`: Claude claims a set of findings is fixed and is
  asking Codex to verify them.
- `SUPERSEDED`: the previous claim was replaced by a newer one before
  audit happened.

### Timestamp rule

- `Last updated` must use ISO 8601 with numeric UTC offset, for example
  `2026-05-18T02:20:00-07:00`.

### Claim rule

Every audit request must include:

- `Claim ID`
- `Against findings revision`
- `Target commit`
- `Claimed fixed`
- `Last updated`

### Claude workflow

Before asking for audit:

1. Read `codex/findings.md`.
2. Implement against its current `Revision`.
3. Run the relevant tests.
4. Update this file to `READY_FOR_AUDIT`.
5. Name the exact finding IDs claimed fixed.
6. Wait for `codex/findings.md` header `Release action:
   COMMIT_AND_PUSH` before doing the final commit/push for the audited
   stdlib batch.

If another implementation batch starts before audit:

- either keep the same claim current and update `Target commit`, or
- mark the old claim `SUPERSEDED` and create a newer one

### Codex workflow

- Read this file during scheduled sweeps.
- Re-verify findings only when a new `READY_FOR_AUDIT` claim exists and
  its `Claim ID` differs from `codex/findings.md`'s `Last claim audited`.
- Do not treat this file as evidence that a finding is fixed. Only
  `codex/findings.md` can confirm that.

## Header

- `State`: READY_FOR_AUDIT
- `Claim ID`: C-001
- `Against findings revision`: 1
- `Target commit`: dd17f87
- `Claimed fixed`: (no specific finding IDs — see Claim C-001
  body for the stdlib + compiler batch ready for review)
- `Last updated`: 2026-05-18T03:30:00-07:00

## Claim C-001
- State: READY_FOR_AUDIT
- Against findings revision: 1 (no OPEN findings)
- Target commit: dd17f87
- Claimed fixed: stdlib batch has no specific finding IDs to
  reference because the ledger is empty at revision 1; this
  claim asks Codex to audit the new stdlib + compiler work
  for correctness and to add findings if anything fails.
- Last updated: 2026-05-18T03:30:00-07:00
- Verification context:
  - `make test` ends with `All tests passed.` at -O0/-O1/-O2
    across passing examples, leetcode, framework, errors
    (compile-fail and runtime-panic), IR matrix, C runtime,
    path, and leak suites.
  - Stdlib items added / replaced:
    - `std/map.ptt` rewritten as an open-addressed hash table
      with the full `Map of K, V` API (reserve / cap / empty /
      set / get / try_get / has / remove / clear / keys /
      values). `get` panics on missing key; `try_get` returns
      `Option of V`. K=int hashed by Knuth multiplicative
      (`int.hash` in `std/math`), K=String by the existing
      djb2-shaped `String.hash`. Deep-clones K and V on
      insert.
    - `std/set.ptt` (`Set of T`) — same machinery, no values,
      with `union` / `intersect` / `difference`.
    - `std/pool.ptt` (`Pool of T`) — append-only arena with
      slot reuse via a free list. No generational handles in
      this version.
    - `std/path.ptt` — pure-string path helpers as a
      free-function module (`path.join` / `path.basename` /
      `path.dirname` / `path.extension` / `path.normalize` /
      `path.is_absolute`).
  - Compiler / runtime root-cause fixes that landed alongside:
    - `arr[i] be now src` / `arr[i] be rep src` in
      NODE_INDEX_ASSIGN (parser, checker, irgen, monomorph
      clone case which had been missing entirely). Lets
      stdlib containers transfer or deep-clone heap-shaped
      values into array slots.
    - `_heap_alloc` zero-fills the returned block on both the
      free-list-reuse and bump paths. Without this, fresh
      `array of byte` allocations saw stale heap bytes and
      the hash-table `states` array reported `1 (FULL)` on
      buckets that had just been allocated empty.
    - `_heap_free` rounds the requested free-size up to a
      16-byte multiple before threading the block onto the
      free list. Freeing a sub-16-byte block (e.g.
      `array of byte with cap 8`) was corrupting adjacent
      free-list metadata.
    - Short-circuit `and` / `or` in `compiler/irgen.c`. The
      previous emit was straight-line; user code's
      `i gt 0 and arr.byte_at(i - 1) eq 47` patterns
      panicked on the right operand even though the left
      already refused the access.
    - `_String_yell` empty-String guard. `yell(String())` was
      crashing on the null `data` pointer; now it skips the
      buffer write and emits the trailing newline.
    - Two-guard if-parse fix in
      `compiler/parser.c::parse_if_continuation`. Two
      consecutive `cond ?{ ... }` statements separated by a
      newline were being attached as a single if/else-if
      chain. Set algebra silently dropped half its work.
      Now: a newline gap terminates the if.
  - New compile-fail / runtime-panic tests:
    - `tests/errors/map_get_missing_key_panics.ptt`
    - `tests/errors/pool_get_stale_panics.ptt`
  - New framework test files:
    - `tests/test_map_methods.ptt` (rewritten to cover full API)
    - `tests/test_set.ptt`
    - `tests/test_pool.ptt`
    - `tests/test_path.ptt`

## Claim Template

```md
## Claim C-###
- State: READY_FOR_AUDIT
- Against findings revision: <N>
- Target commit: <commit hash or working tree note>
- Claimed fixed: F-001, F-002
- Last updated: <ISO 8601 timestamp>
- Verification context:
  - targeted tests run
  - important implementation notes
```

## Change Log

- Claim file created; no pending audit requests yet.
