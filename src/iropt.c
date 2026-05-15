// IR optimization passes (P5 scaffold).
//
// Runs after irgen and before regalloc. Every pass takes an in-place
// pointer to the IRProgram and mutates it; passes are responsible for
// preserving CFG well-formedness (every block ends in a terminator),
// SSA-style vreg numbering (each vreg defined exactly once), and the
// existing RAII bookkeeping (the irgen-emitted _heap_free calls must
// survive every transformation).
//
// Pass ordering, once #22-#26 land, will be:
//
//   1. P5.1 inlining
//      Replaces the AST-level single-give inliner. Inlines small
//      functions whose body fits a budget, then recurses on the
//      caller. Should run first because it exposes more optimization
//      opportunities to every later pass.
//
//   2. P5.2 SRA (scalar replacement of aggregates)
//      Promotes fields of non-escaping structs to virtual registers.
//      Depends on inlining: most struct allocations only become
//      non-escaping after their constructor + accessor methods are
//      inlined into the caller.
//
//   3. P5.3 escape analysis + stack allocation
//      Lowers heap allocations that don't escape into stack
//      allocations. Depends on SRA: a struct that's been fully
//      replaced with vregs has no remaining heap pointer to lower.
//
//   4. P5.4 bounds-check elimination
//      Drops `_panic_oob` calls when range analysis proves the
//      index is safe. Independent of the others, but cheaper to
//      run after they've shrunk the IR.
//
//   5. P5.5 loop-invariant code motion
//      Hoists invariant operations out of loops. Best run last so
//      it sees the final shape after every other transformation.
//
// Today this file is just the scaffold: the dispatch table is empty,
// `iropt_run` is a no-op for every level. Each P5.x commit adds one
// table entry and one new function.

#include <stdlib.h>
#include <string.h>
#include "iropt.h"

typedef void (*IROptPass)(IRProgram *ir);

typedef struct {
    const char *name;       // diagnostic-friendly pass name
    IROptLevel  min_level;  // smallest -O level that enables the pass
    IROptPass   run;        // the actual transformation
} IROptPassEntry;

// Forward declarations of the per-pass entry points. Each lives below
// in its own clearly-marked section.
static void iropt_inline(IRProgram *ir);

// Pass dispatch table. New passes should be inserted in the canonical
// order documented at the top of this file (inlining first because it
// exposes the most opportunities for everything that follows).
static const IROptPassEntry g_passes[] = {
    { "inlining",        IROPT_O1, iropt_inline },          // P5.1
    // { "sra",             IROPT_O1, iropt_sra },             // P5.2
    // { "escape-analysis", IROPT_O1, iropt_escape_analysis }, // P5.3
    // { "bce",             IROPT_O1, iropt_bce },             // P5.4
    // { "licm",            IROPT_O1, iropt_licm },            // P5.5
    { NULL, IROPT_O0, NULL }, // sentinel
};

void iropt_run(IRProgram *ir, IROptLevel level) {
    if (!ir || level == IROPT_O0) return;
    for (const IROptPassEntry *p = g_passes; p->name != NULL; p++) {
        if (level >= p->min_level && p->run) {
            p->run(ir);
        }
    }
}

// ============================================================
// P5.1 — aggressive inlining (single-block leaf functions)
// ============================================================
//
// What this pass does today: replace every IR_CALL whose callee is a
// trivially-leaf function — exactly one basic block, no nested calls,
// terminated by IR_RET / IR_RET_VOID — with a copy of the callee's
// body inlined at the call site. Callee param vregs are substituted
// with the caller's argument vregs; callee local slots are remapped
// to fresh caller slots; callee's IR_RET becomes IR_COPY to the call
// site's destination vreg.
//
// What this does NOT yet do:
//   - Multi-block callees (would need label remapping + block
//     splitting at the call site).
//   - Callees containing IR_CALL (would need recursion-aware budget).
//   - Recursive functions (would loop forever).
//   - Public-symbol elimination (the inlined function is still
//     emitted as a standalone symbol, taking its share of code size;
//     a follow-up dead-symbol pass could prune unused ones).
//
// Why even the trivial case is valuable: post-monomorphization,
// every accessor on a generic type (`Box<T>.get`, `Pair<K,V>.key`,
// etc.) is a single-block function with a load + return. Without
// inlining, every read of `b.value` goes through a `bl _Box_get`
// with stack-spill overhead; with inlining, it becomes a direct
// `ldr`. That's the single biggest perf delta on the std/map hot
// path.

