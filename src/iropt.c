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
#include <stdint.h>
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
static void iropt_sra(IRProgram *ir);
static void iropt_stackify(IRProgram *ir);
static void iropt_bce(IRProgram *ir);

// Pass dispatch table. New passes should be inserted in the canonical
// order documented at the top of this file (inlining first because it
// exposes the most opportunities for everything that follows).
static const IROptPassEntry g_passes[] = {
    { "inlining",        IROPT_O1, iropt_inline },          // P5.1
    { "sra",             IROPT_O1, iropt_sra },             // P5.2
    { "stackify",        IROPT_O1, iropt_stackify },        // P5.3
    { "bce",             IROPT_O1, iropt_bce },             // P5.4
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

// ============================================================
// P5.2 — scalar replacement of aggregates (SRA)
// ============================================================
//
// What this pass does: identify struct allocations whose pointer
// never escapes the function, then replace each field load/store
// with a load/store of a fresh "shadow" stack slot — one slot per
// field. The heap allocation itself (the `_alloc_<X>` call) and
// the matched RAII `_heap_free` call are deleted; the fields live
// in dedicated stack slots that the regalloc can promote to
// registers if their lifetime fits.
//
// Net effect for the std/map hot path: a Pair<K,V> constructed
// inside a method body and never returned / passed elsewhere stops
// being a heap allocation entirely, and its key/value field
// accesses become direct local-slot reads/writes instead of two-
// step `ldr ptr; ldr [ptr+0]` sequences.
//
// Conservative criteria for v1:
//   - Pointer is the dst of an `IR_CALL _alloc_<StructName>`.
//   - Pointer is `IR_STORE_LOCAL`'d to exactly one slot (the
//     var-decl's slot). That slot has no other writers.
//   - Every `IR_LOAD_LOCAL` of that slot produces a vreg whose
//     ENTIRE use set is constant-offset `IR_LOAD %dst, %p, off`
//     or `IR_STORE %p, %v, off`, OR the loaded vreg flows into a
//     `IR_CALL _heap_free %p, %size` (the RAII free we're about
//     to delete).
//   - No other instruction reads or writes the slot.
//
// What this does NOT yet do:
//   - Nested non-escaping allocations (a struct that owns another
//     struct as a field — would need transitive analysis).
//   - Escape via being passed `ref` to another method even if that
//     method is itself non-escaping.
//   - Cross-block escape analysis with branches/loops in the
//     promotion shape (today the regalloc routes locals through
//     stack slots so our shadow-slot rewrite is correct across
//     all CFG shapes; promotion to vregs is the regalloc's job).
//
// Implementation strategy: a single linear scan per function.
//
//   Pass 1 — discovery. For each function, find every
//     `_alloc_<StructName>` call. Look up the struct's field
//     count. Trace the call's dst vreg: it must be stored to
//     exactly one local slot, and the slot must not have other
//     writers. Then walk every IR_LOAD_LOCAL of that slot; for
//     each loaded vreg, check that all its uses are field
//     accesses or the matched _heap_free.
//
//   Pass 2 — rewrite. For each non-escaping slot:
//     - Allocate `field_count` fresh local slots (one per field).
//     - Replace the `_alloc_<X>` call with no-op (we'll mark the
//       instruction as IR_CONST 0 with dst preserved so existing
//       defs don't break; the result is dead and the regalloc's
//       liveness analysis won't allocate a register for it).
//     - Drop the IR_STORE_LOCAL that wrote the pointer to the slot.
//     - For each subsequent IR_LOAD_LOCAL %p, slot=S followed by
//       an IR_STORE %p, %v, off / IR_LOAD %d, %p, off, replace
//       the pair with a single IR_STORE_LOCAL %v, slot=field[off/8]
//       / IR_LOAD_LOCAL %d, slot=field[off/8].
//     - Drop the matched _heap_free call.
//
// We don't need pure SSA for the rewrite to be correct — locals
// already round-trip through stack slots, and the regalloc's
// cross-block call-aware allocation handles re-defined slots
// correctly.

// Lookup by name in the program's IR funcs, returning the struct
// field count if the call name is `_alloc_<StructName>` and the
// struct is registered. Returns -1 otherwise.
//
// The IRProgram parameter is currently unused — see comment below
// about why we trust observed offsets instead of consulting the
// struct registry — but we keep the signature stable so a future
// rev that does want to consult the IR side can plug in here
// without touching every call site.
static int alloc_call_field_count(IRProgram *ir, const char *call_name) {
    (void)ir;
    if (!call_name) return -1;
    if (strncmp(call_name, "alloc_", 6) != 0) return -1;
    // The IR pipeline emits `_alloc_<X>` as a separate symbol that
    // simply calls `_heap_alloc(<size>)`. We don't have the struct's
    // field count in the IR (irgen did know it but didn't preserve
    // the metadata in IRProgram). For now, infer it by inspecting
    // the alloc symbol's body — emitted in main.c as a 6-instruction
    // body whose 3rd instruction (after stp/mov-x29-sp) is a
    // `mov x0, #<size>`. But the IR pipeline doesn't generate
    // IRFunc entries for the `_alloc_*` symbols — those are emitted
    // directly to the .s file from main.c.
    //
    // Since we can't recover the field count from IR alone, we
    // accept any `_alloc_*` call and treat the struct as having an
    // unbounded field count: the field_slot table grows lazily as
    // we encounter offsets. Each new offset gets a fresh local slot.
    return INT32_MAX; // sentinel meaning "unknown but trust offsets we observe"
}

// Find the unique stack slot a vreg gets stored into anywhere in the
// function. VRegs are function-scoped (unique across blocks) so this
// scans every block. Returns the slot index or -1 if there's not
// exactly one writer (or multiple distinct slot destinations).
static int find_unique_storer_slot_func(IRFunc *func, VReg v) {
    int slot = -1;
    for (int bi = 0; bi < func->block_count; bi++) {
        IRBlock *b = &func->blocks[bi];
        for (int i = 0; i < b->count; i++) {
            IRInst *ins = &b->insts[i];
            if (ins->op == IR_STORE_LOCAL && ins->a == v) {
                if (slot != -1 && slot != (int)ins->imm) return -1;
                slot = (int)ins->imm;
            }
        }
    }
    return slot;
}

// Returns 1 iff the slot is written exactly once anywhere in `func`
// (by the IR_STORE_LOCAL at block `expected_b`, instruction
// `expected_i`). Across all blocks; needed because the alloc + store
// can sit in one block while later writes (which would disqualify
// SRA) appear elsewhere.
static int slot_has_single_writer_func(IRFunc *func, int slot,
                                       int expected_b, int expected_i) {
    int writers = 0;
    for (int bi = 0; bi < func->block_count; bi++) {
        IRBlock *b = &func->blocks[bi];
        for (int i = 0; i < b->count; i++) {
            IRInst *ins = &b->insts[i];
            if (ins->op == IR_STORE_LOCAL && (int)ins->imm == slot) {
                writers++;
                if (bi != expected_b || i != expected_i) return 0;
            }
        }
    }
    return writers == 1;
}

// Returns 1 iff every use of `v` anywhere in `func` is one of:
//   - an IR_LOAD with a >= 0 imm (constant offset field read), where
//     v appears as `a` (the base address); OR
//   - an IR_STORE with a >= 0 imm where v is the `a`; OR
//   - a single IR_CALL `_heap_free` whose first arg is v.
// `*offsets_out` is appended with each constant offset seen
// (deduplicated linearly) so the rewrite phase can size the shadow-
// slot table.
//
// Returns 0 (escape) if v is used in any other position: stored to
// memory, returned, passed to a non-free call, etc.
//
// Cross-block: vregs are function-scoped, so a use can appear in any
// block (the loaded pointer might be field-accessed inside a loop
// body that's a different block from where the LOAD_LOCAL sits).
static int uses_are_field_only_func(IRFunc *func, VReg v,
                                    int *offsets_out, int *n_offsets) {
    for (int bi = 0; bi < func->block_count; bi++) {
        IRBlock *b = &func->blocks[bi];
        for (int i = 0; i < b->count; i++) {
            IRInst *ins = &b->insts[i];
            // Field read: IR_LOAD dst, base, imm
            if (ins->op == IR_LOAD && ins->a == v) {
                int off = (int)ins->imm;
                if (off < 0) return 0;
                int seen = 0;
                for (int k = 0; k < *n_offsets; k++) if (offsets_out[k] == off) { seen = 1; break; }
                if (!seen) offsets_out[(*n_offsets)++] = off;
                continue;
            }
            // Field write: IR_STORE base, val, imm
            if (ins->op == IR_STORE && ins->a == v) {
                int off = (int)ins->imm;
                if (off < 0) return 0;
                int seen = 0;
                for (int k = 0; k < *n_offsets; k++) if (offsets_out[k] == off) { seen = 1; break; }
                if (!seen) offsets_out[(*n_offsets)++] = off;
                continue;
            }
            // Free call: _heap_free(v, size)
            if (ins->op == IR_CALL && ins->str && !strcmp(ins->str, "heap_free")
                && ins->arg_count >= 1 && ins->args && ins->args[0] == v) {
                continue;
            }
            // Any other use of v escapes:
            if (ins->a == v || ins->b == v) return 0;
            if (ins->op == IR_CALL && ins->args) {
                for (int k = 0; k < ins->arg_count; k++) {
                    if (ins->args[k] == v) return 0;
                }
            }
            if (ins->op == IR_RET && ins->a == v) return 0;
            if (ins->op == IR_STORE && ins->b == v) {
                // v used as the value being stored — that's an escape
                // (a copy of v is now in some heap location).
                return 0;
            }
        }
    }
    return 1;
}

// Replace an instruction with a no-op IR_CONST that preserves dst
// (so any later DCE / liveness analysis treats it harmlessly). For
// instructions with no dst (calls used as statements), we set dst=-1
// and rely on the rest of the pipeline ignoring the entry.
static void neuter_inst(IRInst *ins) {
    ins->op = IR_CONST;
    ins->imm = 0;
    ins->a = -1;
    ins->b = -1;
    ins->str = NULL;
    ins->args = NULL;
    ins->arg_count = 0;
}

// Run SRA on every block of a function: scan for every `_alloc_<X>`
// call across all blocks, determine if the resulting struct doesn't
// escape (function-wide), and rewrite field accesses + delete the
// alloc/free pair if it qualifies.
//
// Cross-block matters: after P5.1 inlining, a struct constructed in
// the entry block can have its field accesses scattered across the
// loop bodies and conditionals where the inlined methods landed. A
// per-block SRA would neuter the alloc but leave the dangling field
// accesses pointed at a slot whose pointer was never written.
static void sra_func(IRFunc *func) {
    for (int abi = 0; abi < func->block_count; abi++) {
        IRBlock *ablock = &func->blocks[abi];
        for (int ai = 0; ai < ablock->count; ai++) {
            IRInst *alloc = &ablock->insts[ai];
            if (alloc->op != IR_CALL) continue;
            if (alloc_call_field_count(NULL, alloc->str) < 0) continue;
            if (alloc->dst < 0) continue;

            // The pointer is alloc->dst. Find the unique slot it gets
            // stored to (anywhere in the function).
            int slot = find_unique_storer_slot_func(func, alloc->dst);
            if (slot < 0) continue;

            // Identify the IR_STORE_LOCAL that writes the pointer to
            // the slot. With a unique storer slot, the writer is also
            // unique — but the writer can sit in any block, so search
            // function-wide.
            int writer_b = -1;
            int writer_i = -1;
            for (int bi = 0; bi < func->block_count && writer_b < 0; bi++) {
                IRBlock *b = &func->blocks[bi];
                for (int k = 0; k < b->count; k++) {
                    if (b->insts[k].op == IR_STORE_LOCAL
                        && b->insts[k].a == alloc->dst
                        && (int)b->insts[k].imm == slot) {
                        writer_b = bi; writer_i = k; break;
                    }
                }
            }
            if (writer_b < 0) continue;
            if (!slot_has_single_writer_func(func, slot, writer_b, writer_i)) continue;

            // The pointer lives in `slot`. Every IR_LOAD_LOCAL of the
            // slot (anywhere in the function) produces a fresh vreg;
            // check each one's uses. If any loaded vreg escapes,
            // abort SRA for this allocation.
            int offsets[64];
            int n_offsets = 0;
            int safe = 1;
            for (int bi = 0; bi < func->block_count && safe; bi++) {
                IRBlock *b = &func->blocks[bi];
                for (int k = 0; k < b->count; k++) {
                    IRInst *ins = &b->insts[k];
                    if (ins->op != IR_LOAD_LOCAL || (int)ins->imm != slot) continue;
                    VReg loaded = ins->dst;
                    if (!uses_are_field_only_func(func, loaded, offsets, &n_offsets)) {
                        safe = 0; break;
                    }
                }
            }
            // Also: alloc->dst itself, before being stored. Its only
            // use should be the IR_STORE_LOCAL at (writer_b, writer_i).
            // Walk every block and verify no other instruction reads
            // alloc->dst.
            if (safe) {
                for (int bi = 0; bi < func->block_count && safe; bi++) {
                    IRBlock *b = &func->blocks[bi];
                    for (int k = 0; k < b->count; k++) {
                        // Skip the alloc itself and the unique writer.
                        if (bi == abi && k == ai) continue;
                        if (bi == writer_b && k == writer_i) continue;
                        IRInst *ins = &b->insts[k];
                        if (ins->a == alloc->dst || ins->b == alloc->dst) {
                            safe = 0; break;
                        }
                        if (ins->op == IR_CALL && ins->args) {
                            for (int j = 0; j < ins->arg_count; j++) {
                                if (ins->args[j] == alloc->dst) {
                                    safe = 0; break;
                                }
                            }
                        }
                    }
                }
            }
            if (!safe) continue;

            // All checks pass — promote.
            // Allocate one fresh local slot per observed offset.
            int field_slot_for_off[512];
            for (int k = 0; k < 512; k++) field_slot_for_off[k] = -1;
            for (int k = 0; k < n_offsets; k++) {
                int off = offsets[k];
                if (off < 0 || off >= 512) { safe = 0; break; }
                field_slot_for_off[off] = func->local_slots++;
            }
            if (!safe) continue;

            // Pass 2: rewrite.
            // (a) Neuter the alloc call. Its result vreg is now dead.
            neuter_inst(alloc);
            // (b) Drop the IR_STORE_LOCAL that wrote the pointer to slot.
            neuter_inst(&func->blocks[writer_b].insts[writer_i]);
            // (c) For every IR_LOAD_LOCAL of the slot, rewrite every
            //     use of its destination vreg (anywhere in the
            //     function) as a field access on the corresponding
            //     shadow slot. Then neuter the IR_LOAD_LOCAL.
            for (int bi = 0; bi < func->block_count; bi++) {
                IRBlock *b = &func->blocks[bi];
                for (int k = 0; k < b->count; k++) {
                    IRInst *ins = &b->insts[k];
                    if (ins->op != IR_LOAD_LOCAL || (int)ins->imm != slot) continue;
                    VReg loaded = ins->dst;
                    // Rewrite every use of `loaded` across all blocks.
                    for (int ubi = 0; ubi < func->block_count; ubi++) {
                        IRBlock *ub = &func->blocks[ubi];
                        for (int m = 0; m < ub->count; m++) {
                            IRInst *use = &ub->insts[m];
                            if (use->op == IR_LOAD && use->a == loaded) {
                                int off = (int)use->imm;
                                int fs = field_slot_for_off[off];
                                use->op = IR_LOAD_LOCAL;
                                use->a = -1;
                                use->b = -1;
                                use->imm = fs;
                            } else if (use->op == IR_STORE && use->a == loaded) {
                                int off = (int)use->imm;
                                int fs = field_slot_for_off[off];
                                // IR_STORE_LOCAL takes the value in `a`,
                                // slot in imm.
                                use->op = IR_STORE_LOCAL;
                                use->a = use->b;
                                use->b = -1;
                                use->imm = fs;
                            } else if (use->op == IR_CALL && use->str
                                       && !strcmp(use->str, "heap_free")
                                       && use->arg_count >= 1 && use->args
                                       && use->args[0] == loaded) {
                                // The matched RAII free — non-escaping
                                // so no free needed (shadow slots are
                                // on the stack frame).
                                neuter_inst(use);
                            }
                        }
                    }
                    // The IR_LOAD_LOCAL is now dead.
                    neuter_inst(ins);
                }
            }
        }
    }
}

static void iropt_sra(IRProgram *ir) {
    if (!ir || ir->func_count == 0) return;
    for (int fi = 0; fi < ir->func_count; fi++) {
        sra_func(&ir->funcs[fi]);
    }
}

// ============================================================
// P5.3 — escape analysis + stack allocation ("stackify")
// ============================================================
//
// What this pass does: replace `_alloc_<X>` heap allocations whose
// resulting pointer doesn't escape the function with a stack-frame
// allocation. The pointer continues to flow through the IR — every
// `IR_LOAD %d, %p, off` / `IR_STORE %p, %v, off` keeps working
// against the new pointer because the new pointer is the address of
// a region of `field_count` consecutive local slots reserved on the
// stack frame. The matched RAII `_heap_free(p, size)` is neutered
// because the storage now lives on the function's stack frame and
// is reclaimed automatically at frame exit.
//
// Where this complements P5.2 SRA: SRA promotes the *fields* into
// distinct shadow slots and removes the pointer entirely. SRA only
// applies when every use of the pointer is a constant-offset field
// load/store (or the matched _heap_free). Stackify covers the
// allocations that survive SRA — pointers that flow through:
//
//   - `IR_COPY` chains (the irgen pattern for `b is now a`),
//   - calls that the inliner couldn't eat (e.g., the callee was too
//     big or contained nested calls), as long as those calls
//     themselves don't cause the pointer to escape further (we
//     conservatively reject any non-_heap_free call argument here),
//   - phi-style merges where two arms of a branch each store the
//     same pointer (today's IR doesn't lower phis explicitly so
//     this is moot, but the analysis tolerates them).
//
// What this does NOT do (yet):
//   - Inter-procedural escape: if a non-inlined callee receives the
//     pointer as an argument, we conservatively bail. A future rev
//     could analyse the callee's body and propagate the escape
//     verdict.
//   - Phis. The irgen doesn't emit IR_PHI; this pass treats it as
//     an escape if it ever does.
//   - List / map / imap allocations. Their headers carry a runtime
//     data pointer that the allocator owns — moving the header to
//     the stack would require also stack-allocating the data array,
//     which fights the resize logic in `_list_push` / `_map_set`.
//     P6 will rewrite those builtins in pure Potato; until then,
//     stackify ignores them by only matching `_alloc_<StructName>`.
//
// Implementation:
//
//   Pass 1 — alias closure. Build the set of vregs that are
//   transitive aliases of the alloc's dst (via IR_COPY).
//
//   Pass 2 — escape check. For every instruction in the function,
//   if it uses an alias-set vreg in any "escaping" position
//   (returned, stored to memory, passed to a non-_heap_free call,
//   used as the value half of an IR_STORE), abort.
//
//   Pass 3 — rewrite. Determine field count: in the absence of
//   struct metadata in IR, we use the maximum observed
//   constant-offset field index plus one (just like SRA does).
//   Reserve `field_count` consecutive local slots. Replace the
//   IR_CALL alloc with IR_ADDR_LOCAL %dst, slot=base. Rewrite each
//   IR_COPY in the alias chain to point at the new pointer (already
//   the case via the alias relation — we replace the alloc dst, the
//   COPYs propagate it). Neuter the matched IR_CALL _heap_free.

// Track an alias set: vregs that hold the same pointer value as the
// original allocation's dst. Implemented as a bitset over
// func->vreg_count.
static int *alias_set_new(int n) {
    int *bs = malloc(((n + 63) / 64) * sizeof(uint64_t));
    memset(bs, 0, ((n + 63) / 64) * sizeof(uint64_t));
    return bs;
}
static void alias_set_add(int *bs, int v) {
    uint64_t *q = (uint64_t *)bs;
    q[v >> 6] |= (1ULL << (v & 63));
}
static int alias_set_has(int *bs, int v) {
    if (v < 0) return 0;
    uint64_t *q = (uint64_t *)bs;
    return (q[v >> 6] >> (v & 63)) & 1ULL;
}

// Saturate the alias set: keep adding IR_COPY destinations whose
// source is already in the set, until no more changes.
static void alias_set_close(IRFunc *func, int *bs) {
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int bi = 0; bi < func->block_count; bi++) {
            IRBlock *b = &func->blocks[bi];
            for (int i = 0; i < b->count; i++) {
                IRInst *ins = &b->insts[i];
                if (ins->op == IR_COPY && alias_set_has(bs, ins->a)
                    && !alias_set_has(bs, ins->dst)) {
                    alias_set_add(bs, ins->dst);
                    changed = 1;
                }
                // STORE_LOCAL/LOAD_LOCAL through a slot that already
                // received an alias also propagates: alloc.dst is
                // stored to slot S, and every later LOAD_LOCAL of S
                // is a fresh alias. We tolerate that here (the SRA
                // pass already shapes most code into LOAD_LOCAL +
                // field-only uses; stackify mops up the leftovers).
                if (ins->op == IR_LOAD_LOCAL) {
                    // Was the slot ever STORE_LOCAL'd by an alias?
                    int slot = (int)ins->imm;
                    int slot_aliased = 0;
                    for (int wbi = 0; wbi < func->block_count && !slot_aliased; wbi++) {
                        IRBlock *wb = &func->blocks[wbi];
                        for (int wi = 0; wi < wb->count; wi++) {
                            IRInst *wins = &wb->insts[wi];
                            if (wins->op == IR_STORE_LOCAL
                                && (int)wins->imm == slot
                                && alias_set_has(bs, wins->a)) {
                                slot_aliased = 1; break;
                            }
                        }
                    }
                    if (slot_aliased && !alias_set_has(bs, ins->dst)) {
                        alias_set_add(bs, ins->dst);
                        changed = 1;
                    }
                }
            }
        }
    }
}

