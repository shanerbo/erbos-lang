# Potato QA, Peer Review, and Audit Charter

This file is the handoff for Codex sessions acting as the official QA,
peer reviewer, and auditor for the Potato language repository.

Read this after `CLAUDE.md` and before reviewing or changing compiler,
stdlib, test, or language-design code.

## Authority

This file is owned by Codex in the QA / peer reviewer / auditor role.
It is the canonical audit handoff for this repository.

Claude and other implementation agents may read this file, cite it, and
use it as review guidance, but they must not edit, delete, rename,
stage, or commit it. If an implementation agent believes this file is
wrong or stale, it should report the evidence and ask the auditor to
update it.

Only the Codex auditor may modify this file. That keeps one consistent
audit practice and prevents the implementation agent from rewriting the
acceptance criteria it is being reviewed against.

## Role

The auditor is responsible for protecting the language design, finding
behavioral bugs, and making sure implementation, tests, and docs agree.

The auditor is not a rubber stamp. When a change violates the documented
model, the auditor must say so directly, cite the exact evidence, and
propose the smallest coherent fix.

Primary duties:

- Verify that implementation matches `README.md`, `CLAUDE.md`, and
  `docs/design-decisions.md`.
- Review compiler changes for correctness, ownership safety, type-system
  soundness, import behavior, diagnostics, and regression coverage.
- Review stdlib changes for memory ownership, mutation visibility, bounds
  behavior, and API consistency.
- Review docs for contradictions with code and with the accepted design
  decisions.
- Keep a running TODO/checklist before multi-step work and update it as
  work proceeds.
- Never rely on naming, intention, or memory. Every claim must be backed
  by a file read, grep, build, test, or targeted repro.

## Review Rules

These rules are mandatory for any Codex session using this file.

1. Create a TODO list before starting any multi-step audit or
   implementation task.
2. Read the relevant docs before proposing language changes.
3. Use `rg` first for codebase searches.
4. Prefer exact file references and line numbers in review comments.
5. Do not assume behavior from names. Read the code path or run a repro.
6. Do not modify tests to match buggy behavior.
7. If fixing a bug, add a regression test that fails before the fix.
8. If changing syntax or semantics, update docs, examples, and tests in
   the same logical change.
9. Do not introduce hidden behavior, implicit conversions, auto-loaded
   modules, C preprocessor tricks, or `<T>` user-facing generic syntax.
10. Treat `make test` ending with `All tests passed.` as the minimum
    commit gate.

## Design Philosophy To Enforce

Potato is a small, explicit systems language that reads like English,
compiles to native ARM64 macOS code, and avoids GC, refcounting, libc,
and large runtime assumptions.

The core values from the docs:

- Reads like a notebook: `is`, `be`, `give`, `through`.
- Every user-visible decision is explicit.
- No magic conversions, hidden allocations, implicit copies, or
  auto-loaded modules.
- The compiler is honest: one backend, one optimizer, no preprocessor.
- Every feature pays rent by improving the first hour of use or
  preventing bugs in the first thousand lines.
- Heap ownership is single-owner by default.
- Heap moves are explicit with `is now`.
- Deep clones are explicit with `is rep`.
- Plain heap aliases are rejected.
- Borrows are lexical. No long-lived borrow bindings and no lifetime
  annotation system.
- Shared data should use arena plus integer index patterns.

Features that are explicitly not aligned with the current philosophy:

- Operator overloading.
- Traits/interfaces.
- Async/await as a language feature.
- Macros.
- Auto-imported stdlib modules.
- User-facing `<T>` generic syntax.
- `str` as sugar for `String`.

## Audit Status

The first audit was performed on 2026-05-17. It read the repository
documentation recursively and inventoried the compiler, stdlib, examples,
tests, and editor files. It used targeted probes with the existing
`./erbos` binary and produced the original finding list.

The finding ledger was reconciled by Codex on 2026-05-18 after reading the
current commits, the relevant docs, and running `make test` to completion.

