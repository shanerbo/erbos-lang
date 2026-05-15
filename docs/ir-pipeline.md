# IR Pipeline: Status & Postmortems

## Postmortem: "Heap memory corruption after bl calls" — fixed in P0 spike

### Symptom
Struct field access returned 0 after any function call (`bl`), even though
the field was correctly stored before the call. Reproduced with:

```potato
Point is { x int, y int }
spark {
  p is Point()
  p.y be 20
  yell(p.y)   // prints 20
  yell(p.y)   // prints 0   <-- bug
}
```

### Actual root cause: stack frame layout — NOT heap corruption

The bug name was misleading. The corrupted memory was **not** on the heap;
it was in the stack frame, addressed below the stack pointer.

`src/iremit.c::iremit_func` emitted this prologue:

```
stp x29, x30, [sp, #-stack_size]!   ; sp -= stack_size, save fp/lr at [sp]
mov x29, sp                          ; x29 = sp (now == bottom of frame)
```

After this, `x29 == sp`. `IR_STORE_LOCAL` and `IR_LOAD_LOCAL` then addressed
locals as `[x29, #-N]` — i.e. **below** the stack pointer, in the kernel's
red-zone-equivalent or in unmapped memory.

The first `bl _yell` did its own `stp x29, x30, [sp, #-16]!`, which wrote
the caller's saved fp/lr at exactly `[sp - 16]` — the same address the
local was supposed to live at. The store clobbered the heap pointer that
had been written there. The second `ldr x1, [x29, #-16]` then read back
the saved fp instead of the heap pointer; dereferencing the saved fp as a
struct pointer reads zeros, hence the `yell(p.y)` printing `0`.

### Comparison with the working direct codegen

`src/codegen.c::emit_func` uses a different prologue:

```
stp x29, x30, [sp, #-16]!   ; reserve only 16 bytes for fp/lr
mov x29, sp                  ; x29 = top of frame
sub sp, sp, #1024            ; reserve another 1024 bytes for locals
```

Now `x29` is at the saved-fp area (high), `sp` is 1024 bytes below it
(low), and `[x29, #-N]` for `N` in `(0, 1024]` lives between `sp` and
`x29` — valid. That's why direct codegen never had the bug.

### Fix shipped in P0

Changed `iremit.c` to address locals and spills as POSITIVE offsets from
`x29` (which equals `sp` after the prologue). The combined `stp ... !`
already reserves the full frame; everything in `[x29, #16 .. stack_size)`
is valid stack space.