// Returns 1 iff none of the alias-set vregs are used in an escaping
// position anywhere in the function. Also tracks the maximum
// constant-offset field index seen on any field-style use, for
// sizing the reserved slot region. `*max_off_out` is updated to the
// largest byte offset observed; the caller computes (max/8 + 1) as
// the field count.
static int aliases_dont_escape(IRFunc *func, int *aliases, int *max_off_out) {
    *max_off_out = 0;
    for (int bi = 0; bi < func->block_count; bi++) {
        IRBlock *b = &func->blocks[bi];
        for (int i = 0; i < b->count; i++) {
            IRInst *ins = &b->insts[i];
            // Field load/store: alias as base => non-escape, record offset.
            if ((ins->op == IR_LOAD || ins->op == IR_STORE)
                && alias_set_has(aliases, ins->a)) {
                int off = (int)ins->imm;
                if (off < 0) return 0;
                if (off > *max_off_out) *max_off_out = off;
                // For IR_STORE the value (b) is also checked below.
            }
            // IR_STORE with alias as the *value*: escapes (a copy of
            // the pointer now lives in some other heap location).
            if (ins->op == IR_STORE && alias_set_has(aliases, ins->b)) return 0;
            // IR_RET of an alias: escapes.
            if (ins->op == IR_RET && alias_set_has(aliases, ins->a)) return 0;
            // IR_CALL: arg-position uses are escapes unless this is
            // the matched _heap_free (which we'll be deleting).
            if (ins->op == IR_CALL) {
                int is_free = (ins->str && !strcmp(ins->str, "heap_free"));
                if (ins->args) {
                    for (int k = 0; k < ins->arg_count; k++) {
                        if (!alias_set_has(aliases, ins->args[k])) continue;
                        if (is_free && k == 0) continue; // the freed pointer
                        return 0;
                    }
                }
                // The call's own dst can't be an alias of itself
                // (would mean it returned the same pointer it received,
                // which a free doesn't and any other call we treat as
                // an escape edge).
            }
            // IR_STORE_LOCAL of an alias: tolerated (we already
            // closed the alias set across STORE_LOCAL/LOAD_LOCAL).
            if (ins->op == IR_STORE_LOCAL && alias_set_has(aliases, ins->a)) {
                continue;
            }
            // Arithmetic / comparison / logical / phi: any use of an
            // alias is an escape (we don't know what the result
            // means about the original pointer).
            switch (ins->op) {
                case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV:
                case IR_MOD: case IR_NEG:
                case IR_CMP_EQ: case IR_CMP_NE: case IR_CMP_LT:
                case IR_CMP_GT: case IR_CMP_LE: case IR_CMP_GE:
                case IR_AND: case IR_OR: case IR_NOT:
                case IR_PHI:
                    if (alias_set_has(aliases, ins->a) || alias_set_has(aliases, ins->b))
                        return 0;
                    break;
                default:
                    break;
            }
        }
    }
    return 1;
}

