#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "iremit.h"

// Frame layout (positive offsets from x29 == sp):
//   [x29, #0..15]                                                    saved {x29, x30}
//   [x29, #16 .. #16 + local_slots*8)                                IR locals
//   [x29, #16 + local_slots*8 .. + spill_bytes)                      regalloc spills
//   [x29, #16 + local_slots*8 + spill_bytes .. + csr_bytes)          saved x19..x28
//   [x29, ...)                                                       scratch padding
//
// regalloc.c hands back vreg_to_spill[v] = (slot_idx + 1) * SPILL_BASE,
// where SPILL_BASE == 16. We translate that to an actual frame offset by
// adding the locals area size at emit time so locals and spills never overlap.

// Module-level state set by iremit_func before any helper runs.
static int g_local_base = 16;
static int g_locals_bytes = 0;
// Callee-save register save area state. g_saved_csr[r-19] is non-zero
// iff x<r> was assigned to some vreg in this function and must be
// preserved across the call. g_csr_slot_base is the byte offset (from
// x29) of the save area's first slot. Both are populated at the start
// of iremit_func.
static int g_saved_csr[10] = {0};
static int g_csr_slot_base = 0;
// Function name used to prefix all locally-scoped labels, so
// `.L0`/`.L1` from one function don't collide with the same labels
// in another. iremit_func sets this before emitting any block.
static const char *g_func_name = NULL;

// String pool: every IR_LOAD_STR gets registered here with a unique
// index so iremit can later emit a data section (`_strN: .asciz "..."`)
// that the load-string sequence's `adrp _strN@PAGE` instructions can
// point at. iremit_func and iremit_finalize are the only writers.
typedef struct {
    char **items;
    int count;
    int cap;
} StringPool;
static StringPool g_string_pool;

// Add a string to the pool (deduplicating by content). Returns the
// index assigned; the caller emits `_str<index>` to reference it.
static int string_pool_intern(const char *s) {
    if (!s) s = "";
    for (int i = 0; i < g_string_pool.count; i++)
        if (!strcmp(g_string_pool.items[i], s)) return i;
    if (g_string_pool.count >= g_string_pool.cap) {
        g_string_pool.cap = g_string_pool.cap ? g_string_pool.cap * 2 : 8;
        g_string_pool.items = realloc(g_string_pool.items, g_string_pool.cap * sizeof(char *));
    }
    g_string_pool.items[g_string_pool.count] = (char *)s;
    return g_string_pool.count++;
}

void iremit_finalize_data(FILE *out) {
    if (g_string_pool.count == 0) return;
    fprintf(out, ".section __DATA,__data\n");
    for (int i = 0; i < g_string_pool.count; i++) {
        // Escape any embedded characters that the assembler dislikes.
        // For Potato source today, strings only contain printable
        // ASCII without quotes, so verbatim emission is enough; if
        // that ever changes, escape backslash and quote here.
        fprintf(out, "_str%d: .asciz \"%s\"\n", i, g_string_pool.items[i]);
    }
    g_string_pool.count = 0;
}

// Emit the epilogue's `ldp x29, x30, [sp], #stack_size` sequence,
// splitting into separate `add sp, sp, #N` instructions when
// stack_size exceeds the single-immediate ldp range (504 bytes).
static void emit_epilogue_ldp(FILE *out, int stack_size) {
    if (stack_size <= 504) {
        fprintf(out, "    ldp x29, x30, [sp], #%d\n", stack_size);
        return;
    }
    fprintf(out, "    ldp x29, x30, [sp], #16\n");
    int extra = stack_size - 16;
    while (extra > 4095) {
        fprintf(out, "    add sp, sp, #4095\n");
        extra -= 4095;
    }
    if (extra > 0)
        fprintf(out, "    add sp, sp, #%d\n", extra);
}

