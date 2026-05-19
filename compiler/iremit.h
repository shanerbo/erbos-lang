#ifndef IREMIT_H
#define IREMIT_H

#include <stdio.h>
#include "ir.h"
#include "regalloc.h"
#include "target.h"

// Emit ARM64 assembly from IR + register allocation results.
// String literals encountered during emission are accumulated into a
// process-global string pool; iremit_finalize_data writes the data
// section that holds them. Call iremit_finalize_data exactly once,
// after every iremit_func call, before closing the output file.
//
// `target` carries the per-target syntax for address-load and
// section directives — both Darwin (Mach-O, @PAGE/@PAGEOFF) and
// Linux (ELF, :lo12:) lower to the same AArch64 instructions but
// spell the relocations differently.
void iremit_func(FILE *out, const Target *target,
                 IRFunc *func, RegAllocResult *alloc);
void iremit_finalize_data(FILE *out, const Target *target);

#endif