static void stackify_func(IRFunc *func) {
    for (int bi = 0; bi < func->block_count; bi++) {
        IRBlock *b = &func->blocks[bi];
        for (int i = 0; i < b->count; i++) {
            IRInst *alloc = &b->insts[i];
            if (alloc->op != IR_CALL) continue;
            if (!alloc->str) continue;
            if (strncmp(alloc->str, "alloc_", 6) != 0) continue;
            if (alloc->dst < 0) continue;

            // Build the alias set rooted at alloc->dst.
            int *aliases = alias_set_new(func->vreg_count);
            alias_set_add(aliases, alloc->dst);
            alias_set_close(func, aliases);

            int max_off = 0;
            if (!aliases_dont_escape(func, aliases, &max_off)) {
                free(aliases);
                continue;
            }

            // Reserve consecutive slots. Slot offsets are byte units
            // of 8; field count = max_off/8 + 1, but we round up to
            // at least 1 slot (an empty struct still wants a unique
            // address). For an alloc that flows through but is never
            // field-accessed (max_off stays 0 with no IR_LOAD/IR_STORE
            // hits), we still reserve 1 slot so the address is
            // distinct.
            int field_count = (max_off / 8) + 1;
            if (field_count < 1) field_count = 1;
            int base_slot = func->local_slots;
            func->local_slots += field_count;

            // Rewrite the alloc call to materialise the address of
            // the reserved slot region.
            //
            // Zero-init the slots: heap allocations from `_heap_alloc`'s
            // bump path see zeroed mmap pages and the irgen/Counter()
            // pattern relies on every default-initialised field being
            // zero. The stack frame is NOT zeroed by our prologue —
            // `stp x29, x30, [sp, #-N]!` only writes the fp/lr slot —
            // so we must explicitly clear each reserved slot here.
            // Implementation: insert one IR_CONST + IR_STORE_LOCAL pair
            // per slot directly before the rewritten alloc instruction.
            // We patch alloc in place after splicing so its index in
            // the block has shifted past the inserted instructions.
            int zero_pair_count = field_count * 2;
            int new_size = b->count + zero_pair_count;
            if (new_size > b->cap) {
                while (b->cap < new_size) b->cap = b->cap ? b->cap * 2 : 16;
                b->insts = realloc(b->insts, b->cap * sizeof(IRInst));
            }
            // Re-fetch alloc pointer (realloc may have moved the array).
            alloc = &b->insts[i];
            // Make room: shift [i .. count) right by zero_pair_count.
            int tail = b->count - i;
            memmove(&b->insts[i + zero_pair_count], &b->insts[i],
                    tail * sizeof(IRInst));
            b->count = new_size;
            // Fill in the zero-init prelude.
            for (int s = 0; s < field_count; s++) {
                VReg zv = func->vreg_count++;
                IRInst c0 = {0};
                c0.op = IR_CONST;
                c0.dst = zv;
                c0.a = -1; c0.b = -1;
                c0.imm = 0;
                b->insts[i + s * 2] = c0;
                IRInst sl = {0};
                sl.op = IR_STORE_LOCAL;
                sl.dst = -1;
                sl.a = zv;
                sl.b = -1;
                sl.imm = base_slot + s;
                b->insts[i + s * 2 + 1] = sl;
            }
            // Now patch the original alloc (which has moved).
            alloc = &b->insts[i + zero_pair_count];
            alloc->op = IR_ADDR_LOCAL;
            alloc->str = NULL;
            alloc->args = NULL;
            alloc->arg_count = 0;
            alloc->a = -1;
            alloc->b = -1;
            alloc->imm = base_slot;
            // alloc->dst stays the same so every existing alias
            // chain (IR_COPY, STORE_LOCAL+LOAD_LOCAL pairs) keeps
            // pointing at this materialised address.
            // Advance the outer loop's index past the prelude.
            i += zero_pair_count;

            // Neuter the matched RAII free. It's the IR_CALL of
            // "heap_free" whose first arg is in the alias set.
            for (int fbi = 0; fbi < func->block_count; fbi++) {
                IRBlock *fb = &func->blocks[fbi];
                for (int fi2 = 0; fi2 < fb->count; fi2++) {
                    IRInst *ins = &fb->insts[fi2];
                    if (ins->op == IR_CALL && ins->str
                        && !strcmp(ins->str, "heap_free")
                        && ins->arg_count >= 1 && ins->args
                        && alias_set_has(aliases, ins->args[0])) {
                        neuter_inst(ins);
                    }
                }
            }

            free(aliases);
        }
    }
}

