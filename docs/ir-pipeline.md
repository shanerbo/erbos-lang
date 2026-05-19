# IR Pipeline

Reference for the SSA-style IR backend that generates ARM64
machine code. The IR is the only backend.

## Postmortem: "Heap memory corruption after bl calls"

A historical bug worth keeping in the record because the
reproducer is a 6-line program that exposes a subtle stack-frame
layout error.

### Symptom
Struct field access returned 0 after any function call (`bl`),
even though the field was correctly stored before the call.
Reproduced with:

```potato
Point is { x int, y int }
spark {
  p is Point()
  p.y be 20
  yell(p.y)   // prints 20
  yell(p.y)   // prints 0   <-- bug
}
```

### Root cause: stack frame layout (NOT heap corruption)

The bug name was misleading. The corrupted memory was on the
stack, addressed below the stack pointer.

`compiler/iremit.c::iremit_func` originally emitted this prologue:

```
stp x29, x30, [sp, #-stack_size]!   ; sp -= stack_size, save fp/lr at [sp]
mov x29, sp                          ; x29 = sp (now == bottom of frame)
```

After this, `x29 == sp`. `IR_STORE_LOCAL` and `IR_LOAD_LOCAL`
addressed locals as `[x29, #-N]` — i.e. *below* the stack
pointer, in unmapped memory.

The first `bl _yell` did its own `stp x29, x30, [sp, #-16]!`,
which wrote the caller's saved fp/lr at exactly `[sp - 16]` — the
same address the local was supposed to live at. The store
clobbered the heap pointer that had been written there. The
second `ldr x1, [x29, #-16]` then read back the saved fp instead
of the heap pointer; dereferencing the saved fp as a struct
pointer reads zeros.

### Fix

Address locals and spills as POSITIVE offsets from `x29`. The
combined `stp ... !` already reserves the full frame; everything
in `[x29, #16 .. stack_size)` is valid stack space.

Bonus fix: regalloc's spill slots had no awareness of the locals
area. They overlapped. `iremit.c` now translates the raw spill
index from regalloc through `spill_off()` so spills land strictly
above the locals area.

Frame layout post-fix (positive offsets from x29):

```
[x29, #0..15]                              saved {x29, x30}
[x29, #16 .. #16 + local_slots*8)          IR locals
[x29, #16 + local_slots*8 ..)              regalloc spills
[high]                                     scratch padding (256 bytes)
```

### Regression test
`tests/ir/struct_field_after_call.ptt` reads each field twice;
both reads must succeed across `-O0`, `-O1`, and `-O2`.

---

## Architecture

```
Source (.ptt) → Lexer → Parser → Monomorph → Checker → Optimizer
              → IR Generator → IROpt → Register Allocator → ARM64 Emitter ──┐
                  (irgen.c)   (iropt.c)  (regalloc.c)         (iremit.c)    │
                                                                            ▼
                                                       target_{darwin,linux}_arm64.c
                                                       (sections, syscalls, address-load
                                                        relocations, entry-point, toolchain)
```

### Key Files
- `compiler/ir.h` — IR data structures (opcodes, instructions, blocks, functions)
- `compiler/irgen.c` — AST → IR translation, RAII heap-free bookkeeping
- `compiler/iropt.c` — Optimization-pass framework, gated by `-O0`/`-O1`/`-O2`
- `compiler/regalloc.c` — Cross-block, call-aware linear-scan register allocator
- `compiler/iremit.c` — IR → ARM64 assembly emission, prologue/epilogue, string pool. Takes a `Target *` so a single emitter serves both `darwin-arm64` and `linux-arm64`.
- `compiler/runtime_emit.c` — C-emitted runtime helpers (yell, heap, panics, asserts) — see [`runtime.md`](runtime.md). Same target-parameterised pattern.
- `compiler/target.h` + `compiler/target_darwin_arm64.c` + `compiler/target_linux_arm64.c` — per-target callbacks (sections, syscall ABI, address-load relocations, entry-point emission, toolchain driver). See [`linux-arm64-backend-plan.md`](linux-arm64-backend-plan.md).

### Design Decisions

- **Named locals go through stack slots.** Survives basic-block
  transitions cleanly without PHI nodes. The IR generator stamps
  each local with a slot index in `slot_next`; iremit translates
  slot N to byte offset `local_base + N*8` from x29.

- **Cross-block, call-aware regalloc.** Vregs whose live range
  strictly contains an `IR_CALL` are placed in callee-save
  registers (x19..x28) with prologue saves and matching epilogue
  restores. Shorter-lived vregs prefer the temporary range
  x8/x11..x18; **x9 and x10 are reserved** as iremit scratch for
  large-offset frame addressing.

- **Hidden stack slots for loop bounds.** `to`/`step` for
  `through (i from a to b by c)`, and the collection pointer +
  index for `through (x in coll)`. Both pairs use `slot_next++`
  so they don't collide with user locals.

- **RAII heap-free.** Per-local `is_heap` / `is_moved` /
  `alloc_size` parallel arrays; `emit_scope_cleanup` walks live
  heap locals at scope end, at returns, and at function
  fall-off.

- **Optimization passes are data, not code.** `iropt.c` keeps a
  table of `(name, min_level, fn)` triples; `iropt_run(ir, level)`
  walks it and runs every pass whose `min_level` ≤ requested level.
  The shipped passes: inlining, scalar replacement of aggregates,
  escape analysis (stackify), bounds-check elimination, loop-
  invariant code motion. `-O0` skips them all; `-O1` and `-O2`
  run all five. Adding a pass means one new entry in the table.

## How to Test

```bash
./erbos run file.ptt       # default: build, run, clean up (uses -O1)
./erbos test file.ptt      # same; runs `test "..." { }` blocks
./erbos ir file.ptt        # generate .s file only (no assemble/link)

# Optimization-level matrix — `make test-ir` runs every
# tests/ir/*.ptt regression at -O0, -O1, and -O2 via the
# framework runner; assertions must pass at every level.
./erbos -O0 run file.ptt   # skip iropt entirely
./erbos -O1 run file.ptt   # default
./erbos -O2 run file.ptt   # reserved for tuning; identical to -O1 today

make test                  # full regression suite
```
