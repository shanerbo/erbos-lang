#ifndef REGALLOC_H
#define REGALLOC_H

#include "ir.h"

// Maps virtual registers to physical ARM64 registers (x0-x18)
// or spills to stack slots.
// Result: each VReg gets either a physical reg or a stack offset.

#define PHYS_REG_COUNT 19  // x0-x18 (x19-x28 callee-saved, x29=fp, x30=lr)
#define SPILL_BASE 16      // stack spill starts at [x29, #-16], #-32, etc.

typedef struct {
    int *vreg_to_phys;   // vreg → physical reg (0-18), or -1 if spilled
    int *vreg_to_spill;  // vreg → spill slot offset, or -1 if in reg
    int vreg_count;
    int spill_count;     // number of spill slots used
} RegAllocResult;

// Linear scan register allocation
RegAllocResult regalloc_run(IRFunc *func);

#endif
