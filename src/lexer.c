#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include "lexer.h"

static void emit(Lexer *l, TokenType type, char *value) {
    if (l->count >= l->cap) {
        l->cap *= 2;
        l->tokens = realloc(l->tokens, l->cap * sizeof(Token));
    }
    l->tokens[l->count++] = (Token){type, value, l->line, l->col};
}

static char peek(Lexer *l) { return l->src[l->pos]; }
static char advance(Lexer *l) {
    char c = l->src[l->pos++];
    if (c == '\n') { l->line++; l->col = 1; } else { l->col++; }
    return c;
}

static char *slice(Lexer *l, int start, int end) {
    int len = end - start;
    char *s = malloc(len + 1);
    memcpy(s, l->src + start, len);
    s[len] = '\0';
    return s;
}

static TokenType keyword_type(const char *word) {
    if (!strcmp(word, "is"))      return TOK_IS;
    if (!strcmp(word, "nomut"))   return TOK_NOMUT;
    if (!strcmp(word, "give"))    return TOK_GIVE;
    if (!strcmp(word, "through")) return TOK_THROUGH;
    if (!strcmp(word, "from"))    return TOK_FROM;
    if (!strcmp(word, "to"))      return TOK_TO;
    if (!strcmp(word, "by"))      return TOK_BY;
    if (!strcmp(word, "in"))      return TOK_IN;
    if (!strcmp(word, "stop"))    return TOK_STOP;
    if (!strcmp(word, "skip"))    return TOK_SKIP;
    if (!strcmp(word, "infi"))    return TOK_INFI;
    if (!strcmp(word, "nah"))     return TOK_NAH;
    if (!strcmp(word, "now"))     return TOK_NOW;
    if (!strcmp(word, "rep"))     return TOK_REP;
    if (!strcmp(word, "ref"))     return TOK_REF;
    if (!strcmp(word, "be"))      return TOK_BE;
    if (!strcmp(word, "match"))   return TOK_MATCH;
    if (!strcmp(word, "test"))    return TOK_TEST;
    if (!strcmp(word, "assert"))  return TOK_ASSERT;
    if (!strcmp(word, "use"))     return TOK_USE;
    if (!strcmp(word, "as"))      return TOK_AS;
    if (!strcmp(word, "and"))     return TOK_AND;
    if (!strcmp(word, "or"))      return TOK_OR;
    if (!strcmp(word, "not"))     return TOK_NOT;
    if (!strcmp(word, "eq"))      return TOK_EQ_WORD;
    if (!strcmp(word, "ne"))      return TOK_NE_WORD;
    if (!strcmp(word, "gt"))      return TOK_GT_WORD;
    if (!strcmp(word, "lt"))      return TOK_LT_WORD;
    if (!strcmp(word, "ge"))      return TOK_GE_WORD;
    if (!strcmp(word, "le"))      return TOK_LE_WORD;
    if (!strcmp(word, "mod"))     return TOK_MOD_WORD;
    if (!strcmp(word, "true"))    return TOK_TRUE;
    if (!strcmp(word, "false"))   return TOK_FALSE;
    if (!strcmp(word, "nil"))     return TOK_NIL;
    if (!strcmp(word, "list"))    return TOK_LIST;
    if (!strcmp(word, "map"))     return TOK_MAP;
    if (!strcmp(word, "imap"))    return TOK_IMAP;
    if (!strcmp(word, "of"))      return TOK_OF;
    if (!strcmp(word, "task"))    return TOK_TASK;
    if (!strcmp(word, "spark"))   return TOK_SPARK;
    if (!strcmp(word, "int"))     return TOK_INT;
    if (!strcmp(word, "str"))     return TOK_STR_TYPE;
    if (!strcmp(word, "bool"))    return TOK_BOOL;
    if (!strcmp(word, "void"))    return TOK_VOID;
    return TOK_IDENT;
}

void lexer_init(Lexer *l, char *source) {
    l->src = source;
    l->pos = 0;
    l->line = 1;
    l->col = 1;
    l->cap = 256;
    l->count = 0;
    l->tokens = malloc(l->cap * sizeof(Token));
}

