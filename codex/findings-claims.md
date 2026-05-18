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
- `Claim ID`: (none — C-004 was consumed and accepted for F-002;
  batch remains on HOLD because F-003 / F-004 were opened against
  Queue / Deque heap-shaped ownership)
- `Against findings revision`: 8
- `Target commit`: 5fa4ab5 + working tree
- `Claimed fixed`: (none pending; next claim will target
  F-003 + F-004 as one cluster)
- `Last updated`: 2026-05-18T22:41:24+00:00

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
