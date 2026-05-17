// Monomorphization pass for generic structs and methods.
//
// Runs after the parser and BEFORE the checker. The pass:
//
//   1. Collects every generic struct (type_param_count > 0) and every
//      generic method (receiver_type_args != NULL) into template tables,
//      then removes them from the program (they no longer represent
//      emittable code, only blueprints).
//
//   2. Walks every type-name string in the (de-genericised) program —
//      struct field types, function/method param types, return types,
//      variable-declaration types, and the type names embedded in
//      method receiver-type fields — looking for parametric forms like
//      `Box<int>` or `Map<str, int>`. Each unique form is queued for
//      instantiation.
//
//   3. For every queued instantiation, clones the template AST subtree
//      and substitutes the type-parameter names with the concrete
//      argument names in every type-expression string in that clone.
//      Cloning recurses through field types and method bodies so that
//      substitutions inside expressions (var-decls, etc.) are also
//      applied.
//
//   4. Rewrites each parametric type-name string to its mangled form:
//
//          Box<int>            ->  Box__int
//          Map<str, int>       ->  Map__str__int
//          List<Pair<str,int>> ->  List__Pair__str__int
//
//      Both the queued-instantiation table entries and every type
//      expression in the program are rewritten in lockstep so that the
//      checker and codegen see a uniform "no angle brackets" world.
//
//   5. Appends the monomorphic clones to the program's struct and
//      function tables. Discovery of *new* parametric types inside a
//      clone is iterative: cloning may produce new instantiations
//      (e.g. List<Pair<str, int>> contains Pair<str, int>), so the
//      worklist runs to fixpoint.
//
// Discovery of an instantiation whose template is unknown (e.g.
// `Foo<int>` when no `Foo<T>` is declared) is a compile error.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "monomorph.h"

// ---------- string utilities ----------

static char *xstrdup(const char *s) {
    if (!s) return NULL;
    return strdup(s);
}

// Returns 1 if `s` contains a '<' character (i.e. it's a parametric type
// expression like "Box<int>" or "Map<str,int>").
static int is_parametric(const char *s) {
    return s && strchr(s, '<') != NULL;
}

// Strip ALL whitespace from a type-name string. Source-written
// `Pair<str, int>` and parser-emitted `Pair<str,int>` then become the
// same key, so the seen-set deduplicates correctly.
static char *normalize_type(const char *s) {
    if (!s) return NULL;
    int n = (int)strlen(s);
    char *out = malloc(n + 1);
    int j = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] != ' ' && s[i] != '\t') out[j++] = s[i];
    }
    out[j] = '\0';
    return out;
}

// Split a parametric type-name string into its head identifier and
// argument list. For "Map<str, int>" this writes "Map" into out_head
// and ["str", "int"] (allocated, caller frees) into *out_args.
//
// Argument trimming: spaces around commas are stripped. Nested
// generics inside an argument (List<Pair<str, int>>) are kept whole.
//
// Returns the number of args (0 if `s` has no '<').
static int parse_parametric(const char *s, char *out_head, int head_cap,
                            char ***out_args) {
    *out_args = NULL;
    const char *lt = strchr(s, '<');
    if (!lt) {
        // bare name — copy whole into head
        int n = (int)strlen(s);
        if (n >= head_cap) n = head_cap - 1;
        memcpy(out_head, s, n);
        out_head[n] = '\0';
        return 0;
    }
    int hn = (int)(lt - s);
    if (hn >= head_cap) hn = head_cap - 1;
    memcpy(out_head, s, hn);
    out_head[hn] = '\0';

    // Walk past '<' and split on top-level commas.
    int cap = 4, count = 0;
    char **args = malloc(cap * sizeof(char *));
    const char *cur = lt + 1;
    int depth = 1;
    const char *arg_start = cur;
    char buf[256];
    int blen = 0;

    while (*cur) {
        char c = *cur;
        if (c == '<') {
            depth++;
            if (blen + 1 < (int)sizeof(buf)) { buf[blen++] = c; }
            cur++;
            continue;
        }
        if (c == '>') {
            depth--;
            if (depth == 0) {
                // emit current arg (trimmed)
                if (count >= cap) { cap *= 2; args = realloc(args, cap * sizeof(char *)); }
                buf[blen] = '\0';
                // trim leading/trailing spaces
                int s0 = 0;
                while (buf[s0] == ' ') s0++;
                int s1 = blen;
                while (s1 > s0 && buf[s1 - 1] == ' ') s1--;
                int alen = s1 - s0;
                char *a = malloc(alen + 1);
                memcpy(a, buf + s0, alen);
                a[alen] = '\0';
                args[count++] = a;
                blen = 0;
                cur++;
                break;
            }
            if (blen + 1 < (int)sizeof(buf)) { buf[blen++] = c; }
            cur++;
            continue;
        }
        if (c == ',' && depth == 1) {
            // top-level separator — emit arg
            if (count >= cap) { cap *= 2; args = realloc(args, cap * sizeof(char *)); }
            buf[blen] = '\0';
            int s0 = 0;
            while (buf[s0] == ' ') s0++;
            int s1 = blen;
            while (s1 > s0 && buf[s1 - 1] == ' ') s1--;
            int alen = s1 - s0;
            char *a = malloc(alen + 1);
            memcpy(a, buf + s0, alen);
            a[alen] = '\0';
            args[count++] = a;
            blen = 0;
            cur++;
            (void)arg_start;
            arg_start = cur;
            continue;
        }
        if (blen + 1 < (int)sizeof(buf)) { buf[blen++] = c; }
        cur++;
    }
    *out_args = args;
    return count;
}

