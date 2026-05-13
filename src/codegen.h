#ifndef CODEGEN_H
#define CODEGEN_H

#include "ast.h"

void codegen(Node *program, const char *output_path);
void codegen_tests(Node *program, const char *output_path);

#endif
