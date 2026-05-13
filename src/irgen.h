#ifndef IRGEN_H
#define IRGEN_H

#include "ast.h"
#include "ir.h"

IRProgram *irgen_generate(Node *program);

#endif
