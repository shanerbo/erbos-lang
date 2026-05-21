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

    // Pass 1: first-def + last-use for every vreg, plus per-block
    // global instruction index ranges for the loop fixup. Also
    // tracks the cumulative count of IR_CALL instructions so Pass 2
    // can answer "does any call fall strictly inside [start, end)?"
    // in O(1) per vreg via a prefix-sum subtraction (T14).
    int *block_first = malloc(func->block_count * sizeof(int));
    int *block_last = malloc(func->block_count * sizeof(int));

    // T14: count total instructions to size the call-prefix array.
    int total_insts = 0;
    for (int bi = 0; bi < func->block_count; bi++)
        total_insts += func->blocks[bi].count;
    // call_prefix[k] = number of IR_CALL with global index < k. Length
    // total_insts + 1 so call_prefix[total_insts] is well-defined.
    int *call_prefix = calloc((size_t)total_insts + 1, sizeof(int));

    // T14: header-block lookup table. For every block label that's
    // actually used as a target, label_to_block[label] is the block
    // index (or -1 if no block has that label). Sized to max_label+1.
    int max_label = 0;
    for (int bi = 0; bi < func->block_count; bi++)
        if (func->blocks[bi].label > max_label)
            max_label = func->blocks[bi].label;
    int *label_to_block = malloc(((size_t)max_label + 1) * sizeof(int));
    for (int i = 0; i <= max_label; i++) label_to_block[i] = -1;
    for (int bi = 0; bi < func->block_count; bi++) {
        int lbl = func->blocks[bi].label;
        if (lbl >= 0 && lbl <= max_label) label_to_block[lbl] = bi;
    }

    int inst_idx = 0;
    for (int bi = 0; bi < func->block_count; bi++) {
        IRBlock *b = &func->blocks[bi];
        block_first[bi] = inst_idx;
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
            // T14: extend the call-prefix sum.
            call_prefix[inst_idx + 1] = call_prefix[inst_idx]
                                      + (inst->op == IR_CALL ? 1 : 0);
            inst_idx++;
        }
        block_last[bi] = inst_idx - 1;
    }

    // Pass 1b: loop back-edge extension. Linear-scan computes first-def
    // and last-use as if the function were straight-line, but a vreg
    // alive across the back-edge of a loop stays live across every
    // iteration of that loop. Without this, the allocator can reuse
    // the vreg's physical register for another vreg later in the same
    // linear range — the latter clobbers the former on the second
    // iteration. (P5.5 LICM is the first pass to systematically expose
    // cross-iteration live ranges; before LICM, irgen re-loaded such
    // values each iteration via IR_LOAD_LOCAL, hiding the issue.)
    //
    // Algorithm: for every terminator in block B that targets a block
    // H with H's block index <= B's, treat (H, B) as a loop. For each
    // vreg `v` whose live range ALREADY OVERLAPS [header_first,
    // back_last] (defined before back_last and used at or after
    // header_first), extend the range so it covers the whole [start,
    // max(end, back_last)]. We iterate to a fixed point because a
    // vreg whose range was extended by one back-edge might now also
    // overlap an enclosing back-edge.
    int ext_changed = 1;
    int ext_iters = 0;
    while (ext_changed && ext_iters < 64) {
        ext_changed = 0;
        ext_iters++;
        for (int bi = 0; bi < func->block_count; bi++) {
            IRBlock *b = &func->blocks[bi];
            if (b->count == 0) continue;
            IRInst *term = &b->insts[b->count - 1];
            int targets[2] = { -1, -1 };
            int n_targets = 0;
            if (term->op == IR_BR)        targets[n_targets++] = term->label;
            else if (term->op == IR_BR_COND) {
                targets[n_targets++] = term->label;
                targets[n_targets++] = term->label2;
            }
            for (int t = 0; t < n_targets; t++) {
                /* T14: O(1) header-block lookup via the precomputed
                 * label_to_block[] table. */
                int header_bi = (targets[t] >= 0 && targets[t] <= max_label)
                              ? label_to_block[targets[t]] : -1;
                if (header_bi < 0 || header_bi > bi) continue;
                int header_first = block_first[header_bi];
                int back_last = block_last[bi];
                for (int v = 0; v < func->vreg_count; v++) {
                    if (ranges[v].start < 0) continue;
                    // Range overlaps [header_first, back_last] if
                    // start <= back_last and end >= header_first.
                    if (ranges[v].start > back_last) continue;
                    if (ranges[v].end < header_first) continue;
                    if (ranges[v].end < back_last) {
                        ranges[v].end = back_last;
                        ext_changed = 1;
                    }
                }
            }
        }
    }
    free(block_first);
    free(block_last);
    free(label_to_block);


    // Pass 2: mark every vreg whose live range strictly contains an
    // IR_CALL. "Strictly contains" means the call instruction index is
    // in (start, end); a call that itself defines or last-uses the
    // vreg doesn't clobber it in flight.
    //
    // T14: O(V) total instead of O(I*V). call_prefix[k] = number of
    // IR_CALL at indices < k, so the count of calls strictly inside
    // (start, end) is call_prefix[end] - call_prefix[start + 1].
    // crosses_call iff that difference > 0.
    for (int v = 0; v < func->vreg_count; v++) {
        if (ranges[v].start < 0) continue;
        int s = ranges[v].start + 1;
        int e = ranges[v].end;
        if (s >= e) continue;
        if (call_prefix[e] - call_prefix[s] > 0)
            ranges[v].crosses_call = 1;
    }
    free(call_prefix);
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
            // Skip x9, x10, x11 — iremit reserves them as scratch for
            // large-offset frame addressing (`add x10, x29, #N` /
            // `ldr x9, [x29, #N]`), spill load/store sequences, and
            // IR_LOAD/IR_LOAD_LOCAL spill destinations (which write
            // x11 then store it). If a vreg landed in x9, x10, or
            // x11, those scratch sequences would clobber the value
            // being stored. (Originally only x9/x10 were skipped;
            // P5.5 LICM exposed the x11 case by introducing
            // cross-iteration vregs that get spilled, with the
            // spill-load using x11 as scratch and clobbering an
            // earlier still-live vreg in x11.)
            for (int r = 8; r <= 18; r++) {
                if (r == 9 || r == 10 || r == 11) continue;
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
