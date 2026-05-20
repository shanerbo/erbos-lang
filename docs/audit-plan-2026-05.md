# Audit Cleanup + Optimization Plan — 2026-05

This document is the concrete implementation plan for the
post-linux-arm64 audit findings: dead-code removal across the C
compiler, dedupe of retired-feature residue, doc reconciliation,
and a hot-path optimization sweep that replaces several O(N) /
O(N²) scans with hashtable lookups.

The structure mirrors `linux-arm64-backend-plan.md`: phases gate on
each other, every commit ends with `make test` green, and the
deeper-risk work waits until safer work proves the harness is
stable.

## Scope

In scope:
- C compiler frontend (`compiler/*.c`, `compiler/*.h`).
- Stdlib documentation (`std/STDLIB_CHECKLIST.md`) — drift fixes.
- A small handful of stdlib comments that mis-describe the
  factory legacy-form usage.

Out of scope:
- The green-thread runtime in `compiler/runtime/` — separate
  module, not wired into compiled output.
- Stdlib semantic changes, new methods, or layout adjustments.
- Anything that changes language-observable behavior.

## Decisions taken before drafting this plan

### D1 — `_task_collapse` — RESOLVED 2026-05-20: KEEP

Symbol exists end-to-end (runtime emission +
`checker.c:941` recognising `t.collapse()` + `irgen.c:795` lowering),
but zero test or example caller.

**Decision: keep, document.** First-principles reasoning recorded
in `docs/design-decisions.md` (`2026-05-20 — _task_collapse retained
as roadmap scaffolding`). T07' executed; T07 dropped from the plan.

### D2 — `TYPE_STR`

`checker.c:429, 434` still produce `TYPE_STR` (operator `+` on
String operands), even though the comment at line 80 says it
"is no longer produced." Two clean options:

- **Rename `TYPE_STR` → `TYPE_STRING`** and fix the comment.
  ~30 min, no behavior change. Default of this plan.
- **Delete `TYPE_STR` entirely** and route `+(String, String)`
  through `make_struct("String")`. ~half a day, ~10 reader sites
  need lockstep update, medium risk.

Plan defaults to rename-only. If the deeper deletion is wanted,
T06 expands.

## Phases at a glance

| Phase | Commits | Theme | Risk |
|---|---|---|---|
| 0 — Foundation | 1 | hashmap util | low |
| 1 — Tier-1 deletions | 3 | mechanical dead-code drops | low |
| 2 — Tier-2 sweep | 2 | retired-feature residue | low-med |
| 3 — Decision-gated | 1 | T07 (delete) or T07' (document) | low |
| 4 — Optimization | 7 | linear/quadratic → hash | low → med |
| **Total** | **14** | | |

Estimated cumulative `make test` wall-clock win after Phase 4:
**15-25%**, dominated by T10 / T11 / T12.

## Phase 0 — Foundation

### T01 — Add `compiler/hashmap.{h,c}`

A small generic open-addressed string→pointer hashmap. Underpins
T08–T13. Approximately 150 LOC of new code, no API changes
elsewhere in this commit.

Required surface (used by later tasks):

- `Hashmap *hashmap_new(size_t initial_cap);`
- `void hashmap_free(Hashmap *m);`
- `void hashmap_put(Hashmap *m, const char *key, void *value);`
- `void *hashmap_get(const Hashmap *m, const char *key);`
- `int hashmap_contains(const Hashmap *m, const char *key);`
- Iteration: `for (HashmapIter it = hashmap_begin(m); it.valid; it = hashmap_next(it)) { ... }`

Properties expected:

- String keys are `strdup`ed at `put` time so the caller doesn't
  have to keep them alive.
- Collision strategy: open addressing with linear probing.
- Resize threshold: 75% load factor; doubling.
- `O(1)` amortised everywhere.

Verification:

