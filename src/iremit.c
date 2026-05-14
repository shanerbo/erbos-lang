#include <stdlib.h>
#include <string.h>
#include "iremit.h"

// Get physical register number for a vreg
static int phys(RegAllocResult *a, VReg v) {
    if (v < 0) return 0;
    return a->vreg_to_phys[v];
}

static int is_spilled(RegAllocResult *a, VReg v) {
    return v >= 0 && a->vreg_to_phys[v] < 0;
}

static int spill_off(RegAllocResult *a, VReg v) {
    return a->vreg_to_spill[v];
}

// Ensure vreg value is in a physical register. If spilled, load into scratch (x9).
// Returns the physical register number to use.
static int ensure_reg(FILE *out, RegAllocResult *a, VReg v) {
    if (v < 0) return 0;
    if (!is_spilled(a, v)) return phys(a, v);
    // Load from spill slot into x9
    int off = spill_off(a, v);
    if (off <= 255)
        fprintf(out, "    ldr x9, [x29, #-%d]\n", off);
    else
        fprintf(out, "    sub x9, x29, #%d\n    ldr x9, [x9]\n", off);
    return 9; // x9
}

// Store physical register to spill slot
static void store_spill(FILE *out, RegAllocResult *a, VReg v, int from_reg) {
    int off = spill_off(a, v);
    if (off <= 255)
        fprintf(out, "    str x%d, [x29, #-%d]\n", from_reg, off);
    else
        fprintf(out, "    sub x10, x29, #%d\n    str x%d, [x10]\n", off, from_reg);
}

// Emit result to dst vreg (physical or spill)
static void emit_to_dst(FILE *out, RegAllocResult *a, VReg dst, int src_reg) {
    if (dst < 0) return;
    if (!is_spilled(a, dst)) {
        int d = phys(a, dst);
        if (d != src_reg)
            fprintf(out, "    mov x%d, x%d\n", d, src_reg);
    } else {
        store_spill(out, a, dst, src_reg);
    }
}