Bonus fix: regalloc's spill slots were originally numbered `16, 32, 48,
…` with no awareness of the locals area. They overlapped the locals.
`iremit.c` now translates the raw spill index from regalloc through
`spill_off()` so spills land strictly above the locals area.

Frame layout post-fix (positive offsets from x29):

```
[x29, #0..15]                              saved {x29, x30}
[x29, #16 .. #16 + local_slots*8)          IR locals
[x29, #16 + local_slots*8 ..)              regalloc spills
[high]                                     scratch padding (256 bytes)
```

### Regression test

`tests/ir/struct_field_after_call.ptt` plus its `.expected` file. Wired
into `make test` via the new `test-ir` target, which compiles each
`tests/ir/*.ptt` through the IR backend, assembles + links + runs it,
and compares stdout against the sibling `.expected`. If the layout bug
ever reappears, the test will print zeros and fail the suite.

---

## IR Pipeline Status

### Working
| Feature | Status | Notes |
|---------|--------|-------|
| Int/str/bool literals | ✅ | |
| Arithmetic (+,-,*,/,mod) | ✅ | |
| Comparisons (eq,ne,lt,gt,le,ge) | ✅ | |
| Function calls | ✅ | Args in x0-x7, result in x0 |
| User method calls | ✅ | Full dispatch (enum constructors, user methods on structs, builtin collection methods, import aliases, fallback) — P4.3 |
| Variables | ✅ | All go through stack slots |
| if/nah | ✅ | Multi-branch with basic blocks |
| through-range loops | ✅ | to/step stored in hidden slots |
| infi loops | ✅ | With optional condition |
| stop/skip | ✅ | |
| Struct construction | ✅ | Calls _alloc_StructName |
| Field access | ✅ | Survives `bl` calls (P0 fix) |
| Field access after calls | ✅ | Fixed in P0 (stack-layout bug) |
| Field assign | ✅ | |
| Cross-block / call-aware regalloc | ✅ | P4.2 — callee-save x19..x28 chosen for vregs that span calls; prologue/epilogue save & restore |
| Large stack frames (> 504 bytes) | ✅ | Prologue splits `stp ..., [sp, #-N]!` when N exceeds the immediate range — P4.3 |
| Function-scoped labels | ✅ | `L_<func>_<n>` to prevent label collisions across functions — P4.3 |
| `len()` on list/map/string | ✅ | Direct header load for collections, `_str_len` call for strings — P4.3 |
| Built-in constructors (`list()`, `map()`, `imap()`, `task()`) | ✅ | Remapped to `_list_new` / `_map_new` / `_imap_new` / inline 0 — P4.3 |

### Implemented after the foundation
Every NODE_* the parser produces now lowers correctly through the
IR pipeline. The following landed across P4.3a through P4.3e2 and
the regalloc fix in P4.3e:

| Feature | Notes |
|---------|-------|
| `NODE_LIST_LIT` (list literal `[1, 2, 3]`) | P4.3a |
| `NODE_MAP_LIT` (map literal `["a" to 1]`) | P4.3a |
| `NODE_INDEX` (`xs[i]`) with bounds check | P4.3a + the bounds-check addition that landed alongside P4.3f |
| `through (x in collection)` (collection iteration) | P4.3b |
| String literals + `IR_LOAD_STR` | P4.3b — string pool emitted via `iremit_finalize_data` |
| String interpolation (`"hello {name}"`) | P4.3c (also fixed an `_int_to_str` ABI bug) |
| `match` pattern matching | P4.3d |
| RAII (scope-end heap free) | P4.3e — per-local heap tracking; emit `_heap_free` at scope end / returns / function fall-off |
| Test blocks (`test "..." { ... }` + `assert(...)`) | P4.3e2 — per-test `_test_<N>` synthesis + multi-test `_start` runner |

### IR backend usage
The IR backend is the only backend. `./erbos run <file.ptt>` and
`./erbos test <file.ptt>` go through the IR pipeline; `./erbos ir
<file.ptt>` emits the .s only without assembling, useful for
inspecting generated code. The legacy direct codegen (`src/codegen.c`)
was retired in P4.3g; runtime helpers (yell, heap allocator, str
ops, list/map/imap builtins, panic + assert handlers) live in
`src/runtime_emit.c`.

---

## Architecture

```
Source (.ptt) → Lexer → Parser → Monomorph → Checker → Optimizer
              → IR Generator → Register Allocator → ARM64 Emitter
                  (irgen.c)        (regalloc.c)        (iremit.c)
```

### Key Files
- `src/ir.h` — IR data structures (opcodes, instructions, blocks, functions)
- `src/irgen.c` — AST → IR translation, RAII heap-free bookkeeping
- `src/regalloc.c` — Cross-block, call-aware linear-scan register allocator
- `src/iremit.c` — IR → ARM64 assembly emission, prologue/epilogue, string pool
- `src/runtime_emit.c` — C-emitted runtime helpers (yell, heap, str/list/map/imap, panics, asserts)

### Design Decisions
- **Named locals go through stack slots** — survives basic-block transitions
  cleanly without PHI nodes. The IR generator stamps each local with a slot
  index in `slot_next`; iremit translates slot N to byte offset
  `local_base + N*8` from x29.
- **Cross-block, call-aware regalloc** — vregs whose live range strictly
  contains an `IR_CALL` are placed in callee-save registers (x19..x28)
  with prologue saves and matching epilogue restores. Shorter-lived
  vregs prefer the temporary range x8/x11..x18; **x9 and x10 are reserved**
  as iremit scratch for large-offset frame addressing.
- **Hidden stack slots for loop bounds** — `to`/`step` for `through (i from a to b by c)`,
  and the collection pointer + index for `through (x in coll)`. Both pairs
  use `slot_next++` so they don't collide with user locals.
- **RAII heap-free** — per-local `is_heap` / `is_moved` / `alloc_size`
  parallel arrays; emit_scope_cleanup walks live heap locals at scope end,
  at returns, and at function fall-off.

### How to Test
```bash
./erbos run file.ptt       # default: build, run, clean up via the IR pipeline
./erbos test file.ptt      # same; the test framework runs in the binary
./erbos ir file.ptt        # generates .s file only (no assemble/link)
# Then manually: as + ld + run

make test                  # full regression — 80 OK lines as of P4.3g
```