// Mangle "Box<int>" -> "Box__int", "Map<str, int>" -> "Map__str__int".
// Recursive: nested generics are mangled inside-out.
//
// Returns a freshly allocated string the caller must free.
static char *mangle_type(const char *s) {
    if (!is_parametric(s)) return xstrdup(s);
    char head[128];
    char **args = NULL;
    int n = parse_parametric(s, head, sizeof(head), &args);
    // Build "head__arg1__arg2__..." with each arg recursively mangled.
    int cap = 256;
    char *buf = malloc(cap);
    int len = (int)strlen(head);
    if (len + 1 > cap) { cap = len + 1; buf = realloc(buf, cap); }
    memcpy(buf, head, len);
    buf[len] = '\0';
    for (int i = 0; i < n; i++) {
        char *m = mangle_type(args[i]);
        int ml = (int)strlen(m);
        while (len + 2 + ml + 1 > cap) { cap *= 2; buf = realloc(buf, cap); }
        buf[len++] = '_';
        buf[len++] = '_';
        memcpy(buf + len, m, ml);
        len += ml;
        buf[len] = '\0';
        free(m);
    }
    for (int i = 0; i < n; i++) free(args[i]);
    free(args);
    return buf;
}

// Substitute type-parameter names in a parametric type expression `s`
// with their concrete argument values. params/concrete are parallel
// arrays of length count. The returned string is freshly allocated.
//
// Substitution applies to bare identifiers AND to head-of-instantiation
// names. So if K -> "str" and V -> "int", "Pair<K, V>" becomes
// "Pair<str, int>", and a bare "K" becomes "str".
static char *substitute_type(const char *s, char **params, char **concrete,
                             int count) {
    if (!s) return NULL;
    // Bare identifier — direct lookup.
    if (!is_parametric(s)) {
        for (int i = 0; i < count; i++) {
            if (!strcmp(s, params[i])) return xstrdup(concrete[i]);
        }
        return xstrdup(s);
    }
    // Parametric: substitute inside each arg, then rebuild as
    // "head<arg1, arg2, ...>".
    char head[128];
    char **args = NULL;
    int n = parse_parametric(s, head, sizeof(head), &args);
    // Substitute the head name too in case the user wrote
    // "K<...>" (currently unreachable — type params don't have type
    // params themselves — but cheap to handle).
    char *new_head = xstrdup(head);
    for (int i = 0; i < count; i++) {
        if (!strcmp(new_head, params[i])) {
            free(new_head);
            new_head = xstrdup(concrete[i]);
            break;
        }
    }
    int cap = 256;
    char *buf = malloc(cap);
    int len = (int)strlen(new_head);
    if (len + 1 > cap) { cap = len + 1; buf = realloc(buf, cap); }
    memcpy(buf, new_head, len);
    buf[len] = '\0';
    if (n > 0) {
        if (len + 1 + 1 > cap) { cap *= 2; buf = realloc(buf, cap); }
        buf[len++] = '<';
        buf[len] = '\0';
        for (int i = 0; i < n; i++) {
            char *sa = substitute_type(args[i], params, concrete, count);
            int sal = (int)strlen(sa);
            while (len + (i > 0 ? 1 : 0) + sal + 2 > cap) { cap *= 2; buf = realloc(buf, cap); }
            // Use a no-space ',' separator so the rebuilt string matches
            // the format produced by parse_type_name (also no-space).
            // Without this, identical instantiations land in `seen` as
            // two distinct strings and produce duplicate symbols.
            if (i > 0) { buf[len++] = ','; }
            memcpy(buf + len, sa, sal);
            len += sal;
            buf[len] = '\0';
            free(sa);
        }
        buf[len++] = '>';
        buf[len] = '\0';
    }
    free(new_head);
    for (int i = 0; i < n; i++) free(args[i]);
    free(args);
    return buf;
}

// ---------- AST cloning ----------

static Node *clone_node(Node *n);

static Node **clone_node_array(Node **arr, int count) {
    if (!arr || count == 0) return NULL;
    Node **r = malloc(count * sizeof(Node *));
    for (int i = 0; i < count; i++) r[i] = clone_node(arr[i]);
    return r;
}

static char **clone_string_array(char **arr, int count) {
    if (!arr || count == 0) return NULL;
    char **r = malloc(count * sizeof(char *));
    for (int i = 0; i < count; i++) r[i] = xstrdup(arr[i]);
    return r;
}

