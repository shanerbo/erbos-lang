# Findings Ledger

This file is the live auditor-to-implementer handoff for repo findings.

## Ownership

- `Codex` is the auditor and the only writer of this file.
- `Claude` may read this file at any time, but must not edit it.
- `Claude` must communicate fix claims through
  `codex/findings-claims.md`, not by rewriting this ledger.

## Contract

### Auditor state

The header below tells the implementer whether an audit pass is still
in motion.

- `ACTIVE`: auditor may still add, split, or re-rank findings in the
  current pass.
- `QUIESCENT`: current pass is complete; Claude may treat the current
  revision as the full set of known findings from that pass.

### Revision rule

- Every audit pass must bump `Revision`.
- `Last updated` must change on every write.
- Timestamps must use ISO 8601 with numeric UTC offset, for example
  `2026-05-18T02:15:00-07:00`.
- `Audited commit` should name the repo commit or branch head the pass
  was based on when practical.
- `Last claim audited` should record the most recent implementer claim
  that Codex fully processed.

### Finding lifecycle

Each finding must use one of these states:

- `OPEN`: auditor confirmed the issue and it still stands.
- `FIX CLAIMED`: implementer says it is fixed, but auditor has not yet
  verified it.
- `VERIFIED`: auditor re-checked the fix and accepts it.
- `CLOSED`: historical terminal state after the fix is verified and the
  finding is no longer active.

Only Codex moves a finding into `VERIFIED` or `CLOSED`.

### Claim handshake

- `Claude` writes fix claims into `codex/findings-claims.md`.
- A claim is audit-ready only when its header state is
  `READY_FOR_AUDIT`.
- A claim is pending only when `codex/findings-claims.md` header
  `State` is `READY_FOR_AUDIT` and its `Claim ID` differs from this
  file's header `Last claim audited`.
- If `codex/findings-claims.md` still says `READY_FOR_AUDIT` but its
  `Claim ID` already matches this file's header `Last claim audited`,
  that claim is already consumed and must not be treated as still
  pending.
- Codex verifies findings only after a new ready claim exists.
- Codex records the consumed claim ID in this file's header as
  `Last claim audited`.
- If no new ready claim exists, periodic sweeps should not churn the
  status of existing findings mid-implementation.

### Release handoff

- `Codex` is the only authority that may authorize the final
  repo-work `commit and push` handoff for the current audited batch.
- The findings header also carries a separate `Conclusion` field.
- `ALL_CLEAR` means there are no active known findings for the current
  audited batch and no in-flight audit still open against that batch.
- `FINDINGS_OPEN` means at least one finding is still active, an audit
  is in flight, or the current batch has not yet been cleared by Codex.
- `Conclusion` and `Release action` are related but not identical:
  `Conclusion` answers whether the audited batch is clear of known
  findings, while `Release action` answers whether Claude may land it.
- The findings header carries this as `Release action`.
- `HOLD` means Claude must keep implementing or wait for audit.
- `COMMIT_AND_PUSH` means Codex has verified the current claim, the
  relevant findings are accepted, and Claude may create the intended
  commit and push it.
- When practical, Codex should also add a short `Release note`
  describing the approved scope.

### Conclusion transition rule

- On audit start, Codex should set `Conclusion: FINDINGS_OPEN`.
- After consuming a new ready claim:
  - if any finding remains `OPEN` or `FIX CLAIMED`, keep
    `Conclusion: FINDINGS_OPEN`
  - if zero active findings remain and the audit is complete, set
    `Conclusion: ALL_CLEAR`
- Any newly-added finding must flip `Conclusion` back to
  `FINDINGS_OPEN`.
- If no new ready claim exists, periodic sweeps should leave
  `Conclusion` unchanged.

### Claude consumption rule

Claude should:

0. Run the custom `/findings` worker command for this repo, typically
   under Claude's built-in scheduler as `/loop 5m /findings`.
1. Read this file before each substantial implementation batch.
2. Treat `OPEN` findings as mandatory work.
3. Treat `FIX CLAIMED` as not yet accepted.
4. Avoid claiming a phase complete while any relevant finding is still
   `OPEN` or `FIX CLAIMED`.
5. Re-check this file after each implementation batch in case a newer
   `Revision` exists.
6. When you want verification, update `codex/findings-claims.md`
   instead of touching this file.