void lexer_tokenize(Lexer *l) {
    while (peek(l) != '\0') {
        char c = peek(l);

        // Skip spaces/tabs
        if (c == ' ' || c == '\t' || c == '\r') {
            advance(l);
            continue;
        }

        // Newline
        if (c == '\n') {
            advance(l);
            // Collapse multiple newlines
            if (l->count == 0 || l->tokens[l->count-1].type != TOK_NEWLINE)
                emit(l, TOK_NEWLINE, NULL);
            continue;
        }

        // Single-line comment
        if (c == '/' && l->src[l->pos+1] == '/') {
            while (peek(l) != '\n' && peek(l) != '\0') advance(l);
            continue;
        }

        // Multi-line comment
        if (c == '/' && l->src[l->pos+1] == '*') {
            advance(l); advance(l);
            while (!(peek(l) == '*' && l->src[l->pos+1] == '/')) {
                if (peek(l) == '\0') { fprintf(stderr, "error: unterminated comment\n"); exit(1); }
                advance(l);
            }
            advance(l); advance(l);
            continue;
        }

        // Numbers (including negative literals)
        if (isdigit(c) || (c == '-' && isdigit(l->src[l->pos + 1]) &&
            (l->count == 0 || l->tokens[l->count-1].type == TOK_ASSIGN ||
             l->tokens[l->count-1].type == TOK_LPAREN ||
             l->tokens[l->count-1].type == TOK_COMMA ||
             l->tokens[l->count-1].type == TOK_IS ||
             l->tokens[l->count-1].type == TOK_NEWLINE))) {
            int start = l->pos;
            if (c == '-') advance(l);
            while (isdigit(peek(l))) advance(l);
            emit(l, TOK_INT_LIT, slice(l, start, l->pos));
            continue;
        }

        // Identifiers / keywords
        if (isalpha(c) || c == '_') {
            int start = l->pos;
            while (isalnum(peek(l)) || peek(l) == '_') advance(l);
            char *word = slice(l, start, l->pos);
            TokenType t = keyword_type(word);
            emit(l, t, word);
            continue;
        }

        // Strings
        if (c == '"') {
            advance(l); // skip opening "
            int start = l->pos;
            while (peek(l) != '"' && peek(l) != '\0') advance(l);
            if (peek(l) == '\0') { fprintf(stderr, "error: unterminated string\n"); exit(1); }
            char *s = slice(l, start, l->pos);
            advance(l); // skip closing "
            emit(l, TOK_STR_LIT, s);
            continue;
        }

        // Two-char operators
        if (c == '=' && l->src[l->pos+1] == '=') { advance(l); advance(l); emit(l, TOK_EQ, NULL); continue; }
        if (c == '=' && l->src[l->pos+1] == '>') { advance(l); advance(l); emit(l, TOK_ARROW, NULL); continue; }
        if (c == '!' && l->src[l->pos+1] == '=') { advance(l); advance(l); emit(l, TOK_NEQ, NULL); continue; }
        if (c == '<' && l->src[l->pos+1] == '=') { advance(l); advance(l); emit(l, TOK_LTE, NULL); continue; }
        if (c == '>' && l->src[l->pos+1] == '=') { advance(l); advance(l); emit(l, TOK_GTE, NULL); continue; }

        // Single-char tokens
        advance(l);
        switch (c) {
            case '(': emit(l, TOK_LPAREN, NULL); break;
            case ')': emit(l, TOK_RPAREN, NULL); break;
            case '{': emit(l, TOK_LBRACE, NULL); break;
            case '}': emit(l, TOK_RBRACE, NULL); break;
            case '[': emit(l, TOK_LBRACKET, NULL); break;
            case ']': emit(l, TOK_RBRACKET, NULL); break;
            case ',': emit(l, TOK_COMMA, NULL); break;
            case '.': emit(l, TOK_DOT, NULL); break;
            case '|': emit(l, TOK_PIPE, NULL); break;
            case '?': emit(l, TOK_QUESTION, NULL); break;
            case '=': emit(l, TOK_ASSIGN, NULL); break;
            case '+': emit(l, TOK_PLUS, NULL); break;
            case '-': emit(l, TOK_MINUS, NULL); break;
            case '*': emit(l, TOK_STAR, NULL); break;
            case '/': emit(l, TOK_SLASH, NULL); break;
            case '%': emit(l, TOK_PERCENT, NULL); break;
            case '<': emit(l, TOK_LT, NULL); break;
            case '>': emit(l, TOK_GT, NULL); break;
            default:
                fprintf(stderr, "error: unexpected character '%c' at line %d\n", c, l->line);
                exit(1);
        }
    }
    emit(l, TOK_EOF, NULL);
}