// Clone an entire AST subtree. Strings are duplicated so substitution
// in the clone doesn't poison the template.
static Node *clone_node(Node *n) {
    if (!n) return NULL;
    Node *c = calloc(1, sizeof(Node));
    *c = *n; // shallow copy — fixed up below per node type.
    switch (n->type) {
        case NODE_INT_LIT:
        case NODE_BOOL_LIT:
        case NODE_STOP:
        case NODE_SKIP:
            break;
        case NODE_STR_LIT:
            c->str_lit.value = xstrdup(n->str_lit.value);
            break;
        case NODE_IDENT:
            c->ident.name = xstrdup(n->ident.name);
            break;
        case NODE_BINARY:
            c->binary.left = clone_node(n->binary.left);
            c->binary.right = clone_node(n->binary.right);
            break;
        case NODE_UNARY:
            c->unary.operand = clone_node(n->unary.operand);
            break;
        case NODE_CALL:
            c->call.name = xstrdup(n->call.name);
            c->call.args = clone_node_array(n->call.args, n->call.arg_count);
            break;
        case NODE_METHOD_CALL:
            c->method_call.object = clone_node(n->method_call.object);
            c->method_call.method = xstrdup(n->method_call.method);
            c->method_call.args = clone_node_array(n->method_call.args, n->method_call.arg_count);
            c->method_call.resolved_struct_name = xstrdup(n->method_call.resolved_struct_name);
            break;
        case NODE_FIELD_ACCESS:
            c->field_access.object = clone_node(n->field_access.object);
            c->field_access.field = xstrdup(n->field_access.field);
            c->field_access.struct_name = xstrdup(n->field_access.struct_name);
            break;
        case NODE_FIELD_ASSIGN:
            c->field_assign.object = clone_node(n->field_assign.object);
            c->field_assign.field = xstrdup(n->field_assign.field);
            c->field_assign.value = clone_node(n->field_assign.value);
            c->field_assign.struct_name = xstrdup(n->field_assign.struct_name);
            break;
        case NODE_INDEX:
            c->index_access.object = clone_node(n->index_access.object);
            c->index_access.index = clone_node(n->index_access.index);
            break;
        case NODE_VAR_DECL:
            c->var_decl.name = xstrdup(n->var_decl.name);
            c->var_decl.type_name = xstrdup(n->var_decl.type_name);
            c->var_decl.elem_type_name = xstrdup(n->var_decl.elem_type_name);
            c->var_decl.key_type_name = xstrdup(n->var_decl.key_type_name);
            c->var_decl.val_type_name = xstrdup(n->var_decl.val_type_name);
            c->var_decl.value = clone_node(n->var_decl.value);
            break;
        case NODE_ASSIGN:
            c->assign.name = xstrdup(n->assign.name);
            c->assign.value = clone_node(n->assign.value);
            break;
        case NODE_IF:
            c->if_stmt.conds = clone_node_array(n->if_stmt.conds, n->if_stmt.branch_count);
            c->if_stmt.bodies = clone_node_array(n->if_stmt.bodies, n->if_stmt.branch_count);
            c->if_stmt.nah_body = clone_node(n->if_stmt.nah_body);
            break;
        case NODE_THROUGH_RANGE:
            c->through_range.var_name = xstrdup(n->through_range.var_name);
            c->through_range.from = clone_node(n->through_range.from);
            c->through_range.to = clone_node(n->through_range.to);
            c->through_range.by = clone_node(n->through_range.by);
            c->through_range.body = clone_node(n->through_range.body);
            break;
        case NODE_THROUGH_IN:
            c->through_in.var_name = xstrdup(n->through_in.var_name);
            c->through_in.collection = clone_node(n->through_in.collection);
            c->through_in.body = clone_node(n->through_in.body);
            break;
        case NODE_INFI:
            c->infi.cond = clone_node(n->infi.cond);
            c->infi.body = clone_node(n->infi.body);
            break;
        case NODE_GIVE:
            c->give.value = clone_node(n->give.value);
            break;
        case NODE_BLOCK: {
            int count = n->block.stmts.count;
            c->block.stmts.count = count;
            c->block.stmts.cap = count;
            c->block.stmts.items = clone_node_array(n->block.stmts.items, count);
            break;
        }
        case NODE_LIST_LIT:
            c->list_lit.items = clone_node_array(n->list_lit.items, n->list_lit.count);
            break;
        case NODE_MAP_LIT:
            c->map_lit.keys = clone_node_array(n->map_lit.keys, n->map_lit.count);
            c->map_lit.values = clone_node_array(n->map_lit.values, n->map_lit.count);
            break;
        case NODE_MATCH: {
            c->match_expr.expr = clone_node(n->match_expr.expr);
            int ac = n->match_expr.arm_count;
            c->match_expr.arm_variant_names = clone_string_array(n->match_expr.arm_variant_names, ac);
            c->match_expr.arm_binding_counts = malloc(ac * sizeof(int));
            for (int i = 0; i < ac; i++) c->match_expr.arm_binding_counts[i] = n->match_expr.arm_binding_counts[i];
            c->match_expr.arm_bindings = malloc(ac * sizeof(char **));
            for (int i = 0; i < ac; i++)
                c->match_expr.arm_bindings[i] = clone_string_array(n->match_expr.arm_bindings[i], n->match_expr.arm_binding_counts[i]);
            c->match_expr.arm_bodies = clone_node_array(n->match_expr.arm_bodies, ac);
            break;
        }
        case NODE_ASSERT:
            c->assert_stmt.condition = clone_node(n->assert_stmt.condition);
            break;
        case NODE_FUNC_DEF:
            c->func_def.name = xstrdup(n->func_def.name);
            c->func_def.receiver_type = xstrdup(n->func_def.receiver_type);
            c->func_def.param_names = clone_string_array(n->func_def.param_names, n->func_def.param_count);
            c->func_def.param_types = clone_string_array(n->func_def.param_types, n->func_def.param_count);
            if (n->func_def.param_is_ref) {
                c->func_def.param_is_ref = malloc(n->func_def.param_count * sizeof(int));
                for (int i = 0; i < n->func_def.param_count; i++)
                    c->func_def.param_is_ref[i] = n->func_def.param_is_ref[i];
            }
            c->func_def.return_type = xstrdup(n->func_def.return_type);
            c->func_def.body = clone_node(n->func_def.body);
            c->func_def.receiver_type_args = clone_string_array(n->func_def.receiver_type_args, n->func_def.receiver_type_arg_count);
            break;
        case NODE_STRUCT_DEF:
            c->struct_def.name = xstrdup(n->struct_def.name);
            c->struct_def.field_names = clone_string_array(n->struct_def.field_names, n->struct_def.field_count);
            c->struct_def.field_types = clone_string_array(n->struct_def.field_types, n->struct_def.field_count);
            c->struct_def.type_params = clone_string_array(n->struct_def.type_params, n->struct_def.type_param_count);
            break;
        default:
            // Other node kinds (program, enum_def, test_def) are not
            // expected inside a generic template body. Clone them
            // shallowly; if a future feature lands one in a template,
            // this is the place to extend.
            break;
    }
    return c;
}

