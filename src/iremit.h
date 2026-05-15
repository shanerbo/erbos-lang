#ifndef IREMIT_H
#define IREMIT_H

#include <stdio.h>
#include "ir.h"
#include "regalloc.h"

// Emit ARM64 assembly from IR + register allocation results.
// String literals encountered during emission are accumulated into a
// process-global string pool; iremit_finalize_data writes the data
// section that holds them. Call iremit_finalize_data exactly once,
// after every iremit_func call, before closing the output file.
void iremit_func(FILE *out, IRFunc *func, RegAllocResult *alloc);
void iremit_finalize_data(FILE *out);

#endif