At reconciliation time, `make test` ended with `All tests passed.` The
only remaining untracked path shown by `git status --short` was this
`AUDITING.md`; re-check `git status --short` before starting any new
review.

## Finding Ledger

### Fixed and accepted

These findings were verified as fixed by commit history, regression tests,
targeted repros, or the full `make test` gate. Re-check the named tests
before editing the same area.

- P0-1: Module-qualified calls are now type/arity checked.
  Fixed in `c845de5`. Covered by `tests/errors/alias_call_*.ptt`.
- P0-2: Call-site `ref` is preserved and validated.
  Fixed in `c845de5`, then strengthened in `73ae9b4` for receiver
  field/index chains. Covered by `tests/errors/missing_call_ref*.ptt`,
  `tests/errors/extra_call_ref.ptt`, `tests/errors/ref_self_via_*.ptt`,
  and `tests/test_ref_self_walk.ptt`.
- P0-3: Lexical block scope is enforced.
  Fixed in `ebf52d6`. Covered by `tests/errors/scope_*_after_block.ptt`
  and `tests/test_block_scope.ptt`.
- P0-4: Field reassignment is type-checked.
  Fixed in `5b336eb`. Covered by `tests/errors/field_assign_*.ptt`.
- P0-5: Explicit variable annotations are enforced.
  Fixed in `5b336eb`. Covered by `tests/errors/var_annotation_*.ptt`.
- P0-6: Unknown type names now error.
  Fixed in `5b336eb`. Covered by `tests/errors/unknown_type_*.ptt`.
- P0-7: Plain heap aliases are rejected for reassignment and field
  assignment.
  Fixed in `f9318ba`. Covered by `tests/errors/heap_reassign.ptt`,
  `tests/errors/heap_field_assign.ptt`, and `tests/test_heap_reassign.ptt`.
- P0-8: Recursive drop functions are generated for owned heap fields.
  Fixed in `ce79160`, with later borrow-aware String drop refinement in
  `9e215c1`. Covered by `tests/test_recursive_drop.ptt`,
  `tests/test_self_type_drop.ptt`, and `tests/test_spudlock_fixes.ptt`.
- P1-9: Missing-return analysis now checks control-flow paths.
  Fixed in `9e215c1`. Covered by `tests/errors/missing_return*.ptt` and
  `tests/test_must_return.ptt`.
- P1-10: Empty `List.pop` panics cleanly and `erbos run` propagates
  signal failures.
  Fixed in `9e215c1`. Covered by
  `tests/errors/empty_pop_panics.ptt` and
  `tests/test_empty_pop_panics.ptt`.
- P1-11: Transitive imports resolve from the importer directory and module
  identity follows resolved file paths.
  Fixed across `f945013`, `0dbe11d`, `484082f`, and `aab4262`. Covered by
  `tests/test_transitive_imports.ptt`,
  `tests/test_transitive_imports_dup_filenames.ptt`, and
  `tests/test_alias_shadowing.ptt`.
- P1-12: Same module imported under two aliases now produces a compile-time
  duplicate-alias error.
  Fixed in `f945013`. Covered by `tests/errors/dup_alias.ptt`.
- P1-14: Missing `use std/string` for String methods now reports a checker
  diagnostic.
  Fixed in `cb72b2a`. Covered by `tests/errors/missing_use_string.ptt`.
- P2-15: Docs no longer claim `String` is auto-loaded.
  Fixed in `c7aed74`.
- P2-16: Legacy `str` syntax is retired with a teaching parser error.
  Fixed in `1d3630b`. Covered by `tests/errors/legacy_str_type.ptt`.
- P2-18: `array`, `with`, and `cap` are documented as contextual words.
  Fixed in `61bdae5`.
- P2-19: Runtime test failures now propagate as non-zero through
  `make test`.
  Fixed in `c7aed74`; current `make test` ran the runtime tests and
  completed successfully.
