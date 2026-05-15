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

// Pass dispatch table. New passes should be inserted in the canonical
// order documented at the top of this file (inlining first because it
// exposes the most opportunities for everything that follows).
static const IROptPassEntry g_passes[] = {
    { "inlining",        IROPT_O1, iropt_inline },          // P5.1
    { "sra",             IROPT_O1, iropt_sra },             // P5.2
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