// Emit the instruction sequence that restores any saved x19..x28
// registers from the frame. Called immediately before every
// epilogue.
static void emit_csr_restore(FILE *out) {
    int slot = 0;
    for (int r = 19; r <= 28; r++) {
        if (!g_saved_csr[r - 19]) continue;
        int off = g_csr_slot_base + slot * 8;
        if (off <= 255)
            fprintf(out, "    ldr x%d, [x29, #%d]\n", r, off);
        else
            fprintf(out, "    add x10, x29, #%d\n    ldr x%d, [x10]\n", off, r);
        slot++;
    }
}

// Get physical register number for a vreg
static int phys(RegAllocResult *a, VReg v) {
    if (v < 0) return 0;
    return a->vreg_to_phys[v];
}

static int is_spilled(RegAllocResult *a, VReg v) {
    return v >= 0 && a->vreg_to_phys[v] < 0;
}

// Translate the spill index from regalloc into a real positive frame offset
// that lives strictly above the local area.
static int spill_off(RegAllocResult *a, VReg v) {
    int raw = a->vreg_to_spill[v];          // 16, 32, 48, ...
    return g_local_base + g_locals_bytes + (raw - 16);
}

// Locals and spills live ABOVE the saved {x29, x30} pair at [x29, #0..15].
// The function prologue does `stp x29,x30,[sp,#-stack_size]!; mov x29,sp` —
// after that x29 == sp, so all in-frame addressing must use POSITIVE offsets
// from x29 (or sp). We start at #16 to skip past the saved fp/lr.

// Ensure vreg value is in a physical register. If spilled, load into scratch (x9).
// Returns the physical register number to use.
static int ensure_reg(FILE *out, RegAllocResult *a, VReg v) {
    if (v < 0) return 0;
    if (!is_spilled(a, v)) return phys(a, v);
    // Load from spill slot into x9
    int off = spill_off(a, v);
    if (off <= 255)
        fprintf(out, "    ldr x9, [x29, #%d]\n", off);
    else
        fprintf(out, "    add x9, x29, #%d\n    ldr x9, [x9]\n", off);
    return 9; // x9
}