- P2-20: Internal comments that implied user-facing `<T>` syntax were
  cleaned up while preserving internal mangled type strings.
  Fixed in `e689cf0`.

### Partially fixed

- P1-13: Paths with spaces now work because assembler/linker/run path
  arguments are quoted.
  Fixed for ordinary spaces in `cb72b2a`, but still partial: the compiler
  still uses shell command strings and paths containing a literal `'` break.
  The full fix remains argv-based process spawning.

### Still open / active audit items

- P0/P1: Method-call return aliases can still mutate caller storage through
  non-`ref` parameters.
  Confirmed repro:
  `touch(xs List of Counter) { xs.get(0).bump() }` mutates `xs` even though
  `xs` is non-`ref`. This is documented in `docs/design-decisions.md` as
  task `#142`. A correct fix needs method return-alias metadata such as
  "returns fresh" versus "returns alias of self/argument".
- P1/P2: Named-arg `String` constructor leak status needs doc
  reconciliation.
  `docs/design-decisions.md` still documents this as a deterministic bounded
  leak, but `make test-leaks` now verifies the focused constructor lowering
  does not call `_alloc_String` for the overwritten literal field. Review the
  implementation and update the design log before declaring the item fully
  closed.
- P1/P2: Shell command construction should be removed.
  Even after the path-with-spaces fix, `system()` remains a weak point for
  unusual path characters and command-injection class bugs. Replace with
  `posix_spawn`/`exec` argv calls and explicit status handling.
- P2: Legacy `TYPE_UNKNOWN` and `_yell` behavior should continue shrinking.
  Many concrete unknown-type paths now error, but this remains an audit
  watch item until transitional unknown-type successes and magic dispatch
  paths are intentionally eliminated or documented.
- P2: `AUDITING.md` itself is currently Codex-owned and untracked.
  That is acceptable only if the intended policy is local-only audit
  memory. If other sessions must reliably inherit it from git, Codex needs
  an explicit decision to stage/commit it without allowing implementation
  agents to edit it.

### Regression gates added after reconciliation

- `tests/errors/ref_self_via_list_get_return.ptt` is a permanent
  compile-fail guard for task `#142`: a `ref self` method call through
  `List.get` on a non-`ref` parameter must be rejected.
- `make test-paths` is included in `make test` and creates a source path
  containing both a space and a literal apostrophe. It currently fails until
  shell command construction is replaced with argv-based process spawning.
- `make test-leaks` is included in `make test` and lowers
  `tests/leaks/named_arg_string_literal.ptt` to assembly, then verifies the
  named-arg constructor body does not call `_alloc_String` for a field that
  is immediately overwritten by a string literal.

## Language Overhaul Checklist

Use this checklist for planning language work. Each item should have
compiler changes, docs, examples if user-facing, and regression tests.

### Ownership completeness

- Enforce heap ownership for var decls, assignments, field assignments,
  constructor fields, returns, and container operations.
- Require explicit move or clone syntax for heap-shaped values.
- Invalidate moved sources consistently.
- Generate recursive drop functions for owned fields.
- Drop replaced field values before overwrite.

Value:

- Makes the core language model true everywhere.
- Prevents hidden aliases, leaks, use-after-free, and double-free paths.

### Explicit mutation

- Preserve call-site `ref` in AST.
- Check `ref` for direct calls, methods, module calls, and generic
  instantiations.
- Reject mutation through non-`ref` heap params.

Value:

- Keeps source code honest about mutation.
- Matches README examples and the documented calling convention.

### Type-system hardening

- Unknown types must error.
- Explicit annotations must be constraints.
- Field writes must match field types.
- Return paths must be proven for non-void functions.
- `nil` compatibility should be narrow and documented.

Value:

- Removes accidental C-like behavior.
- Makes static types meaningful.

### Import/module correctness

- Resolve transitive imports relative to the importing file.
- Type-check alias-qualified calls.
- Decide and enforce duplicate-alias policy.
- Keep no auto-loads.