- `make` clean, no warnings under `-Wall -Wextra -std=c11`.
- `make test` ends with `All tests passed.` (no test should
  exercise hashmap directly yet — it's just compiled in).

Commit: `chore(compiler): add string-keyed hashmap utility`

## Phase 1 — Tier-1 deletions

Mechanical deletions of code paths verified dead by the audit.
Each commit ends `make test` green; cross-check Linux backend on
one Docker exec at the end of the phase.

### T02 — Drop unused runtime symbols + dead writes

Three independent mini-deletions, one commit:

- `_write_bytes` runtime helper.
  - Delete `emit_write_bytes` and its call in `runtime_emit.c`.
  - Drop the entry from the `runtime_emit.h` inventory comment.
  - Update the stale "the compiler emits `bl _heap_alloc` /
    `bl _heap_free` / `bl _write_bytes`" sentence in
    `compiler/checker.c:655`.
- `_panic_capacity` runtime handler + its rodata.
  - Delete the handler emission in `runtime_emit.c:449-453`.
  - Delete the `_cap_msg` / `_cap_arr` / `_cap_str` triple at
    lines 474-478.
  - Drop both names from the `runtime_emit.h` inventory comment.
- `len()` builtin's dead `resolved_type` writes.
  - Delete the three `n->call.args[0]->resolved_type = ...`
    assignments at `compiler/checker.c:617-619`. The `len()`
    lowering at `irgen.c:692-706` ignores the value entirely.

Verification:

- `make test` green.
- Confirm symbol removal sticks: `./erbos ir examples/hello.ptt`
  followed by `grep -c '_write_bytes\|_panic_capacity\|_cap_str' hello.s` → 0.

Commit: `chore(compiler): drop dead runtime symbols (_write_bytes, _panic_capacity) and len() dead writes`

### T03 — Drop unused IR / token / target enum members

Four independent enum-member deletions, one commit:

- `IR_ARG` (`ir.h:30`) — never emitted, no handler.
- `TOK_SUB_WORD`, `TOK_MUL_WORD`, `TOK_DIV_WORD` (`token.h:41-43`) —
  reserved-but-never-emitted; lexer has no rule for them.
  Note: `TOK_ADD_WORD` stays (it has defensive uses in
  checker / irgen).
- `TARGET_LOOKUP_NOT_IMPLEMENTED` (`target.h:152`) and the
  `else if (r == TARGET_LOOKUP_NOT_IMPLEMENTED)` branch in
  `main.c:806-810`. Unreachable since the linux-arm64 backend
  landed.
- `Target.name` field (`target.h:33`) and the two `.name = "..."`
  initializers in `target_darwin_arm64.c:176` and
  `target_linux_arm64.c:190`. Field is never read.

Verification:

- `make test` green.
- Manually verify `--target=foo`'s diagnostic still says
  "is not a recognized target" (the `UNKNOWN` arm — the only one
  left).

Commit: `chore(compiler): drop unused IR/token/target enum members`

### T04 — Stdlib checklist drift fixes (doc-only)

`std/STDLIB_CHECKLIST.md` corrections:

- The "Verified current files" header at lines 32-46 misses
  `path.ptt`, `pool.ptt`, `set.ptt`. Add three bullets matching
  the existing format. The deeper sections at lines 496-549
  (`set`), 694-744 (`pool`), 850-896 (`path`) already document
  the shipped APIs — only the top-of-file roster is stale.
- `StringBuilder.cap(self StringBuilder) int` (`std/string_builder.ptt:30`)
  is shipped and tested but missing from the spec at line 601.
  Add it.
- The "Required additions" math section at line 988 lists `clamp`,
  `gcd`, `lcm`, `pow_mod`, `sqrt_floor`. `std/math.ptt` also
  ships `min`, `max`, `abs`, `pow`, `int.hash`. Acknowledge them
  so an outsider doesn't think they're undeclared API.
- `std/option.ptt:13-15` and `std/result.ptt:13-15` claim the
  legacy `Option of T .Some(...)` value-form is "gone." Reality:
  it's gone in user code; the stdlib factories themselves (lines
  69, 75 in each file) still use it as the canonical
  implementation, allowed by the provenance check in
  `compiler/checker.c:720-731`. Reword to match.

Verification:

- `make test` green (doc-only change).

Commit: `docs(stdlib): reconcile checklist drift and Option/Result factory comments`

## Phase 2 — Tier-2 retired-feature sweep

Larger mechanical sweeps. Each is fully dead, but the diffs touch
many lines so they get their own commits.

### T05 — Drop legacy `list` / `map` / `imap` token + parser branches

The lexer comment at `compiler/lexer.c:70-76` already says these
tokens are "cosmetic compatibility" — and indeed the lexer
doesn't produce them anymore. The parser still has live-looking
branches that test for them; every test always returns false
at runtime. Coordinated removal:

- `compiler/token.h` — remove `TOK_LIST`, `TOK_MAP`, `TOK_IMAP`
  enum members.
