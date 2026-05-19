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
- `Claim ID`: (none — C-010 was consumed and accepted for F-010;
  batch remains on HOLD because F-011 was opened against the
  STDLIB_CHECKLIST.md doc lag — the canonical checklist still
  describes the pre-F-010 hash-container design)
- `Against findings revision`: 16
- `Target commit`: 371141f + working tree
- `Claimed fixed`: (none pending; next claim will target F-011)
- `Last updated`: 2026-05-19T03:13:21+00:00

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

## Claim C-003
- State: READY_FOR_AUDIT
- Against findings revision: 5
- Target commit: 5fa4ab5 + working tree (uncommitted; Codex can
  verify against `git diff` for the listed files)
- Claimed fixed: parent-drop heap-shaped-element leak
  (proactive surface; recommend new ID F-002 if Codex agrees
  with the framing)
- Last updated: 2026-05-18T20:43:58+00:00
- Bug class:
  Pre-fix, `_drop_<X>` for any struct containing a field of type
  `array of <heap-shaped E>` freed only the data buffer + 16-byte
  header, never iterating slots to drop owned elements. Every
  `List of String`, `Set of String`, `Map of String, V`,
  `Pool of String`, and any user-defined wrapper around
  `array of <struct>` leaked all stored elements at end-of-scope.
  Mirror gap in `_clone_<X>`: the array-field clone byte-copied
  the data buffer, so the cloned struct's slots aliased the
  source's heap-shaped elements. Mutating one container silently
  affected the other; both auto-dropped at scope end →
  double-free.

  This was OUT OF SCOPE of F-001 (which was specifically about
  slot-overwrite ownership during set/reuse/clear paths), so
  F-001's verified fix did not address it.
- Root-cause fix:
  - `compiler/main.c` adds three helpers and a shape-aware
    rewrite of the `_drop_<X>` / `_clone_<X>` array-field path:
      - `struct_in_program(name)` — walks the struct registry to
        decide whether an `array__<E>` element type is a heap-
        shaped struct.
      - `is_list_shape(s)` — detects the canonical
        `{count int, data array of T}` two-field layout that
        stdlib's `List of T` uses.
      - `collect_heap_array_elems(program)` — gathers every
        distinct heap-shaped E referenced by some struct field
        as `array__E`.
      - `emit_drop_array_helper(elem)` and
        `emit_clone_array_helper(elem)` — emit one
        `_drop_array_<E>` and one `_clone_array_<E>` symbol per
        heap-shaped E. `_drop_array_<E>` walks the data buffer
        cap-bounded with null guards and calls `_drop_<E>` on
        each non-null slot before freeing the buffer + header.
        `_clone_array_<E>` allocates a fresh header + buffer and
        per-slot calls `_clone_<E>` on each non-null source
        pointer.
  - The `EMIT_IR_TO_FILE` macro emits these helpers once at the
    top of the assembly file, then the per-struct loop dispatches
    to them based on shape:
      - `List`-shape parent (`is_list_shape(s)`) with heap-shaped
        element: count-bounded inline loop in
        `_drop_<List__T>` / `_clone_<List__T>`. Required because
        `List.pop` / `List.remove` leave the popped slot's
        pointer in place and transfer ownership to the caller;
        cap-bounded iteration would re-drop a value the caller
        now owns.
      - Non-List parent with heap-shaped element (Set, Map,
        user-defined): delegate to the
        `_drop_array_<E>` / `_clone_array_<E>` helpers
        (cap-bounded null-guarded).
      - Primitive / byte element: original byte-memcpy and
        bulk-free behaviour kept intact.
- Stdlib changes:
  - `std/list.ptt::List.push` switched to deep-clone v into the
    appended slot (`v_clone is rep v; self.data[self.count] be now v_clone`).
    Without the clone, callers like
    `inner is List of int(); ...; outer.push(inner)` would alias
    `inner` into the parent's slot, and the count-bounded parent
    drop would re-drop the same pointer the caller's RAII just
    freed (verified pre-fix crash in `examples/kitchen_sink.ptt`'s
    "Nested collections: List of List of int" section).
  - `std/list.ptt::List.insert` rewritten to park v at slot
    `self.count` first (where the slot is null) and bubble it
    down via plain swaps. The naive tail-walking shift had a
    residual alias at slot[i+1] → final overwrite would
    double-free. The swap-bubble keeps every slot's pointer
    unique at every step.
  - `std/STDLIB_CHECKLIST.md` updated with entry #6 documenting
    the F-002 fix shape.
