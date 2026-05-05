#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "checker.h"

#define MAX_SYMS 256

typedef struct {
    char *name;
    Type type;
} Symbol;

typedef struct {
    Symbol syms[MAX_SYMS];
    int count;
    // Function info
    char **func_names;
    Type *func_types;
    int *func_param_counts;
    int func_count;
    // Struct names
    char **struct_names;
    int struct_count;
} Checker;

static Type make_type(TypeKind k) { return (Type){k, NULL}; }
static Type make_struct(const char *name) { return (Type){TYPE_STRUCT, (char *)name}; }

static void set_sym(Checker *c, const char *name, Type t) {
    // Update existing
    for (int i = 0; i < c->count; i++) {
        if (!strcmp(c->syms[i].name, name)) { c->syms[i].type = t; return; }
    }
    c->syms[c->count++] = (Symbol){(char *)name, t};
}

static Type get_sym(Checker *c, const char *name) {
    for (int i = c->count - 1; i >= 0; i--)
        if (!strcmp(c->syms[i].name, name)) return c->syms[i].type;
    return make_type(TYPE_UNKNOWN);
}

static Type get_func_return(Checker *c, const char *name) {
    for (int i = 0; i < c->func_count; i++)
        if (!strcmp(c->func_names[i], name)) return c->func_types[i];
    return make_type(TYPE_UNKNOWN);
}

static int is_struct(Checker *c, const char *name) {
    for (int i = 0; i < c->struct_count; i++)
        if (!strcmp(c->struct_names[i], name)) return 1;
    return 0;
}

static const char *type_name(Type t) {
    switch (t.kind) {
        case TYPE_INT: return "int";
        case TYPE_STR: return "str";
        case TYPE_BOOL: return "bool";
        case TYPE_VOID: return "void";
        case TYPE_LIST: return "list";
        case TYPE_MAP: return "map";
        case TYPE_STRUCT: return t.struct_name ? t.struct_name : "struct";
        case TYPE_TASK: return "task";
        case TYPE_UNKNOWN: return "unknown";
    }
    return "?";
}

static Type check_expr(Checker *c, Node *n);

