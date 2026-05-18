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

1. Read this file before each substantial implementation batch.
2. Treat `OPEN` findings as mandatory work.
3. Treat `FIX CLAIMED` as not yet accepted.
4. Avoid claiming a phase complete while any relevant finding is still
   `OPEN` or `FIX CLAIMED`.
5. Re-check this file after each implementation batch in case a newer
   `Revision` exists.
6. When you want verification, update `codex/findings-claims.md`
   instead of touching this file.
7. Treat `Conclusion: ALL_CLEAR` as the explicit signal that the
   current audited batch has no active known findings and no in-flight
   code audit still open against it.
8. Do not commit or push an audited implementation batch unless this file's
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
- `Revision`: 5
- `Last updated`: 2026-05-18T13:11:34-07:00
- `Audited commit`: 1935bb3 + working tree
- `Last claim audited`: C-002
- `Conclusion`: ALL_CLEAR
- `Release action`: COMMIT_AND_PUSH
- `Release note`: Claim C-002 accepted; F-001 is fixed and this finding batch may land as its own commit.

## Active Findings

None.

## Closed Findings

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
