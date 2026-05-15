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
| Variables | ✅ | All go through stack slots |
| if/nah | ✅ | Multi-branch with basic blocks |
| through-range loops | ✅ | to/step stored in hidden slots |
| infi loops | ✅ | With optional condition |
| stop/skip | ✅ | |
| Struct construction | ✅ | Calls _alloc_StructName |
| Field access | ✅ | Survives `bl` calls (P0 fix) |
| Field access after calls | ✅ | Fixed in P0 (stack-layout bug) |
| Field assign | ✅ | |

### Not Yet Implemented in IR backend
These work in the direct codegen but not yet in the IR pipeline. P3
(generics) and P4.2 (cross-block regalloc) are the prerequisites for
landing them cleanly without re-introducing the kind of latent bug P0
just fixed.

| Feature | Depends On |
|---------|-----------|
| Lists / Maps / Enums | Same heap path as structs — likely close to working now; needs targeted tests in `tests/ir/` |
| through-in (collection iteration) | Lists |
| RAII (scope-end free) | Liveness propagation — pending P4.2 |
| String interpolation | `_str_concat` builtin emission from IR |
| match (pattern matching) | Enums |

---

## Architecture

```
Source (.ptt) → Lexer → Parser → Checker → Optimizer → IR Generator → Register Allocator → ARM64 Emitter
                                                         (irgen.c)      (regalloc.c)        (iremit.c)
```

### Key Files
- `src/ir.h` — IR data structures (opcodes, instructions, blocks, functions)
- `src/irgen.c` — AST → IR translation
- `src/regalloc.c` — Linear scan register allocation (vreg → physical reg)
- `src/iremit.c` — IR → ARM64 assembly emission

### Design Decisions
- **All named locals use stack slots** — Correct across basic blocks without PHI nodes
- **No caller-save around calls** — Since locals are on stack, they survive calls naturally
- **Register allocator assigns regs to temporaries only** — Within a single basic block
- **Hidden stack slots for loop bounds** — `to` and `step` values stored to survive iterations

### How to Test
```bash
# IR pipeline (experimental)
./erbos ir file.ptt        # generates .s file only
# Then manually: as + ld + run

# Old codegen (default, all tests pass)
./erbos run file.ptt       # compile + run
./erbos test file.ptt      # run test framework
make test                  # full regression
```
