#ifndef CHECKER_H
#define CHECKER_H

#include "ast.h"
#include "types.h"

// Type checks the AST. Prints errors and exits on type mismatch.
void checker_run(Node *program);

#endif
