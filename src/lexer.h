#ifndef LEXER_H
#define LEXER_H

#include "token.h"

typedef struct {
    char *src;
    int pos;
    int line;
    int col;
    Token *tokens;
    int count;
    int cap;
} Lexer;

void lexer_init(Lexer *l, char *source);
void lexer_tokenize(Lexer *l);

#endif
