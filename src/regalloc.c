#include <stdlib.h>
#include <string.h>
#include "regalloc.h"

// Linear scan register allocator
// Simple: assign registers in order, spill when full.
// Liveness: a vreg is live from its definition to its last use.

typedef struct {
    int vreg;
    int start;  // instruction index of definition
    int end;    // instruction index of last use
} LiveRange;

static void compute_liveness(IRFunc *func, LiveRange *ranges) {
    // Initialize: each vreg starts and ends at its definition
    for (int i = 0; i < func->vreg_count; i++) {
        ranges[i].vreg = i;
        ranges[i].start = -1;
        ranges[i].end = -1;
    }

    // Walk all instructions, track first def and last use
    int inst_idx = 0;
    for (int bi = 0; bi < func->block_count; bi++) {
        IRBlock *b = &func->blocks[bi];
        for (int ii = 0; ii < b->count; ii++) {
            IRInst *inst = &b->insts[ii];
            // Definition
            if (inst->dst >= 0 && inst->dst < func->vreg_count) {
                if (ranges[inst->dst].start < 0)
                    ranges[inst->dst].start = inst_idx;
                ranges[inst->dst].end = inst_idx;
            }
            // Uses
            if (inst->a >= 0 && inst->a < func->vreg_count)
                ranges[inst->a].end = inst_idx;
            if (inst->b >= 0 && inst->b < func->vreg_count)
                ranges[inst->b].end = inst_idx;
            // Call args
            if (inst->op == IR_CALL && inst->args) {
                for (int ai = 0; ai < inst->arg_count; ai++)
                    if (inst->args[ai] >= 0 && inst->args[ai] < func->vreg_count)
                        ranges[inst->args[ai]].end = inst_idx;
            }
            inst_idx++;
        }
    }
}

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

    // Compute live ranges
    LiveRange *ranges = calloc(func->vreg_count, sizeof(LiveRange));
    compute_liveness(func, ranges);

    // Linear scan: assign physical registers
    // Track which physical regs are free and when they become free
    int reg_free_at[PHYS_REG_COUNT]; // instruction index when reg becomes free
    for (int i = 0; i < PHYS_REG_COUNT; i++) reg_free_at[i] = -1;

    // Reserve x0 for return values and first few for params
    // Params get x0..x7, allocate others from x8 up
    int next_reg = func->param_count < 8 ? func->param_count : 8;

    // Assign params to x0..x7
    for (int i = 0; i < func->param_count && i < 8; i++) {
        res.vreg_to_phys[i] = i;
        reg_free_at[i] = ranges[i].end;
    }

    // Assign remaining vregs
    for (int i = func->param_count; i < func->vreg_count; i++) {
        if (ranges[i].start < 0) continue; // unused vreg

        // Try to find a free register
        int assigned = -1;

        // First: check if any reg's live range has ended
        for (int r = next_reg; r < PHYS_REG_COUNT; r++) {
            if (reg_free_at[r] < ranges[i].start) {
                assigned = r;
                break;
            }
        }
        // Also check lower regs (x0-x7) if they're free
        if (assigned < 0) {
            for (int r = 0; r < next_reg; r++) {
                if (reg_free_at[r] < ranges[i].start) {
                    assigned = r;
                    break;
                }
            }
        }

        if (assigned >= 0) {
            res.vreg_to_phys[i] = assigned;
            reg_free_at[assigned] = ranges[i].end;
        } else {
            // Spill to stack
            res.vreg_to_spill[i] = (res.spill_count + 1) * SPILL_BASE;
            res.spill_count++;
        }
    }

    free(ranges);
    return res;
}