// Lookup: find the IRFunc with the given name, or NULL.
static IRFunc *find_func(IRProgram *ir, const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < ir->func_count; i++) {
        if (ir->funcs[i].name && !strcmp(ir->funcs[i].name, name))
            return &ir->funcs[i];
    }
    return NULL;
}

// Inlinability check: returns 1 iff the callee can be inlined verbatim
// into a single caller block. Conservative; a future commit can
// loosen any of these.
//
// Note: IR free functions and methods don't always terminate in
// IR_RET / IR_RET_VOID — irgen relies on iremit's fall-off epilogue
// for void-returning functions whose body never executes a `give`.
// Such bodies are still inlinable; we treat the absence of a
// terminator as an implicit IR_RET_VOID.
static int is_inlinable(IRFunc *callee) {
    if (!callee) return 0;
    if (callee->block_count != 1) return 0;
    IRBlock *b = &callee->blocks[0];
    if (b->count == 0) return 0;
    // Reject if the body contains any nested call or any branch:
    // this pass only handles straight-line single-block bodies.
    for (int i = 0; i < b->count; i++) {
        IROp op = b->insts[i].op;
        if (op == IR_CALL) return 0;
        if (op == IR_BR || op == IR_BR_COND || op == IR_LABEL) return 0;
    }
    return 1;
}

// Remap a vreg from the callee's numbering to the caller's. Callee's
// param vregs (0..param_count-1) map to the caller's argument vregs;
// every other callee vreg gets a fresh number drawn from the caller's
// vreg_count counter via the `vreg_map` table.
//
// vreg_map[i] holds the caller-vreg that callee vreg i maps to, or -1
// if not yet assigned. A callee vreg appearing as a use before its
// def (shouldn't happen in well-formed IR but we guard anyway) gets
// allocated lazily.
static VReg remap_vreg(VReg v, int *vreg_map, int callee_vreg_count,
                       VReg *call_args, int param_count,
                       IRFunc *caller) {
    if (v < 0) return v;
    if (v < param_count) {
        // Param substitution: callee's vreg N is the Nth arg from the call site.
        return call_args[v];
    }
    if (v >= callee_vreg_count) return v; // defensive: shouldn't happen
    if (vreg_map[v] < 0) {
        vreg_map[v] = caller->vreg_count++;
    }
    return vreg_map[v];
}