// Walk an AST subtree and substitute type-parameter occurrences in every
// type-expression string we own.
static void substitute_in_node(Node *n, char **params, char **concrete, int count);

static void substitute_in_array(Node **arr, int len, char **params,
                                char **concrete, int count) {
    if (!arr) return;
    for (int i = 0; i < len; i++) substitute_in_node(arr[i], params, concrete, count);
}

static char *sub(const char *old_, char **params, char **concrete, int count) {
    if (!old_) return NULL;
    return substitute_type(old_, params, concrete, count);
}

static void substitute_in_node(Node *n, char **params, char **concrete, int count) {
    if (!n) return;
    switch (n->type) {
        case NODE_VAR_DECL: {
            // No-free policy: the slot may point at a shared lexer
            // value or at a parser-baked literal ("task"/"list"/"map").
            // Replace pointers but never free them.
            char *t;
            t = sub(n->var_decl.type_name, params, concrete, count);
            if (t) { n->var_decl.type_name = t; }
            t = sub(n->var_decl.elem_type_name, params, concrete, count);
            if (t) { n->var_decl.elem_type_name = t; }
            t = sub(n->var_decl.key_type_name, params, concrete, count);
            if (t) { n->var_decl.key_type_name = t; }
            t = sub(n->var_decl.val_type_name, params, concrete, count);
            if (t) { n->var_decl.val_type_name = t; }
            substitute_in_node(n->var_decl.value, params, concrete, count);
            break;
        }
        case NODE_ASSIGN:
            substitute_in_node(n->assign.value, params, concrete, count);
            break;
        case NODE_FIELD_ASSIGN:
            substitute_in_node(n->field_assign.object, params, concrete, count);
            substitute_in_node(n->field_assign.value, params, concrete, count);
            break;
        case NODE_FIELD_ACCESS:
            substitute_in_node(n->field_access.object, params, concrete, count);
            break;
        case NODE_BINARY:
            substitute_in_node(n->binary.left, params, concrete, count);
            substitute_in_node(n->binary.right, params, concrete, count);
            break;
        case NODE_UNARY:
            substitute_in_node(n->unary.operand, params, concrete, count);
            break;
        case NODE_CALL: {
            // Construction of an instantiated struct: `Box(...)` becomes `Box<int>(...)`
            // already during type-name substitution above, but the call.name is a
            // separate string that may itself name a type parameter (e.g. `T()`).
            // No-free: the call.name may be a lexer-shared pointer.
            char *t = sub(n->call.name, params, concrete, count);
            if (t) { n->call.name = t; }
            substitute_in_array(n->call.args, n->call.arg_count, params, concrete, count);
            break;
        }
        case NODE_METHOD_CALL:
            substitute_in_node(n->method_call.object, params, concrete, count);
            substitute_in_array(n->method_call.args, n->method_call.arg_count, params, concrete, count);
            break;
        case NODE_INDEX:
            substitute_in_node(n->index_access.object, params, concrete, count);
            substitute_in_node(n->index_access.index, params, concrete, count);
            break;
        case NODE_LIST_LIT:
            substitute_in_array(n->list_lit.items, n->list_lit.count, params, concrete, count);
            break;
        case NODE_MAP_LIT:
            substitute_in_array(n->map_lit.keys, n->map_lit.count, params, concrete, count);
            substitute_in_array(n->map_lit.values, n->map_lit.count, params, concrete, count);
            break;
        case NODE_IF:
            substitute_in_array(n->if_stmt.conds, n->if_stmt.branch_count, params, concrete, count);
            substitute_in_array(n->if_stmt.bodies, n->if_stmt.branch_count, params, concrete, count);
            substitute_in_node(n->if_stmt.nah_body, params, concrete, count);
            break;
        case NODE_THROUGH_RANGE:
            substitute_in_node(n->through_range.from, params, concrete, count);
            substitute_in_node(n->through_range.to, params, concrete, count);
            substitute_in_node(n->through_range.by, params, concrete, count);
            substitute_in_node(n->through_range.body, params, concrete, count);
            break;
        case NODE_THROUGH_IN:
            substitute_in_node(n->through_in.collection, params, concrete, count);
            substitute_in_node(n->through_in.body, params, concrete, count);
            break;
        case NODE_INFI:
            substitute_in_node(n->infi.cond, params, concrete, count);
            substitute_in_node(n->infi.body, params, concrete, count);
            break;
        case NODE_GIVE:
            substitute_in_node(n->give.value, params, concrete, count);
            break;
        case NODE_BLOCK:
            substitute_in_array(n->block.stmts.items, n->block.stmts.count, params, concrete, count);
            break;
        case NODE_MATCH:
            substitute_in_node(n->match_expr.expr, params, concrete, count);
            substitute_in_array(n->match_expr.arm_bodies, n->match_expr.arm_count, params, concrete, count);
            break;
        case NODE_ASSERT:
            substitute_in_node(n->assert_stmt.condition, params, concrete, count);
            break;
        default:
            break;
    }
}

// ---------- mangling pass over the program ----------
// Walks every type-expression string in the program and rewrites
// parametric forms (Box<int>) to mangled symbol-safe forms (Box__int).

static void mangle_string_in_place(char **slot) {
    if (!slot || !*slot || !is_parametric(*slot)) return;
    // Same no-free policy as normalize_string_in_place: type-name
    // string ownership is heterogeneous in the AST, so replace the
    // slot but leave the old pointer where it is.
    *slot = mangle_type(*slot);
}

static void mangle_in_node(Node *n);

static void mangle_in_array(Node **arr, int len) {
    if (!arr) return;
    for (int i = 0; i < len; i++) mangle_in_node(arr[i]);
}

