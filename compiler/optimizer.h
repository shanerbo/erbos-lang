#ifndef OPTIMIZER_H
#define OPTIMIZER_H

#include "ast.h"

// Runs optimization passes on the AST (after type checking, before codegen)
void optimizer_run(Node *program);

#endif
