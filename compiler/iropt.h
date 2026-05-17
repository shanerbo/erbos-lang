#ifndef IROPT_H
#define IROPT_H

#include "ir.h"

// Optimization levels (P5.0 scaffold).
//
// IROPT_O0 — skip iropt entirely. Useful for debugging the IR pipeline:
//            the .s output reflects exactly what the irgen + regalloc +
//            iremit chain produces, with no transformations layered on.
//
// IROPT_O1 — default. Each P5.x pass enables itself at this level
//            (or higher) once it lands; the actual pass set grows
//            commit-by-commit through P5.1-P5.5.
//
// IROPT_O2 — reserved for tuning later. Currently identical to O1.
typedef enum {
    IROPT_O0 = 0,
    IROPT_O1 = 1,
    IROPT_O2 = 2,
} IROptLevel;

// Run every iropt pass enabled at `level` against `ir` in place.
// Pass order matters and is documented at the dispatch table inside
// iropt.c. A no-op call (IROPT_O0, or any level with no enabled
// passes) is fine and just returns.
void iropt_run(IRProgram *ir, IROptLevel level);

#endif
