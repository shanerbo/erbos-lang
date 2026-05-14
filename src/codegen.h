#ifndef CODEGEN_H
#define CODEGEN_H

#include <stdio.h>
#include "ast.h"

void codegen(Node *program, const char *output_path);
void codegen_tests(Node *program, const char *output_path);
void codegen_emit_builtins(FILE *out);

#endif