7. After Codex consumes your claim, do not treat the same `Claim ID`
   in `codex/findings-claims.md` as still pending.
8. After Codex consumes your claim, rewrite
   `codex/findings-claims.md` so its header no longer advertises that
   consumed claim as pending before you start the next unrelated batch.
9. Treat `Conclusion: ALL_CLEAR` as the explicit signal that the
   current audited batch has no active known findings and no in-flight
   code audit still open against it.
10. Do not commit or push an audited implementation batch unless this file's
   header says both `Conclusion: ALL_CLEAR` and
   `Release action: COMMIT_AND_PUSH`.

### Scope rule

This file is for:

- compiler, runtime, stdlib, docs, tests, or repo-level bugs exposed by
  active implementation work
- docs/code contradictions that block truthful completion claims
- missing or weak regression coverage
- design-level correctness issues that must be fixed at the root

This file is not for:

- vague suggestions
- style-only nits
- speculative concerns without verification

## Header

- `State`: QUIESCENT
- `Revision`: 12
- `Last updated`: 2026-05-18T18:07:19-07:00
- `Audited commit`: c8cdd04 + working tree
- `Last claim audited`: C-007
- `Conclusion`: FINDINGS_OPEN
- `Release action`: HOLD
- `Release note`: Claim C-007 accepted for F-008; stdlib optimization findings F-005 and F-007 still keep the batch on HOLD.

## Active Findings

### F-005
- State: OPEN
- Severity: P2
- Area: stdlib | bench
- Opened at: 2026-05-18T17:23:25-07:00
- Last reviewed at: 2026-05-18T17:23:25-07:00
- Audited commit: c8cdd04 + working tree
- Evidence:
  - [std/map.ptt](/Users/erbos/erbos-lang/std/map.ptt:11)
  - [std/map.ptt](/Users/erbos/erbos-lang/std/map.ptt:52)
  - [std/map.ptt](/Users/erbos/erbos-lang/std/map.ptt:117)
  - [std/map.ptt](/Users/erbos/erbos-lang/std/map.ptt:140)
  - [std/set.ptt](/Users/erbos/erbos-lang/std/set.ptt:28)
  - [std/set.ptt](/Users/erbos/erbos-lang/std/set.ptt:58)
  - [std/set.ptt](/Users/erbos/erbos-lang/std/set.ptt:84)
  - [tests/bench/map_bench.ptt](/Users/erbos/erbos-lang/tests/bench/map_bench.ptt:31)
- Problem:
  The hash containers still use toy-grade probe math. `Map` and `Set`
  reduce the key hash with a plain `h mod cap`, so power-of-two bucket
  counts depend only on the low `log2(cap)` bits of the hash. On top of
  that, every probe step recomputes `map_probe_index(start + off, cap)` /
  `set_probe_index(start + off, cap)`, which means a modulo/division on
  every collision walk, every miss, and every rehash insertion. Industrial
  open-addressed tables mix once into a start bucket, then advance with a
  branch-or-mask step, not a fresh modular reduction at each slot.
- Required fix:
  Replace the current bucket reduction and probe walk with a real
  hash-table path shared by `Map` and `Set`: stronger bucket selection
  than low-bit modulo, plus a linear probe cursor that increments and
  wraps from the already-mixed start bucket instead of re-dividing on each
  step.
- Required tests:
  - retain all current `Map` / `Set` behavior tests unchanged
  - add adversarial collision regressions for patterned integer keys
    (same low bits, different high bits)
  - add benchmark coverage for `Map.get` / `Map.set` probe-heavy paths
    and matching `Set.has` / `Set.add` paths
- Verification notes:
  Opened from code inspection. No user-visible semantic failure is
  claimed here; this is a verified algorithmic/perf gap in the current
  stdlib implementation.

### F-007
- State: OPEN
- Severity: P2
- Area: stdlib | tests | bench
- Opened at: 2026-05-18T17:23:25-07:00
- Last reviewed at: 2026-05-18T17:23:25-07:00
- Audited commit: c8cdd04 + working tree
- Evidence:
  - [std/string.ptt](/Users/erbos/erbos-lang/std/string.ptt:114)
  - [std/string.ptt](/Users/erbos/erbos-lang/std/string.ptt:186)
  - [std/string.ptt](/Users/erbos/erbos-lang/std/string.ptt:206)
  - [tests/test_string_extended.ptt](/Users/erbos/erbos-lang/tests/test_string_extended.ptt:13)
  - [tests/test_string_extended.ptt](/Users/erbos/erbos-lang/tests/test_string_extended.ptt:43)
  - [tests/test_string_extended.ptt](/Users/erbos/erbos-lang/tests/test_string_extended.ptt:67)
