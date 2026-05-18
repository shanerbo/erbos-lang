#ifndef PARSER_H
#define PARSER_H

#include "ast.h"
#include "lexer.h"

typedef struct {
    Token *tokens;
    int count;
    int pos;
    const char *filename;
    // Language-law allow-list flag: when set, any
    // `Type.variant(...)` AST nodes the parser produces from this
    // source carry `is_stdlib_enum_factory = 1`, which the checker
    // requires to permit the legacy enum value-formation shape.
    // Set by main.c only when the absolute source path matches the
    // bundled-stdlib std/option.ptt or std/result.ptt. Not set for
    // any user file, even ones with the same basename. Defaults to
    // 0 from parser_init's zero-fill of unused fields; main.c
    // assigns it after parser_init.
    int is_stdlib_enum_factory_file;
} Parser;

void parser_init(Parser *p, Lexer *l);
Node *parser_parse(Parser *p);

#endif
