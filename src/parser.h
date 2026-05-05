#ifndef PARSER_H
#define PARSER_H

#include "ast.h"
#include "lexer.h"

typedef struct {
    Token *tokens;
    int count;
    int pos;
    const char *filename;
} Parser;

void parser_init(Parser *p, Lexer *l);
Node *parser_parse(Parser *p);

#endif
