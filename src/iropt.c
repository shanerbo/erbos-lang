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
#include "iropt.h"

typedef void (*IROptPass)(IRProgram *ir);

typedef struct {
    const char *name;       // diagnostic-friendly pass name
    IROptLevel  min_level;  // smallest -O level that enables the pass
    IROptPass   run;        // the actual transformation
} IROptPassEntry;

// Empty for now; #22-#26 each prepend / append one entry as their
// pass lands. New passes should be inserted in the canonical order
// documented at the top of this file.
static const IROptPassEntry g_passes[] = {
    // { "inlining",        IROPT_O1, iropt_inline },          // P5.1
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
