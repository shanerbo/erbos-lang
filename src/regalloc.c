#include <stdlib.h>
#include <string.h>
#include "regalloc.h"

// Linear-scan register allocator (P4.2 cross-block, call-aware).
//
// Live ranges are computed across all basic blocks. A vreg whose live
// range strictly contains an `IR_CALL` is flagged crosses_call; the
// allocator then refuses to put it in a caller-save register
// (x0..x18) and prefers a callee-save register (x19..x28), falling
// back to a stack spill.

typedef struct {
    int vreg;
    int start;          // instruction index of first definition
    int end;            // instruction index of last use
    int crosses_call;   // 1 if any IR_CALL falls inside (start, end)
} LiveRange;

static void compute_liveness(IRFunc *func, LiveRange *ranges) {
    for (int i = 0; i < func->vreg_count; i++) {
        ranges[i].vreg = i;
        ranges[i].start = -1;
        ranges[i].end = -1;
        ranges[i].crosses_call = 0;
    }

    // Pass 1: first-def + last-use for every vreg.
    int inst_idx = 0;
    for (int bi = 0; bi < func->block_count; bi++) {
        IRBlock *b = &func->blocks[bi];
        for (int ii = 0; ii < b->count; ii++) {
            IRInst *inst = &b->insts[ii];
            if (inst->dst >= 0 && inst->dst < func->vreg_count) {
                if (ranges[inst->dst].start < 0)
                    ranges[inst->dst].start = inst_idx;
                ranges[inst->dst].end = inst_idx;
            }
            if (inst->a >= 0 && inst->a < func->vreg_count)
                ranges[inst->a].end = inst_idx;
            if (inst->b >= 0 && inst->b < func->vreg_count)
                ranges[inst->b].end = inst_idx;
            if (inst->op == IR_CALL && inst->args) {
                for (int ai = 0; ai < inst->arg_count; ai++)
                    if (inst->args[ai] >= 0 && inst->args[ai] < func->vreg_count)
                        ranges[inst->args[ai]].end = inst_idx;
            }
            inst_idx++;
        }
    }

    // Pass 2: mark every vreg whose live range strictly contains an
    // IR_CALL. "Strictly contains" means the call instruction index is
    // in (start, end); a call that itself defines or last-uses the
    // vreg doesn't clobber it in flight.
    inst_idx = 0;
    for (int bi = 0; bi < func->block_count; bi++) {
        IRBlock *b = &func->blocks[bi];
        for (int ii = 0; ii < b->count; ii++) {
            if (b->insts[ii].op == IR_CALL) {
                for (int v = 0; v < func->vreg_count; v++) {
                    if (ranges[v].start >= 0 &&
                        ranges[v].start < inst_idx &&
                        ranges[v].end > inst_idx) {
                        ranges[v].crosses_call = 1;
                    }
                }
            }
            inst_idx++;
        }
    }
}

// ARM64 register classes:
//   x0..x18  - caller-save (clobbered by any `bl`).
//              x0..x7 also serve as argument / return registers.
//   x19..x28 - callee-save (preserved across calls).
// We don't currently use x29 (frame pointer) or x30 (link register).
//
// crosses_call vregs MUST be in [x19, x28] or spilled; non-crossing
// vregs prefer [x8, x18] (the temporary range) so x0..x7 stay free
// for argument passing.

RegAllocResult regalloc_run(IRFunc *func) {
    RegAllocResult res;
    res.vreg_count = func->vreg_count;
    res.vreg_to_phys = malloc(func->vreg_count * sizeof(int));
    res.vreg_to_spill = malloc(func->vreg_count * sizeof(int));
    res.spill_count = 0;

    for (int i = 0; i < func->vreg_count; i++) {
        res.vreg_to_phys[i] = -1;
        res.vreg_to_spill[i] = -1;
    }

    if (func->vreg_count == 0) return res;

    LiveRange *ranges = calloc(func->vreg_count, sizeof(LiveRange));
    compute_liveness(func, ranges);

    int reg_free_at[PHYS_REG_COUNT];
    for (int i = 0; i < PHYS_REG_COUNT; i++) reg_free_at[i] = -1;

    // Params come in via x0..x7. Reserve those mappings up to the
    // param's last use, then the register can be recycled.
    int param_regs = func->param_count < 8 ? func->param_count : 8;
    for (int i = 0; i < param_regs; i++) {
        res.vreg_to_phys[i] = i;
        reg_free_at[i] = ranges[i].end;
    }

    for (int i = func->param_count; i < func->vreg_count; i++) {
        if (ranges[i].start < 0) continue; // unused vreg

        int assigned = -1;
        if (ranges[i].crosses_call) {
            // Must use a callee-save register, or spill.
            for (int r = 19; r <= 28; r++) {
                if (reg_free_at[r] < ranges[i].start) { assigned = r; break; }
            }
        } else {
            // Prefer a non-arg temporary in [x8, x18]; fall back to
            // arg registers [x0, x7] only if everything else is busy.
            for (int r = 8; r <= 18; r++) {
                if (reg_free_at[r] < ranges[i].start) { assigned = r; break; }
            }
            if (assigned < 0) {
                for (int r = 0; r < param_regs; r++) {
                    if (reg_free_at[r] < ranges[i].start) { assigned = r; break; }
                }
            }
            // Last resort: a callee-save reg that happens to be free.
            // Cheap to use; we just pay the prologue/epilogue save.
            if (assigned < 0) {
                for (int r = 19; r <= 28; r++) {
                    if (reg_free_at[r] < ranges[i].start) { assigned = r; break; }
                }
            }
        }

        if (assigned >= 0) {
            res.vreg_to_phys[i] = assigned;
            reg_free_at[assigned] = ranges[i].end;
        } else {
            // Spill to stack. Slot indices stay 1-based to keep the
            // historical SPILL_BASE arithmetic compatible with iremit's
            // re-base into the post-locals area.
            res.vreg_to_spill[i] = (res.spill_count + 1) * SPILL_BASE;
            res.spill_count++;
        }
    }

    free(ranges);
    return res;
}