static Type check_expr(Checker *c, Node *n) {
    switch (n->type) {
        case NODE_INT_LIT: return make_type(TYPE_INT);
        case NODE_STR_LIT: return make_type(TYPE_STR);
        case NODE_BOOL_LIT: return make_type(TYPE_BOOL);
        case NODE_IDENT: return get_sym(c, n->ident.name);
        case NODE_BINARY: {
            Type left = check_expr(c, n->binary.left);
            Type right = check_expr(c, n->binary.right);
            switch (n->binary.op) {
                case TOK_PLUS: case TOK_ADD_WORD:
                    if (left.kind == TYPE_STR && right.kind == TYPE_STR) {
                        n->resolved_type = 2; // str
                        return make_type(TYPE_STR);
                    }
                    if (left.kind != TYPE_INT || right.kind != TYPE_INT) {
                        fprintf(stderr, "error:%d: cannot use '%s' + '%s'\n", n->line, type_name(left), type_name(right));
                        exit(1);
                    }
                    n->resolved_type = 1; // int
                    return make_type(TYPE_INT);
                case TOK_MINUS: case TOK_STAR: case TOK_SLASH: case TOK_PERCENT: case TOK_MOD_WORD:
                    if (left.kind != TYPE_INT || right.kind != TYPE_INT) {
                        fprintf(stderr, "error:%d: arithmetic requires int, got '%s' and '%s'\n", n->line, type_name(left), type_name(right));
                        exit(1);
                    }
                    n->resolved_type = 1;
                    return make_type(TYPE_INT);
                case TOK_EQ: case TOK_NEQ: case TOK_LT: case TOK_GT: case TOK_LTE: case TOK_GTE:
                case TOK_EQ_WORD: case TOK_NE_WORD: case TOK_LT_WORD: case TOK_GT_WORD: case TOK_LE_WORD: case TOK_GE_WORD:
                    // For eq/ne on strings, mark so codegen can use _str_eq
                    if (left.kind == TYPE_STR && right.kind == TYPE_STR) n->resolved_type = 2;
                    else n->resolved_type = 1;
                    return make_type(TYPE_BOOL);
                case TOK_AND: case TOK_OR:
                    return make_type(TYPE_BOOL);
                default: return make_type(TYPE_UNKNOWN);
            }
        }
        case NODE_UNARY:
            return check_expr(c, n->unary.operand);
        case NODE_CALL: {
            const char *name = n->call.name;
            // Built-in constructors
            if (is_struct(c, name)) return make_struct(name);
            if (!strcmp(name, "list") || !strcmp(name, "list_new")) return make_type(TYPE_LIST);
            if (!strcmp(name, "map") || !strcmp(name, "map_new")) return make_type(TYPE_MAP);
            if (!strcmp(name, "task")) return make_type(TYPE_TASK);
            // Built-in functions (variable arg counts, skip validation)
            if (!strcmp(name, "yell")) { if (n->call.arg_count > 0) check_expr(c, n->call.args[0]); return make_type(TYPE_VOID); }
            if (!strcmp(name, "map_get")) return make_type(TYPE_INT);
            if (!strcmp(name, "map_keys")) return make_type(TYPE_LIST);
            if (!strcmp(name, "map_set") || !strcmp(name, "map_len") || !strcmp(name, "list_len") || !strcmp(name, "list_push")) return make_type(TYPE_VOID);
            if (!strcmp(name, "list_pop")) return make_type(TYPE_INT);
            if (!strcmp(name, "str_concat")) return make_type(TYPE_STR);
            if (!strcmp(name, "int_to_str")) return make_type(TYPE_STR);
            // User function — validate arg count
            int found = 0;
            for (int i = 0; i < c->func_count; i++) {
                if (!strcmp(c->func_names[i], name)) {
                    found = 1;
                    if (n->call.arg_count != c->func_param_counts[i]) {
                        fprintf(stderr, "error:%d: function '%s' expects %d arguments, got %d\n",
                            n->line, name, c->func_param_counts[i], n->call.arg_count);
                        exit(1);
                    }
                    // Check arg expressions
                    for (int j = 0; j < n->call.arg_count; j++)
                        check_expr(c, n->call.args[j]);
                    return c->func_types[i];
                }
            }
            if (!found) {
                fprintf(stderr, "error:%d: unknown function '%s'\n", n->line, name);
                exit(1);
            }
            return make_type(TYPE_UNKNOWN);
        }
        case NODE_METHOD_CALL: {
            const char *m = n->method_call.method;
            if (!strcmp(m, "len")) return make_type(TYPE_INT);
            if (!strcmp(m, "pop")) return make_type(TYPE_INT);
            if (!strcmp(m, "get")) return make_type(TYPE_INT);
            if (!strcmp(m, "keys")) return make_type(TYPE_LIST);
            // User method — check function return
            return get_func_return(c, m);
        }
        case NODE_FIELD_ACCESS: return make_type(TYPE_INT); // struct fields are int for now
        case NODE_INDEX: return make_type(TYPE_INT);
        case NODE_LIST_LIT: return make_type(TYPE_LIST);
        default: return make_type(TYPE_UNKNOWN);
    }
}

static void check_stmt(Checker *c, Node *n);

