#ifndef IREMIT_H
#define IREMIT_H

#include <stdio.h>
#include "ir.h"
#include "regalloc.h"

// Emit ARM64 assembly from IR + register allocation results
void iremit_func(FILE *out, IRFunc *func, RegAllocResult *alloc);

#endif