- `compiler/parser.c` — delete dead branches at:
  - `:248-254` — list/map/imap factory call parsing (the
    `TOK_TASK` arm survives).
  - `:340-342` — type-position predicate accepting these tokens.
  - `:1019-1043` — var-decl annotation parsing for `list of T` /
    `map of K, V` / `imap of int, V`.
  - `:1356-1357` — type-name acceptance set.
  - `:1370-1376` — type-spelling fallbacks (`"list"`, `"map"`,
    `"imap"`).
  - `:1413-1421` — `next_starts_type` / `looks_like_param_boundary`
    heuristic.
  - `:1636-1638`, `:1656-1658` — receiver-type segment fallbacks.
- `compiler/lexer.c:70-76` — remove the explanatory comment.
- `compiler/checker.c:269-271` — delete dead
  `parse_type_str("list" / "map" / "imap")` arms.
- `compiler/checker.c:553-554` — delete legacy free-function
  `list()`, `list_new()`, `map()`, `map_new()`, `imap()` /
  `map_keys()` builtins (lines 553-554, 640).
- `compiler/checker.c:614-621` — delete free-function `len(x)`
  builtin (zero callers; everyone uses `.len()`).

Verification:

- `make test` green.
- `tests/errors/legacy_str_type.ptt` and any other expected-error
  test using legacy keyword forms keeps erroring (parser will now
  surface the error one step earlier — at the identifier-not-a-type
  layer instead of the dedicated branch).
- One Docker exec of `examples/test_algo.s` to confirm the Linux
  backend still produces identical output.

Commit: `refactor(compiler): drop dead legacy list/map/imap tokens and parser branches (ε2)`

### T06 — TYPE_STR fossil cleanup (default: rename-only)

If D2 = rename-only:

- Rename `TYPE_STR` → `TYPE_STRING` across `compiler/types.h`,
  `compiler/checker.c`, `compiler/irgen.c`, `compiler/iropt.c`.
  Touch every reader (~10 sites) at once.
- Delete the stale "γ7 TYPE_STR is no longer produced" comment
  at `compiler/checker.c:80-83`.
- Drop the dead `"str"` return-type mapping at
  `compiler/checker.c:2113`.

If D2 = full deletion (re-scope this task before starting):

- Same comment + return-type drops.
- Replace each `make_type(TYPE_STR)` with
  `make_struct("String")`.
- Delete the `TYPE_STR` enum member entirely.
- Re-route every `TYPE_STR` reader to the equivalent
  `TYPE_STRUCT && strcmp(name, "String") == 0` shape — or, more
  cleanly, introduce a small `is_string_type(Type *t)` predicate
  and route everything through that.

Verification:

- `make test` green.
- IR matrix at `-O0`/`-O1`/`-O2` all green.

Commit: `refactor(checker): rename TYPE_STR -> TYPE_STRING; drop dead str-return mapping`

## Phase 3 — Decision-gated work

### T07 — Delete `_task_collapse` end-to-end (if D1 = delete)

Three coordinated edits in one commit:

- `compiler/runtime_emit.c:136` — drop the `_task_collapse`
  emission inside `emit_task_builtins`.
- `compiler/checker.c:956` — drop the `collapse` arm of the
  task-method check.
- `compiler/irgen.c:795` — drop the `t.collapse()` lowering arm.

Verification:

- `make test` green (no test calls `.collapse()`).
- A user program that writes `t.collapse()` will now error at
  the checker with "unknown method." That's the desired
  behavior.

Commit: `refactor(compiler): drop unused .collapse() task method`

### T07' — Keep + document (if D1 = keep)

Append a `2026-05-20 — task.collapse() retained as roadmap` note
to `docs/design-decisions.md` recording that the symbol is
intentional and reserved for future task semantics.

Commit: `docs(design): record task.collapse() retention rationale`

## Phase 4 — Hot-path optimization

Each commit verifies on **both** targets: `make test` (Darwin)
and one Docker container exec of a representative program
(`examples/test_algo.s`) confirming the Linux backend output is
unchanged.

The order is intentional: smallest blast radius first; regalloc
last because regalloc bugs surface as silent runtime corruption.

### T08 — iremit string-pool: linear → hash

Site: `compiler/iremit.c:41-55`, called from line 352 per
`IR_LOAD_STR`.

Today: every literal lookup is `O(P)` where P = pool size.

Change: keep `g_string_pool.items[]` (stable emit order matters
for symbol layout) but back the intern check with a
`Hashmap<const char* → int>` from T01. New: `O(1)` per intern.

Verification:

- `make test` green.
- For `examples/hello.ptt`, generated `.s` is byte-identical
  before/after — the pool is content-deterministic, so emit
  order should not change.

Estimated win: 1-2% on `make test`. Confidence: high on
mechanics, medium on magnitude.

Commit: `perf(compiler): hash-back iremit string pool intern (linear scan -> O(1))`

### T09 — `EMIT_IR_TO_FILE` macro: struct-name hash

Sites: `compiler/main.c:1352-1666` — five inner walks
(`:1403-1413, :1497-1502, :1517-1523, :1661-1666`) plus
`struct_in_program()` at `:579`, all asking "is this name a
struct in the program?"

Change: build one `Hashmap<struct_name → struct_idx>` at the top
of `EMIT_IR_TO_FILE`, replace all five inner walks and
`struct_in_program()` with one hit each. New: `O(S × F)` per
emit instead of `O(S² × F)`.

Verification:

- `make test` green.
- Diff `tests/test_algo.s` before/after — should be byte-identical.

Estimated win: 1-2%. Confidence: high.

Commit: `perf(compiler): O(1) struct-name lookup in EMIT_IR_TO_FILE`

### T10 — irgen struct + field hashtables

Sites: `compiler/irgen.c:587, 609, 1096, 1311, 1475, 1488-1502, 1518` —
seven inner walks of `c->program->program.structs` to find a
field by name.

Change: at `irgen_generate` entry, build:

- `Hashmap<struct_name → StructInfo*>` — one entry per program
  struct.
- Per-struct `Hashmap<field_name → (offset, type)>` — built
  lazily on first lookup.

Plumb the table through `IRGenCtx`. Replace all seven walks with
hash hits.

Verification:

- `make test` green.
- IR matrix at all three opt levels green.
- Byte-identical IR output for `tests/test_algo.s` and
  `tests/ir/struct_field_after_call.ptt`.

Estimated win: 5-10% on `make test`. Confidence: medium.

Commit: `perf(compiler): O(1) struct + field lookup in irgen (was O(S*F) per access)`

### T11 — checker function table: fixed array → hash

Sites: `compiler/checker.c:6-7, 33-37, 207-225` —
`FuncInfo funcs[512]` linearly scanned by `find_func` and
`find_method`.

Change: replace the fixed array with
`Hashmap<(receiver_type, name) → FuncInfo*>`. Same lookup serves
both `find_func` (NULL receiver) and `find_method` (typed
receiver). Removes the brittle `MAX_FUNCS=512` cap.

Implementation note: receiver type can be encoded into the key
as a prefix (e.g., `"<recv>:<name>"`); the encoding is a private
hashmap-internal detail.

Verification:

- `make test` green.
- IR matrix at all three opt levels green.
- One Docker container exec to confirm Linux output unchanged.

