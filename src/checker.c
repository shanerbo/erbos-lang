#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "checker.h"

#define MAX_SYMS 256
#define MAX_FUNCS 128

typedef struct {
    char *name;
    Type type;
} Symbol;

typedef struct {
    char *name;
    char **field_names;
    char **field_types;
    int field_count;
} StructInfo;

typedef struct {
    char *name;
    Type return_type;
    int param_count;
    char **param_types_str;
} FuncInfo;

typedef struct {
    Symbol syms[MAX_SYMS];
    int count;
    FuncInfo funcs[MAX_FUNCS];
    int func_count;
    StructInfo *structs;
    int struct_count;
    // Current function context
    Type cur_return_type;
    int found_give;
} Checker;

static Type make_type(TypeKind k) { return (Type){k, NULL, NULL, NULL, NULL}; }
static Type make_struct(const char *name) { return (Type){TYPE_STRUCT, (char *)name, NULL, NULL, NULL}; }

static Type *alloc_type(Type t) {
    Type *p = malloc(sizeof(Type));
    *p = t;
    return p;
}

static Type make_list_of(Type elem) {
    Type t = {TYPE_LIST, NULL, alloc_type(elem), NULL, NULL};
    return t;
}

static Type make_map_of(Type key, Type val) {
    Type t = {TYPE_MAP, NULL, NULL, alloc_type(key), alloc_type(val)};
    return t;
}

static int types_equal(Type a, Type b) {
    if (a.kind != b.kind) return 0;
    if (a.kind == TYPE_STRUCT) return a.struct_name && b.struct_name && !strcmp(a.struct_name, b.struct_name);
    return 1;
}

static void set_sym(Checker *c, const char *name, Type t) {
    for (int i = 0; i < c->count; i++)
        if (!strcmp(c->syms[i].name, name)) { c->syms[i].type = t; return; }
    c->syms[c->count++] = (Symbol){(char *)name, t};
}

static Type get_sym(Checker *c, const char *name) {
    for (int i = c->count - 1; i >= 0; i--)
        if (!strcmp(c->syms[i].name, name)) return c->syms[i].type;
    return make_type(TYPE_UNKNOWN);
}

static FuncInfo *find_func(Checker *c, const char *name) {
    for (int i = 0; i < c->func_count; i++)
        if (!strcmp(c->funcs[i].name, name)) return &c->funcs[i];
    return NULL;
}

static int is_struct(Checker *c, const char *name) {
    for (int i = 0; i < c->struct_count; i++)
        if (!strcmp(c->structs[i].name, name)) return 1;
    return 0;
}

static StructInfo *find_struct(Checker *c, const char *name) {
    for (int i = 0; i < c->struct_count; i++)
        if (!strcmp(c->structs[i].name, name)) return &c->structs[i];
    return NULL;
}

static int struct_has_field(Checker *c, const char *struct_name, const char *field) {
    StructInfo *s = find_struct(c, struct_name);
    if (!s) return 1; // unknown struct, don't error
    for (int i = 0; i < s->field_count; i++)
        if (!strcmp(s->field_names[i], field)) return 1;
    return 0;
}

