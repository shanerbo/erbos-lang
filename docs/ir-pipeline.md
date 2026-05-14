# IR Pipeline: Known Issues & Next Steps

## Critical Bug: Heap Memory Corruption After `bl` Calls

### Symptom
Struct field access returns 0 after any function call (`bl`), even though the field was correctly stored before the call.

```potato
Point is {
  x int
  y int
}
spark {
  p is Point()
  p.x be 10
  p.y be 20
  yell(p.y)   // prints 20 ✓
  yell(p.y)   // prints 0  ✗ (should be 20)
}
```

### What Works
- Single field access before any call: ✓
- Field store (`p.y be 20`): ✓ (verified by reading immediately after)
- Multiple yells of plain ints: ✓
- Loops, if/nah, arithmetic: all ✓

### What's Broken
- Any `bl` instruction corrupts the heap memory where struct fields are stored
- Affects: `_yell`, `_alloc_*`, any user function call
- The struct pointer (stored in a stack slot) remains valid — it's the *pointed-to memory* that gets zeroed

### Debugging Done
1. Stack frame overlap ruled out — increased frame to 512 bytes, same result
2. Caller-save/restore removed — all locals go through stack slots, same result
3. Local slot overlap with save area — fixed, same result
4. Verified `_yell_int` only uses its own stack buffer (`[sp, #32]`, 64 bytes below its frame)
5. Verified `_heap_alloc` uses mmap (syscall 197) for fresh pages

### Likely Root Causes (not yet verified)
1. **`_heap_alloc` mmap returns address overlapping with stack** — On macOS ARM64, stack is at high addresses, heap should be low. But if the binary has no `__PAGEZERO` or unusual layout, mmap might return unexpected addresses.
2. **`_yell_int` write syscall corrupts memory** — The write syscall (x16=#4) takes buffer pointer in x1. If x1 is wrong, kernel could write to heap. But yell_int sets x1 = sp+32 (its own buffer).
3. **`_alloc_Point` returns uninitialized/reused memory** — The free-list allocator might return a block that gets reused. But we never free anything in this test.

### How to Debug
```bash
cd ~/erbos-lang
./erbos ir /tmp/test.ptt
as -o /tmp/test.o /tmp/test.s
ld -o /tmp/test /tmp/test.o -lSystem -syslibroot $(xcrun --show-sdk-path) -e _start
lldb /tmp/test
# Set breakpoint after field store:
# (lldb) b _spark
# (lldb) r
# Step to after "str x1, [x2, #8]" (p.y = 20)
# Check: x register read, memory read [x2+8]
# Step through bl _yell
# Check: memory read [same address] — should still be 20
```

### Generated Assembly (minimal repro)
```asm
_spark:
    stp x29, x30, [sp, #-288]!
    mov x29, sp
    bl _alloc_Point          ; returns heap ptr in x0
    str x0, [x29, #-16]     ; store p to local slot
    mov x1, #20
    ldr x2, [x29, #-16]     ; load p
    str x1, [x2, #8]        ; p.y = 20
    ldr x1, [x29, #-16]     ; load p
    ldr x2, [x1, #8]        ; x2 = p.y (should be 20)
    mov x0, x2
    bl _yell                 ; prints 20 ✓
    mov x1, x0
    ldr x1, [x29, #-16]     ; load p again
    ldr x2, [x1, #8]        ; x2 = p.y (returns 0 ✗)
    mov x0, x2
    bl _yell                 ; prints 0
```

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
| Field access (single) | ✅ | Before any call |
| Field assign | ✅ | |

### Blocked by Heap Bug
| Feature | Status | Notes |
|---------|--------|-------|
| Field access after calls | ❌ | Heap corruption |
| Lists | ❌ | Same heap pattern |
| Maps | ❌ | Same heap pattern |
| Enums | ❌ | Uses heap alloc |

### Not Yet Implemented
| Feature | Depends On |
|---------|-----------|
| through-in (collection iteration) | Lists |
| RAII (scope-end free) | Heap bug fix |
| String interpolation | String concat builtin |
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
