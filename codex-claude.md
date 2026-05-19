# Codex + Claude Audit/Fix Workflow

This document describes the repo-local workflow where:

- `Codex` is the auditor
- `Claude` is the implementer
- both coordinate through two repo files plus one Codex heartbeat

The goal is simple:

1. `Codex` finds and records verified issues.
2. `Claude` fixes one issue or one inseparable root-cause cluster at a time.
3. `Codex` audits each fix claim.
4. only after a claim is accepted may `Claude` commit and push that batch.

## 1. How it works

The workflow has four moving parts:

1. `codex/findings.md`
   - Codex-owned findings ledger
   - source of truth for finding state, audit conclusion, and release authorization

2. `codex/findings-claims.md`
   - Claude-owned claims file
   - used only to request re-audit of specific fixes

3. `.claude/commands/findings.md`
   - Claude worker command
   - run under Claude CLI scheduler as `/loop 5m /findings`

4. `potato-findings-sweep`
   - Codex heartbeat automation
   - audits new claims on a 10-minute cadence

High-level loop:

1. `Codex` writes verified findings into `codex/findings.md`.
2. `Claude` reads the ledger and picks exactly one `OPEN` finding, unless multiple findings are the same inseparable root cause.
3. `Claude` fixes it, adds tests, runs verification, then writes a claim into `codex/findings-claims.md`.
4. `Codex` heartbeat sees the new claim and audits only the claimed finding IDs plus directly adjacent root-cause issues exposed by the same area.
5. `Codex` updates `codex/findings.md` with the verdict.
6. If the claim is accepted and the audited batch is clear, `Claude` commits and pushes that claim immediately.
7. Then `Claude` moves to the next finding.

Core invariant:

- one claim -> one audit -> one immediate commit/push

No waterfall batching:

- do not fix claim A
- then fix claim B
- then commit A+B together later

Each accepted claim lands on its own before the next unrelated finding starts.

## 2. Protocol

### 2.1 Source of truth

`codex/findings.md` is the only source of truth for:

- whether a finding is open or closed
- whether an audit is in progress
- whether the current batch is clear
- whether `Claude` may commit and push

`codex/findings-claims.md` is not truth. It is only an audit request channel.

### 2.2 Pending-claim rule

A claim is pending only when both are true:

- `codex/findings-claims.md` header `State` is `READY_FOR_AUDIT`
- its `Claim ID` differs from `codex/findings.md` header `Last claim audited`

If `State` is still `READY_FOR_AUDIT` but the `Claim ID` already matches `Last claim audited`, that claim is already consumed and must not be treated as pending anymore.

### 2.3 Finding lifecycle

In `codex/findings.md`, findings use these states:

- `OPEN`
- `FIX CLAIMED`
- `VERIFIED`
- `CLOSED`

Only `Codex` moves a finding into `VERIFIED` or `CLOSED`.

### 2.4 Claim lifecycle

In `codex/findings-claims.md`, claims use these states:

- `IDLE`
- `READY_FOR_AUDIT`
- `SUPERSEDED`

`Claude` writes claims. `Codex` reads them but does not edit them.

### 2.5 Audit state

`codex/findings.md` header `State`:

- `ACTIVE`
  - current Codex audit pass may still add or change findings
- `QUIESCENT`
  - current Codex pass is done for now

### 2.6 Conclusion and release gate

`codex/findings.md` carries two separate gates:

- `Conclusion`
  - `ALL_CLEAR`
  - `FINDINGS_OPEN`

- `Release action`
  - `HOLD`
  - `COMMIT_AND_PUSH`

Meaning:

- `ALL_CLEAR` means the current audited batch has no active known findings and no in-flight audit still open against it.
- `FINDINGS_OPEN` means at least one finding is still active, or the batch has not been cleared yet.
- `COMMIT_AND_PUSH` means Codex authorizes landing the current audited batch.
- `HOLD` means Claude must wait or keep fixing.

Commit/push rule:

`Claude` may commit and push only when `codex/findings.md` says both:

- `Conclusion: ALL_CLEAR`
- `Release action: COMMIT_AND_PUSH`

Silence is not clearance.

### 2.7 Codex audit behavior

On each heartbeat sweep:

1. read both contract files
2. if no new pending claim exists:
   - do not re-verify old findings mid-implementation
   - only add clearly new unrelated findings if they are actually verified
3. if a new pending claim exists:
   - set ledger header to:
     - `State: ACTIVE`
     - `Conclusion: FINDINGS_OPEN`
     - `Release action: HOLD`
   - audit the claim
   - set `Last claim audited` to the consumed `Claim ID`
   - update findings honestly
   - return header `State` to `QUIESCENT`
