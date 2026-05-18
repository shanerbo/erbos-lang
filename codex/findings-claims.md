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
6. Wait for `codex/findings.md` header `Conclusion: ALL_CLEAR`
   together with `Release action: COMMIT_AND_PUSH` before doing the
   final commit/push for the audited implementation batch.

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

- `State`: IDLE
- `Claim ID`: C-002
- `Against findings revision`: 4
- `Target commit`: 1935bb3 + working tree
- `Claimed fixed`: F-001 (accepted; no pending audit request)
- `Last updated`: 2026-05-18T13:11:34-07:00

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

## Claim C-002
- State: READY_FOR_AUDIT
- Against findings revision: 3
- Target commit: 1935bb3 + working tree (uncommitted; Codex
  may verify against `git diff` for the listed files)
- Claimed fixed: F-001
- Last updated: 2026-05-18T20:35:00+00:00
- Verification context:
  - `make test` ends with `All tests passed.` from a clean
    rebuild on the working tree.
  - `tests/test_heap_slot_drop.ptt` (18 cases including three
    aliasing regression cases) passes at `-O0`, `-O1`, `-O2`.
  - `tests/leaks/heap_slot_drop_emits_drop.ptt` IR-static check
    confirms `bl _drop_String` appears in
    `_List__String_set`, `_Set__String_add`, and
    `_Map__String__String_set` before the slot's `str` of the
    new pointer.
- Root-cause fix:
  - Compiler change at the index-assign lowering layer.
    `compiler/checker.c`'s `NODE_INDEX_ASSIGN` now stores the
    array's element struct name in
    `index_assign.elem_struct_name` whenever the element type
    resolves to a heap-shaped struct in this program.
    `compiler/irgen.c`'s `NODE_INDEX_ASSIGN` reads that field
    and, when `is_move` or `is_rep` is set and the element is
    heap-shaped, null-guards the slot's prior pointer and
    calls `_drop_<elem_struct_name>` before storing the new
    pointer. Plain `arr[i] be src` keeps the legacy raw-store
    semantics so shift/swap loops are not affected.
    `compiler/monomorph.c`'s `clone_node` for
    `NODE_INDEX_ASSIGN` carries the new field through every
    instantiation.
    `compiler/ast.h` declares the new field on
    `index_assign`.
  - Stdlib change: `std/list.ptt::List.set` body switched to
    the clone-then-transfer pattern already in use by
    `Map.set` and `Set.add`:
        v_clone is rep v
        self.data[i] be now v_clone
    The deep-clone is the alias-safety guarantee — caller can
    pass the slot's own current value (`v is xs.get(0); xs.set(0, v)`)
    and the slot still ends up holding an independent block.
    For primitive T, `is rep` collapses to a value copy and
    `be now` collapses to a plain store, so the perf cost is
    nil. The new compiler drop-before-overwrite logic then
    drops the slot's previous occupant before storing the
    clone. Pool's `Pool.set` and the `Pool.insert` reuse path
    both delegate to `List.set`, so the same fix flows
    transitively.
  - Stdlib unchanged but now correct: `std/map.ptt::Map.set`
    (update-existing branch and tombstone-reuse insert) and
    `std/set.ptt::Set.add` already used `arr[i] be now <clone>`;
    they did not drop the previous occupant before because
    the compiler-side drop did not exist. The new compiler
    logic makes them ownership-correct without changing those
    files.
- Files changed:
  - `compiler/ast.h`
  - `compiler/checker.c`
  - `compiler/irgen.c`
  - `compiler/monomorph.c`
  - `std/list.ptt`
  - `std/STDLIB_CHECKLIST.md`
  - `Makefile` (clean target + new leak check)
  - `tests/test_heap_slot_drop.ptt` (new)
  - `tests/leaks/heap_slot_drop_emits_drop.ptt` (new)
- Tests added (per F-001 required-test list):
  - heap-shaped `List.set` replacement stress: covered by
    `List.set replacement stress over the same slot` and
    `List.set on multiple slots after grow keeps content`.
  - `Pool of String` repeated set on one live id plus
    remove+reuse cycles: covered by
    `Pool.set replaces heap-shaped value at a live id` and
    `Pool remove + reuse cycles a slot for a heap-shaped T`
    plus `Pool clear and reinsert heap-shaped values`.
  - `Set of String` remove+re-add into tombstoned buckets and
    clear+reinsert cycles: covered by
    `Set remove + readd into tombstoned bucket drops prior`,
    `Set clear + reinsert cycles`, and
    `Set add stress with collisions exercises tombstone reuse`.
  - `Map of String, String` matching regression: covered by
    `Map.set update-in-place drops previous value`,
    `Map remove + readd into tombstoned bucket`,
    `Map.clear + reinsert cycles`, and
    `Map of String, String survives capacity doublings`.
  - Primitive-T fast path is preserved: `List.set with
    primitive T stays raw-store`. The corresponding IR-static
    check lives in the leaks suite (no `_drop_<...>` symbol
    for primitive element types).
  - Aliasing safety: `List.set self-alias` and
    `List.set cross-slot alias` lock down the convention that
    `List.set(i, v)` deep-clones v before dropping slot[i],
    so v can be the slot's own current value or a value from
    elsewhere in the same list without a use-after-free.
    `Map.set self-alias` is a sentinel that the same
    convention in Map.set survives.
  - IR-static gate that locks the fix in place:
    `tests/leaks/heap_slot_drop_emits_drop.ptt` plus the new
    Makefile `test-leaks` stanza asserts
    `bl _drop_String` is emitted in `_List__String_set`,
    `_Set__String_add`, and `_Map__String__String_set` ahead
    of the slot store.
- Out of F-001 scope (deliberately not changed):
  - Heap-shaped values still leak when the parent
    `List of String` / `Set of String` / `Map of String, String`
    is dropped at scope end, because `_drop_<X>`'s array path
    frees the data buffer without iterating elements. F-001's
    text and required tests are scoped to slot-overwrite leaks
    on reuse, not parent-drop element leaks. If Codex wants
    parent-drop element drop in the same batch, that is a
    separate compiler change that should be claimed as a new
    fix once F-001 is verified.
  - `List.insert`'s shift-loop is unchanged. The shift uses
    plain `be` (alias-shuffle), and the final write at `[i]`
    is also plain `be` because the slot at `[i]` is an alias
    of the value now at `[i+1]` after the shift; a drop there
    would double-free. Heap-shaped insert ownership matches
    the existing pattern (caller passes ownership; List acquires
    it via the final raw store; parameter local is not heap-
    marked so no scope-end double-free).

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
- Header reset to `IDLE` after claim `C-002` was consumed and accepted;
  future claims should create a new `Claim ID` for the next repo-wide
  audit batch.
- Claim C-001 submitted, audited, and rejected for release;
  F-001 was opened against the slot-overwrite leak class.
- Claim C-002 submitted: F-001 fix (compiler + stdlib +
  comprehensive regression tests + IR-static gate). All-level
  `make test` green from a clean rebuild.
- Claim C-002 strengthened: `List.set` switched to the
  clone-then-transfer pattern (`v_clone is rep v; self.data[i] be now v_clone`)
  to lock down alias safety; added three aliasing regression
  tests; full suite still green.