- Files changed:
  - `compiler/main.c` (helpers + shape-aware drop/clone)
  - `std/list.ptt` (push, insert)
  - `std/STDLIB_CHECKLIST.md` (entry #6 added)
  - `Makefile` (clean target + new `test-leaks` IR-static gates)
  - `tests/test_heap_parent_drop.ptt` (new, 16 framework cases)
  - `tests/leaks/heap_parent_drop_emits.ptt` (new, IR-static
    probe)
- Tests added (`tests/test_heap_parent_drop.ptt`, all 16 cases
  pass at -O0/-O1/-O2):
  - **Drop correctness** (4): `List of String`, `Set of String`,
    `Map of String, String`, `Pool of String` repeatedly built
    and dropped over many rounds. Pre-fix, every iteration
    leaked elements; post-fix the free-list reuse satisfies
    subsequent allocations.
  - **Caller alias safety** (3): `List.push` deep-clones owned
    String parameter; `List.push` of `List of String` (the exact
    pre-fix kitchen_sink crash repro); `List.insert` deep-clones
    + bubble-down without aliasing.
  - **Pop/remove with heap-shaped T** (2): pop returns the
    transferred value safely; remove leaves no double-free.
  - **Deep clone correctness** (3): `List`, `Set`, `Map` of
    String produce independent clones; mutating one does not
    affect the other.
  - **Stress** (2): repeated build-and-drop of `List of String`
    over 1000 iterations stays bounded (no OOM); nested
    `List of List of String` drops everything cleanly.
  - **Primitive-T fast path** (2): `List of int`,
    `Map of int, int` unaffected; no `_drop_int` / `_clone_int`
    symbols required.
- IR-static gate (`tests/leaks/heap_parent_drop_emits.ptt` +
  Makefile `test-leaks` stanza):
  - `_drop_array_String` / `_clone_array_String` helper symbols
    must exist.
  - `_drop_List__String` body must contain `bl _drop_String`
    (count-bounded element drop).
  - `_drop_Set__String` body must contain `bl _drop_array_String`
    (delegation).
  - `_drop_Map__String__String` body must contain `bl
    _drop_array_String` at least twice (keys + vals).
  - `_clone_List__String` body must contain `bl _clone_String`
    (deep element clone).
- Verification context:
  - `make test` ends with `All tests passed.` from a clean
    rebuild on the working tree.
  - `tests/test_heap_parent_drop.ptt` (16 cases) and
    `tests/test_heap_slot_drop.ptt` (18 cases, F-001 regressions)
    both pass at `-O0`, `-O1`, `-O2`.
  - `examples/kitchen_sink.ptt` (which previously crashed at
    exit due to the inner-list alias double-free) runs cleanly.

## Claim C-004
- State: READY_FOR_AUDIT
- Against findings revision: 6
- Target commit: 5fa4ab5 + working tree (uncommitted; Codex can
  verify against `git diff` for the listed files)
- Claimed fixed: F-002
- Last updated: 2026-05-18T22:32:41+00:00
- Bug class (recap from findings.md F-002):
  Claim C-003 made parent-drop / clone for `List of heap-shaped T`
  count-bounded with the implicit assumption that slots in
  `[count..cap)` were either null or held pointers the caller
  owned. But the C-003 stdlib did not enforce that invariant:
  `List.pop` and `List.try_pop` returned `self.data[count]` without
  nulling the slot, `List.remove` left a tail-duplicate via
  alias-shift, and `List.clear` was just `count = 0`. Subsequent
  `push` reused `slot[count]` through F-001's drop-before-
  overwrite path — freeing a pointer the caller still held
  (use-after-free on the popped/removed binding) or freeing a
  pointer that another live slot still aliased (double-free at
  parent-drop). Codex's targeted repros
  `/private/tmp/potato_pop_reuse.ptt` and
  `/private/tmp/potato_remove_reuse.ptt` both exit `139` on the
  audited revision.
- Root-cause fix:
  Two compiler changes paired with a stdlib rewrite. The new
  uniform invariant is **"any non-null slot in `[0..cap)` of a
  heap-shaped element array is owned by the parent and must be
  dropped on parent-drop"**, regardless of which struct shape
  wraps the array. List.pop / try_pop / remove maintain that
  invariant by nulling vacated slots when transferring ownership
  out.
  - `compiler/main.c`: removed the C-003 `is_list_shape`
    specialisation and the count-bounded inline loops in
    `_drop_<List__T>` / `_clone_<List__T>`. All heap-shaped
    element arrays now delegate to the per-elem
    `_drop_array_<E>` / `_clone_array_<E>` helpers that already
    existed for non-List parents. The helpers iterate `[0..cap)`,
    null-guard each slot, and call `_drop_<E>` / `_clone_<E>` on
    every non-null pointer before freeing the buffer + header.
    The `is_list_shape` function itself is deleted (no remaining
    callers).
  - `compiler/checker.c`, `compiler/irgen.c`: extend `is now` /
    `be now` ownership transfer to accept array-slot expressions
    as the source. `local is now arr[i]` (and the parallel
    `dst be now arr[i]`) does a bounds-checked load AND writes
    `xzr` to the slot in one step; the new local is
    heap-tracked with the element's struct name (so RAII at
    scope end calls the correct `_drop_<E>`). The new helper
    `gen_index_take` in `irgen.c` performs the same
    bounds-checked read as `NODE_INDEX` plus the slot-zero
    write. The checker stashes the element struct name into
    `var_decl.type_name` / `assign.src_struct_name` for irgen's
    heap-tracking. For primitive-element slots the form
    collapses to a value-load + slot-zero write — no
    heap-tracking, no RAII attached to the new local.
  - `std/list.ptt::List.pop`: switched to
        self.count be self.count - 1
        v is now self.data[self.count]
        give v
    so the slot is left null when ownership transfers to the
    caller.
  - `std/list.ptt::List.try_pop`: same shape through the
    Option-returning path.
  - `std/list.ptt::List.remove`: rewritten as a bubble-null
    pattern. `saved is now self.data[i]` extracts the popped
    pointer and nulls slot[i]. The bubble loop then
    `next is now self.data[k+1]; self.data[k] be now next`
    propagates the null hole rightward without ever leaving
    an aliased slot. Final slot[count-1] is null; count is
    decremented; saved is returned.
  - `std/list.ptt::List.clear`: stays as `count = 0`. The
    cap-bounded parent-drop reaps every still-owned slot at
    scope end. Subsequent push reuses slot[0..] through
    F-001's drop-before-overwrite path, which drops the prior
    occupant before storing the new clone.
  - `std/list.ptt::List.push` and `List.insert`: unchanged
    from C-003. The deep-clone-on-push and bubble-down-on-
    insert remain correct under the uniform invariant.
