#ifndef REGALLOC_H
#define REGALLOC_H

#include "ir.h"

// Maps virtual registers to physical ARM64 registers (x0-x28).
// Result: each VReg gets either a physical reg or a stack offset.
//
//   x0..x18  - caller-save (clobbered by `bl`); x0..x7 also serve as
//              argument/return registers.
//   x19..x28 - callee-save; iremit emits prologue/epilogue saves for
//              every callee-save register the allocator picks.
//   x29 = frame pointer, x30 = link register — never allocated to a
//   vreg.

#define PHYS_REG_COUNT 29  // x0..x28
#define SPILL_BASE 16      // stack spill slots; iremit re-bases above the locals area

typedef struct {
    int *vreg_to_phys;   // vreg → physical reg (0-18), or -1 if spilled
    int *vreg_to_spill;  // vreg → spill slot offset, or -1 if in reg
    int vreg_count;
    int spill_count;     // number of spill slots used
} RegAllocResult;

// Linear scan register allocation
RegAllocResult regalloc_run(IRFunc *func);

#endif
