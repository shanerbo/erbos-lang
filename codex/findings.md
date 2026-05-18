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
  stdlib-phase `commit and push` handoff.
- The findings header carries this as `Release action`.
- `HOLD` means Claude must keep implementing or wait for audit.
- `COMMIT_AND_PUSH` means Codex has verified the current claim, the
  relevant findings are accepted, and Claude may create the intended
  commit and push it.
- When practical, Codex should also add a short `Release note`
  describing the approved scope.

### Claude consumption rule

Claude should:

1. Read this file before each substantial stdlib/compiler batch.
2. Treat `OPEN` findings as mandatory work.
3. Treat `FIX CLAIMED` as not yet accepted.
4. Avoid claiming a phase complete while any relevant finding is still
   `OPEN` or `FIX CLAIMED`.
5. Re-check this file after each implementation batch in case a newer
   `Revision` exists.
6. When you want verification, update `codex/findings-claims.md`
   instead of touching this file.
7. Do not commit or push a stdlib completion batch unless this file's
   header says `Release action: COMMIT_AND_PUSH`.

### Scope rule

This file is for:

- compiler bugs exposed by stdlib work
- docs/code contradictions that block truthful completion claims
- missing or weak regression coverage
- design-level correctness issues that must be fixed at the root

This file is not for:

- vague suggestions
- style-only nits
- speculative concerns without verification

## Header

- `State`: QUIESCENT
- `Revision`: 1
- `Last updated`: 2026-05-18T02:02:00-07:00
- `Audited commit`: current working tree at time of file creation
- `Last claim audited`: none
- `Release action`: HOLD
- `Release note`: none

## Active Findings

None yet in this ledger. Add findings below this section using the
template.

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