- Problem:
  The String search pipeline is still toy-grade. `index_of` linearly
  tries `str_match_at` at every position, `contains` is just a wrapper
  around that, `split` repeats the same scan while allocating slices, and
  `replace` does one full scan to count matches and then another full scan
  to copy. On repeated-prefix needles or large haystacks this is the
  classic O(n*m) path, with duplicated scanning logic spread across three
  public APIs.
- Required fix:
  Introduce a shared search primitive for String and route `index_of`,
  `contains`, `split`, and `replace` through it. At minimum add a
  single-byte fast path; ideally make the general substring scan a
  one-source implementation so these APIs stop hand-rolling repeated
  quadratic scans independently.
- Required tests:
  - retain the existing correctness coverage in
    `tests/test_string_extended.ptt`
  - add benchmark coverage for long haystacks, repeated-prefix needles,
    and single-byte separators/replacements
  - add stress tests that exercise large repeated `split` / `replace`
    workloads without changing semantics
- Verification notes:
  Opened from code inspection. This is a verified algorithmic gap, not a
  claim of incorrect String behavior.

## Closed Findings

### F-008
- State: CLOSED
- Severity: P2
- Area: stdlib | tests | bench
- Opened at: 2026-05-18T17:23:25-07:00
- Last reviewed at: 2026-05-18T18:07:19-07:00
- Audited commit: c8cdd04 + working tree
- Evidence:
  - [std/string_builder.ptt](/Users/erbos/erbos-lang/std/string_builder.ptt:78)
  - [std/string.ptt](/Users/erbos/erbos-lang/std/string.ptt:308)
  - [tests/test_string_builder.ptt](/Users/erbos/erbos-lang/tests/test_string_builder.ptt:18)
  - [tests/bench/string_builder_push_int_bench.ptt](/Users/erbos/erbos-lang/tests/bench/string_builder_push_int_bench.ptt:1)
  - [tests/bench/BASELINE.md](/Users/erbos/erbos-lang/tests/bench/BASELINE.md:1)
- Problem:
  `StringBuilder.push_int` still formats through a temporary owned
  `String`: it calls `int.to_string()`, allocates a heap buffer for that
  String, then immediately copies those bytes back into the builder's
  backing array. That is acceptable for toy usage, but it is not an
  industrial StringBuilder path because every integer append pays an
  avoidable allocation + copy + drop cycle.
- Required fix:
  Write decimal digits directly into the builder. Reuse the existing
  sign/reverse logic from `int.to_string`, but emit into the builder's
  backing buffer (or a tiny stack-local scratch reversed into the
  builder) instead of allocating an intermediate `String`.
- Required tests:
  - retain existing `push_int` behavior tests
  - add zero / negative / large-int stress coverage on repeated
    `push_int` calls
  - add benchmark coverage for many `push_int` appends versus
    `push_string(x.to_string())`
- Verification notes:
  Accepted in claim `C-007`. `StringBuilder.push_int` now emits decimal
  digits directly into the builder buffer without the intermediate
  `String` allocation path. `tests/test_string_builder.ptt` passes with
  the expanded integer-format coverage, the new
  `tests/bench/string_builder_push_int_bench.ptt` emits
  `38890 / 38890 / identical`, `tests/test_collection_materializers.ptt`
  still passes, and a full `make test` re-run ends with
  `All tests passed.`