Estimated win: 3-7%. Confidence: medium. Secondary win: removes
a future cliff (108 funcs / 512 cap = 21% utilization today; one
big stdlib import on top of leetcode is one growth-spurt away
from hitting the cap's `exit(1)`).

Commit: `perf(checker): hash-table function lookup (replace fixed funcs[512])`

### T12 — main.c alias-rewrite: O(local_count) → O(1)

Sites: `compiler/main.c:159-260` (`rewrite_calls_with_prefix`),
`:351-518` (`rewrite_alias_idents_walk`).

Today: per `use` declaration, walks every function body and at
each NODE_CALL does an O(local_count) strcmp scan.

Change: build a `Hashmap<local_name → 1>` once per imported file
at lines 1188-1197, pass into the walker. Same trick for the
`aliases[] / canonicals[]` map used by
`rewrite_alias_idents_walk`. New: O(nodes) per walk instead of
O(nodes × local_count).

Verification:

- `make test` green.
- Aliasing tests in `tests/leetcode/` still pass (those were the
  motivating cases for the alias-rewrite design).

Estimated win: 3-6%. Confidence: medium.

Commit: `perf(compiler): O(1) alias-rewrite call-site lookup during import merge`

### T13 — monomorph worklist: O(seen²) → O(seen)

Sites: `compiler/monomorph.c:792-805` (StrSet linear-scan dedupe),
`:1377-1431` (worklist outer loop scans `seen` linearly to find
unprocessed forms; inner template-table scans).

Change: three pieces:

- Back `StrSet` with a hashtable for `contains`. Keep the
  iteration array (`seen.items[]`) since iteration order matters
  for deterministic output.
- Add a `processed_index` cursor that advances monotonically
  through `seen`. New entries only ever append, so this turns
  the worklist into O(seen).
- Build a template-head `Hashmap<head → template_idx>` at the
  start of `monomorph_run`; replace the three linear lookups in
  `struct_templates`, `enum_templates`, `free_func_templates`.

Order preservation: critical — the iteration array must stay in
the same order the existing implementation uses, so emitted
struct order doesn't change. Diff one IR file before/after to
confirm.

Verification:

- `make test` green.
- IR matrix at all three opt levels green.
- Byte-identical `tests/test_two_sum_map.s` and one program with
  many monomorph instantiations (e.g.,
  `tests/leetcode/test_lru_cache.ptt`).

Estimated win: 1-3% on monomorph-heavy tests. Confidence:
medium-low on magnitude; high on the algorithmic shape being
unnecessary.

Commit: `perf(monomorph): hash-back worklist seen-set and template lookup`

### T14 — regalloc liveness: O(insts × vregs) → O(insts + vregs)

Sites: `compiler/regalloc.c:91-97` (per-back-edge linear search
for the header block), `:124-138` (per-IR_CALL inner-vreg scan
to mark `crosses_call`).

Highest-care commit. Regalloc bugs surface as silent runtime
corruption. Two changes:

- Header-block lookup. Build `label_to_block_idx[]` once per
  function (O(blocks) once). Replace the inner search with an
  array lookup. (Labels are dense small ints in this codebase,
  so a flat array works fine; no hash needed.)
- `crosses_call` analysis. Replace the per-IR_CALL inner-vreg
  scan with a prefix-sum `has_call_in[]` over instruction
  indices, computed once at Pass 1 time. Then for each vreg
  with live range `[start, end]`, `crosses_call =
  prefix[end] - prefix[start] > 0`.

Verification:

- `make test` green.
- IR matrix at all three opt levels green.
- One Docker exec confirms Linux backend still works.
- Byte-identical assembly output for one large test
  (`tests/test_two_sum_map.ptt` after monomorph).

If any one of those checks regresses, abort and bisect — do not
commit a regalloc change with even partial assembly drift.

Estimated win: 1-3% on programs with big functions. Confidence:
medium.

Commit: `perf(regalloc): linear-time crosses_call analysis and header-block lookup`

## Skipped opportunity

The hot-path agent flagged a SRA index opportunity in
`compiler/iropt.c:680-721` (sra_func's quadruple-nested rewrite
loop). Rejected for now: small estimated win (2-5% under -O1),
real risk of touching the optimizer's hottest loop. Revisit if a
profile justifies it.

## Acceptance gates

### Gate A — Phase 1 + 2 done

- `make test` green at every commit.
- No regressions in IR-matrix output (`-O0`, `-O1`, `-O2`).
- Linux backend still produces correct output (one Docker exec
  smoke test at end of phase).

### Gate B — Phase 3 done

- Decision recorded for D1 (either delete-T07 or document-T07').

### Gate C — Phase 4 done

- `make test` ends `All tests passed.`
- IR matrix at all three opt levels green.
- Linux backend smoke-test green (Docker container exec on
  `examples/hello.ptt`, `tests/test_algo.ptt`,
  `tests/leetcode/test_lru_cache.ptt`).
- Wall-clock comparison: time `make test` before T08 and after
  T14; record the result. Target: 15-25% improvement.

## Notes on confidence

- Tier 1 (T02-T04) deletions are mechanical and verified by the
  audit. Risk is near-zero.
- Tier 2 (T05-T06) sweeps are larger but still mechanical.
  Risk is in catching every dead branch — `make test` is the
  gate.
- Tier 4 (T08-T14) optimization commits each require the diff
  output to be byte-identical (or semantically identical with a
  documented reason) to the previous backend output.

## Recommended execution order

1. T01 (foundation — unblocks Phase 4).
2. T02 → T03 → T04 (Tier 1 — quick wins; build confidence).
3. T05 → T06 (Tier 2 — bigger sweeps).
4. Confirm D1 with user; T07 or T07'.
5. T08 → T09 → T10 → T11 → T12 → T13 → T14 (optimization;
   smallest blast radius first, regalloc last).

## Out of scope for this plan

- Self-hosting the compiler in Potato.
- Wiring `compiler/runtime/` into compiled output.
- Operator overloading, traits, async — already documented as
  not on the roadmap.
- Writing a `make test-linux-arm64` target that drives the same
  matrix through `container exec` or `docker exec`. Worth doing
  later but separate from this audit.
