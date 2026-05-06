#ifndef AST_H
#define AST_H

#include "token.h"

typedef enum {
    NODE_PROGRAM,
    NODE_FUNC_DEF,
    NODE_STRUCT_DEF,
    NODE_BLOCK,
    NODE_VAR_DECL,
    NODE_ASSIGN,
    NODE_FIELD_ASSIGN,
    NODE_IF,
    NODE_THROUGH_RANGE,
    NODE_THROUGH_IN,
    NODE_INFI,
    NODE_GIVE,
    NODE_CALL,
    NODE_METHOD_CALL,
    NODE_BINARY,
    NODE_UNARY,
    NODE_INT_LIT,
    NODE_STR_LIT,
    NODE_BOOL_LIT,
    NODE_IDENT,
    NODE_FIELD_ACCESS,
    NODE_INDEX,
    NODE_LIST_LIT,
    NODE_MAP_LIT,
    NODE_STOP,
    NODE_SKIP,
} NodeType;

typedef struct Node Node;

typedef struct {
    Node **items;
    int count;
    int cap;
} NodeList;

struct Node {
    NodeType type;
    int line;
    int resolved_type; // filled by checker: 0=unknown, 1=int, 2=str, 3=bool

    union {
        // NODE_PROGRAM
        struct { NodeList funcs; NodeList structs; } program;

        // NODE_FUNC_DEF
        struct {
            char *name;
            char **param_names;
            char **param_types;
            int *param_is_ref;
            int param_count;
            char *return_type;
            Node *body;
        } func_def;

        // NODE_STRUCT_DEF
        struct {
            char *name;
            char **field_names;
            char **field_types;
            int field_count;
        } struct_def;

        // NODE_BLOCK
        struct { NodeList stmts; } block;

        // NODE_VAR_DECL
        struct {
            char *name;
            char *type_name;
            char *elem_type_name;  // for list of X
            char *key_type_name;   // for map of X to Y
            char *val_type_name;   // for map of X to Y
            int is_nomut;
            int is_move;
            int is_rep;
            Node *value;
        } var_decl;

        // NODE_ASSIGN
        struct { char *name; Node *value; } assign;

        // NODE_FIELD_ASSIGN (obj.field = value)
        struct { Node *object; char *field; Node *value; } field_assign;

        // NODE_IF
        struct {
            Node **conds;
            Node **bodies;
            int branch_count;
            Node *nah_body;
        } if_stmt;

        // NODE_THROUGH_RANGE
        struct {
            char *var_name;
            Node *from;
            Node *to;
            Node *by;
            Node *body;
        } through_range;

        // NODE_THROUGH_IN
        struct {
            char *var_name;
            Node *collection;
            Node *body;
        } through_in;

        // NODE_INFI
        struct {
            Node *cond; // NULL = infinite
            Node *body;
        } infi;

        // NODE_GIVE
        struct { Node *value; } give;

        // NODE_CALL
        struct {
            char *name;
            Node **args;
            int arg_count;
        } call;

        // NODE_METHOD_CALL (obj.method(args))
        struct {
            Node *object;
            char *method;
            Node **args;
            int arg_count;
        } method_call;

        // NODE_BINARY
        struct {
            TokenType op;
            Node *left;
            Node *right;
        } binary;

        // NODE_UNARY
        struct {
            TokenType op;
            Node *operand;
        } unary;

        // NODE_INT_LIT
        struct { long value; } int_lit;

        // NODE_STR_LIT
        struct { char *value; } str_lit;

        // NODE_BOOL_LIT
        struct { int value; } bool_lit;

        // NODE_IDENT
        struct { char *name; } ident;

        // NODE_FIELD_ACCESS (obj.field)
        struct { Node *object; char *field; } field_access;

        // NODE_INDEX (arr[idx])
        struct { Node *object; Node *index; } index_access;

        // NODE_LIST_LIT
        struct { Node **items; int count; } list_lit;

        // NODE_MAP_LIT
        struct { Node **keys; Node **values; int count; } map_lit;
    };
};

#endif