void iremit_func(FILE *out, IRFunc *func, RegAllocResult *alloc) {
    int stack_size = (alloc->spill_count + 2) * 16; // +2 for fp/lr
    if (stack_size % 16 != 0) stack_size += 8;

    // Prologue
    fprintf(out, "_%s:\n", func->name);
    fprintf(out, "    stp x29, x30, [sp, #-%d]!\n", stack_size);
    fprintf(out, "    mov x29, sp\n");

    // Emit each block
    for (int bi = 0; bi < func->block_count; bi++) {
        IRBlock *b = &func->blocks[bi];
        if (bi > 0) fprintf(out, ".L%d:\n", b->label);

        for (int ii = 0; ii < b->count; ii++) {
            IRInst *inst = &b->insts[ii];
            switch (inst->op) {
                case IR_CONST: {
                    int d = is_spilled(alloc, inst->dst) ? 9 : phys(alloc, inst->dst);
                    if (inst->imm >= 0 && inst->imm < 65536)
                        fprintf(out, "    mov x%d, #%lld\n", d, (long long)inst->imm);
                    else if (inst->imm < 0 && inst->imm > -65536)
                        fprintf(out, "    mov x%d, #%lld\n", d, (long long)inst->imm);
                    else
                        fprintf(out, "    mov x%d, #%lld\n", d, (long long)(inst->imm & 0xFFFF));
                    if (is_spilled(alloc, inst->dst))
                        store_spill(out, alloc, inst->dst, 9);
                    break;
                }
                case IR_ADD: case IR_SUB: case IR_MUL: {
                    int ra = ensure_reg(out, alloc, inst->a);
                    // Save ra if it's x9 and b is also spilled
                    int rb;
                    if (is_spilled(alloc, inst->b) && ra == 9) {
                        fprintf(out, "    mov x10, x9\n");
                        rb = ensure_reg(out, alloc, inst->b);
                        ra = 10;
                    } else {
                        rb = ensure_reg(out, alloc, inst->b);
                    }
                    int d = is_spilled(alloc, inst->dst) ? 11 : phys(alloc, inst->dst);
                    const char *op = inst->op == IR_ADD ? "add" : inst->op == IR_SUB ? "sub" : "mul";
                    fprintf(out, "    %s x%d, x%d, x%d\n", op, d, ra, rb);
                    if (is_spilled(alloc, inst->dst))
                        store_spill(out, alloc, inst->dst, 11);
                    break;
                }
                case IR_DIV: case IR_MOD: {
                    int ra = ensure_reg(out, alloc, inst->a);
                    int rb;
                    if (is_spilled(alloc, inst->b) && ra == 9) {
                        fprintf(out, "    mov x10, x9\n");
                        rb = ensure_reg(out, alloc, inst->b);
                        ra = 10;
                    } else {
                        rb = ensure_reg(out, alloc, inst->b);
                    }
                    int d = is_spilled(alloc, inst->dst) ? 11 : phys(alloc, inst->dst);
                    fprintf(out, "    sdiv x%d, x%d, x%d\n", d, ra, rb);
                    if (inst->op == IR_MOD) {
                        fprintf(out, "    msub x%d, x%d, x%d, x%d\n", d, d, rb, ra);
                    }
                    if (is_spilled(alloc, inst->dst))
                        store_spill(out, alloc, inst->dst, 11);
                    break;
                }
                case IR_NEG: {
                    int ra = ensure_reg(out, alloc, inst->a);
                    int d = is_spilled(alloc, inst->dst) ? 11 : phys(alloc, inst->dst);
                    fprintf(out, "    neg x%d, x%d\n", d, ra);
                    if (is_spilled(alloc, inst->dst))
                        store_spill(out, alloc, inst->dst, 11);
                    break;
                }
                case IR_NOT: {
                    int ra = ensure_reg(out, alloc, inst->a);
                    int d = is_spilled(alloc, inst->dst) ? 11 : phys(alloc, inst->dst);
                    fprintf(out, "    cmp x%d, #0\n", ra);
                    fprintf(out, "    cset x%d, eq\n", d);
                    if (is_spilled(alloc, inst->dst))
                        store_spill(out, alloc, inst->dst, 11);
                    break;
                }
                case IR_CMP_EQ: case IR_CMP_NE: case IR_CMP_LT:
                case IR_CMP_GT: case IR_CMP_LE: case IR_CMP_GE: {
                    int ra = ensure_reg(out, alloc, inst->a);
                    int rb;
                    if (is_spilled(alloc, inst->b) && ra == 9) {
                        fprintf(out, "    mov x10, x9\n");
                        rb = ensure_reg(out, alloc, inst->b);
                        ra = 10;
                    } else {
                        rb = ensure_reg(out, alloc, inst->b);
                    }
                    int d = is_spilled(alloc, inst->dst) ? 11 : phys(alloc, inst->dst);
                    fprintf(out, "    cmp x%d, x%d\n", ra, rb);
                    const char *cond;
                    switch (inst->op) {
                        case IR_CMP_EQ: cond = "eq"; break;
                        case IR_CMP_NE: cond = "ne"; break;
                        case IR_CMP_LT: cond = "lt"; break;
                        case IR_CMP_GT: cond = "gt"; break;
                        case IR_CMP_LE: cond = "le"; break;
                        default: cond = "ge"; break;
                    }
                    fprintf(out, "    cset x%d, %s\n", d, cond);
                    if (is_spilled(alloc, inst->dst))
                        store_spill(out, alloc, inst->dst, 11);
                    break;
                }
                case IR_CALL: {
                    // Move args to x0-x7
                    for (int ai = 0; ai < inst->arg_count && ai < 8; ai++) {
                        int r = ensure_reg(out, alloc, inst->args[ai]);
                        if (r != ai)
                            fprintf(out, "    mov x%d, x%d\n", ai, r);
                    }
                    fprintf(out, "    bl _%s\n", inst->str);
                    // Result in x0 → move to dst
                    emit_to_dst(out, alloc, inst->dst, 0);
                    break;
                }
                case IR_RET: {
                    int ra = ensure_reg(out, alloc, inst->a);
                    if (ra != 0)
                        fprintf(out, "    mov x0, x%d\n", ra);
                    fprintf(out, "    ldp x29, x30, [sp], #%d\n", stack_size);
                    fprintf(out, "    ret\n");
                    break;
                }
                case IR_RET_VOID: {
                    fprintf(out, "    ldp x29, x30, [sp], #%d\n", stack_size);
                    fprintf(out, "    ret\n");
                    break;
                }
                case IR_COPY: {
                    int ra = ensure_reg(out, alloc, inst->a);
                    emit_to_dst(out, alloc, inst->dst, ra);
                    break;
                }
                case IR_BR:
                    fprintf(out, "    b .L%d\n", inst->label);
                    break;
                case IR_BR_COND: {
                    int ra = ensure_reg(out, alloc, inst->a);
                    fprintf(out, "    cbnz x%d, .L%d\n", ra, inst->label);
                    fprintf(out, "    b .L%d\n", inst->label2);
                    break;
                }
                case IR_LABEL:
                    fprintf(out, ".L%d:\n", inst->label);
                    break;
                default:
                    break;
            }
        }
    }

    // If function doesn't end with ret, add epilogue
    if (func->block_count > 0) {
        IRBlock *last = &func->blocks[func->block_count - 1];
        if (last->count == 0 || (last->insts[last->count-1].op != IR_RET && last->insts[last->count-1].op != IR_RET_VOID)) {
            fprintf(out, "    ldp x29, x30, [sp], #%d\n", stack_size);
            fprintf(out, "    ret\n");
        }
    }
}
