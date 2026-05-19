Run this command as Claude's repo findings worker for
`/Users/erbos/erbos-lang`.

Typical scheduling entry:

- `/loop 5m /findings`

Work under the repo-wide findings contract in
`/Users/erbos/erbos-lang`.

Read before every iteration:
- `codex/findings.md`
- `codex/findings-claims.md`

Ownership:
- Never edit `codex/findings.md`.
- You may edit exactly one file under `codex/`:
  - `codex/findings-claims.md`
- Do not edit any other file under `codex/`.

Core rule:
- One claim -> one Codex audit -> one immediate commit/push -> then next unrelated finding.
- Never waterfall multiple cleared claims into one later commit.
- Work one finding at a time unless multiple findings are the same inseparable root cause.
- A claim is pending only when `codex/findings-claims.md` header
  `State` is `READY_FOR_AUDIT` and its `Claim ID` differs from
  `codex/findings.md` header `Last claim audited`.
- If `codex/findings-claims.md` still says `READY_FOR_AUDIT` but its
  `Claim ID` already matches `codex/findings.md` header
  `Last claim audited`, that claim is already consumed by Codex and
  must not block the next iteration as still pending.
- Ignore older `ALL_CLEAR` / `COMMIT_AND_PUSH` verdicts that belong to an
  earlier claim. A release verdict is valid only when
  `codex/findings.md` header `Last claim audited` matches your current
  `Claim ID`.

Per iteration:
1. Read both findings files.
2. Treat the header claim as pending only when
   `codex/findings-claims.md` header `State` is `READY_FOR_AUDIT` and
   its `Claim ID` differs from `codex/findings.md` header
   `Last claim audited`.
3. If the claims header still says `READY_FOR_AUDIT` but its `Claim ID`
   already matches `codex/findings.md` header `Last claim audited`,
   treat that claim as already consumed. It is not pending anymore.
4. If you already have a pending claim, do not start a new unrelated
   finding. Check whether `codex/findings.md` now has:
   - `Last claim audited` equal to your current `Claim ID`
5. If your current claim has been audited:
   - if `Conclusion: ALL_CLEAR` and `Release action: COMMIT_AND_PUSH`, commit and push that claim immediately, then continue next iteration with the next finding
   - if `Conclusion: FINDINGS_OPEN` or `Release action: HOLD`, treat the resulting open findings as mandatory work
   - after handling the verdict, rewrite `codex/findings-claims.md`
     so its header no longer advertises that consumed claim as pending
     before you start the next unrelated batch
6. If you do not have a pending claim and there is an `OPEN` relevant finding, pick exactly one finding or one inseparable root-cause cluster, implement the root-cause fix, add comprehensive tests, run targeted tests, run full `make test`, then update `codex/findings-claims.md` with:
   - `State: READY_FOR_AUDIT`
   - new `Claim ID`
   - `Against findings revision`
   - `Target commit`
   - exact `Claimed fixed`
   - `Last updated` in ISO 8601 with numeric UTC offset
7. If there is no actionable open finding and no pending claim, do nothing this iteration.

Quality bar:
- No workaround
- No compatibility shim
- No temporary bypass
- No sugarcoated tests
- Fix root causes at the correct layer
- Add comprehensive regression coverage

Per-claim landing:
- One accepted claim -> one immediate commit/push -> only then move to
  the next unrelated finding.