### F-006
- State: CLOSED
- Severity: P2
- Area: stdlib | bench
- Opened at: 2026-05-18T17:23:25-07:00
- Last reviewed at: 2026-05-18T17:36:02-07:00
- Audited commit: c8cdd04 + working tree
- Evidence:
  - [std/map.ptt](/Users/erbos/erbos-lang/std/map.ptt:88)
  - [std/map.ptt](/Users/erbos/erbos-lang/std/map.ptt:104)
  - [std/set.ptt](/Users/erbos/erbos-lang/std/set.ptt:168)
  - [std/set.ptt](/Users/erbos/erbos-lang/std/set.ptt:208)
  - [std/set.ptt](/Users/erbos/erbos-lang/std/set.ptt:243)
  - [tests/test_collection_materializers.ptt](/Users/erbos/erbos-lang/tests/test_collection_materializers.ptt:1)
  - [tests/bench/map_keys_values_bench.ptt](/Users/erbos/erbos-lang/tests/bench/map_keys_values_bench.ptt:1)
  - [tests/bench/set_materializers_bench.ptt](/Users/erbos/erbos-lang/tests/bench/set_materializers_bench.ptt:1)
  - [tests/bench/BASELINE.md](/Users/erbos/erbos-lang/tests/bench/BASELINE.md:1)
- Problem:
  The container materializers knew their output size bounds but still
  built from zero-capacity collections. `Map.keys`, `Map.values`, and
  `Set.values` returned `List`s without reserving `self.count`, and
  `Set.intersect` / `Set.difference` likewise built fresh sets without
  reserving the obvious upper bounds. That forced avoidable growth,
  repeated element copying, and extra hash-table rebuilds in exactly the
  iteration-heavy paths the bench workload exercises.
- Required fix:
  Reserve exact or conservative output capacity up front in every
  collection materializer/algebra helper that already knows its result
  bound (`self.count`, `min(self.count, other.count)`, `self.count`,
  etc.). Keep the public semantics unchanged; this is a capacity-planning
  cleanup, not an API change.
- Required tests:
  - retain all existing behavior tests unchanged
  - add benchmark coverage for `Map.keys`, `Map.values`, `Set.values`,
    `Set.intersect`, and `Set.difference`
  - add at least one regression that forces large result materialization
    so the pre-sizing path is exercised
- Verification notes:
  Accepted in claim `C-006`. `Map.keys`, `Map.values`, `Set.values`,
  `Set.intersect`, and `Set.difference` now reserve their known output
  bounds up front. The pre-existing `test_map_methods.ptt` and
  `test_set.ptt` suites still pass, the new
  `tests/test_collection_materializers.ptt` 14-case workload passes, and
  both new bench programs emit the claimed sums:
  `map_keys_values_bench.ptt` -> `4096 / 8386560 / 58705920`,
  `set_materializers_bench.ptt` -> `2048 / 2048 / 2096128 / 1024 / 1572352 / 1024 / 523776`.

### F-004
- State: CLOSED
- Severity: P1
- Area: compiler | stdlib | tests
- Opened at: 2026-05-18T15:02:37-07:00
- Last reviewed at: 2026-05-18T16:09:13-07:00
- Audited commit: 5fa4ab5 + working tree
- Evidence:
  - [std/deque.ptt](/Users/erbos/erbos-lang/std/deque.ptt:29)
  - [std/deque.ptt](/Users/erbos/erbos-lang/std/deque.ptt:63)
  - [std/deque.ptt](/Users/erbos/erbos-lang/std/deque.ptt:86)
  - [std/deque.ptt](/Users/erbos/erbos-lang/std/deque.ptt:97)
  - [std/deque.ptt](/Users/erbos/erbos-lang/std/deque.ptt:111)
  - [std/deque.ptt](/Users/erbos/erbos-lang/std/deque.ptt:166)
  - [compiler/main.c](/Users/erbos/erbos-lang/compiler/main.c:1885)
  - [tests/test_deque.ptt](/Users/erbos/erbos-lang/tests/test_deque.ptt:175)
  - Original runtime repro on `5fa4ab5 + working tree` before claim `C-005`:
    - `/Users/erbos/erbos-lang/erbos run /private/tmp/potato_deque_heap_alias.ptt`
      printed `7` and exited `139`
  - Re-check during claim `C-005` on the current working tree:
    - `/Users/erbos/erbos-lang/erbos run /private/tmp/potato_deque_heap_alias.ptt`
      prints `1` and exits `0`
  - Full suite re-check on the same working tree ends with
    `All tests passed.`
- Problem:
  `Deque of T` had the same unsound transition as `Queue`, but on both
  ends. `push_back` / `push_front` did raw slot stores for heap-shaped
  `T`, while `pop_front` / `pop_back` / `try_pop_*` / `clear` left
  non-null slots behind in the ring, and the generic parent drop helper
  later re-dropped those pointers.