static void iropt_stackify(IRProgram *ir) {
    if (!ir || ir->func_count == 0) return;
    for (int fi = 0; fi < ir->func_count; fi++) {
        stackify_func(&ir->funcs[fi]);
    }
}

// ============================================================
// P5.4 — bounds-check elimination (BCE)
// ============================================================
//
// What this pass does: drop the `IR_BR_COND` + matched panic
// CALL/BR sequence on a pair-shaped negative / out-of-range check
// when range analysis proves the index is safe.
//
// The irgen lowers `xs[i]` into:
//
//   %count   = LOAD %obj, 8                       ; cap=0, count=8, data=16
//   %zero    = CONST 0
//   %is_neg  = CMP_LT %idx, %zero
//   BR_COND %is_neg, neg_panic, neg_ok
//   neg_panic: CALL panic_oob; BR neg_ok
//   neg_ok:    %is_oob = CMP_GE %idx, %count
//              BR_COND %is_oob, oob_panic, oob_ok
//   oob_panic: CALL panic_oob; BR oob_ok
//   oob_ok:    ... actual indexed load ...
//
// We run two independent rewrites. Each, if it succeeds, replaces
// the CMP / BR_COND with an unconditional BR to the "ok" branch
// and neuters the panic call (still safe — the panic block ends
// in a BR to the same ok label, so the emitted CFG remains
// well-formed and the panic block becomes unreachable; downstream
// regalloc / iremit still emit it but nothing branches there).
//
// 1. Lower-bound BCE — `idx < 0` is provably false.
//    Walk the slot's writers (every IR_STORE_LOCAL targeting the
//    slot from which `idx` was loaded). If every value stored is
//    "non-negative-by-construction" (a non-negative IR_CONST, or
//    an IR_ADD where both operands are non-negative-by-construction
//    via this same recursive criterion, or an IR_LOAD_LOCAL of the
//    same slot — i.e. the loop-step pattern), the index can never
//    be negative.
//
// 2. Upper-bound BCE — `idx >= count` is provably false.
//    Skipped in v1: requires alias analysis on `obj` and a proof
//    that the loop's exit condition was tested against the same
//    `count`. Will land as a follow-up commit if the std/map
//    bench shows it dominates the remaining overhead.