static void mangle_in_node(Node *n) {
    if (!n) return;
    switch (n->type) {
        case NODE_VAR_DECL:
            mangle_string_in_place(&n->var_decl.type_name);
            mangle_string_in_place(&n->var_decl.elem_type_name);
            mangle_string_in_place(&n->var_decl.key_type_name);
            mangle_string_in_place(&n->var_decl.val_type_name);
            mangle_in_node(n->var_decl.value);
            break;
        case NODE_ASSIGN:
            mangle_in_node(n->assign.value);
            break;
        case NODE_FIELD_ASSIGN:
            mangle_in_node(n->field_assign.object);
            mangle_in_node(n->field_assign.value);
            break;
        case NODE_FIELD_ACCESS:
            mangle_in_node(n->field_access.object);
            break;
        case NODE_BINARY:
            mangle_in_node(n->binary.left);
            mangle_in_node(n->binary.right);
            break;
        case NODE_UNARY:
            mangle_in_node(n->unary.operand);
            break;
        case NODE_CALL:
            mangle_string_in_place(&n->call.name);
            mangle_in_array(n->call.args, n->call.arg_count);
            break;
        case NODE_METHOD_CALL:
            mangle_in_node(n->method_call.object);
            mangle_in_array(n->method_call.args, n->method_call.arg_count);
            break;
        case NODE_INDEX:
            mangle_in_node(n->index_access.object);
            mangle_in_node(n->index_access.index);
            break;
        case NODE_LIST_LIT:
            mangle_in_array(n->list_lit.items, n->list_lit.count);
            break;
        case NODE_MAP_LIT:
            mangle_in_array(n->map_lit.keys, n->map_lit.count);
            mangle_in_array(n->map_lit.values, n->map_lit.count);
            break;
        case NODE_IF:
            mangle_in_array(n->if_stmt.conds, n->if_stmt.branch_count);
            mangle_in_array(n->if_stmt.bodies, n->if_stmt.branch_count);
            mangle_in_node(n->if_stmt.nah_body);
            break;
        case NODE_THROUGH_RANGE:
            mangle_in_node(n->through_range.from);
            mangle_in_node(n->through_range.to);
            mangle_in_node(n->through_range.by);
            mangle_in_node(n->through_range.body);
            break;
        case NODE_THROUGH_IN:
            mangle_in_node(n->through_in.collection);
            mangle_in_node(n->through_in.body);
            break;
        case NODE_INFI:
            mangle_in_node(n->infi.cond);
            mangle_in_node(n->infi.body);
            break;
        case NODE_GIVE:
            mangle_in_node(n->give.value);
            break;
        case NODE_BLOCK:
            mangle_in_array(n->block.stmts.items, n->block.stmts.count);
            break;
        case NODE_MATCH:
            mangle_in_node(n->match_expr.expr);
            mangle_in_array(n->match_expr.arm_bodies, n->match_expr.arm_count);
            break;
        case NODE_ASSERT:
            mangle_in_node(n->assert_stmt.condition);
            break;
        default:
            break;
    }
}

// ---------- collecting parametric type-name occurrences ----------
//
// While walking, accumulate every distinct parametric form we see so we
// know what concrete instantiations to materialise.

typedef struct {
    char **items;
    int count;
    int cap;
} StrSet;

static int strset_contains(StrSet *s, const char *v) {
    for (int i = 0; i < s->count; i++)
        if (!strcmp(s->items[i], v)) return 1;
    return 0;
}

static void strset_add(StrSet *s, const char *v) {
    if (strset_contains(s, v)) return;
    if (s->count >= s->cap) {
        s->cap = s->cap ? s->cap * 2 : 8;
        s->items = realloc(s->items, s->cap * sizeof(char *));
    }
    s->items[s->count++] = xstrdup(v);
}

static void collect_from_string(StrSet *seen, const char *s) {
    if (!s || !is_parametric(s)) return;
    strset_add(seen, s);
}

static void collect_in_node(Node *n, StrSet *seen);

static void collect_in_array(Node **arr, int len, StrSet *seen) {
    if (!arr) return;
    for (int i = 0; i < len; i++) collect_in_node(arr[i], seen);
}