- Required fix:
  Give `Deque` an explicit heap-shaped ownership policy. Pushes must
  clone or move into slots intentionally, pops must retire or clear the
  physical slot they just transferred out, and clear/reuse must not
  leave queue-owned and caller-owned pointers intermingled in the same
  ring buffer.
- Required tests:
  - `Deque of List of int` and owned `String` locals, not just literals
  - `push_front` / `push_back` alias safety for heap-shaped `T`
  - `pop_front` / `pop_back` followed by continued use of the returned
    value
  - `clear` + reuse on heap-shaped `T`
  - wraparound plus heap-shaped pushes/pops from both ends
- Verification notes:
  Accepted in claim `C-005`. The fix moves ring-buffer extraction to
  `is now self.data[...]`, updates growth paths to transfer ownership
  instead of duplicating live pointers, and keeps heap-shaped pushes on
  the clone-then-transfer path. The owned-String deque repro no longer
  crashes, and the full `make test` suite stays green.

### F-003
- State: CLOSED
- Severity: P1
- Area: compiler | stdlib | tests
- Opened at: 2026-05-18T15:02:37-07:00
- Last reviewed at: 2026-05-18T16:09:13-07:00
- Audited commit: 5fa4ab5 + working tree
- Evidence:
  - [std/queue.ptt](/Users/erbos/erbos-lang/std/queue.ptt:50)
  - [std/queue.ptt](/Users/erbos/erbos-lang/std/queue.ptt:95)
  - [std/queue.ptt](/Users/erbos/erbos-lang/std/queue.ptt:107)
  - [std/queue.ptt](/Users/erbos/erbos-lang/std/queue.ptt:117)
  - [std/queue.ptt](/Users/erbos/erbos-lang/std/queue.ptt:142)
  - [compiler/main.c](/Users/erbos/erbos-lang/compiler/main.c:1885)
  - [tests/test_queue.ptt](/Users/erbos/erbos-lang/tests/test_queue.ptt:132)
  - Original runtime repro on `5fa4ab5 + working tree` before claim `C-005`:
    - `/Users/erbos/erbos-lang/erbos run /private/tmp/potato_queue_heap_alias.ptt`
      printed `1` and exited `139`
  - Re-check during claim `C-005` on the current working tree:
    - `/Users/erbos/erbos-lang/erbos run /private/tmp/potato_queue_heap_alias.ptt`
      prints `17` and exits `0`
  - Full suite re-check on the same working tree ends with
    `All tests passed.`
- Problem:
  `Queue of T` was not migrated to the new heap-shaped ownership model.
  `Queue.push` did a raw circular-buffer slot store, `Queue.pop` /
  `Queue.try_pop` / `Queue.clear` left stale non-null slots behind, and
  the generic parent drop helper later treated those stale slots as
  queue-owned.
- Required fix:
  Establish a real queue invariant for heap-shaped `T`. `push` must
  clone or explicitly transfer ownership into the slot, `pop` /
  `try_pop` / `clear` must clear or otherwise retire transferred-out
  slots, and the parent drop/clone strategy must match the queue's
  circular `head`/`count` semantics instead of blindly iterating every
  non-null capacity slot.
- Required tests:
  - `Queue of List of int` and `Queue of String` with owned locals, not
    just String literals
  - `push` + scope-end drop for heap-shaped `T`
  - `pop` / `try_pop` followed by continued use of the returned value
  - `clear` + reuse on heap-shaped `T`
  - wraparound and growth cases on heap-shaped `T`
- Verification notes:
  Accepted in claim `C-005`. The fix uses ownership transfer on slot
  extraction, transfers live ring-buffer entries during growth instead
  of duplicating them, and keeps heap-shaped pushes on the clone-then-
  transfer path. The owned-String queue repro now preserves the caller
  local and exits cleanly, and the full `make test` suite stays green.

