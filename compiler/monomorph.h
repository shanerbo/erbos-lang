#ifndef MONOMORPH_H
#define MONOMORPH_H

#include "ast.h"

// Replace every generic struct/method template + every parametric type
// expression in the program with concrete instantiations. After this
// pass runs, the AST has no NODE_STRUCT_DEF with type_param_count > 0,
// no NODE_FUNC_DEF with receiver_type_args, and no type-name string
// containing '<' or '>'.
//
// Names are mangled by replacing the angle-bracket form with underscores:
//
//     Box<int>            ->  Box__int
//     Map<str, int>       ->  Map__str__int
//     List<Pair<str,int>> ->  List__Pair__str__int
//
// The pass is idempotent: running it on an already-monomorphic program
// is a no-op.
void monomorph_run(Node *program);

#endif