// Returns 1 if vreg `v` is provably >= 0.
//   - IR_CONST with imm >= 0 → yes.
//   - IR_ADD %a, %b where both operands are recursively non-negative → yes.
//   - IR_LOAD_LOCAL of a slot whose every writer is non-negative → yes.
//   - Anything else → no (conservative).
//
// `seen_slots` is a set of slot indices we've already visited, to
// break cycles (the loop pattern: slot read on the right of an
// IR_ADD that gets stored back to the same slot).
static int is_known_nonneg(IRFunc *func, VReg v,
                           int *seen_slots, int n_seen);

static int slot_writers_all_nonneg(IRFunc *func, int slot,
                                   int *seen_slots, int n_seen) {
    // Cycle guard.
    for (int k = 0; k < n_seen; k++) if (seen_slots[k] == slot) return 1;
    if (n_seen >= 64) return 0;
    int new_seen[64];
    memcpy(new_seen, seen_slots, n_seen * sizeof(int));
    new_seen[n_seen] = slot;
    int n_new = n_seen + 1;
    int found_writer = 0;
    for (int bi = 0; bi < func->block_count; bi++) {
        IRBlock *b = &func->blocks[bi];
        for (int i = 0; i < b->count; i++) {
            IRInst *ins = &b->insts[i];
            if (ins->op == IR_STORE_LOCAL && (int)ins->imm == slot) {
                found_writer = 1;
                if (!is_known_nonneg(func, ins->a, new_seen, n_new))
                    return 0;
            }
        }
    }
    // A slot with no writers (defensive: shouldn't happen for an
    // index used in an IR_LOAD_LOCAL) — treat as unknown.
    return found_writer;
}

