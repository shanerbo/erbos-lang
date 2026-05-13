#ifndef TOKEN_H
#define TOKEN_H

typedef enum {
    // Literals
    TOK_INT_LIT,
    TOK_STR_LIT,
    TOK_IDENT,

    // Keywords
    TOK_IS,
    TOK_NOMUT,
    TOK_GIVE,
    TOK_THROUGH,
    TOK_FROM,
    TOK_TO,
    TOK_BY,
    TOK_IN,
    TOK_STOP,
    TOK_SKIP,
    TOK_INFI,
    TOK_NAH,
    TOK_NOW,
    TOK_REP,
    TOK_REF,
    TOK_BE,
    TOK_MATCH,
    TOK_TEST,
    TOK_ASSERT,
    TOK_USE,
    TOK_AS,
    TOK_AND,
    TOK_OR,
    TOK_NOT,
    TOK_EQ_WORD,    // eq
    TOK_NE_WORD,    // ne
    TOK_GT_WORD,    // gt
    TOK_LT_WORD,    // lt
    TOK_GE_WORD,    // ge
    TOK_LE_WORD,    // le
    TOK_ADD_WORD,   // unused - reserved
    TOK_SUB_WORD,   // unused - reserved
    TOK_MUL_WORD,   // unused - reserved
    TOK_DIV_WORD,   // unused - reserved
    TOK_MOD_WORD,   // mod
    TOK_TRUE,
    TOK_FALSE,
    TOK_NIL,
    TOK_LIST,
    TOK_MAP,
    TOK_IMAP,
    TOK_OF,
    TOK_TASK,

    // Types
    TOK_INT,
    TOK_STR_TYPE,
    TOK_BOOL,
    TOK_VOID,

    // Symbols
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_LBRACKET,
    TOK_RBRACKET,
    TOK_COMMA,
    TOK_DOT,
    TOK_QUESTION,
    TOK_ASSIGN,     // =
    TOK_ARROW,      // =>
    TOK_PIPE,       // |

    // Operators
    TOK_PLUS,
    TOK_MINUS,
    TOK_STAR,
    TOK_SLASH,
    TOK_PERCENT,
    TOK_EQ,         // ==
    TOK_NEQ,        // !=
    TOK_LT,
    TOK_GT,
    TOK_LTE,
    TOK_GTE,

    // Special
    TOK_NEWLINE,
    TOK_EOF,
} TokenType;

typedef struct {
    TokenType type;
    char *value;
    int line;
    int col;
} Token;

#endif