// Replace a single call instruction inside `caller`'s `block` at
// position `call_idx` with the inlined body of `callee`. Returns the
// number of instructions inserted (so the caller-side iteration can
// skip past them).
static int inline_one_call(IRFunc *caller, IRBlock *block, int call_idx,
                           IRFunc *callee) {
    IRInst call = block->insts[call_idx];
    IRBlock *callee_b = &callee->blocks[0];

    // Allocate the vreg-remap table. Each callee vreg either maps to
    // a caller arg (if it's a param) or to a fresh caller vreg.
    int *vreg_map = malloc(callee->vreg_count * sizeof(int));
    for (int i = 0; i < callee->vreg_count; i++) vreg_map[i] = -1;

    // Allocate the local-slot remap table. Callee slot K maps to a
    // fresh caller slot; the caller's local_slots count grows by the
    // callee's count. (caller->local_slots is the slot_next counter
    // that irgen left at the end of generation; iremit sizes the
    // frame from this value.)
    int *slot_map = malloc(callee->local_slots * sizeof(int));
    for (int i = 0; i < callee->local_slots; i++) {
        slot_map[i] = caller->local_slots++;
    }

    // Build the new instruction sequence: callee body up to (but not
    // including) any explicit terminator, with vregs and slots
    // remapped, then a final IR_COPY for the return value if the
    // callee returns one.
    //
    // Three terminator shapes:
    //   IR_RET %x       — body ends with explicit return-with-value
    //   IR_RET_VOID     — body ends with explicit void return
    //   (none)          — fall-off; treat as implicit IR_RET_VOID
    int term_idx = callee_b->count - 1;
    IROp term_op = callee_b->insts[term_idx].op;
    int has_explicit_terminator = (term_op == IR_RET || term_op == IR_RET_VOID);
    int body_count = has_explicit_terminator ? term_idx : callee_b->count;
    int new_count = body_count;
    if (term_op == IR_RET && call.dst >= 0) new_count += 1; // IR_COPY

    IRInst *new_insts = malloc(new_count * sizeof(IRInst));
    int out = 0;
    for (int i = 0; i < body_count; i++) {
        IRInst src = callee_b->insts[i];
        IRInst dst_inst = src;
        // Remap dst (skip for instructions that don't write a vreg
        // — STORE/STORE_LOCAL/RET_VOID — but those have dst = -1 by
        // construction, which remap_vreg passes through unchanged).
        dst_inst.dst = remap_vreg(src.dst, vreg_map, callee->vreg_count,
                                  call.args, callee->param_count, caller);
        dst_inst.a = remap_vreg(src.a, vreg_map, callee->vreg_count,
                                call.args, callee->param_count, caller);
        dst_inst.b = remap_vreg(src.b, vreg_map, callee->vreg_count,
                                call.args, callee->param_count, caller);
        // Remap call args (defensive — is_inlinable already rejects
        // nested calls, so this branch is unreachable today, but
        // costs nothing and prevents a foot-gun if the inlinability
        // bar widens later).
        if (src.op == IR_CALL && src.args && src.arg_count > 0) {
            VReg *new_args = malloc(src.arg_count * sizeof(VReg));
            for (int j = 0; j < src.arg_count; j++) {
                new_args[j] = remap_vreg(src.args[j], vreg_map,
                                         callee->vreg_count,
                                         call.args, callee->param_count,
                                         caller);
            }
            dst_inst.args = new_args;
        }
        // Remap local slots (STORE_LOCAL / LOAD_LOCAL use `imm` as
        // the slot index).
        if (src.op == IR_STORE_LOCAL || src.op == IR_LOAD_LOCAL) {
            int s = (int)src.imm;
            if (s >= 0 && s < callee->local_slots) {
                dst_inst.imm = slot_map[s];
            }
        }
        new_insts[out++] = dst_inst;
    }
    if (term_op == IR_RET && call.dst >= 0) {
        // The terminator's value vreg becomes the source of an IR_COPY
        // into the call site's destination. (For RET_VOID we emit
        // nothing — call.dst is unused; if the caller sequenced the
        // call result anyway it would already have been a dead store.)
        VReg ret_v = remap_vreg(callee_b->insts[term_idx].a, vreg_map,
                                callee->vreg_count, call.args,
                                callee->param_count, caller);
        IRInst copy = {0};
        copy.op = IR_COPY;
        copy.dst = call.dst;
        copy.a = ret_v;
        copy.b = -1;
        new_insts[out++] = copy;
    }

    // Splice new_insts into block at call_idx, replacing the call.
    int new_size = (block->count - 1) + new_count;
    if (new_size > block->cap) {
        while (block->cap < new_size) block->cap = block->cap ? block->cap * 2 : 16;
        block->insts = realloc(block->insts, block->cap * sizeof(IRInst));
    }
    int tail_count = block->count - call_idx - 1;
    if (tail_count > 0) {
        memmove(&block->insts[call_idx + new_count],
                &block->insts[call_idx + 1],
                tail_count * sizeof(IRInst));
    }
    memcpy(&block->insts[call_idx], new_insts, new_count * sizeof(IRInst));
    block->count = new_size;

    free(new_insts);
    free(slot_map);
    free(vreg_map);
    return new_count;
}

// Inline every eligible call in every block of every function.
// Single-pass: an inlined body might expose new inlinable calls (e.g.
// after we materialise a Box<T>.get inside Pair<K,V>.get), but the
// trivial-leaf criterion guarantees no nested calls in inlined bodies,
// so a single pass suffices for the current bar. When the inlinability
// bar widens to allow nested calls, this should be a fixed-point loop.
static void iropt_inline(IRProgram *ir) {
    if (!ir || ir->func_count == 0) return;
    for (int fi = 0; fi < ir->func_count; fi++) {
        IRFunc *caller = &ir->funcs[fi];
        for (int bi = 0; bi < caller->block_count; bi++) {
            IRBlock *block = &caller->blocks[bi];
            for (int ii = 0; ii < block->count; ii++) {
                IRInst *inst = &block->insts[ii];
                if (inst->op != IR_CALL) continue;
                IRFunc *callee = find_func(ir, inst->str);
                if (!callee) continue;        // builtin or unknown — leave alone
                if (callee == caller) continue; // recursion: out of scope for v1
                if (!is_inlinable(callee)) continue;
                int inserted = inline_one_call(caller, block, ii, callee);
                // Step the iteration past the inlined body. The
                // inserted block may itself have new vregs we want to
                // see in subsequent passes, but for inlining alone
                // skipping is fine — those inserts are not calls
                // (is_inlinable rejected callees with nested calls).
                ii += inserted - 1; // -1 because the loop's ii++ adds 1
            }
        }
    }
}