Value:

- Makes multi-file projects predictable.
- Preserves explicit imports without linker surprises.

### Diagnostics

- Add caret/source-context errors.
- Report missing imports at checker time.
- Include searched import paths when resolution fails.
- Prefer user syntax in messages, not internal mangled names.

Value:

- Improves first-hour usability, which the design docs say is worth
  paying for.

### `Result of T to E`

- Implement word-style result syntax from the roadmap.
- Use it for operations that can fail without crashing.
- Keep it explicit; do not add exceptions.

Value:

- Enables serious programs while matching the language philosophy.
- Supports safer stdlib APIs.

### File I/O

- Add explicit File I/O after or alongside `Result`.
- Return `Result` from fallible file operations.
- Avoid ambient globals or hidden runtime state.

Value:

- Makes Potato useful for real command-line tools.
- Already appears in the roadmap.

### Fail-fast builtin

- Add a small `panic` or equivalent fail-fast primitive for impossible
  states and stdlib invariant failures.
- It should print a clear message and exit non-zero.
- It should not become exceptions or recoverable control flow.

Value:

- Lets stdlib keep honest contracts before every API has a checked
  result form.

### Stdlib safety APIs

- Add checked `List` and `Map` operations.
- Prefer names that read clearly in Potato style.
- Avoid traits or operator overloading.

Value:

- Gives users safe defaults without expanding the type system too far.

### Default debug output

- Generate default `_Type_yell` or equivalent debug formatting as
  described in the roadmap.
- Keep custom formatting explicit if added later.

Value:

- High first-hour value.
- Helps tests and examples without adding complex formatting traits.

### Runtime decision

- The green-thread runtime exists but is not wired into compiled Potato
  programs.
- Do not add task syntax until runtime tests are real and the ownership
  model for tasks/channels is designed.
- It is acceptable to keep the runtime parked if it does not yet pay
  rent.

Value:

- Prevents async/runtime complexity from undermining the small-language
  goal.

## Required Regression Tests For First Fix Pass

Add compile-fail or run tests for these before claiming the first audit
phase is complete:

- Alias call arity mismatch.
- Alias call wrong argument type.
- Missing call-site `ref` for a ref parameter.
- Extra call-site `ref` for a non-ref parameter.
- Mutation through non-ref heap parameter.
- Accessing a block local after block exit.
- Accessing a heap block local after block exit.
- Assigning wrong type to struct field.
- Explicit var annotation mismatch.
- Unknown parameter, return, field, and variable type names.
- Heap reassignment without `now` or `rep`.
- Field heap assignment without `now` or `rep`.
- Missing return path in non-void function.
- Empty `List.pop`.
- Out-of-bounds `List.get`.
- Transitive sibling import.
- Same module imported under two aliases.
- Source path containing spaces.
- String method without `use std/string`.
- Legacy `str` syntax, once removal is chosen.

## Review Output Format

For code reviews, lead with findings ordered by severity.

Each finding should include:

- Severity: P0, P1, P2, or P3.
- Exact file and line reference.
- What breaks.
- Why the behavior contradicts docs, tests, or code invariants.
- A minimal fix direction.
- Whether a regression test is required.

After findings, include open questions and then a short summary. If there
are no findings, say that directly and list any residual test gaps.

## Continuation Checklist For Future Codex Sessions

1. Run `git status --short`.
2. Read `CLAUDE.md`.
3. Read this file.
4. For language changes, read `docs/design-decisions.md` before
   proposing anything.
5. Use `rg` to find all code paths for the construct being changed.
6. Create and maintain a TODO plan.
7. Add failing tests first or at least prove the bug with a targeted
   repro before editing.
8. Make the smallest coherent change.
9. Run the narrow test first, then `make test` before final handoff.
10. If an audit finding is fixed, invalidated, or replaced by a better
    diagnosis, report the evidence to the Codex auditor so this file can
    be updated by the file owner.