static Type parse_type_str(Checker *c, const char *t) {
    if (!t) return make_type(TYPE_VOID);
    if (!strcmp(t, "int")) return make_type(TYPE_INT);
    if (!strcmp(t, "str")) return make_type(TYPE_STR);
    if (!strcmp(t, "bool")) return make_type(TYPE_BOOL);
    if (!strcmp(t, "void")) return make_type(TYPE_VOID);
    if (!strcmp(t, "list")) return make_type(TYPE_LIST);
    if (!strcmp(t, "map")) return make_type(TYPE_MAP);
    if (is_struct(c, t)) return make_struct(t);
    return make_type(TYPE_INT);
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
                        n->resolved_type = 2;
                        return make_type(TYPE_STR);
                    }
                    if ((left.kind == TYPE_STR && right.kind == TYPE_UNKNOWN) ||
                        (left.kind == TYPE_UNKNOWN && right.kind == TYPE_STR)) {
                        n->resolved_type = 2;
                        return make_type(TYPE_STR);
                    }
                    if (left.kind == TYPE_UNKNOWN || right.kind == TYPE_UNKNOWN) {
                        n->resolved_type = 1;
                        return make_type(TYPE_INT);
                    }
                    if (left.kind != TYPE_INT || right.kind != TYPE_INT) {
                        fprintf(stderr, "error:%d: cannot use '%s' + '%s'\n", n->line, type_name(left), type_name(right));
                        exit(1);
                    }
                    n->resolved_type = 1;
                    return make_type(TYPE_INT);
                case TOK_MINUS: case TOK_STAR: case TOK_SLASH: case TOK_PERCENT: case TOK_MOD_WORD:
                    if (left.kind == TYPE_UNKNOWN || right.kind == TYPE_UNKNOWN) {
                        n->resolved_type = 1;
                        return make_type(TYPE_INT);
                    }
                    if (left.kind != TYPE_INT || right.kind != TYPE_INT) {
                        fprintf(stderr, "error:%d: arithmetic requires int, got '%s' and '%s'\n", n->line, type_name(left), type_name(right));
                        exit(1);
                    }
                    n->resolved_type = 1;
                    return make_type(TYPE_INT);
                case TOK_EQ: case TOK_NEQ: case TOK_LT: case TOK_GT: case TOK_LTE: case TOK_GTE:
                case TOK_EQ_WORD: case TOK_NE_WORD: case TOK_LT_WORD: case TOK_GT_WORD: case TOK_LE_WORD: case TOK_GE_WORD:
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
            if (is_struct(c, name)) return make_struct(name);
            if (!strcmp(name, "list") || !strcmp(name, "list_new")) return make_type(TYPE_LIST);
            if (!strcmp(name, "map") || !strcmp(name, "map_new")) return make_type(TYPE_MAP);
            if (!strcmp(name, "task")) return make_type(TYPE_TASK);
            if (!strcmp(name, "yell")) { if (n->call.arg_count > 0) check_expr(c, n->call.args[0]); return make_type(TYPE_VOID); }
            if (!strcmp(name, "len")) {
                if (n->call.arg_count != 1) { fprintf(stderr, "error:%d: len() takes 1 argument\n", n->line); exit(1); }
                Type arg_t = check_expr(c, n->call.args[0]);
                if (arg_t.kind == TYPE_MAP) n->call.args[0]->resolved_type = 4;
                else if (arg_t.kind == TYPE_STR) n->call.args[0]->resolved_type = 5;
                else n->call.args[0]->resolved_type = 3;
                return make_type(TYPE_INT);
            }
            if (!strcmp(name, "str_len")) { if (n->call.arg_count > 0) check_expr(c, n->call.args[0]); return make_type(TYPE_INT); }
            if (!strcmp(name, "char_at")) { for (int j=0;j<n->call.arg_count;j++) check_expr(c, n->call.args[j]); return make_type(TYPE_STR); }
            if (!strcmp(name, "list_set")) { for (int j=0;j<n->call.arg_count;j++) check_expr(c, n->call.args[j]); return make_type(TYPE_VOID); }
            if (!strcmp(name, "map_get")) return make_type(TYPE_INT);
            if (!strcmp(name, "map_keys")) return make_type(TYPE_LIST);
            if (!strcmp(name, "map_set") || !strcmp(name, "map_len") || !strcmp(name, "list_len") || !strcmp(name, "list_push")) return make_type(TYPE_VOID);
            if (!strcmp(name, "list_pop")) return make_type(TYPE_INT);
            if (!strcmp(name, "str_concat")) return make_type(TYPE_STR);
            if (!strcmp(name, "int_to_str")) return make_type(TYPE_STR);
            // User function
            FuncInfo *fi = find_func(c, name);
            if (!fi) {
                fprintf(stderr, "error:%d: unknown function '%s'\n", n->line, name);
                exit(1);
            }
            if (n->call.arg_count != fi->param_count) {
                fprintf(stderr, "error:%d: function '%s' expects %d arguments, got %d\n",
                    n->line, name, fi->param_count, n->call.arg_count);
                exit(1);
            }
            // Check arg types
            for (int j = 0; j < n->call.arg_count; j++) {
                Type arg_t = check_expr(c, n->call.args[j]);
                Type expected = parse_type_str(c, fi->param_types_str[j]);
                if (arg_t.kind != TYPE_UNKNOWN && expected.kind != TYPE_UNKNOWN && !types_equal(arg_t, expected)) {
                    fprintf(stderr, "error:%d: argument %d of '%s' expects '%s', got '%s'\n",
                        n->line, j + 1, name, type_name(expected), type_name(arg_t));
                    exit(1);
                }
            }
            return fi->return_type;
        }
        case NODE_METHOD_CALL: {
            const char *m = n->method_call.method;
            Type obj_t = check_expr(c, n->method_call.object);
            if (!strcmp(m, "push")) {
                if (n->method_call.arg_count > 0) {
                    Type arg_t = check_expr(c, n->method_call.args[0]);
                    // Validate element type if list has generic info
                    if (obj_t.elem_type && arg_t.kind != TYPE_UNKNOWN && obj_t.elem_type->kind != TYPE_UNKNOWN) {
                        if (!types_equal(arg_t, *obj_t.elem_type)) {
                            fprintf(stderr, "error:%d: cannot push '%s' into list of '%s'\n",
                                n->line, type_name(arg_t), type_name(*obj_t.elem_type));
                            exit(1);
                        }
                    }
                }
                return make_type(TYPE_VOID);
            }
            if (!strcmp(m, "len")) return make_type(TYPE_INT);
            if (!strcmp(m, "pop")) {
                if (obj_t.elem_type) return *obj_t.elem_type;
                return make_type(TYPE_UNKNOWN);
            }
            if (!strcmp(m, "get")) {
                if (obj_t.val_type) return *obj_t.val_type;
                return make_type(TYPE_UNKNOWN);
            }
            if (!strcmp(m, "keys")) return make_type(TYPE_LIST);
            if (!strcmp(m, "set") || !strcmp(m, "fire") || !strcmp(m, "collapse")) return make_type(TYPE_VOID);
            // User method
            FuncInfo *fi = find_func(c, m);
            if (fi) return fi->return_type;
            return make_type(TYPE_UNKNOWN);
        }
        case NODE_FIELD_ACCESS: {
            Type obj_t = check_expr(c, n->field_access.object);
            if (obj_t.kind == TYPE_STRUCT && obj_t.struct_name) {
                if (!struct_has_field(c, obj_t.struct_name, n->field_access.field)) {
                    fprintf(stderr, "error:%d: struct '%s' has no field '%s'\n",
                        n->line, obj_t.struct_name, n->field_access.field);
                    exit(1);
                }
            }
            return make_type(TYPE_INT); // all fields are int for now
        }
        case NODE_INDEX: {
            Type obj_t = check_expr(c, n->index_access.object);
            check_expr(c, n->index_access.index);
            // Return element type if known
            if (obj_t.elem_type) return *obj_t.elem_type;
            return make_type(TYPE_UNKNOWN);
        }
        case NODE_LIST_LIT: {
            Type elem_t = make_type(TYPE_UNKNOWN);
            for (int i = 0; i < n->list_lit.count; i++) {
                Type t = check_expr(c, n->list_lit.items[i]);
                if (i == 0) { elem_t = t; }
                else if (t.kind != TYPE_UNKNOWN && elem_t.kind != TYPE_UNKNOWN && !types_equal(t, elem_t)) {
                    fprintf(stderr, "error:%d: list element type mismatch: expected '%s', got '%s'\n",
                        n->line, type_name(elem_t), type_name(t));
                    exit(1);
                }
            }
            if (elem_t.kind != TYPE_UNKNOWN) return make_list_of(elem_t);
            return make_type(TYPE_LIST);
        }
        case NODE_MAP_LIT: {
            Type key_t = make_type(TYPE_UNKNOWN);
            Type val_t = make_type(TYPE_UNKNOWN);
            for (int i = 0; i < n->map_lit.count; i++) {
                Type kt = check_expr(c, n->map_lit.keys[i]);
                Type vt = check_expr(c, n->map_lit.values[i]);
                if (i == 0) { key_t = kt; val_t = vt; }
                else {
                    if (kt.kind != TYPE_UNKNOWN && key_t.kind != TYPE_UNKNOWN && !types_equal(kt, key_t)) {
                        fprintf(stderr, "error:%d: map key type mismatch: expected '%s', got '%s'\n",
                            n->line, type_name(key_t), type_name(kt));
                        exit(1);
                    }
                    if (vt.kind != TYPE_UNKNOWN && val_t.kind != TYPE_UNKNOWN && !types_equal(vt, val_t)) {
                        fprintf(stderr, "error:%d: map value type mismatch: expected '%s', got '%s'\n",
                            n->line, type_name(val_t), type_name(vt));
                        exit(1);
                    }
                }
            }
            if (key_t.kind != TYPE_UNKNOWN && val_t.kind != TYPE_UNKNOWN)
                return make_map_of(key_t, val_t);
            return make_type(TYPE_MAP);
        }
        default: return make_type(TYPE_UNKNOWN);
    }
}