static int is_known_nonneg(IRFunc *func, VReg v,
                           int *seen_slots, int n_seen) {
    if (v < 0) return 0;
    // Find the unique definition of `v` (vregs are SSA-shaped: at
    // most one def in the function).
    for (int bi = 0; bi < func->block_count; bi++) {
        IRBlock *b = &func->blocks[bi];
        for (int i = 0; i < b->count; i++) {
            IRInst *ins = &b->insts[i];
            if (ins->dst != v) continue;
            switch (ins->op) {
                case IR_CONST:
                    return ins->imm >= 0;
                case IR_ADD:
                    return is_known_nonneg(func, ins->a, seen_slots, n_seen)
                        && is_known_nonneg(func, ins->b, seen_slots, n_seen);
                case IR_MUL:
                    // x*y >= 0 if both are >= 0 (x,y signed: both
                    // non-negative => product non-negative, modulo
                    // overflow which we treat as a non-issue here).
                    return is_known_nonneg(func, ins->a, seen_slots, n_seen)
                        && is_known_nonneg(func, ins->b, seen_slots, n_seen);
                case IR_LOAD_LOCAL:
                    return slot_writers_all_nonneg(func, (int)ins->imm,
                                                   seen_slots, n_seen);
                case IR_COPY:
                    return is_known_nonneg(func, ins->a, seen_slots, n_seen);
                default:
                    return 0;
            }
        }
    }
    return 0;
}