static void check_stmt(Checker *c, Node *n) {
    switch (n->type) {
        case NODE_VAR_DECL: {
            Type t = check_expr(c, n->var_decl.value);
            set_sym(c, n->var_decl.name, t);
            break;
        }
        case NODE_ASSIGN: {
            Type existing = get_sym(c, n->assign.name);
            Type new_t = check_expr(c, n->assign.value);
            if (existing.kind != TYPE_UNKNOWN && new_t.kind != TYPE_UNKNOWN && existing.kind != new_t.kind) {
                fprintf(stderr, "error:%d: cannot assign '%s' to variable '%s' of type '%s'\n",
                    n->line, type_name(new_t), n->assign.name, type_name(existing));
                exit(1);
            }
            break;
        }
        case NODE_IF:
            for (int i = 0; i < n->if_stmt.branch_count; i++) {
                Type cond_t = check_expr(c, n->if_stmt.conds[i]);
                (void)cond_t; // conditions can be int (truthy) or bool
                Node *body = n->if_stmt.bodies[i];
                for (int j = 0; j < body->block.stmts.count; j++)
                    check_stmt(c, body->block.stmts.items[j]);
            }
            if (n->if_stmt.nah_body) {
                Node *body = n->if_stmt.nah_body;
                for (int j = 0; j < body->block.stmts.count; j++)
                    check_stmt(c, body->block.stmts.items[j]);
            }
            break;
        case NODE_THROUGH_RANGE:
            set_sym(c, n->through_range.var_name, make_type(TYPE_INT));
            for (int j = 0; j < n->through_range.body->block.stmts.count; j++)
                check_stmt(c, n->through_range.body->block.stmts.items[j]);
            break;
        case NODE_THROUGH_IN:
            set_sym(c, n->through_in.var_name, make_type(TYPE_UNKNOWN)); // element type
            for (int j = 0; j < n->through_in.body->block.stmts.count; j++)
                check_stmt(c, n->through_in.body->block.stmts.items[j]);
            break;
        case NODE_INFI:
            if (n->infi.cond) check_expr(c, n->infi.cond);
            for (int j = 0; j < n->infi.body->block.stmts.count; j++)
                check_stmt(c, n->infi.body->block.stmts.items[j]);
            break;
        case NODE_BLOCK:
            for (int j = 0; j < n->block.stmts.count; j++)
                check_stmt(c, n->block.stmts.items[j]);
            break;
        case NODE_GIVE:
            if (n->give.value) check_expr(c, n->give.value);
            break;
        case NODE_CALL:
        case NODE_METHOD_CALL:
            check_expr(c, n);
            break;
        default:
            break;
    }
}

void checker_run(Node *program) {
    Checker c = {0};

    // Register structs
    c.struct_names = malloc(program->program.structs.count * sizeof(char *));
    c.struct_count = program->program.structs.count;
    for (int i = 0; i < c.struct_count; i++)
        c.struct_names[i] = program->program.structs.items[i]->struct_def.name;

    // Register function return types
    c.func_names = malloc(program->program.funcs.count * sizeof(char *));
    c.func_types = malloc(program->program.funcs.count * sizeof(Type));
    c.func_param_counts = malloc(program->program.funcs.count * sizeof(int));
    c.func_count = program->program.funcs.count;
    for (int i = 0; i < c.func_count; i++) {
        Node *f = program->program.funcs.items[i];
        c.func_names[i] = f->func_def.name;
        c.func_param_counts[i] = f->func_def.param_count;
        if (!f->func_def.return_type) c.func_types[i] = make_type(TYPE_VOID);
        else if (!strcmp(f->func_def.return_type, "int")) c.func_types[i] = make_type(TYPE_INT);
        else if (!strcmp(f->func_def.return_type, "str")) c.func_types[i] = make_type(TYPE_STR);
        else if (!strcmp(f->func_def.return_type, "bool")) c.func_types[i] = make_type(TYPE_BOOL);
        else if (is_struct(&c, f->func_def.return_type)) c.func_types[i] = make_struct(f->func_def.return_type);
        else {
            fprintf(stderr, "error: unknown return type '%s' for function '%s'\n", f->func_def.return_type, f->func_def.name);
            exit(1);
        }
    }

    // Check each function body
    for (int i = 0; i < program->program.funcs.count; i++) {
        Node *f = program->program.funcs.items[i];
        c.count = 0; // reset local symbols per function

        // Register params
        for (int j = 0; j < f->func_def.param_count; j++) {
            const char *pt = f->func_def.param_types[j];
            Type t;
            if (!strcmp(pt, "int")) t = make_type(TYPE_INT);
            else if (!strcmp(pt, "str")) t = make_type(TYPE_STR);
            else if (!strcmp(pt, "bool")) t = make_type(TYPE_BOOL);
            else if (is_struct(&c, pt)) t = make_struct(pt);
            else t = make_type(TYPE_INT);
            set_sym(&c, f->func_def.param_names[j], t);
        }

        Node *body = f->func_def.body;
        for (int j = 0; j < body->block.stmts.count; j++)
            check_stmt(&c, body->block.stmts.items[j]);
    }
}