### F-002
- State: CLOSED
- Severity: P1
- Area: compiler | stdlib
- Opened at: 2026-05-18T14:07:46-07:00
- Last reviewed at: 2026-05-18T15:37:07-07:00
- Audited commit: 5fa4ab5 + working tree
- Evidence:
  - [std/list.ptt](/Users/erbos/erbos-lang/std/list.ptt:116)
  - [std/list.ptt](/Users/erbos/erbos-lang/std/list.ptt:125)
  - [std/list.ptt](/Users/erbos/erbos-lang/std/list.ptt:244)
  - [compiler/checker.c](/Users/erbos/erbos-lang/compiler/checker.c:1504)
  - [compiler/irgen.c](/Users/erbos/erbos-lang/compiler/irgen.c:1118)
  - [compiler/main.c](/Users/erbos/erbos-lang/compiler/main.c:1880)
  - [tests/test_heap_parent_drop.ptt](/Users/erbos/erbos-lang/tests/test_heap_parent_drop.ptt:206)
  - Targeted repros on `5fa4ab5 + working tree` now pass:
    - `/Users/erbos/erbos-lang/erbos run /private/tmp/potato_pop_reuse.ptt`
      prints `42` and exits `0`
    - `/Users/erbos/erbos-lang/erbos run /private/tmp/potato_remove_reuse.ptt`
      prints `7` and exits `0`
  - Full suite re-check on the same working tree ends with
    `All tests passed.`
- Problem:
  Claim C-003 made List heap-shaped clone/drop count-bounded but left
  post-count live pointers behind after `pop`, `try_pop`, `remove`, and
  `clear`, which made `push`/`insert` reuse unsafe and caused leaks.
- Required fix:
  Establish a coherent post-`count` ownership invariant for
  `List of heap-shaped T`, including slot extraction/nulling when
  ownership leaves the list and parent-drop logic that matches that
  invariant.
- Required tests:
  - `List.pop` + later `push` / `insert` on heap-shaped T
  - `List.try_pop` + later `push` / `insert`
  - `List.remove` + later `push` / `insert`
  - `List.clear` + reuse / scope-end drop for heap-shaped T
  - nested `List of List of int` / `List of String`
- Verification notes:
  Accepted in claim `C-004`. The fix extends `is now` to raw array slots
  and rewrites `List.pop`, `List.try_pop`, and `List.remove` to vacate
  slots when ownership transfers out. `compiler/main.c` now uses the
  cap-bounded null-guarded array helpers uniformly for heap-shaped array
  fields, and the targeted repros plus full `make test` confirm the
  previous live-tail crash path is gone.

### F-001
- State: CLOSED
- Severity: P1
- Area: compiler | stdlib
- Opened at: 2026-05-18T03:40:31-07:00
- Last reviewed at: 2026-05-18T13:07:19-07:00
- Audited commit: 1935bb3 + working tree
- Evidence:
  - [std/pool.ptt](/Users/erbos/erbos-lang/std/pool.ptt:63)
  - [std/list.ptt](/Users/erbos/erbos-lang/std/list.ptt:148)
  - [std/set.ptt](/Users/erbos/erbos-lang/std/set.ptt:105)
  - [compiler/irgen.c](/Users/erbos/erbos-lang/compiler/irgen.c:1593)
  - Fresh full-suite rebuild on `dd17f87` ends with `All tests passed.`,
    but a generated IR probe for `List of String.set(0, "b")` still
    lowers `_List__String_set` to a raw `str` store with no
    `_drop_String` call before overwriting the previous owned slot.
- Problem:
  Heap-shaped element replacement is still ownership-incorrect. Pool
  slot reuse goes through `self.items.set(id, value)`, Set tombstone
  reuse writes a fresh clone into a previously-live bucket, and the
  shared array-slot lowering in `NODE_INDEX_ASSIGN` only emits a raw
  store. The previous owned value in that live slot is never dropped
  before overwrite, so `Pool.set`, Pool remove+reuse, Set remove+readd,
  Set clear+reinsert, and the analogous Map paths leak heap-shaped
  entries even though the current tests stay green.
- Required fix:
  Add a real drop-before-overwrite path for live owned array/list slots
  and apply it at the layers that know slot liveness. A blanket
  compiler-side "always drop on index assign" is not sufficient because
  many writes target previously-uninitialized capacity. The final fix
  must make `List.set` and the Set/Map/Pool reuse/clear paths ownership-
  correct for heap-shaped values without regressing primitive or fresh-
  slot stores.
- Required tests:
  - heap-shaped `List.set` replacement stress that proves the previous
    value is dropped, not leaked
  - `Pool of String`: repeated `set` on one live id plus remove+reuse
    cycles
  - `Set of String`: remove+re-add into tombstoned buckets and
    clear+reinsert cycles
  - matching `Map of String, String` regression because Set copies the
    same bucket-reuse machinery