// Locate the panic block's bounds-check pair. Given a block that
// begins with the negative-check sequence:
//
//   %is_neg = CMP_LT %idx, %zero
//   BR_COND %is_neg, neg_panic, neg_ok
//
// returns positions of the CMP, BR_COND, and the index vreg. We
// pattern-match by looking for adjacent CMP_LT/CMP_GE → BR_COND →
// CALL "panic_oob" sequences anywhere in the function and consider
// each pair.

// Find the panic-call block from a BR_COND's `label`. Returns the
// IRBlock pointer, or NULL if not a panic block (no CALL panic_oob
// at the start).
static IRBlock *find_block_by_label(IRFunc *func, int label) {
    for (int bi = 0; bi < func->block_count; bi++) {
        if (func->blocks[bi].label == label) return &func->blocks[bi];
    }
    return NULL;
}

static int block_starts_with_panic_oob(IRBlock *b) {
    if (!b || b->count == 0) return 0;
    IRInst *first = &b->insts[0];
    return first->op == IR_CALL && first->str
           && !strcmp(first->str, "panic_oob");
}

// Eliminate one bounds check at index `br_idx` in block `b`. The
// instruction at br_idx is an IR_BR_COND whose label leads to a
// panic block. We rewrite the BR_COND to an unconditional BR to
// label2 (the "ok" path), turning the check into a no-op that
// regalloc/iremit can leave alone — the panic block is now
// unreachable.
static void eliminate_check(IRBlock *b, int cmp_idx, int br_idx) {
    // The CMP itself is dead now (its dst is only used by the
    // BR_COND we're rewriting). Neuter it.
    neuter_inst(&b->insts[cmp_idx]);
    // Convert BR_COND to BR by zeroing the cond-op fields.
    IRInst *br = &b->insts[br_idx];
    br->op = IR_BR;
    br->a = -1;
    br->b = -1;
    // br->label stays as the original "false" target — wait, the
    // shape is: BR_COND %cond, label_true, label2. label2 is the
    // "false" branch (cond==0). For our bounds checks, label2 is
    // the ok label (the no-panic path). After proving the cond is
    // always false, we want to branch to label2.
    br->label = br->label2;
    br->label2 = 0;
}