static void collect_in_node(Node *n, StrSet *seen) {
    if (!n) return;
    switch (n->type) {
        case NODE_VAR_DECL:
            collect_from_string(seen, n->var_decl.type_name);
            collect_from_string(seen, n->var_decl.elem_type_name);
            collect_from_string(seen, n->var_decl.key_type_name);
            collect_from_string(seen, n->var_decl.val_type_name);
            collect_in_node(n->var_decl.value, seen);
            break;
        case NODE_ASSIGN:
            collect_in_node(n->assign.value, seen);
            break;
        case NODE_FIELD_ASSIGN:
            collect_in_node(n->field_assign.object, seen);
            collect_in_node(n->field_assign.value, seen);
            break;
        case NODE_FIELD_ACCESS:
            collect_in_node(n->field_access.object, seen);
            break;
        case NODE_BINARY:
            collect_in_node(n->binary.left, seen);
            collect_in_node(n->binary.right, seen);
            break;
        case NODE_UNARY:
            collect_in_node(n->unary.operand, seen);
            break;
        case NODE_CALL:
            collect_from_string(seen, n->call.name);
            collect_in_array(n->call.args, n->call.arg_count, seen);
            break;
        case NODE_METHOD_CALL:
            collect_in_node(n->method_call.object, seen);
            collect_in_array(n->method_call.args, n->method_call.arg_count, seen);
            break;
        case NODE_INDEX:
            collect_in_node(n->index_access.object, seen);
            collect_in_node(n->index_access.index, seen);
            break;
        case NODE_LIST_LIT:
            collect_in_array(n->list_lit.items, n->list_lit.count, seen);
            break;
        case NODE_MAP_LIT:
            collect_in_array(n->map_lit.keys, n->map_lit.count, seen);
            collect_in_array(n->map_lit.values, n->map_lit.count, seen);
            break;
        case NODE_IF:
            collect_in_array(n->if_stmt.conds, n->if_stmt.branch_count, seen);
            collect_in_array(n->if_stmt.bodies, n->if_stmt.branch_count, seen);
            collect_in_node(n->if_stmt.nah_body, seen);
            break;
        case NODE_THROUGH_RANGE:
            collect_in_node(n->through_range.from, seen);
            collect_in_node(n->through_range.to, seen);
            collect_in_node(n->through_range.by, seen);
            collect_in_node(n->through_range.body, seen);
            break;
        case NODE_THROUGH_IN:
            collect_in_node(n->through_in.collection, seen);
            collect_in_node(n->through_in.body, seen);
            break;
        case NODE_INFI:
            collect_in_node(n->infi.cond, seen);
            collect_in_node(n->infi.body, seen);
            break;
        case NODE_GIVE:
            collect_in_node(n->give.value, seen);
            break;
        case NODE_BLOCK:
            collect_in_array(n->block.stmts.items, n->block.stmts.count, seen);
            break;
        case NODE_MATCH:
            collect_in_node(n->match_expr.expr, seen);
            collect_in_array(n->match_expr.arm_bodies, n->match_expr.arm_count, seen);
            break;
        case NODE_ASSERT:
            collect_in_node(n->assert_stmt.condition, seen);
            break;
        default:
            break;
    }
}

static void collect_in_func(Node *f, StrSet *seen) {
    for (int i = 0; i < f->func_def.param_count; i++)
        collect_from_string(seen, f->func_def.param_types[i]);
    collect_from_string(seen, f->func_def.return_type);
    collect_in_node(f->func_def.body, seen);
}

static void collect_in_struct(Node *s, StrSet *seen) {
    for (int i = 0; i < s->struct_def.field_count; i++)
        collect_from_string(seen, s->struct_def.field_types[i]);
}

// ---------- normalize: strip whitespace from every type-name string ----------

static void normalize_in_node(Node *n);

static void normalize_string_in_place(char **slot) {
    if (!slot || !*slot) return;
    // Only rewrite when there is actually whitespace to strip. Beyond
    // saving work, this matters because many type-name string slots in
    // the AST point either at a lexer-owned token value (shared across
    // multiple AST nodes) or at a literal `"task"`/`"list"`/`"map"`
    // baked into the parser. Replacing them is fine, but freeing them
    // would either corrupt the lexer's token table or trip a
    // free-on-literal undefined-behaviour. Compiler runs are short
    // and one-shot, so we orphan the old pointer rather than free it.
    int has_ws = 0;
    for (const char *p = *slot; *p; p++) {
        if (*p == ' ' || *p == '\t') { has_ws = 1; break; }
    }
    if (!has_ws) return;
    *slot = normalize_type(*slot);
}

static void normalize_in_array(Node **arr, int len) {
    if (!arr) return;
    for (int i = 0; i < len; i++) normalize_in_node(arr[i]);
}

static void normalize_in_node(Node *n) {
    if (!n) return;
    switch (n->type) {
        case NODE_VAR_DECL:
            normalize_string_in_place(&n->var_decl.type_name);
            normalize_string_in_place(&n->var_decl.elem_type_name);
            normalize_string_in_place(&n->var_decl.key_type_name);
            normalize_string_in_place(&n->var_decl.val_type_name);
            normalize_in_node(n->var_decl.value);
            break;
        case NODE_ASSIGN: normalize_in_node(n->assign.value); break;
        case NODE_FIELD_ASSIGN:
            normalize_in_node(n->field_assign.object);
            normalize_in_node(n->field_assign.value);
            break;
        case NODE_FIELD_ACCESS: normalize_in_node(n->field_access.object); break;
        case NODE_BINARY:
            normalize_in_node(n->binary.left);
            normalize_in_node(n->binary.right);
            break;
        case NODE_UNARY: normalize_in_node(n->unary.operand); break;
        case NODE_CALL:
            normalize_string_in_place(&n->call.name);
            normalize_in_array(n->call.args, n->call.arg_count);
            break;
        case NODE_METHOD_CALL:
            normalize_in_node(n->method_call.object);
            normalize_in_array(n->method_call.args, n->method_call.arg_count);
            break;
        case NODE_INDEX:
            normalize_in_node(n->index_access.object);
            normalize_in_node(n->index_access.index);
            break;
        case NODE_LIST_LIT: normalize_in_array(n->list_lit.items, n->list_lit.count); break;
        case NODE_MAP_LIT:
            normalize_in_array(n->map_lit.keys, n->map_lit.count);
            normalize_in_array(n->map_lit.values, n->map_lit.count);
            break;
        case NODE_IF:
            normalize_in_array(n->if_stmt.conds, n->if_stmt.branch_count);
            normalize_in_array(n->if_stmt.bodies, n->if_stmt.branch_count);
            normalize_in_node(n->if_stmt.nah_body);
            break;
        case NODE_THROUGH_RANGE:
            normalize_in_node(n->through_range.from);
            normalize_in_node(n->through_range.to);
            normalize_in_node(n->through_range.by);
            normalize_in_node(n->through_range.body);
            break;
        case NODE_THROUGH_IN:
            normalize_in_node(n->through_in.collection);
            normalize_in_node(n->through_in.body);
            break;
        case NODE_INFI:
            normalize_in_node(n->infi.cond);
            normalize_in_node(n->infi.body);
            break;
        case NODE_GIVE: normalize_in_node(n->give.value); break;
        case NODE_BLOCK: normalize_in_array(n->block.stmts.items, n->block.stmts.count); break;
        case NODE_MATCH:
            normalize_in_node(n->match_expr.expr);
            normalize_in_array(n->match_expr.arm_bodies, n->match_expr.arm_count);
            break;
        case NODE_ASSERT: normalize_in_node(n->assert_stmt.condition); break;
        default: break;
    }
}