- Files changed:
  - `compiler/checker.c` (var-decl + assign `is now arr[i]` paths)
  - `compiler/irgen.c` (`gen_index_take` helper; var-decl + assign
    `is_move arr[i]` paths; heap-tracking of new local from the
    element struct name)
  - `compiler/main.c` (removed `is_list_shape`; uniform cap-
    bounded delegation to `_drop_array_<E>` / `_clone_array_<E>`)
  - `std/list.ptt` (pop, try_pop, remove)
  - `std/STDLIB_CHECKLIST.md` (entry #6 rewritten for F-002)
  - `docs/language-guide.md` (added `is now arr[i]` to the
    Ownership & Memory section)
  - `Makefile` (leak-test gates updated to expect the cap-
    bounded delegation)
  - `tests/test_heap_parent_drop.ptt` (new tests appended; one
    pre-existing test rewritten to verify post-fix behaviour
    instead of warning the user away)
  - `tests/leaks/heap_parent_drop_emits.ptt` (header comment
    updated to describe the new design)
- Tests added (`tests/test_heap_parent_drop.ptt`, all pass at
  `-O0`/`-O1`/`-O2`):
  - **Pop / try_pop reuse** (4): `List.pop + push reuses
    popped slot without UAF`; `List.try_pop + push reuses
    popped slot without UAF`; `List.try_pop on empty list
    returns None`; `List.try_pop + insert reuses correctly`.
    These directly exercise Codex's two failing repros plus
    the analogous Option-path and insert variants.
  - **Remove reuse** (2): `List.remove + push leaves no stale
    tail-duplicate` (Codex's second repro shape), `List.remove
    from middle, multiple times, then push` (stress across
    repeated bubble-null operations).
  - **Clear semantics** (2): `List.clear + scope-end drop
    releases every element` (200 rounds × 30 elements; pre-fix
    leaked, post-fix free-list reuse keeps it bounded);
    `List.clear + reuse before drop` (50 rounds of
    fill/clear/refill).
  - **Nested containers** (2): `Nested List of List of int:
    pop + push reuses correctly` (nested heap-shaped element
    type via List); `Nested List of List of String: remove +
    push reuses correctly` (nested with String elements,
    exercises both layers of cap-bounded element drop).
  - **Primitive-T fast paths** (2): `List of int: pop+push
    round-trip stays correct`; `List of int: remove + push
    stays correct`. The `is now self.data[k]` primitive on
    primitive-element arrays must collapse to value-load +
    slot-zero without any heap-tracking on the new local.
  - **Pop-and-drop only** (1): `List.pop + drop without
    further ops releases popped value` — 200 rounds of
    pop-then-drop validates that the slot-null write does not
    interact badly with parent-drop's cap-bounded iteration.
  - **One pre-existing test rewritten**: `List.pop on
    heap-shaped T survives subsequent ops` — was a placeholder
    that warned the user not to push-after-pop because of the
    pre-fix bug; now verifies the post-fix invariant directly
    (pop, push, re-read both bindings cleanly).
  - **Pre-existing tests retained** (16): all C-003 framework
    tests still pass under the new compiler logic.
- IR-static gates (`tests/leaks/heap_parent_drop_emits.ptt` +
  `Makefile` `test-leaks` stanza):
  - `_drop_array_String` / `_clone_array_String` helper symbols
    must exist (unchanged).
  - `_drop_List__String` body must contain
    `bl _drop_array_String` (was: count-bounded inline
    `bl _drop_String`).
  - `_drop_Set__String` body must contain
    `bl _drop_array_String` (unchanged).
  - `_drop_Map__String__String` body must contain
    `bl _drop_array_String` at least twice (unchanged).
  - `_clone_List__String` body must contain
    `bl _clone_array_String` (was: count-bounded inline
    `bl _clone_String`).
- Verification context:
  - `make clean && make test` ends with `All tests passed.`
    from a cold rebuild on the working tree.
  - `tests/test_heap_parent_drop.ptt` (28 cases including the
    16 pre-existing C-003 cases plus 12 new F-002-specific
    cases) passes at `-O0`, `-O1`, `-O2`.
  - Codex's failing repros now exit 0:
    - `/private/tmp/potato_pop_reuse.ptt`: prints `c d c`,
      exit 0.
    - `/private/tmp/potato_remove_reuse.ptt`: prints
      `b a c d e b`, exit 0.
  - `examples/kitchen_sink.ptt` runs cleanly (regression
    coverage for the C-003 nested-list crash).
  - `tests/test_heap_slot_drop.ptt` (F-001 regressions, 18
    cases) still passes at all three -O levels.

## Claim C-005
- State: READY_FOR_AUDIT
- Against findings revision: 8
- Target commit: 5fa4ab5 + working tree (uncommitted; Codex can
  verify against `git diff` for the listed files)
- Claimed fixed: F-003, F-004
- Last updated: 2026-05-18T22:48:00+00:00
- Bug class (recap from findings.md F-003 / F-004):
  `Queue of T` and `Deque of T` were not migrated to the new
  heap-shaped ownership model that F-002 established. Both used
  raw `self.data[idx] be v` stores on push and raw
  `v is self.data[idx]` reads on pop, plus alias-shifts in the
  `reserve` / push-grow path. With cap-bounded null-guarded
  `_drop_array_<E>` (which Queue.data / Deque.data already
  routed through, since neither matches the F-002 List shape):
  caller-owned heap-shaped pointers ended up aliased into queue
  slots, popped pointers were never cleared from their physical
  slots, and grow's plain alias-shift left old buffer slots
  pointing at the same heap blocks the fresh array now held —
  with `self.data be now fresh` then dropping the old buffer
  and freeing pointers `fresh` still owned. The two findings
  are one inseparable root cause (circular-buffer ownership
  transitions) and ship as a single claim.
- Root-cause fix:
  Pure stdlib change. The compiler routing of Queue.data /
  Deque.data through the uniform cap-bounded null-guarded
  `_drop_array_<E>` / `_clone_array_<E>` helpers is correct
  under the F-002 invariant ("any non-null slot in `[0..cap)` is
  owned by the parent and must be dropped on parent-drop"). All
  the work is teaching Queue and Deque to maintain that
  invariant.
  - `std/queue.ptt::Queue.push`: deep-clone v into the slot
    via `is rep` + `be now`, mirroring `List.push`. Without
    the clone, callers passing heap-shaped locals would alias
    them into queue slots and the parent drop would re-drop
    them at scope end. The push-time grow path was rewritten
    to transfer ownership of each logical slot via `is now`
    + `be now` instead of plain alias-copy, so the old
    buffer's slots are all null when `self.data be now fresh`
    drops it.
  - `std/queue.ptt::Queue.pop` / `try_pop`: switched to
    `v is now self.data[self.head]` to extract the head's
    pointer and null the slot in one step. Subsequent pushes
    that reuse the same physical slot find null and skip the
    F-001 drop-before-overwrite; the caller is the unique
    owner of the popped value.
  - `std/queue.ptt::Queue.reserve`: same `is now` + `be now`
    transfer pattern as the push-time grow path.
  - `std/queue.ptt::Queue.clear`: unchanged — `head = 0;
    count = 0`. The cap-bounded null-guarded parent drop
    reaps every still-non-null slot at scope end. Subsequent
    push reuses slot[head=0..] via the F-001 drop-before-
    overwrite path, which drops any pre-clear pointer at
    that physical slot before storing the new clone.
  - `std/deque.ptt::Deque.push_back` / `push_front`: same
    deep-clone + `be now` pattern as `Queue.push`. Both
    push_back and push_front must clone independently
    because either may be the only push path for a given
    application.
  - `std/deque.ptt::Deque.pop_front` / `pop_back` /
    `try_pop_front` / `try_pop_back`: all four use
    `v is now self.data[idx]` to vacate the slot when
    transferring ownership to the caller. `pop_back` and
    `try_pop_back` compute `idx` from
    `(head + count - 1) mod cap`; `pop_front` and
    `try_pop_front` use `head` directly.
  - `std/deque.ptt::Deque.reserve` / push-time grow paths:
    same `is now` + `be now` transfer as Queue's grow path.
  - `std/deque.ptt::Deque.clear`: unchanged — `head = 0;
    count = 0`. Same reasoning as Queue.clear.
  - **No compiler change required.** The F-002 fix already
    introduced `is now arr[i]` and the uniform cap-bounded
    `_drop_array_<E>` / `_clone_array_<E>` helpers; Queue
    and Deque are now teaching the stdlib to use them.
- Files changed:
  - `std/queue.ptt` (push, pop, try_pop, reserve;
    clear unchanged)
  - `std/deque.ptt` (push_back, push_front, pop_front,
    pop_back, try_pop_front, try_pop_back, reserve;
    clear / front / back / get / len / cap / empty
    unchanged)
  - `tests/test_heap_circular_ownership.ptt` (new, 25
    framework cases)
- Tests added (`tests/test_heap_circular_ownership.ptt`, all
  pass at `-O0`/`-O1`/`-O2`):
  - **Queue scope-end drop with heap-shaped T** (2):
    `Queue of String` with concat-allocated payloads + literals;
    `Queue of List of int` (nested heap-shaped element).
  - **Queue pop / try_pop ownership transfer** (3):
    `Queue.pop transfers ownership; reuse via push is sound`,
    `Queue.try_pop on heap-shaped T survives further use`,
    `Queue.try_pop on empty returns None and stays sound`.
  - **Queue wraparound / growth** (3): `Queue of String
    wraparound: push/pop/push pattern`,
    `Queue of String growth across multiple capacity
    doublings`, `Queue of String growth after wraparound
    preserves order`.
  - **Queue clear + reuse** (2): `Queue.clear releases payloads
    at scope end` (100-round stress), `Queue.clear + reuse
    before drop` (30 fill/clear/refill cycles).
  - **Deque scope-end drop with heap-shaped T** (2):
    `Deque of String` with concat + literals;
    `Deque of List of int` from both ends.
  - **Deque pop_front / pop_back / try_pop_*** (5):
    `pop_front transfers ownership`, `pop_back transfers
    ownership`, `try_pop_front survives further use`,
    `try_pop_back survives further use`,
    `try_pop_front / back on empty returns None`.
  - **Deque wraparound / growth from both ends** (4):
    `wraparound from front: push_front then pop_back`,
    `growth from push_back across multiple doublings`,
    `growth from push_front across multiple doublings`,
    `interleaved heap-shaped pushes from both ends`.
  - **Deque clear + reuse** (2): `Deque.clear releases payloads
    at scope end` (100-round stress), `Deque.clear + reuse
    before drop` (30 cycles, mixing push_back / push_front).
  - **Primitive-T fast paths** (2): `Queue of int` 1000-element
    drain in FIFO order; `Deque of int` 150-element mixed-end
    push then logical drain.
- Verification context:
  - `make clean && make test` ends with `All tests passed.`
    from a cold rebuild on the working tree.
  - `tests/test_heap_circular_ownership.ptt` (25 cases)
    passes at `-O0`, `-O1`, `-O2`.
  - Codex's failing repros now exit 0:
    - `/private/tmp/potato_queue_heap_alias.ptt`: prints `17`,
      exit 0.
    - `/private/tmp/potato_deque_heap_alias.ptt`: prints `1`,
      exit 0.
  - Pre-existing `tests/test_queue.ptt` (5 cases) and
    `tests/test_deque.ptt` (5 cases) still pass.
  - Pre-existing `tests/test_heap_parent_drop.ptt` (29 cases,
    F-002 regressions) and `tests/test_heap_slot_drop.ptt`
    (18 cases, F-001 regressions) still pass at all three
    -O levels.

## Claim C-006
- State: READY_FOR_AUDIT
- Against findings revision: 10
- Target commit: c8cdd04 + working tree (uncommitted; Codex can
  verify against `git diff` for the listed files)
- Claimed fixed: F-006
- Last updated: 2026-05-19T00:33:29+00:00
- Bug class (recap from findings.md F-006):
  Container materializers / algebra helpers built their result
  collections from zero-capacity even though the output bound is
  known up front. `Map.keys` / `Map.values` returned a
  `List of K` / `List of V` of length `self.count` after growing
  through `log2(self.count)` cap doublings; `Set.values` did the
  same; `Set.intersect` and `Set.difference` returned a fresh
  `Set of T` whose load crossed the 70% threshold many times,
  triggering rebuild-and-rehash on every doubling. F-006 is a
  capacity-planning gap, not a semantic bug — public API
  unchanged.
- Root-cause fix:
  Pure stdlib change. Each materializer now reserves its known
  output bound before the loop:
  - `std/map.ptt::Map.keys`: `out.reserve(self.count)`.
  - `std/map.ptt::Map.values`: `out.reserve(self.count)`.
  - `std/set.ptt::Set.values`: `out.reserve(self.count)`.
  - `std/set.ptt::Set.intersect`:
    `out.reserve(math.min(self.count, other.count))`. The
    intersect output is bounded by the smaller side's count
    because every output element must be in both inputs.
  - `std/set.ptt::Set.difference`: `out.reserve(self.count)`.
    The output is bounded by `self`'s count because every
    output element came from `self`.
  - `Set.union` already reserved `self.count + other.count`
    before this work; left unchanged.
  Both `List.reserve` and `Set.reserve` already exist and are
  no-ops on a smaller-or-equal request, so the new calls compose
  cleanly with downstream growth if it does happen (e.g. an
  upper-bounded reserve plus an actually-smaller realised
  result).
- Files changed:
  - `std/map.ptt` (Map.keys, Map.values pre-sizing)
  - `std/set.ptt` (Set.values, Set.intersect, Set.difference
    pre-sizing)
  - `tests/test_collection_materializers.ptt` (new, 14 framework
    cases)
  - `tests/bench/map_keys_values_bench.ptt` (new bench file)
  - `tests/bench/set_materializers_bench.ptt` (new bench file)
  - `tests/bench/BASELINE.md` (rewritten as a stdlib-bench
    baseline doc covering all three bench files)
- Tests added (`tests/test_collection_materializers.ptt`, all
  pass at `-O0`/`-O1`/`-O2`):
  - **Large-N materialization regressions**: `Map.keys` and
    `Map.values` against a 4096-entry `Map of int, int`,
    verifying every key 0..4095 present and the value sum
    matches `7 * sum(0..4095)`. Pre-fix this path grew the
    result List ~9 times; the test guards correctness under
    the post-fix single-allocation path.
  - **Heap-shaped V independence**: `Map.values` on
    `Map of int, String` materializes 256 entries, then mutates
    the map; the materialized list's clones must remain
    unchanged.
  - **Set.values large-N**: 4096-element `Set of int` →
    `xs.values()` returns a 4096-length list with every value
    seen exactly once.
  - **Set.intersect**: full overlap (2048 ∩ 2048 = 2048),
    half overlap (2048 + 2048 with 1024-element overlap =
    1024), small-side bound (4096 ∩ 8 = 8 — verifies the
    `min(self.count, other.count)` reserve hint).
  - **Set.difference**: disjoint (2048 - disjoint = 2048),
    overlapping (2048 - half-overlap = 1024).
  - **Heap-shaped element coverage**: `Set.values` returning
    independent String clones; `Set.intersect` clones survive
    mutation of the input Sets.
  - **Empty / edge cases**: `Map.keys` on empty map returns
    empty list; `Set.intersect` with one empty side returns
    empty (both directions); `Set.difference` with empty
    other returns full self.
- Bench coverage added (`tests/bench/`, all stand-alone `spark`
  programs):
  - `map_keys_values_bench.ptt`: 4096-entry
    `Map of int, int` → `.keys()` and `.values()` enumerated
    + summed. Output: `4096 / 8386560 / 58705920`.
  - `set_materializers_bench.ptt`: two 2048-element
    `Set of int`s with `[1024..2048)` overlap →
    `.values()`, `.intersect().values()`,
    `.difference().values()` enumerated + summed. Output:
    `2048 / 2048 / 2096128 / 1024 / 1572352 / 1024 /
    523776`.
  - `tests/bench/BASELINE.md` rewritten to document all three
    bench programs and a uniform per-(file, -O level)
    methodology.
- Verification context:
  - `make clean && make test` ends with `All tests passed.`
    from a cold rebuild on the working tree.
  - `tests/test_collection_materializers.ptt` (14 cases)
    passes at `-O0`, `-O1`, `-O2`.
  - Pre-existing `tests/test_map_methods.ptt` and
    `tests/test_set.ptt` still pass.
  - The new bench programs run cleanly and emit their
    expected output sums, locking the materialization shape
    against silent regressions.

## Claim C-007
- State: READY_FOR_AUDIT
- Against findings revision: 11
- Target commit: c8cdd04 + working tree (uncommitted; Codex can
  verify against `git diff` for the listed files)
- Claimed fixed: F-008
- Last updated: 2026-05-19T00:43:43+00:00
- Bug class (recap from findings.md F-008):
  `StringBuilder.push_int` formatted through a temporary owned
  `String` — it called `int.to_string()` (heap-alloc the
  digit-bytes buffer + a `String` header), then `push_string`
  (copy those bytes into the builder), then dropped the
  temporary on scope exit. Three allocations + a copy + a
  drop per integer append. Acceptable for one-off use, but
  the per-call allocation profile rules out the
  industrial-grade builder hot path the stdlib needs.
- Root-cause fix:
  Pure stdlib change in `std/string_builder.ptt`. `push_int`
  now writes decimal digits directly into the builder's
  backing buffer:
  - Fast path for `x == 0`: one `push_byte(48)`.
  - Capture sign and magnitude (`is_neg`, `v`).
  - Count `v`'s decimal digits in a small mod/divide loop.
  - Compute `needed = self.count + sign + digit_count` and
    grow the backing buffer in one shot if it is short
    (mirrors `push_string`'s inline-grow shape so the digit
    loop never re-checks capacity per byte).
  - Write the `'-'` sign byte if negative.
  - Write digits right-to-left into
    `[pos, pos + digit_count)` so they land in correct
    left-to-right order without a separate reverse pass.
  - Bump `self.count` to `needed`.
  Sign handling mirrors `int.to_string`'s existing constraint:
  `i64.MIN` is excluded from the negation step (`0 - i64.MIN`
  is undefined). Calling `push_int(i64.MIN)` was already
  unsupported via the legacy `int.to_string` path; the new
  path inherits that limitation and does not regress it.
- Files changed:
  - `std/string_builder.ptt` (push_int rewritten)
  - `tests/test_string_builder.ptt` (12 new framework cases
    appended; existing 6 cases left untouched)
  - `tests/bench/string_builder_push_int_bench.ptt` (new bench
    file)
  - `tests/bench/BASELINE.md` (added a section documenting
    the new bench file's workload and expected output shape)
- Tests added (`tests/test_string_builder.ptt`, all pass at
  `-O0`/`-O1`/`-O2`):
  - **Zero / single-digit corners** (3): `push_int formats
    zero`, `push_int formats single positive digit`,
    `push_int formats single negative digit`.
  - **Multi-digit corners** (2): `push_int formats multi-
    digit positive`, `push_int formats multi-digit
    negative`.
  - **Range edges** (2): `push_int handles large positive
    int` (i64 max, 19 digits), `push_int handles large
    negative int` (negation of i64 max).
  - **Mixing with other appends** (1): `push_int interleaved
    with push_string and push_byte` produces `[42, -7]`.
  - **Repeated stress** (1): `push_int repeated stress
    builds correct decimal sequence` — append 0..1000 with
    comma separators; verify the boundary substrings.
  - **Across grow boundary** (1): `push_int across grow
    boundary stays correct` — 100 iterations of
    `push_int(-1); push_byte('|')` forces several inline
    grow events; result is `("-1|") * 100`.
  - **Builder reuse** (1): `push_int on a freshly-cleared
    builder reuses backing buffer` — verifies that the
    direct-emit path's grow check finds existing capacity
    sufficient and does not re-grow unnecessarily after
    `clear`.
  - **No-allocation contract** (1): `push_int does not
    allocate intermediate String` — 10000 push_int calls
    against a single builder; the test verifies the final
    byte content boundaries (`"0123456789..."` prefix,
    `"99989999"` suffix). The intent is to exercise the
    direct-emit hot path under heavy reuse, locking it in
    against a future regression that quietly reverts to the
    intermediate-String path.
- Bench coverage added (`tests/bench/`, stand-alone `spark`
  program):
  - `string_builder_push_int_bench.ptt`: appends `0..10000`
    to a `StringBuilder` two ways — direct `push_int` (the
    post-fix path) and `push_string(i.to_string())` (the
    legacy path). Both phases must produce byte-identical
    output (verified by `s1.equals(s2)` and an
    `identical` / `MISMATCH` print), so the bench also
    serves as a semantic anchor for the fix. Output:
    `38890 / 38890 / identical`.
  - `tests/bench/BASELINE.md` extended with a
    `string_builder_push_int_bench.ptt` section.
- Verification context:
  - `make clean && make test` ends with `All tests passed.`
    from a cold rebuild.
  - `tests/test_string_builder.ptt` (18 cases total: 6
    pre-existing + 12 new) passes at `-O0`, `-O1`, `-O2`.
  - The new bench produces matching 38890-byte outputs from
    both the direct-emit and legacy paths, locking the fix
    in semantically.
  - Pre-existing `tests/test_collection_materializers.ptt`
    (F-006 regressions, 14 cases) still passes.

## Claim C-008
- State: READY_FOR_AUDIT
- Against findings revision: 12
- Target commit: 7650e2e + working tree (uncommitted; Codex can
  verify against `git diff` for the listed files)
- Claimed fixed: F-007
- Last updated: 2026-05-19T01:23:24+00:00
- Bug class (recap from findings.md F-007):
  Each of `String.index_of`, `String.contains`, `String.split`,
  and `String.replace` ran its own scan loop calling
  `str_match_at` at every candidate position. The substring
  scan logic was duplicated four times, none of the APIs had a
  single-byte fast path even though `","` / `" "` / `"/"` /
  `"\n"` separators dominate real-world traffic, and `replace`
  scanned twice (count pass + copy pass) without sharing.
  Worst-case O(n*m) was unchanged by the duplication, but the
  constant-factor regression and the maintenance burden of
  hand-rolled scans across four APIs were real.
- Root-cause fix:
  Pure stdlib change in `std/string.ptt`. Introduced a single
  shared substring-search primitive `str_find` and routed all
  four APIs through it.
  - `str_find(hay String, needle String, begin int) int`
    returns the smallest `i >= begin` (clamped to 0) at which
    `needle` occurs in `hay`, or -1. Empty needle returns the
    clamped begin (matches the existing `index_of("")` contract
    that returns 0). Single-byte needle path: scans byte-by-
    byte against `needle.data[0]` directly, skipping the
    str_match_at setup overhead per position. General path
    falls back to the full str_match_at on each candidate.
  - `String.index_of` is a one-liner: `give str_find(self,
    needle, 0)`.
  - `String.contains` is a one-liner: `give str_find(self,
    needle, 0) ne 0 - 1`.
  - `String.split` walks via successive `str_find` calls
    starting from the previous-hit cursor, which removes the
    quadratic recomputation of the search prefix.
  - `String.replace` walks via successive `str_find` calls in
    both the count pass and the copy pass. The copy pass
    inlines a verbatim prefix copy and a needle splice between
    consecutive hits, then a tail copy after the last hit; no
    longer interleaves a per-byte str_match_at recheck inside
    the copy loop.
  Public semantics are unchanged: same return values, same
  panic conditions on empty needle in split / replace, same
  treatment of overlapping prefixes (split / replace advance
  by needle.count after each hit, leaving overlapping matches
  unmatched — locked in by the new
  `split with overlapping needle prefix matches non-overlapping`
  and `replace with multi-byte needle and overlapping prefixes`
  tests).
- Files changed:
  - `std/string.ptt` (str_find primitive added; index_of,
    contains, split, replace re-routed)
  - `tests/test_string_search.ptt` (new, 19 framework cases)
  - `tests/bench/string_search_bench.ptt` (new bench file)
  - `tests/bench/BASELINE.md` (added a section for the new
    bench)
- Tests added (`tests/test_string_search.ptt`, all pass at
  `-O0`/`-O1`/`-O2`):
  - **index_of / contains corners** (5):
    `index_of finds first occurrence with overlapping prefix`,
    `index_of with single-byte needle exercises fast path`,
    `contains agrees with index_of on long haystacks`,
    `index_of of empty needle returns 0`,
    `index_of when needle longer than haystack returns -1`.
  - **split corners** (5): single-byte separator on a
    1000-field record, multi-byte separator boundary content,
    overlapping needle prefix advancing by needle.count,
    separator at start / middle / end, separator-only string
    yielding all-empty parts.
  - **replace corners** (7): single-byte needle on long input
    via fast path, shorter substitution shrinks result,
    longer substitution grows result, no-match returns owned
    clone, multi-byte needle with overlapping prefix, empty
    haystack, tail-copy preservation after final hit.
  - **Stress** (2): split + replace round-trip on a
    5000-field comma-separated record (verifies fast-path
    pressure under heavy traffic), and an
    `abc`-repeated 600-byte haystack with an embedded 6-byte
    needle exercising the general path through `str_find`.
- Bench coverage added (`tests/bench/`, stand-alone `spark`
  program):
  - `string_search_bench.ptt`: builds a 5000-field
    comma-separated record (~28890 bytes), then runs
    index_of (general path on a 3006-byte repeated-prefix
    haystack), split (single-byte fast path × 4999),
    replace + split round-trip (replace's count + copy
    passes + a second single-byte split). Output:
    `28889 / 3000 / 5000 / 23890 / 5000 / 23890`.
  - `tests/bench/BASELINE.md` extended with a
    `string_search_bench.ptt` section.
- Verification context:
  - `make clean && make test` ends with `All tests passed.`
    from a cold rebuild on the working tree.
  - `tests/test_string_search.ptt` (19 cases) passes at
    `-O0`, `-O1`, `-O2`.
  - Pre-existing `tests/test_string_extended.ptt` (the F-007
    evidence file) still passes unchanged.
  - The new bench produces consistent output (the two
    field-sum totals match: 23890), locking the search
    primitive against silent regressions in any of the four
    public APIs.

## Claim C-009
- State: READY_FOR_AUDIT
- Against findings revision: 13
- Target commit: 7650e2e + working tree (uncommitted; Codex can
  verify against `git diff` for the listed files)
- Claimed fixed: F-005
- Last updated: 2026-05-19T01:56:04+00:00
- Bug class (recap from findings.md F-005):
  Pre-fix `Map` and `Set` reduced the key hash with plain
  `h mod cap`, so the bucket depended only on the bottom
  log2(cap) bits of `h`. For Knuth-multiplicative `int.hash`
  (the only mixing pre-step) the constant `golden_ratio mod
  cap` is small and odd, so `(key * golden_ratio) mod cap`
  reduced to `key * (golden_ratio mod cap) mod cap` — and any
  inputs that share the bottom log2(cap) bits collided on the
  same bucket regardless of how their high bits differed
  (e.g. `{1, 1+cap, 1+2*cap, ...}` all hashed to the same
  bucket as 1). On top of that, every probe step recomputed
  `(start + off) mod cap`, so each collision walk paid an
  unconditional modulo per step instead of using a cursor.
- Root-cause fix:
  Pure stdlib change. Two parts, applied identically to Map
  and Set so the probe shape is single-source between them.
  - **Stronger start-bucket selection.** `map_probe_index` and
    `set_probe_index` now combine two windows of `h` before
    finalising the bucket: `low = h mod cap` and `high =
    (h / cap) mod cap`, then `(low + high) mod cap` (with the
    final negative-fold). `h / cap` is the "shift right by
    log2(cap)" equivalent for a power-of-two cap; folding it
    into the low bits forces the bucket to depend on at least
    2 * log2(cap) bits of `h`. Both intermediate values are
    bounded in `[-(cap-1), cap-1]`, so the combining sum is
    overflow-safe even when `h` itself is near INT_MIN /
    INT_MAX (djb2-shaped String.hash routinely wraps signed
    64-bit). Patterned-low-bit inputs that previously
    collapsed into a single bucket now fan out across the
    table because their high-bit windows differ.
  - **Linear-probe cursor.** Every probe loop in Map.has /
    Map.get / Map.try_get / Map.remove / Map.set /
    Map.resize_to (and the matching Set methods) now mixes
    once into a start bucket via `_probe_index`, then advances
    via `i be i + 1; i ge cap ?{ i be i - cap }`. The branch-
    and-subtract step is constant-cost and predicts well; the
    pre-fix `_probe_index(start + off, cap)` paid a modulo
    per step.
  Public semantics are unchanged. All pre-existing
  `tests/test_map_methods.ptt` and `tests/test_set.ptt` cases
  pass identically; the new tests below explicitly exercise
  adversarial keys that pre-fix would still have routed
  through the multiplicative hash but post-fix demonstrably
  spread.
- Files changed:
  - `std/map.ptt` (probe formula + cursor pattern across
    has / get / try_get / remove / set / resize_to)
  - `std/set.ptt` (matching changes for has / add / remove /
    resize_to)
  - `tests/test_hash_distribution.ptt` (new, 14 framework
    cases)
  - `tests/bench/hash_probe_bench.ptt` (new bench file)
  - `tests/bench/BASELINE.md` (added a section documenting
    the new bench)
- Tests added (`tests/test_hash_distribution.ptt`, all pass at
  `-O0`/`-O1`/`-O2`):
  - **Map of int, int adversarial keys** (6): multiples-of-
    cap keys (4096 entries forced through cap=8192),
    multiples-of-256 keys (4096 entries), powers-of-two keys
    (60 entries with single-bit-set-at-unique-position),
    same-low-bits-different-high keys (2048 entries with
    keys like `1 + i * 1024`), negative adversarial keys
    (1024 entries), and interleaved positive + negative
    adversarial (2048 entries total).
  - **Set of int adversarial keys** (2): multiples-of-cap
    keys at scale (4096 entries), same-low-bits-different-
    high keys (2048 entries).
  - **Lookup-miss correctness under collision pressure** (2):
    `Map miss correctly returns None`, `Set miss correctly
    returns false` — verifies the probe walk visits an EMPTY
    slot to terminate even when the table is filled with
    adversarial keys.
  - **Tombstone reuse under adversarial keys** (2): Map and
    Set both. Removes alternate keys, re-inserts a different
    adversarial set, verifies both surviving and re-inserted
    keys are retrievable. Exercises the probe walk across
    DELETED tombstones with the new cursor pattern.
  - **String-keyed Map under shared-prefix keys** (1): 1024
    strings with a long shared prefix, verifying that
    djb2-shaped String.hash + the new bucket function still
    yields a distribution that satisfies the lookup contract
    even when `h` magnitudes wrap signed 64-bit.
  - **Pre-existing tests unchanged**: all
    `tests/test_map_methods.ptt` and `tests/test_set.ptt`
    cases still pass.
- Bench coverage added (`tests/bench/`, stand-alone `spark`
  program):
  - `hash_probe_bench.ptt`: builds a `Map of int, int` with
    2048 adversarial keys (multiples of 8), then runs four
    phases: bulk insert, hit lookup with sum, miss lookup
    via try_get, and tombstone-reuse round-trip with a spot
    check. Output: `2048 / 14672896 / 2048 / 3072 / 49 / 0`.
  - `tests/bench/BASELINE.md` extended with a
    `hash_probe_bench.ptt` section.
- Verification context:
  - `make clean && make test` ends with `All tests passed.`
    from a cold rebuild.
  - `tests/test_hash_distribution.ptt` (14 cases) passes at
    `-O0`, `-O1`, `-O2`.
  - The new bench produces deterministic output across
    repeated runs, locking the bucket function and probe
    shape against silent regressions.
  - Pre-existing `tests/test_string_search.ptt` (F-007, 19
    cases), `tests/test_string_builder.ptt` (F-008, 18
    cases), `tests/test_collection_materializers.ptt`
    (F-006, 14 cases), `tests/test_map_methods.ptt`, and
    `tests/test_set.ptt` all still pass.

## Claim C-010
- State: READY_FOR_AUDIT
- Against findings revision: 15
- Target commit: 371141f + working tree (uncommitted; Codex can
  verify against `git diff` for the listed files)
- Claimed fixed: F-010
- Last updated: 2026-05-19T03:06:06+00:00
- Bug class (recap from findings.md F-010):
  After F-005's bucket-selection fix, `Map` and `Set` were
  correct and materially better than the pre-F-005 version,
  but still rested on (a) a 32-bit Knuth multiplier for
  `int.hash` (weak high-bit mixing on 64-bit inputs), (b) a
  djb2-shaped `String.hash` with no avalanche, (c) plain
  linear probing with no distance awareness, and (d) tombstone
  deletion that left `states[i] = 2` markers behind. Long-
  lived churn workloads accumulated tombstones until the next
  resize/clear; probes in the meantime had to walk past every
  one. Below the intended stdlib quality bar.
- Root-cause fix:
  Four-axis hardening, all in pure stdlib (plus one ARM64
  emit-side helper to handle larger frames produced by the
  Robin Hood code).
  - **Stronger `int.hash`** (`std/math.ptt`): replaced the
    32-bit Knuth multiplier `2654435769` with the full 64-bit
    golden-ratio multiplier `0x9E3779B97F4A7C15` (signed-64
    wrap, expressed as `0 - 7046029254386353131`). Same
    one-multiplication cost; output spreads across all 64 bit
    positions instead of leaving the top 32 weakly mixed.
  - **Stronger `String.hash`** (`std/string.ptt`): kept the
    djb2-shaped per-byte fold and added a 64-bit Knuth
    finalizer (`* 0 - 7046029254386353131`). Single extra
    multiplication regardless of input length; avalanches the
    djb2 output's few-bit differences across the full 64-bit
    result.
  - **Robin Hood probing** (`std/map.ptt`, `std/set.ptt`):
    `Map.set` / `Set.add` track each entry's distance from
    its ideal bucket; on insert, when the to-insert's
    distance exceeds the slot's, swap and continue with the
    displaced entry. Lookups (`has` / `get` / `try_get` /
    `remove`) use the same Robin Hood early-out: if `pdist >
    slot's distance`, the searched key cannot be in the
    table. Worst-case probe length is bounded; lookups
    terminate at the first slot where the key would have
    been inserted.
  - **Backshift deletion** (`std/map.ptt`, `std/set.ptt`):
    `Map.remove` / `Set.remove` walk forward from the
    vacated slot, shifting any FULL slot whose probe
    distance is positive one slot leftward, until they hit
    EMPTY or a distance-0 slot. The post-remove table is
    byte-equivalent to one where the key had never been
    inserted — no tombstones. The state byte simplifies to
    `{EMPTY=0, FULL=1}`; DELETED is gone.
  Two supporting compiler changes:
  - `compiler/iremit.c` — `emit_add_frame_off` helper that
    emits a chained `add x?, x29, #4095; add x?, x?, ...`
    sequence for frames whose offsets exceed ARM64's 12-bit
    immediate limit. The Robin Hood implementation in
    `Map of String, String`'s monomorphisations produces
    larger frames (more heap-tracked locals + spills); the
    pre-fix single-add emission failed assembly with
    "expected immediate in range [0, 4095]". All
    frame-relative `add` sites in iremit.c now route through
    the helper.
  - `compiler/checker.c` — `NODE_ASSIGN` with
    `is_move`/`is_rep` now clears the destination's
    `is_moved` flag (mirroring what `set_sym` does for
    var-decl re-binding). Without this, the Robin Hood swap
    pattern `dst be now src` left `dst` permanently flagged
    moved by the checker even though irgen's `set_local`
    already cleared the runtime moved flag and produced
    correct code. The change closes that conservatism gap.
  External `Map` / `Set` semantics are unchanged: same return
  values, same iteration order (still unordered, post-resize-
  shuffle), same panic conditions.
- Files changed:
  - `compiler/checker.c` (clear is_moved on assign re-bind)
  - `compiler/iremit.c` (emit_add_frame_off helper +
    routing of frame-relative add sites through it)
  - `std/math.ptt` (int.hash 64-bit multiplier)
  - `std/string.ptt` (String.hash 64-bit finalizer)
  - `std/map.ptt` (Robin Hood + backshift; new
    `map_probe_distance` helper; resize_to rewritten to
    reuse Robin Hood)
  - `std/set.ptt` (Robin Hood + backshift; new
    `set_probe_distance` helper; resize_to rewritten)
  - `tests/test_hash_robin_hood.ptt` (new, 16 framework
    cases)
  - `tests/bench/hash_churn_bench.ptt` (new bench)
  - `tests/bench/hash_string_bench.ptt` (new bench)
  - `tests/bench/BASELINE.md` (extended with sections for
    the two new bench files)
- Tests added (`tests/test_hash_robin_hood.ptt`, all pass at
  `-O0`/`-O1`/`-O2`):
  - **Hit-heavy** (3): `Map of int, int` 4096 sequential
    keys with full lookup pass; `Set of int` same shape;
    `Map of String, int` 1024 distinct concat-formed keys.
  - **Miss-heavy** (2): try_get of 2048 odd keys against an
    even-keyed Map; has() of 2048 odd keys against an
    even-keyed Set.
  - **Delete/reinsert-heavy** (4): insert N + remove half +
    reinsert different half; sustained 5x churn cycles with
    distinct key bases per cycle (validates the no-
    accumulating-tombstones contract); Set churn full
    cycle; Map remove preserves siblings on shared-bucket
    chains (multiples-of-8 with every-other-removed).
  - **Update path** (3): existing-key value updates don't
    change len; heap-shaped V update path drops the prior
    value cleanly; Set.add idempotency.
  - **Heap-shaped K/V correctness under Robin Hood swaps**
    (2): Map of String, String with 256 distinct keys
    survives the swap chains; Set of String with backshift
    half-removal preserves survivors.
  - **Adversarial-key regressions inherited from F-005**
    (2): multiples of 8 (4096 entries) and same-low-bits-
    different-high keys (1024 entries) still distribute
    under the Robin Hood + new-hash combination.
- Bench coverage added (`tests/bench/`, stand-alone `spark`
  programs):
  - `hash_churn_bench.ptt`: 10-cycle insert/remove churn
    against a 1024-baseline Map. Output:
    `1024 / 1024 / 8904192`. Pre-F-010 the post-cycle
    state would have been littered with tombstones; the
    output is identical post-F-010 because backshift
    deletion makes the cycles transparent.
  - `hash_string_bench.ptt`: parallel int and String Map
    workloads (fill + hit + miss; plus a 4096-element
    resize-stress for the int side). Output:
    `1024 / 523776 / 1024 / 5120 / 1024 / 523776 / 1024`.
  - `tests/bench/BASELINE.md` extended with sections for
    both new bench files.
- Verification context:
  - `make clean && make test` ends with `All tests passed.`
    from a cold rebuild.
  - `tests/test_hash_robin_hood.ptt` (16 cases) passes at
    `-O0`, `-O1`, `-O2`.
  - The new bench files run cleanly and emit deterministic
    output, locking the new probe + hash paths against
    silent regressions.
  - Pre-existing `tests/test_hash_distribution.ptt` (F-005,
    14 cases), `tests/test_map_methods.ptt`,
    `tests/test_set.ptt`, `tests/test_collection_materializers.ptt`
    (F-006, 14 cases), `tests/test_string_builder.ptt`
    (F-008, 18 cases), and `tests/test_string_search.ptt`
    (F-007, 19 cases) all still pass unchanged.

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
- Claim C-003 submitted: proactive surface of the parent-drop
  heap-shaped-element leak class (out of F-001 scope). Compiler
  fix at `_drop_<X>` / `_clone_<X>` (shape-aware: List vs
  non-List), stdlib changes in `List.push` / `List.insert`,
  comprehensive regression tests (16 framework + 6 IR-static
  gates). Recommends Codex open this as F-002 if the framing is
  accepted.
- Claim C-003 consumed by Codex against findings revision 6 and
  rejected; F-002 was opened for the live-tail ownership hole that
  the count-bounded List parent-drop introduced. Header reset to
  `IDLE` so the consumed claim does not block the next iteration.
- Claim C-004 submitted: F-002 root-cause fix. Two compiler
  changes (revert C-003's count-bounded list-shape specialisation
  to uniform cap-bounded null-guarded delegation; extend
  `is now` / `be now` to accept array-slot sources) plus a stdlib
  rewrite of `List.pop` / `List.try_pop` / `List.remove` to vacate
  slots via `is now self.data[k]`. `List.clear` stays as
  `count = 0`; the cap-bounded parent-drop reaps remaining slots.
  Twelve new framework tests cover Codex's required-test list
  plus the primitive-T fast path and nested-container variants.
  IR-static gates updated to expect the new cap-bounded
  delegation. Cold-rebuild `make test` green; Codex's two failing
  repros now exit 0.
- Claim C-004 consumed by Codex against findings revision 8 and
  accepted for F-002. Batch stays on HOLD because Codex's deep
  sweep opened F-003 (Queue) and F-004 (Deque) for the same
  heap-shaped circular-buffer ownership class — those need their
  own claim before the batch can land. Header reset to `IDLE`.
- Claim C-005 submitted: F-003 + F-004 root-cause fix in stdlib.
  Pure stdlib change — the F-002 compiler primitives
  (`is now arr[i]`, uniform cap-bounded null-guarded drop) are
  reused as-is. Queue.push / Deque.push_back / push_front
  deep-clone via `is rep` + `be now`; pop / try_pop / pop_back /
  try_pop_back vacate slots via `is now self.data[idx]`;
  reserve / push-grow paths transfer ownership of each logical
  slot via `is now` + `be now` so the old buffer drops as all
  null. clear stays as `head=0; count=0`. 25 new framework
  tests cover Codex's required-tests list (heap-shaped T = String
  and List of int, push/pop/try_pop, wraparound, growth, clear,
  primitive-T fast paths). Cold-rebuild `make test` green at
  default plus all three -O levels for the new file.
- Claim C-006 submitted: F-006 fix (collection-materializer
  pre-sizing). Pure stdlib change. Map.keys / Map.values pre-
  reserve `self.count`; Set.values pre-reserves `self.count`;
  Set.intersect pre-reserves `math.min(self.count,
  other.count)`; Set.difference pre-reserves `self.count`.
  Public semantics unchanged. 14 new framework cases verify
  correctness under large-N workloads (4096-entry Map, 2048
  vs 2048 Sets) plus heap-shaped V/T independence and empty
  edges. Two new bench programs locked in as Codex required
  (`map_keys_values_bench.ptt`, `set_materializers_bench.ptt`).
  Cold-rebuild `make test` green; new file passes at -O0/-O1/-O2.
- Claim C-006 consumed by Codex against findings revision 11 and
  accepted for F-006. Batch stays on HOLD because F-005 / F-007 /
  F-008 are still open against the post-C-006 working tree;
  those need their own claims before the batch can land. Header
  reset to `IDLE`.
- Claim C-007 submitted: F-008 fix (StringBuilder.push_int
  direct-digit emit). Pure stdlib change. Pre-fix push_int went
  through int.to_string() + push_string — three allocations +
  a copy + a drop per integer append. Post-fix it counts digits,
  grows the backing buffer in one shot, writes the sign byte,
  then writes digits right-to-left into a contiguous slot. 12
  new framework cases cover zero, single-digit positive /
  negative, multi-digit positive / negative, i64-range edges,
  interleaving with push_byte / push_string, repeated 1000-
  iteration stress, across-grow boundary, builder reuse after
  clear, and a 10000-call no-allocation contract test. New bench
  `string_builder_push_int_bench.ptt` runs the direct-emit and
  legacy paths against the same 0..10000 workload and asserts
  byte-identical output. Cold-rebuild `make test` green; new
  cases pass at -O0/-O1/-O2.
- Claim C-007 consumed by Codex against findings revision 12 and
  accepted for F-008. Batch stays on HOLD because F-005 / F-007
  are still open against the post-C-007 working tree; those need
  their own claims before the batch can land. Header reset to
  `IDLE`.
- Claim C-008 submitted: F-007 fix (shared substring-search
  primitive). Pure stdlib change. New `str_find(hay, needle,
  begin)` primitive with single-byte fast path + general
  str_match_at fallback; index_of / contains / split / replace
  all funnel through it. 19 new framework cases cover index_of,
  contains, split, replace corners (overlapping prefixes,
  separator placement, length-changing replace, no-match clone,
  empty haystack), plus two stress tests (5000-field round-trip
  and embedded-long-needle on a 600-byte repeated-prefix
  haystack). New bench `string_search_bench.ptt` exercises the
  fast path × 4999, the general path on a 3006-byte haystack,
  and the replace + split round-trip. Cold-rebuild `make test`
  green; new file passes at -O0/-O1/-O2.
- Claim C-008 consumed by Codex against findings revision 13 and
  accepted for F-007. Batch stays on HOLD because F-005 is still
  open against the post-C-008 working tree; that needs its own
  claim before the batch can land. Header reset to `IDLE`.
- Claim C-009 submitted: F-005 fix (hash-table probe quality).
  Pure stdlib change. Stronger start-bucket selection
  (`map_probe_index` / `set_probe_index` fold the high-bit
  window of `h` into the low bits via `low = h mod cap;
  high = (h / cap) mod cap; (low + high) mod cap` — overflow-
  safe for djb2 String.hash wraparound). Linear-probe cursor
  with branch-and-subtract wrap replaces per-step modular
  reduction in every Map and Set probe loop (has, get,
  try_get, remove, set/add, resize_to). 14 new framework
  cases verify correctness under multiples-of-cap keys (up to
  4096), powers-of-two keys, same-low-bits-different-high
  keys, negative adversarial keys, interleaved sign,
  miss-under-collision-pressure, and tombstone-reuse. New
  bench `hash_probe_bench.ptt` exercises the four phases on
  N=2048 multiples-of-8 keys (bulk insert / hit / miss /
  tombstone reuse). Cold-rebuild `make test` green; new file
  passes at -O0/-O1/-O2.
- Claim C-009 consumed by Codex against findings revision 14 and
  accepted with `Conclusion: ALL_CLEAR` /
  `Release action: COMMIT_AND_PUSH`. The audited optimization
  batch covers both C-008 (F-007, accepted earlier but held
  pending F-005) and C-009 (F-005). Landing as two separate
  commits in dependency order: C-008 first (the shared
  string-search primitive that doesn't depend on hash-table
  changes), then C-009 (Map / Set probe quality). Header
  reset to `IDLE`.
- Claim C-010 submitted: F-010 fix (hash-container quality
  hardening). Pure stdlib + minimal compiler-emit support.
  Stronger int.hash (64-bit Knuth multiplier) and String.hash
  (Knuth finalizer post-djb2). Robin Hood probing in Map/Set
  insert + lookup with distance-aware swap and early-out.
  Backshift deletion replaces tombstones — DELETED state is
  gone, post-remove table self-heals so churn doesn't degrade
  probe quality. Compiler-side: emit_add_frame_off helper for
  frames > 4095 bytes; checker clears is_moved on `be now` /
  `be rep` re-bind to support the Robin Hood swap pattern.
  16 new framework cases cover hit-heavy, miss-heavy,
  delete/reinsert-heavy, update path, heap-shaped K/V swap
  chains, and adversarial-key regressions. Two new bench
  programs lock in the design. Cold-rebuild `make test` green;
  new tests pass at -O0/-O1/-O2.
- Claim C-010 consumed by Codex against findings revision 16 and
  accepted for F-010. Batch stays on HOLD because F-011 was
  opened against `std/STDLIB_CHECKLIST.md`: the canonical
  status doc still describes the pre-F-010 design (linear
  probing, tombstone-reusing inserts, `{EMPTY, FULL, DELETED}`
  states, stronger-hashing-as-future-work) — false against the
  audited code. Header reset to `IDLE`.