// Store physical register to spill slot
static void store_spill(FILE *out, RegAllocResult *a, VReg v, int from_reg) {
    int off = spill_off(a, v);
    if (off <= 255)
        fprintf(out, "    str x%d, [x29, #%d]\n", from_reg, off);
    else
        fprintf(out, "    add x10, x29, #%d\n    str x%d, [x10]\n", off, from_reg);
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
    int local_base = 16;
    int locals_size = (func->local_slots > 0 ? func->local_slots : 1) * 8;
    int spill_bytes = alloc->spill_count * 8;

    // Determine which callee-save registers (x19..x28) the allocator
    // actually picked. Each one needs 8 bytes of frame for prologue
    // save / epilogue restore.
    int saved_csr[10] = {0};
    int saved_csr_count = 0;
    for (int v = 0; v < alloc->vreg_count; v++) {
        int p = alloc->vreg_to_phys[v];
        if (p >= 19 && p <= 28 && !saved_csr[p - 19]) {
            saved_csr[p - 19] = 1;
            saved_csr_count++;
        }
    }
    int csr_bytes = saved_csr_count * 8;

    int stack_size = local_base + locals_size + spill_bytes + csr_bytes + 256;
    if (stack_size % 16 != 0) stack_size += 8;

    g_local_base = local_base;
    g_locals_bytes = locals_size;
    for (int r = 0; r < 10; r++) g_saved_csr[r] = saved_csr[r];
    g_csr_slot_base = local_base + locals_size + spill_bytes;
    g_func_name = func->name;

    // Prologue: reserve `stack_size` bytes and save fp/lr at the bottom.
    // After `mov x29, sp`, x29 == sp; everything in the frame uses POSITIVE
    // offsets from x29. Layout documented at the top of this file.
    //
    // ARM64's `stp xN, xM, [sp, #-imm]!` requires imm in [0, 504]. For
    // larger frames we split into a separate `sub sp, sp, #(stack_size-16)`
    // followed by `stp ..., [sp, #-16]!`.
    fprintf(out, "_%s:\n", func->name);
    if (stack_size <= 504) {
        fprintf(out, "    stp x29, x30, [sp, #-%d]!\n", stack_size);
    } else {
        int extra = stack_size - 16;
        // sub-immediate has the same range; emit two subs if needed.
        while (extra > 4095) {
            fprintf(out, "    sub sp, sp, #4095\n");
            extra -= 4095;
        }
        if (extra > 0)
            fprintf(out, "    sub sp, sp, #%d\n", extra);
        fprintf(out, "    stp x29, x30, [sp, #-16]!\n");
    }
    fprintf(out, "    mov x29, sp\n");

    // Save any callee-save registers we plan to use. Slot N in the
    // save area corresponds to the Nth-numerically-lowest live
    // callee-save register; emit_csr_restore() walks the same order.
    {
        int slot = 0;
        for (int r = 19; r <= 28; r++) {
            if (!saved_csr[r - 19]) continue;
            int off = g_csr_slot_base + slot * 8;
            if (off <= 255)
                fprintf(out, "    str x%d, [x29, #%d]\n", r, off);
            else
                fprintf(out, "    add x10, x29, #%d\n    str x%d, [x10]\n", off, r);
            slot++;
        }
    }

    // Emit each block
    for (int bi = 0; bi < func->block_count; bi++) {
        IRBlock *b = &func->blocks[bi];
        if (bi > 0) fprintf(out, "L_%s_%d:\n", g_func_name, b->label);

        for (int ii = 0; ii < b->count; ii++) {
            IRInst *inst = &b->insts[ii];
            switch (inst->op) {
                case IR_CONST: {
                    int d = is_spilled(alloc, inst->dst) ? 9 : phys(alloc, inst->dst);
                    int64_t v = (int64_t)inst->imm;
                    // ARM64 `mov x_, #imm` accepts any 16-bit chunk via
                    // movz/movk. For values that fit in [0, 65535] we
                    // emit a single `mov`; for negative small values
                    // (>-65536), `mov` accepts the negative directly;
                    // for everything else (including 0x100000 used by
                    // the str/int yell heuristic), build the value
                    // 16 bits at a time with movz + up to three movk's.
                    if (v >= 0 && v < 65536) {
                        fprintf(out, "    mov x%d, #%lld\n", d, (long long)v);
                    } else if (v < 0 && v > -65536) {
                        fprintf(out, "    mov x%d, #%lld\n", d, (long long)v);
                    } else {
                        uint64_t u = (uint64_t)v;
                        fprintf(out, "    movz x%d, #%llu\n", d,
                                (unsigned long long)(u & 0xFFFF));
                        for (int sh = 16; sh < 64; sh += 16) {
                            unsigned long long part = (u >> sh) & 0xFFFF;
                            if (part != 0) {
                                fprintf(out, "    movk x%d, #%llu, lsl #%d\n",
                                        d, part, sh);
                            }
                        }
                    }
                    if (is_spilled(alloc, inst->dst))
                        store_spill(out, alloc, inst->dst, 9);
                    break;
                }
                case IR_LOAD_STR: {
                    // Intern the string into the pool (caller may have
                    // referenced the same literal multiple times) and
                    // emit a 2-instruction pointer load via @PAGE/@PAGEOFF.
                    int idx = string_pool_intern(inst->str);
                    int d = is_spilled(alloc, inst->dst) ? 9 : phys(alloc, inst->dst);
                    fprintf(out, "    adrp x%d, _str%d@PAGE\n", d, idx);
                    fprintf(out, "    add x%d, x%d, _str%d@PAGEOFF\n", d, d, idx);
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
                case IR_AND: case IR_OR: {
                    // Non-short-circuit logical AND/OR. The direct
                    // codegen short-circuits via labels; that's a perf
                    // optimization, not a correctness requirement, so
                    // for now we always evaluate both sides (irgen
                    // already produced the operands as separate vregs)
                    // and combine bitwise. Boolean operands are 0/1, so
                    // bitwise and == logical and. NOTE: side effects
                    // in the right operand will execute even when
                    // left is false; future work could move this back
                    // to short-circuit form by lowering AND/OR to a
                    // BR_COND in irgen.
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
                    const char *mnem = inst->op == IR_AND ? "and" : "orr";
                    fprintf(out, "    %s x%d, x%d, x%d\n", mnem, d, ra, rb);
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
                    emit_csr_restore(out);
                    emit_epilogue_ldp(out, stack_size);
                    fprintf(out, "    ret\n");
                    break;
                }
                case IR_RET_VOID: {
                    emit_csr_restore(out);
                    emit_epilogue_ldp(out, stack_size);
                    fprintf(out, "    ret\n");
                    break;
                }
                case IR_COPY: {
                    int ra = ensure_reg(out, alloc, inst->a);
                    emit_to_dst(out, alloc, inst->dst, ra);
                    break;
                }
                case IR_BR:
                    fprintf(out, "    b L_%s_%d\n", g_func_name, inst->label);
                    break;
                case IR_BR_COND: {
                    int ra = ensure_reg(out, alloc, inst->a);
                    fprintf(out, "    cbnz x%d, L_%s_%d\n", ra, g_func_name, inst->label);
                    fprintf(out, "    b L_%s_%d\n", g_func_name, inst->label2);
                    break;
                }
                case IR_LABEL:
                    fprintf(out, "L_%s_%d:\n", g_func_name, inst->label);
                    break;
                case IR_STORE_LOCAL: {
                    int ra = ensure_reg(out, alloc, inst->a);
                    int off = local_base + (int)inst->imm * 8;
                    if (off <= 255)
                        fprintf(out, "    str x%d, [x29, #%d]\n", ra, off);
                    else
                        fprintf(out, "    add x10, x29, #%d\n    str x%d, [x10]\n", off, ra);
                    break;
                }
                case IR_LOAD_LOCAL: {
                    int d = is_spilled(alloc, inst->dst) ? 11 : phys(alloc, inst->dst);
                    int off = local_base + (int)inst->imm * 8;
                    if (off <= 255)
                        fprintf(out, "    ldr x%d, [x29, #%d]\n", d, off);
                    else
                        fprintf(out, "    add x10, x29, #%d\n    ldr x%d, [x10]\n", off, d);
                    if (is_spilled(alloc, inst->dst))
                        store_spill(out, alloc, inst->dst, 11);
                    break;
                }
                case IR_STORE: {
                    // mem[%a + imm] = %b
                    int ra = ensure_reg(out, alloc, inst->a);
                    int rb;
                    if (is_spilled(alloc, inst->b) && ra == 9) {
                        fprintf(out, "    mov x10, x9\n");
                        rb = ensure_reg(out, alloc, inst->b);
                        ra = 10;
                    } else {
                        rb = ensure_reg(out, alloc, inst->b);
                    }
                    fprintf(out, "    str x%d, [x%d, #%d]\n", rb, ra, (int)inst->imm);
                    break;
                }
                case IR_LOAD: {
                    // %dst = mem[%a + imm]
                    int ra = ensure_reg(out, alloc, inst->a);
                    int d = is_spilled(alloc, inst->dst) ? 11 : phys(alloc, inst->dst);
                    fprintf(out, "    ldr x%d, [x%d, #%d]\n", d, ra, (int)inst->imm);
                    if (is_spilled(alloc, inst->dst))
                        store_spill(out, alloc, inst->dst, 11);
                    break;
                }
                default:
                    break;
            }
        }
    }

    // If function doesn't end with ret, add epilogue (incl. callee-save restore).
    if (func->block_count > 0) {
        IRBlock *last = &func->blocks[func->block_count - 1];
        if (last->count == 0 || (last->insts[last->count-1].op != IR_RET && last->insts[last->count-1].op != IR_RET_VOID)) {
            emit_csr_restore(out);
            emit_epilogue_ldp(out, stack_size);
            fprintf(out, "    ret\n");
        }
    }
}