static void bce_func(IRFunc *func) {
    for (int bi = 0; bi < func->block_count; bi++) {
        IRBlock *b = &func->blocks[bi];
        for (int i = 0; i + 1 < b->count; i++) {
            IRInst *cmp = &b->insts[i];
            IRInst *br  = &b->insts[i + 1];
            if (br->op != IR_BR_COND) continue;
            if (cmp->dst != br->a) continue;
            // The "true" target must be a block whose first
            // instruction is CALL panic_oob.
            IRBlock *panic_b = find_block_by_label(func, br->label);
            if (!block_starts_with_panic_oob(panic_b)) continue;
            // Lower-bound check: CMP_LT %idx, %zero (where %zero is
            // a CONST 0 — verify by tracing the b operand).
            if (cmp->op == IR_CMP_LT) {
                // Confirm cmp->b is a CONST 0.
                int b_is_zero = 0;
                for (int sbi = 0; sbi < func->block_count; sbi++) {
                    IRBlock *sb = &func->blocks[sbi];
                    for (int si = 0; si < sb->count; si++) {
                        if (sb->insts[si].dst == cmp->b
                            && sb->insts[si].op == IR_CONST) {
                            b_is_zero = (sb->insts[si].imm == 0);
                            goto found_b;
                        }
                    }
                }
            found_b:;
                if (!b_is_zero) continue;
                int seen[64];
                if (!is_known_nonneg(func, cmp->a, seen, 0)) continue;
                eliminate_check(b, i, i + 1);
                continue;
            }
            // Upper-bound check is left for a follow-up — would
            // require proving cmp->a (idx) < cmp->b (count) at
            // this program point, typically by relating count to
            // a loop's exit condition.
        }
    }
}

static void iropt_bce(IRProgram *ir) {
    if (!ir || ir->func_count == 0) return;
    for (int fi = 0; fi < ir->func_count; fi++) {
        bce_func(&ir->funcs[fi]);
    }
}