static void normalize_program(Node *program) {
    for (int i = 0; i < program->program.structs.count; i++) {
        Node *s = program->program.structs.items[i];
        for (int j = 0; j < s->struct_def.field_count; j++)
            normalize_string_in_place(&s->struct_def.field_types[j]);
    }
    for (int i = 0; i < program->program.funcs.count; i++) {
        Node *f = program->program.funcs.items[i];
        for (int j = 0; j < f->func_def.param_count; j++)
            normalize_string_in_place(&f->func_def.param_types[j]);
        normalize_string_in_place(&f->func_def.return_type);
        normalize_string_in_place(&f->func_def.receiver_type);
        normalize_in_node(f->func_def.body);
    }
    for (int i = 0; i < program->program.tests.count; i++)
        normalize_in_node(program->program.tests.items[i]->test_def.body);
}

// ---------- main pass ----------

// Normalise every type-name string in the entire program. After this,
// `Pair<str, int>` and `Pair<str,int>` always resolve to the same key.
static void normalize_program(Node *program);

void monomorph_run(Node *program) {
    // 0. Strip whitespace from every type-name string in the program so
    //    `Pair<str, int>` (source spelling) and `Pair<str,int>` (parser
    //    output) are not mistaken for two distinct instantiations.
    normalize_program(program);

    // 1. Split program into generic templates vs. concrete code.
    //    Generic templates are removed from the program (they'd be
    //    invalid input to the checker anyway).
    Node *struct_templates[64];
    int struct_template_count = 0;
    Node *func_templates[256];
    int func_template_count = 0;

    NodeList kept_structs = {0};
    NodeList kept_funcs = {0};

    for (int i = 0; i < program->program.structs.count; i++) {
        Node *s = program->program.structs.items[i];
        if (s->struct_def.type_param_count > 0) {
            if (struct_template_count >= 64) {
                fprintf(stderr, "error: too many generic struct templates (max 64)\n");
                exit(1);
            }
            struct_templates[struct_template_count++] = s;
        } else {
            if (kept_structs.count >= kept_structs.cap) {
                kept_structs.cap = kept_structs.cap ? kept_structs.cap * 2 : 8;
                kept_structs.items = realloc(kept_structs.items, kept_structs.cap * sizeof(Node *));
            }
            kept_structs.items[kept_structs.count++] = s;
        }
    }
    for (int i = 0; i < program->program.funcs.count; i++) {
        Node *f = program->program.funcs.items[i];
        if (f->func_def.receiver_type_arg_count > 0) {
            if (func_template_count >= 256) {
                fprintf(stderr, "error: too many generic method templates (max 256)\n");
                exit(1);
            }
            func_templates[func_template_count++] = f;
        } else {
            if (kept_funcs.count >= kept_funcs.cap) {
                kept_funcs.cap = kept_funcs.cap ? kept_funcs.cap * 2 : 8;
                kept_funcs.items = realloc(kept_funcs.items, kept_funcs.cap * sizeof(Node *));
            }
            kept_funcs.items[kept_funcs.count++] = f;
        }
    }
    program->program.structs = kept_structs;
    program->program.funcs = kept_funcs;

    // 2. Worklist: every parametric form discovered anywhere in the
    //    program. Start with the existing concrete code; instantiations
    //    we materialise may introduce more.
    //
    //    We always run the discovery + validation pass even if there
    //    are no templates, so a stray `Foo<int>()` in source produces
    //    a clear monomorph-level error instead of falling through to
    //    a generic "unknown function" message from the checker.
    StrSet seen = {0};
    StrSet processed = {0};

    for (int i = 0; i < program->program.structs.count; i++)
        collect_in_struct(program->program.structs.items[i], &seen);
    for (int i = 0; i < program->program.funcs.count; i++)
        collect_in_func(program->program.funcs.items[i], &seen);
    for (int i = 0; i < program->program.tests.count; i++)
        collect_in_node(program->program.tests.items[i]->test_def.body, &seen);

    // 3. For each unprocessed parametric form, find its template, clone,
    //    substitute, append. New parametric forms inside the clone go
    //    back onto the worklist.
    while (1) {
        // Pick a seen form that hasn't been processed yet.
        const char *form = NULL;
        for (int i = 0; i < seen.count; i++) {
            if (!strset_contains(&processed, seen.items[i])) {
                form = seen.items[i];
                break;
            }
        }
        if (!form) break;

        // Mark processed early so the loop terminates even if
        // instantiation fails to add new things.
        strset_add(&processed, form);

        // Decompose: head + args.
        char head[128];
        char **args = NULL;
        int arg_count = parse_parametric(form, head, sizeof(head), &args);
        if (arg_count == 0) {
            // Defensive: a non-parametric form snuck onto the seen set.
            // That should be impossible because is_parametric() gates
            // every collect_from_string() insertion.
            continue;
        }

        // Find matching struct template.
        Node *st = NULL;
        for (int i = 0; i < struct_template_count; i++) {
            if (!strcmp(struct_templates[i]->struct_def.name, head)) { st = struct_templates[i]; break; }
        }
        if (!st) {
            // `array<T>` is a built-in type form (Phase α). The
            // checker handles it as TYPE_ARRAY; the monomorphizer
            // shouldn't try to find a user template for it.
            if (!strcmp(head, "array")) {
                continue;
            }
            fprintf(stderr, "error: cannot instantiate '%s' — no generic type named '%s' is in scope\n",
                form, head);
            exit(1);
        }
        if (st->struct_def.type_param_count != arg_count) {
            fprintf(stderr, "error: '%s' provides %d type argument(s), but template '%s' expects %d\n",
                form, arg_count, head, st->struct_def.type_param_count);
            exit(1);
        }

        // Clone the struct template and substitute type-params.
        Node *clone = clone_node(st);
        // Rewrite the clone's name to its mangled form so the
        // resulting symbol lines up with the rest of the program
        // after the global mangling pass below. clone_node already
        // xstrdup'd the name, so the clone's slot owns its memory and
        // a free here is safe — but we follow the no-free convention
        // for consistency with the rest of the pass.
        clone->struct_def.name = mangle_type(form);
        clone->struct_def.type_param_count = 0;
        // Substitute type parameters in every field type of the clone.
        // The clone's field_types entries were xstrdup'd so we own
        // them, but the no-free convention applies uniformly: we
        // overwrite without freeing.
        for (int i = 0; i < clone->struct_def.field_count; i++) {
            clone->struct_def.field_types[i] = substitute_type(
                clone->struct_def.field_types[i],
                st->struct_def.type_params, args,
                st->struct_def.type_param_count);
        }
        // Each new field type may itself be parametric — surface those
        // into the worklist.
        for (int i = 0; i < clone->struct_def.field_count; i++)
            collect_from_string(&seen, clone->struct_def.field_types[i]);

        // Append to program.
        if (program->program.structs.count >= program->program.structs.cap) {
            program->program.structs.cap = program->program.structs.cap ? program->program.structs.cap * 2 : 8;
            program->program.structs.items = realloc(program->program.structs.items,
                program->program.structs.cap * sizeof(Node *));
        }
        program->program.structs.items[program->program.structs.count++] = clone;

        // For every method template attached to this generic receiver,
        // produce a corresponding monomorphic method on the new struct.
        for (int mi = 0; mi < func_template_count; mi++) {
            Node *mt = func_templates[mi];
            if (!mt->func_def.receiver_type) continue;
            if (strcmp(mt->func_def.receiver_type, head) != 0) continue;
            if (mt->func_def.receiver_type_arg_count != arg_count) {
                fprintf(stderr, "error: method '%s.%s' has %d type parameter(s), instantiation '%s' provides %d\n",
                    head, mt->func_def.name, mt->func_def.receiver_type_arg_count,
                    form, arg_count);
                exit(1);
            }
            Node *mc = clone_node(mt);
            // The method receiver's static type is the mangled form.
            // No-free: keep the no-free convention uniformly.
            mc->func_def.receiver_type = xstrdup(clone->struct_def.name);
            // Drop the receiver-type-args list so the post-monomorph
            // checker treats this as an ordinary method.
            mc->func_def.receiver_type_arg_count = 0;
            // Substitute type params in param types and return type.
            for (int i = 0; i < mc->func_def.param_count; i++) {
                mc->func_def.param_types[i] = substitute_type(
                    mc->func_def.param_types[i],
                    mt->func_def.receiver_type_args, args,
                    mt->func_def.receiver_type_arg_count);
            }
            if (mc->func_def.return_type) {
                mc->func_def.return_type = substitute_type(
                    mc->func_def.return_type,
                    mt->func_def.receiver_type_args, args,
                    mt->func_def.receiver_type_arg_count);
            }
            // Substitute type params inside the body.
            substitute_in_node(mc->func_def.body,
                               mt->func_def.receiver_type_args, args,
                               mt->func_def.receiver_type_arg_count);
            // New parametric forms may appear post-substitution.
            for (int i = 0; i < mc->func_def.param_count; i++)
                collect_from_string(&seen, mc->func_def.param_types[i]);
            collect_from_string(&seen, mc->func_def.return_type);
            collect_in_node(mc->func_def.body, &seen);

            if (program->program.funcs.count >= program->program.funcs.cap) {
                program->program.funcs.cap = program->program.funcs.cap ? program->program.funcs.cap * 2 : 8;
                program->program.funcs.items = realloc(program->program.funcs.items,
                    program->program.funcs.cap * sizeof(Node *));
            }
            program->program.funcs.items[program->program.funcs.count++] = mc;
        }

        for (int i = 0; i < arg_count; i++) free(args[i]);
        free(args);
    }

    // 4. Mangle every parametric type-expression string in the program.
    //    After this point no `<` or `>` should appear in any type name.
    for (int i = 0; i < program->program.structs.count; i++) {
        Node *s = program->program.structs.items[i];
        for (int j = 0; j < s->struct_def.field_count; j++)
            mangle_string_in_place(&s->struct_def.field_types[j]);
    }
    for (int i = 0; i < program->program.funcs.count; i++) {
        Node *f = program->program.funcs.items[i];
        for (int j = 0; j < f->func_def.param_count; j++)
            mangle_string_in_place(&f->func_def.param_types[j]);
        mangle_string_in_place(&f->func_def.return_type);
        if (f->func_def.receiver_type)
            mangle_string_in_place(&f->func_def.receiver_type);
        mangle_in_node(f->func_def.body);
    }
    for (int i = 0; i < program->program.tests.count; i++)
        mangle_in_node(program->program.tests.items[i]->test_def.body);

    // Cleanup of seen / processed sets.
    for (int i = 0; i < seen.count; i++) free(seen.items[i]);
    free(seen.items);
    for (int i = 0; i < processed.count; i++) free(processed.items[i]);
    free(processed.items);
}