4. after audit:
   - if any finding remains `OPEN` or `FIX CLAIMED`, keep:
     - `Conclusion: FINDINGS_OPEN`
     - `Release action: HOLD`
   - if zero active findings remain, set:
     - `Conclusion: ALL_CLEAR`
   - if that audited batch is ready to land, set:
     - `Release action: COMMIT_AND_PUSH`

### 2.8 Claude worker behavior

On each `/findings` loop iteration:

1. read `codex/findings.md`
2. read `codex/findings-claims.md`
3. if there is a pending claim:
   - do not start a new unrelated finding
   - wait for Codex to consume it
4. if the claim was consumed:
   - if the ledger says `ALL_CLEAR` + `COMMIT_AND_PUSH`, commit and push immediately
   - otherwise treat the resulting open findings as mandatory work
5. if there is no pending claim and there is an `OPEN` finding:
   - fix exactly one finding or inseparable cluster
   - add comprehensive tests
   - run targeted tests and full `make test`
   - write a new claim
6. after Codex consumes a claim:
   - rewrite the claims header so it no longer advertises the consumed claim as pending before starting the next unrelated batch

## 3. How to set up Codex

### 3.1 Required files

Ensure these exist:

- `/Users/erbos/erbos-lang/codex/findings.md`
- `/Users/erbos/erbos-lang/codex/findings-claims.md`

`findings.md` must carry the contract header fields:

- `State`
- `Revision`
- `Last updated`
- `Audited commit`
- `Last claim audited`
- `Conclusion`
- `Release action`
- `Release note`

### 3.2 Required automation

Codex needs a heartbeat automation that audits claims.

Current repo setup:

- automation id: `potato-findings-sweep`
- config file:
  - `/Users/erbos/.codex/automations/potato-findings-sweep/automation.toml`
- current cadence:
  - `RRULE:FREQ=MINUTELY;INTERVAL=10`

The heartbeat prompt must follow the two-file contract:

- `findings.md` is Codex-owned
- `findings-claims.md` is Claude-owned
- only audit when a new pending claim exists
- otherwise leave the ledger stable unless a clearly new unrelated finding is verified

### 3.3 Codex operating rules

`Codex` must:

- write only `codex/findings.md`
- never edit `codex/findings-claims.md`
- verify claims against actual code and tests
- avoid speculative findings
- bump `Revision` on every ledger write
- use ISO 8601 timestamps with numeric UTC offset

## 4. How to set up Claude

### 4.1 Required command

Ensure this file exists:

- `/Users/erbos/erbos-lang/.claude/commands/findings.md`

That command is the Claude worker for this repo.

### 4.2 Start the worker

In Claude CLI, from the repo, run:

```text
/loop 5m /findings
```

That loop is the implementer side of the contract.

### 4.3 Claude operating rules

`Claude` must:

- read `codex/findings.md` before each substantial batch
- read `codex/findings-claims.md` before each substantial batch
- never edit `codex/findings.md`
- only edit `codex/findings-claims.md` under `codex/`
- fix one finding at a time unless multiple findings are the same inseparable root cause
- add comprehensive tests
- run targeted tests and full `make test`
- write a claim only after the fix is ready for audit
- commit and push immediately after an accepted claim

### 4.4 What goes into a claim

Every audit-ready claim must include:

- `Claim ID`
- `Against findings revision`
- `Target commit`
- `Claimed fixed`
- `Last updated`

Preferred claim header flow:

1. `IDLE` when nothing is pending
2. `READY_FOR_AUDIT` when asking Codex to verify
3. `SUPERSEDED` only when replacing an older unconsumed claim

## 5. File ownership

### Codex-owned

- `/Users/erbos/erbos-lang/codex/findings.md`
- `/Users/erbos/.codex/automations/potato-findings-sweep/automation.toml`

### Claude-owned

- `/Users/erbos/erbos-lang/codex/findings-claims.md`
- `/Users/erbos/erbos-lang/.claude/commands/findings.md`

### Shared repo code

Both agents may change repo code, tests, docs, and stdlib files as part of normal implementation work, but the coordination files remain single-writer:

- Codex owns the ledger
- Claude owns the claims file

That single-writer rule is what keeps the workflow stable.

## 6. Fast start for a new session

If a new audit/fix session needs to pick up quickly:

1. read `/Users/erbos/erbos-lang/codex/findings.md`
2. read `/Users/erbos/erbos-lang/codex/findings-claims.md`
3. check whether a claim is actually pending using the `Claim ID` vs `Last claim audited` rule
4. if running Codex, confirm the heartbeat automation exists and is active
5. if running Claude, start:

```text
/loop 5m /findings
```

6. respect the landing rule:

- one claim -> one audit -> one immediate commit/push