static int has_give_in_block(Node *block);

static int has_give_in_stmts(Node **stmts, int count) {
    for (int i = 0; i < count; i++) {
        if (stmts[i]->type == NODE_GIVE) return 1;
        if (stmts[i]->type == NODE_IF) {
            // Check all branches
            for (int b = 0; b < stmts[i]->if_stmt.branch_count; b++)
                if (has_give_in_block(stmts[i]->if_stmt.bodies[b])) return 1;
            if (stmts[i]->if_stmt.nah_body && has_give_in_block(stmts[i]->if_stmt.nah_body)) return 1;
        }
        if (stmts[i]->type == NODE_BLOCK && has_give_in_block(stmts[i])) return 1;
    }
    return 0;
}

static int has_give_in_block(Node *block) {
    return has_give_in_stmts(block->block.stmts.items, block->block.stmts.count);
}

static void check_stmt(Checker *c, Node *n);

static void check_stmt(Checker *c, Node *n) {
    switch (n->type) {
        case NODE_VAR_DECL: {
            Type t = check_expr(c, n->var_decl.value);
            // Enforce: bare list()/map() constructors must have type annotations
            if (t.kind == TYPE_LIST && !n->var_decl.elem_type_name &&
                n->var_decl.value->type == NODE_CALL &&
                (!strcmp(n->var_decl.value->call.name, "list") || !strcmp(n->var_decl.value->call.name, "list_new"))) {
                fprintf(stderr, "error:%d: list must specify element type: 'list of <type>'\n", n->line);
                exit(1);
            }
            if (t.kind == TYPE_MAP && !n->var_decl.key_type_name &&
                n->var_decl.value->type == NODE_CALL &&
                (!strcmp(n->var_decl.value->call.name, "map") || !strcmp(n->var_decl.value->call.name, "map_new"))) {
                fprintf(stderr, "error:%d: map must specify key and value types: 'map of <key> to <value>'\n", n->line);
                exit(1);
            }
            // If declaration has explicit generic type, use it
            if (n->var_decl.elem_type_name && t.kind == TYPE_LIST) {
                t = make_list_of(parse_type_str(c, n->var_decl.elem_type_name));
            }
            if (n->var_decl.key_type_name && n->var_decl.val_type_name && t.kind == TYPE_MAP) {
                t = make_map_of(parse_type_str(c, n->var_decl.key_type_name),
                                parse_type_str(c, n->var_decl.val_type_name));
            }
            set_sym(c, n->var_decl.name, t);
            break;
        }
        case NODE_ASSIGN: {
            Type existing = get_sym(c, n->assign.name);
            Type new_t = check_expr(c, n->assign.value);
            if (existing.kind != TYPE_UNKNOWN && new_t.kind != TYPE_UNKNOWN && !types_equal(existing, new_t)) {
                fprintf(stderr, "error:%d: cannot assign '%s' to variable '%s' of type '%s'\n",
                    n->line, type_name(new_t), n->assign.name, type_name(existing));
                exit(1);
            }
            break;
        }
        case NODE_FIELD_ASSIGN: {
            check_expr(c, n->field_assign.object);
            check_expr(c, n->field_assign.value);
            // Validate field exists if we know the struct type
            // field_assign.object is the base object before the field
            // We'd need the object's type — for now skip deep validation here
            break;
        }
        case NODE_IF:
            for (int i = 0; i < n->if_stmt.branch_count; i++) {
                check_expr(c, n->if_stmt.conds[i]);
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
            check_expr(c, n->through_range.from);
            check_expr(c, n->through_range.to);
            if (n->through_range.by) check_expr(c, n->through_range.by);
            set_sym(c, n->through_range.var_name, make_type(TYPE_INT));
            for (int j = 0; j < n->through_range.body->block.stmts.count; j++)
                check_stmt(c, n->through_range.body->block.stmts.items[j]);
            break;
        case NODE_THROUGH_IN:
            check_expr(c, n->through_in.collection);
            set_sym(c, n->through_in.var_name, make_type(TYPE_UNKNOWN));
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
        case NODE_GIVE: {
            if (n->give.value) {
                Type give_t = check_expr(c, n->give.value);
                // Check return type compatibility
                if (c->cur_return_type.kind == TYPE_VOID) {
                    fprintf(stderr, "error:%d: cannot return a value from a void function\n", n->line);
                    exit(1);
                }
                if (give_t.kind != TYPE_UNKNOWN && c->cur_return_type.kind != TYPE_UNKNOWN &&
                    !types_equal(give_t, c->cur_return_type)) {
                    fprintf(stderr, "error:%d: return type mismatch: expected '%s', got '%s'\n",
                        n->line, type_name(c->cur_return_type), type_name(give_t));
                    exit(1);
                }
            }
            c->found_give = 1;
            break;
        }
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
    c.struct_count = program->program.structs.count;
    c.structs = malloc(c.struct_count * sizeof(StructInfo));
    for (int i = 0; i < c.struct_count; i++) {
        Node *s = program->program.structs.items[i];
        c.structs[i].name = s->struct_def.name;
        c.structs[i].field_names = s->struct_def.field_names;
        c.structs[i].field_types = s->struct_def.field_types;
        c.structs[i].field_count = s->struct_def.field_count;
    }

    // Register functions
    c.func_count = program->program.funcs.count;
    for (int i = 0; i < c.func_count && i < MAX_FUNCS; i++) {
        Node *f = program->program.funcs.items[i];
        c.funcs[i].name = f->func_def.name;
        c.funcs[i].param_count = f->func_def.param_count;
        c.funcs[i].param_types_str = f->func_def.param_types;
        if (!f->func_def.return_type) c.funcs[i].return_type = make_type(TYPE_VOID);
        else if (!strcmp(f->func_def.return_type, "int")) c.funcs[i].return_type = make_type(TYPE_INT);
        else if (!strcmp(f->func_def.return_type, "str")) c.funcs[i].return_type = make_type(TYPE_STR);
        else if (!strcmp(f->func_def.return_type, "bool")) c.funcs[i].return_type = make_type(TYPE_BOOL);
        else if (is_struct(&c, f->func_def.return_type)) c.funcs[i].return_type = make_struct(f->func_def.return_type);
        else {
            fprintf(stderr, "error: unknown return type '%s' for function '%s'\n", f->func_def.return_type, f->func_def.name);
            exit(1);
        }
    }

    // Check each function body
    for (int i = 0; i < program->program.funcs.count; i++) {
        Node *f = program->program.funcs.items[i];
        c.count = 0;
        c.cur_return_type = c.funcs[i].return_type;
        c.found_give = 0;

        // Register params
        for (int j = 0; j < f->func_def.param_count; j++) {
            Type t = parse_type_str(&c, f->func_def.param_types[j]);
            set_sym(&c, f->func_def.param_names[j], t);
        }

        Node *body = f->func_def.body;
        for (int j = 0; j < body->block.stmts.count; j++)
            check_stmt(&c, body->block.stmts.items[j]);

        // Check for missing return
        if (c.cur_return_type.kind != TYPE_VOID && !c.found_give) {
            if (!has_give_in_block(body)) {
                fprintf(stderr, "error: function '%s' must return '%s' but has no give statement\n",
                    f->func_def.name, type_name(c.cur_return_type));
                exit(1);
            }
        }
    }
}