- Verification notes:
  Claim `C-002` fixes the root cause at the correct layers:
  checker tags heap-shaped array element types, monomorph preserves the
  metadata, irgen now emits a null-guarded `_drop_<elem>` before
  `be now` / `be rep` overwrites, and `List.set` switched to
  `self.data[i] be now v` so live-slot replacement actually takes that
  path. A fresh rebuild from source plus full `make test` passed, and
  the new regressions now cover both behavior
  (`tests/test_heap_slot_drop.ptt`) and emitted code
  (`tests/leaks/heap_slot_drop_emits_drop.ptt`) for `List`, `Pool`,
  `Set`, and `Map` heap-shaped overwrite/reuse paths.

## Finding Template

```md
### F-###
- State: OPEN
- Severity: P0 | P1 | P2 | P3
- Area: compiler | checker | parser | monomorph | stdlib | docs | tests
- Opened at: <ISO 8601 timestamp>
- Last reviewed at: <ISO 8601 timestamp>
- Audited commit: <commit or working tree note>
- Evidence:
  - [/absolute/path/file.ext](/absolute/path/file.ext:line)
  - command / runtime repro summary
- Problem:
  Clear statement of the verified issue.
- Required fix:
  Root-cause fix only. No workaround, no compatibility shim.
- Required tests:
  Enumerate the coverage that must exist before this can be verified.
- Verification notes:
  Filled in by auditor when re-checking the fix.
```

## Change Log

- Revision 1: created single-writer findings contract and empty ledger.
- Revision 1: added release handoff gate (`HOLD` /
  `COMMIT_AND_PUSH`) so Claude only commits after auditor approval.
- Revision 2: started audit pass for claim `C-001` against commit
  `dd17f87`; release gate held during verification.
- Revision 2: completed audit of claim `C-001`; release remains on HOLD
  because F-001 was found in array-slot overwrite ownership semantics.
- Revision 3: added the `Conclusion` field with `ALL_CLEAR` /
  `FINDINGS_OPEN` semantics so scheduler-driven audits can explicitly
  signal whether a batch is clear of known findings.
- Revision 4: audited claim `C-002` against commit `1935bb3 + working tree`;
  accepted the F-001 fix, closed the finding, and marked the batch
  `ALL_CLEAR` with `COMMIT_AND_PUSH`.
- Revision 5: generalized the ledger contract from stdlib-only work to
  repo-wide implementation and audit batches.
- Revision 6: audited claim `C-003` against commit `5fa4ab5 + working tree`;
  rejected the parent-drop batch, opened F-002 for the incomplete
  List-tail ownership model, and returned the ledger to
  `FINDINGS_OPEN` / `HOLD`.
- Revision 7: deep code sweep against `5fa4ab5 + working tree` added
  F-003 and F-004 for queue/deque heap-shaped ownership unsoundness,
  with direct runtime repros and explicit coverage gaps in the current
  tests.
- Revision 8: audited claim `C-004` against `5fa4ab5 + working tree`,
  accepted and closed F-002, re-verified that F-003/F-004 still
  reproduce, and kept the batch on `FINDINGS_OPEN` / `HOLD`.
- Revision 9: audited claim `C-005` against `5fa4ab5 + working tree`,
  accepted and closed F-003 and F-004 after the owned-String queue and
  deque repros stopped crashing, and set the batch to `ALL_CLEAR` /
  `COMMIT_AND_PUSH`.
- Revision 10: deep stdlib optimization audit against
  `c8cdd04 + working tree` opened F-005 through F-008 for
  hash-container probe quality, collection materializer reserve gaps,
  naive String search paths, and `StringBuilder.push_int`'s temporary
  allocation path.
- Revision 11: audited claim `C-006` against `c8cdd04 + working tree`,
  accepted and closed F-006 after verifying the Map/Set suites, the new
  collection-materializer framework test, and both new bench programs;
  F-005, F-007, and F-008 remain open so the batch stays on HOLD.
- Revision 12: audited claim `C-007` against `c8cdd04 + working tree`,
  accepted and closed F-008 after verifying the expanded
  StringBuilder suite, the new `string_builder_push_int_bench.ptt`
  output contract, and a full `make test` pass; F-005 and F-007 remain
  open so the batch stays on HOLD.
