#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "checker.h"

#define MAX_SYMS 256
#define MAX_FUNCS 512

typedef struct {
    char *name;
    Type type;
    int is_ref;     // 1 if param is ref (mutable borrow)
    int is_nomut;   // 1 if declared `nomut x is ...`
    int is_moved;   // 1 if ownership has moved out (`b is now a`)
} Symbol;

typedef struct {
    char *name;
    char **field_names;
    char **field_types;
    int field_count;
} StructInfo;

typedef struct {
    char *name;
    char *receiver_type;        // NULL for free functions
    Type return_type;
    int param_count;
    char **param_types_str;
    int *param_is_ref;
} FuncInfo;

typedef struct {
    Symbol syms[MAX_SYMS];
    int count;
    FuncInfo funcs[MAX_FUNCS];
    int func_count;
    StructInfo *structs;
    int struct_count;
    // Import aliases (e.g. `use std/math` -> "math"); referenced as
    // bare IDENTs in method-call positions, so the checker has to
    // know about them to avoid flagging them as undefined.
    char **import_aliases;
    int import_count;
    // The parsed program root. Needed by the NODE_MATCH path to
    // find enum variant field types when binding match-arm
    // parameters into the symbol table.
    Node *program;
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

// α3: `array of T` typed-storage primitive. Distinct from TYPE_LIST.
static Type make_array_of(Type elem) {
    Type t = {TYPE_ARRAY, NULL, alloc_type(elem), NULL, NULL};
    return t;
}

static int types_equal(Type a, Type b) {
    // γ7: TYPE_STR is no longer produced — `parse_type_str("str")`
    // returns TYPE_STRUCT("String") and `NODE_STR_LIT` resolves to
    // the same. The TYPE_STR arm survives in this comparison as a
    // dead branch (cosmetic; nothing creates a TYPE_STR value).
    int a_is_string = (a.kind == TYPE_STR) ||
        (a.kind == TYPE_STRUCT && a.struct_name && !strcmp(a.struct_name, "String"));
    int b_is_string = (b.kind == TYPE_STR) ||
        (b.kind == TYPE_STRUCT && b.struct_name && !strcmp(b.struct_name, "String"));
    if (a_is_string && b_is_string) return 1;
    // α8: TYPE_BYTE and TYPE_INT are interchangeable at the value
    // level. `arr[i] be 65` writes the int 65 as a byte; reads
    // produce int. The byte-vs-int distinction only affects the
    // element-size of `array of byte` allocation/indexing.
    if ((a.kind == TYPE_INT && b.kind == TYPE_BYTE) ||
        (a.kind == TYPE_BYTE && b.kind == TYPE_INT)) return 1;
    if (a.kind != b.kind) return 0;
    if (a.kind == TYPE_STRUCT) return a.struct_name && b.struct_name && !strcmp(a.struct_name, b.struct_name);
    // α3: TYPE_ARRAY equality requires matching element type.
    if (a.kind == TYPE_ARRAY) {
        if (!a.elem_type || !b.elem_type) return 0;
        return types_equal(*a.elem_type, *b.elem_type);
    }
    return 1;
}

// Render a post-monomorph mangled type name back to its
// user-facing word-style form for diagnostics. The mangled form
// from monomorph uses `__` as the head/arg separator and between
// successive args:
//
//   "Foo"                   -> "Foo"
//   "Option__int"           -> "Option of int"
//   "Result__int__String"   -> "Result of int, String"
//   "List__List__int"       -> "List of List of int"
//
// Output goes into `out` (size `outsz`); always NUL-terminated.
static void format_mangled_word_style(const char *mangled, char *out, int outsz) {
    if (outsz <= 0) return;
    if (!mangled) { out[0] = '\0'; return; }
    int oi = 0;
    int saw_first_sep = 0;
    int i = 0;
    while (mangled[i] && oi + 1 < outsz) {
        // `__` is the separator. The first one becomes ` of `;
        // each subsequent one becomes `, `.
        if (mangled[i] == '_' && mangled[i + 1] == '_') {
            const char *sep = saw_first_sep ? ", " : " of ";
            int sl = (int)strlen(sep);
            for (int k = 0; k < sl && oi + 1 < outsz; k++) out[oi++] = sep[k];
            saw_first_sep = 1;
            i += 2;
            continue;
        }
        out[oi++] = mangled[i++];
    }
    out[oi] = '\0';
}

static void set_sym(Checker *c, const char *name, Type t) {
    for (int i = 0; i < c->count; i++)
        if (!strcmp(c->syms[i].name, name)) {
            c->syms[i].type = t;
            // Reassigning a moved-out symbol re-binds it; clear the
            // moved flag so subsequent reads are valid.
            c->syms[i].is_moved = 0;
            return;
        }
    if (c->count >= MAX_SYMS) { fprintf(stderr, "error: too many variables (max %d)\n", MAX_SYMS); exit(1); }
    c->syms[c->count++] = (Symbol){(char *)name, t, -1, 0, 0}; // -1 = local
}

static void set_sym_with_flags(Checker *c, const char *name, Type t, int is_nomut) {
    for (int i = 0; i < c->count; i++)
        if (!strcmp(c->syms[i].name, name)) {
            c->syms[i].type = t;
            c->syms[i].is_nomut = is_nomut;
            c->syms[i].is_moved = 0;
            return;
        }
    if (c->count >= MAX_SYMS) { fprintf(stderr, "error: too many variables (max %d)\n", MAX_SYMS); exit(1); }
    c->syms[c->count++] = (Symbol){(char *)name, t, -1, is_nomut, 0};
}

static void set_sym_ref(Checker *c, const char *name, Type t, int is_ref) {
    for (int i = 0; i < c->count; i++)
        if (!strcmp(c->syms[i].name, name)) {
            c->syms[i].type = t;
            c->syms[i].is_ref = is_ref;
            return;
        }
    c->syms[c->count++] = (Symbol){(char *)name, t, is_ref, 0, 0};
}

static void mark_moved_sym(Checker *c, const char *name) {
    for (int i = c->count - 1; i >= 0; i--)
        if (!strcmp(c->syms[i].name, name)) { c->syms[i].is_moved = 1; return; }
}

// Reserved for future use — the move check is currently inlined into
// NODE_IDENT's symbol-table walk because the same pass already needs
// to look up the symbol.
__attribute__((unused))
static int is_moved_sym(Checker *c, const char *name) {
    for (int i = c->count - 1; i >= 0; i--)
        if (!strcmp(c->syms[i].name, name)) return c->syms[i].is_moved;
    return 0;
}

static int is_nomut_sym(Checker *c, const char *name) {
    for (int i = c->count - 1; i >= 0; i--)
        if (!strcmp(c->syms[i].name, name)) return c->syms[i].is_nomut;
    return 0;
}

static int get_sym_is_ref(Checker *c, const char *name) {
    for (int i = c->count - 1; i >= 0; i--)
        if (!strcmp(c->syms[i].name, name)) return c->syms[i].is_ref;
    return 0;
}

static Type get_sym(Checker *c, const char *name) {
    for (int i = c->count - 1; i >= 0; i--)
        if (!strcmp(c->syms[i].name, name)) return c->syms[i].type;
    return make_type(TYPE_UNKNOWN);
}

// find_func looks up a *free* function (receiver_type == NULL).
static FuncInfo *find_func(Checker *c, const char *name) {
    for (int i = 0; i < c->func_count; i++)
        if (!c->funcs[i].receiver_type && !strcmp(c->funcs[i].name, name))
            return &c->funcs[i];
    return NULL;
}

// find_method looks up a method on a specific struct/enum receiver type.
// Returns NULL if no such method exists; caller decides what to do
// (try free function, try built-in, etc.).
static FuncInfo *find_method(Checker *c, const char *receiver_type, const char *name) {
    if (!receiver_type) return NULL;
    for (int i = 0; i < c->func_count; i++)
        if (c->funcs[i].receiver_type &&
            !strcmp(c->funcs[i].receiver_type, receiver_type) &&
            !strcmp(c->funcs[i].name, name))
            return &c->funcs[i];
    return NULL;
}

static int is_struct(Checker *c, const char *name) {
    for (int i = 0; i < c->struct_count; i++)
        if (!strcmp(c->structs[i].name, name)) return 1;
    return 0;
}

static int is_enum_type(Checker *c, const char *name) {
    if (!c->program) return 0;
    for (int i = 0; i < c->program->program.enums.count; i++) {
        Node *e = c->program->program.enums.items[i];
        if (!strcmp(e->enum_def.name, name)) return 1;
    }
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
    // γ7: `str` is now an alias for the canonical `String` stdlib
    // struct. The legacy TYPE_STR kind is no longer produced by
    // anything in the checker. Programs that say `s str` and
    // programs that say `s String` produce identical types, both
    // backed by the std/string struct definition.
    if (!strcmp(t, "str")) return make_struct("String");
    if (!strcmp(t, "String")) return make_struct("String");
    if (!strcmp(t, "bool")) return make_type(TYPE_BOOL);
    if (!strcmp(t, "byte")) return make_type(TYPE_BYTE);
    if (!strcmp(t, "void")) return make_type(TYPE_VOID);
    if (!strcmp(t, "list")) return make_type(TYPE_LIST);
    if (!strcmp(t, "map")) return make_type(TYPE_MAP);
    if (!strcmp(t, "imap")) return make_type(TYPE_MAP); // imap is a map variant
    // α3: `array<T>` (legacy <>-bracketed form from
    // parse_type_name) and `array__T` (post-monomorph mangled
    // form) both denote TYPE_ARRAY of the element type. Strip
    // the wrapping prefix and recurse.
    if (!strncmp(t, "array<", 6)) {
        int n = (int)strlen(t);
        if (n > 7 && t[n - 1] == '>') {
            char inner[256];
            int ilen = n - 7;
            if (ilen >= (int)sizeof(inner)) ilen = (int)sizeof(inner) - 1;
            memcpy(inner, t + 6, ilen);
            inner[ilen] = '\0';
            return make_array_of(parse_type_str(c, inner));
        }
    }
    if (!strncmp(t, "array__", 7)) {
        return make_array_of(parse_type_str(c, t + 7));
    }
    if (is_struct(c, t)) {
        Type r = make_struct(t);
        // Populate val_type / key_type for monomorphised stdlib
        // generic types so downstream code (e.g. `through (x in
        // h.items)` element-type lookup) sees the parameter type.
        // Without this, after `parse_type_str("List__Item")` the
        // resulting Type has struct_name="List__Item" but
        // val_type=NULL, and the through-in checker can't bind the
        // loop variable's type. Symptom: `x.name` falls through the
        // untyped-receiver path and gets miscategorized as int.
        if (!strncmp(t, "List__", 6)) {
            r.val_type = alloc_type(parse_type_str(c, t + 6));
        } else if (!strncmp(t, "Map__", 5)) {
            // `Map__K__V` — split at the first __ after the head.
            const char *p = t + 5;
            const char *sep = strstr(p, "__");
            if (sep) {
                int klen = (int)(sep - p);
                char kbuf[256];
                if (klen >= (int)sizeof(kbuf)) klen = (int)sizeof(kbuf) - 1;
                memcpy(kbuf, p, klen);
                kbuf[klen] = '\0';
                r.key_type = alloc_type(parse_type_str(c, kbuf));
                r.val_type = alloc_type(parse_type_str(c, sep + 2));
            }
        }
        return r;
    }
    // Codex audit P0-6: unknown type names must be a hard error.
    // Pre-fix this fell through to TYPE_INT, so misspelled type
    // names silently compiled and behaved as int. After
    // monomorphisation every type-parameter T should already be
    // substituted to a concrete name, so any name we don't
    // recognize at this point is genuinely unknown.
    fprintf(stderr,
        "error: unknown type name '%s' (no struct, enum, or "
        "primitive matches)\n", t);
    exit(1);
}

static const char *type_name(Type t) {
    switch (t.kind) {
        case TYPE_INT: return "int";
        // P2-16: TYPE_STR is fossil; nothing in the source path
        // produces it now. If we ever surface one in an error
        // message, show the user-facing name `String` rather than
        // the retired `str`.
        case TYPE_STR: return "String";
        case TYPE_BOOL: return "bool";
        case TYPE_VOID: return "void";
        case TYPE_LIST: return "list";
        case TYPE_MAP: return "map";
        case TYPE_STRUCT: return t.struct_name ? t.struct_name : "struct";
        case TYPE_TASK: return "task";
        case TYPE_ARRAY: return "array";
        case TYPE_BYTE: return "byte";
        case TYPE_UNKNOWN: return "unknown";
    }
    return "?";
}

static Type check_expr(Checker *c, Node *n);

static Type check_expr(Checker *c, Node *n) {
    switch (n->type) {
        case NODE_INT_LIT: return make_type(TYPE_INT);
        // γ7: literals are now typed as TYPE_STRUCT("String")
        // — the canonical stdlib struct. iremit lays them out
        // accordingly (see iremit_finalize_data).
        case NODE_STR_LIT: return make_struct("String");
        case NODE_BOOL_LIT: return make_type(TYPE_BOOL);
        case NODE_IDENT: {
            // Unknown identifiers used to be caught only by the direct
            // codegen ("error: undefined variable 'x'") — when the IR
            // backend became the default, those errors silently
            // disappeared. Catch them here so both paths reject the
            // same set of programs. Likewise for use-after-move.
            for (int i = c->count - 1; i >= 0; i--)
                if (!strcmp(c->syms[i].name, n->ident.name)) {
                    if (c->syms[i].is_moved) {
                        fprintf(stderr, "error:%d: use of moved variable '%s'\n",
                            n->line, n->ident.name);
                        exit(1);
                    }
                    return c->syms[i].type;
                }
            // Under the new language law, a bare type expression is
            // not a value. Names that resolve to a struct or enum
            // type used as bare values get a teaching error pointing
            // the user at the value-formation forms.
            if (is_struct(c, n->ident.name) || is_enum_type(c, n->ident.name)) {
                char display[256];
                format_mangled_word_style(n->ident.name, display, sizeof(display));
                fprintf(stderr,
                    "error:%d: type expression `%s` is not a value\n",
                    n->line, display);
                if (is_enum_type(c, n->ident.name)) {
                    fprintf(stderr,
                        "  help: enum values are formed with factories; "
                        "use `none of T ()`, `some of T (v)`, "
                        "`ok of T, E (v)`, or `err of T, E (e)`\n");
                } else {
                    fprintf(stderr,
                        "  help: form a value with `%s()` or "
                        "`%s(field is value, ...)`\n",
                        display, display);
                }
                exit(1);
            }
            // Import aliases (e.g. `math` from `use std/math`) are
            // valid in method-call object position; the NODE_METHOD_CALL
            // case dispatches them as `_<alias>_<func>` calls. The bare
            // alias still has no value type, but accept it here so a
            // method call like `math.max(...)` doesn't error before the
            // method-call case can intercept it.
            for (int i = 0; i < c->import_count; i++) {
                if (c->import_aliases[i] && !strcmp(c->import_aliases[i], n->ident.name))
                    return make_type(TYPE_UNKNOWN);
            }
            fprintf(stderr, "error:%d: undefined variable '%s'\n",
                n->line, n->ident.name);
            exit(1);
        }
        case NODE_BINARY: {
            Type left = check_expr(c, n->binary.left);
            Type right = check_expr(c, n->binary.right);
            switch (n->binary.op) {
                case TOK_PLUS: case TOK_ADD_WORD: {
                    // γ4: TYPE_STR and TYPE_STRUCT("String") are
                    // interchangeable for `+` until γ7 deletes the
                    // legacy TYPE_STR. Both lower to `_str_concat`.
                    int left_is_str = (left.kind == TYPE_STR) ||
                        (left.kind == TYPE_STRUCT && left.struct_name &&
                         !strcmp(left.struct_name, "String"));
                    int right_is_str = (right.kind == TYPE_STR) ||
                        (right.kind == TYPE_STRUCT && right.struct_name &&
                         !strcmp(right.struct_name, "String"));
                    if (left_is_str && right_is_str) {
                        n->resolved_type = 2;
                        return make_type(TYPE_STR);
                    }
                    if ((left_is_str && right.kind == TYPE_UNKNOWN) ||
                        (left.kind == TYPE_UNKNOWN && right_is_str)) {
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
                }
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
                {
                    // γ4: until TYPE_STR is deleted (γ7), `String`
                    // (TYPE_STRUCT("String")) and `str` (TYPE_STR)
                    // are interchangeable for binary operators.
                    // Both lower to the same `_str_eq` symbol that
                    // walks the byte buffer through the array-of-byte
                    // header.
                    int left_is_str = (left.kind == TYPE_STR) ||
                        (left.kind == TYPE_STRUCT && left.struct_name &&
                         !strcmp(left.struct_name, "String"));
                    int right_is_str = (right.kind == TYPE_STR) ||
                        (right.kind == TYPE_STRUCT && right.struct_name &&
                         !strcmp(right.struct_name, "String"));
                    if (left_is_str && right_is_str) n->resolved_type = 2;
                    else n->resolved_type = 1;
                    return make_type(TYPE_BOOL);
                }
                case TOK_AND: case TOK_OR:
                    return make_type(TYPE_BOOL);
                default: return make_type(TYPE_UNKNOWN);
            }
        }
        case NODE_UNARY:
            return check_expr(c, n->unary.operand);
        case NODE_CALL: {
            const char *name = n->call.name;
            if (is_struct(c, name)) {
                // Named-field formation: validate every arg's field name
                // exists, every declared field is set exactly once, and
                // each arg's value type matches the field's declared
                // type. Order in source is free.
                if (n->call.arg_names) {
                    StructInfo *si = find_struct(c, name);
                    // Quick sanity check + index lookup table.
                    int *seen = calloc(si->field_count, sizeof(int));
                    for (int i = 0; i < n->call.arg_count; i++) {
                        const char *fname = n->call.arg_names[i];
                        int field_idx = -1;
                        for (int j = 0; j < si->field_count; j++) {
                            if (!strcmp(si->field_names[j], fname)) { field_idx = j; break; }
                        }
                        if (field_idx < 0) {
                            fprintf(stderr,
                                "error:%d: struct '%s' has no field '%s'\n",
                                n->line, name, fname);
                            exit(1);
                        }
                        if (seen[field_idx]) {
                            fprintf(stderr,
                                "error:%d: field '%s' set more than once in '%s' constructor\n",
                                n->line, fname, name);
                            exit(1);
                        }
                        seen[field_idx] = 1;
                        Type arg_t = check_expr(c, n->call.args[i]);
                        Type expected = parse_type_str(c, si->field_types[field_idx]);
                        if (arg_t.kind != TYPE_UNKNOWN && expected.kind != TYPE_UNKNOWN &&
                            !types_equal(arg_t, expected)) {
                            fprintf(stderr,
                                "error:%d: field '%s' of '%s' expects '%s', got '%s'\n",
                                n->line, fname, name, type_name(expected), type_name(arg_t));
                            exit(1);
                        }
                    }
                    for (int j = 0; j < si->field_count; j++) {
                        if (!seen[j]) {
                            fprintf(stderr,
                                "error:%d: field '%s' missing in '%s' constructor (named-arg form requires every field)\n",
                                n->line, si->field_names[j], name);
                            exit(1);
                        }
                    }
                    free(seen);
                } else if (n->call.arg_count > 0) {
                    // Positional struct constructor with args is no
                    // longer supported. Today there's no checker pass
                    // validating it, and silent truncation by field
                    // count is a footgun. Force the named-arg form.
                    fprintf(stderr,
                        "error:%d: positional struct constructors are not supported; use named-arg form '%s(field is value, ...)'\n",
                        n->line, name);
                    exit(1);
                }
                return make_struct(name);
            }
            // Reject named args on anything that isn't a struct
            // constructor — function calls and built-ins are positional.
            if (n->call.arg_names) {
                fprintf(stderr,
                    "error:%d: named arguments are only valid in struct constructors, not in call to '%s'\n",
                    n->line, name);
                exit(1);
            }
            if (!strcmp(name, "list") || !strcmp(name, "list_new")) return make_type(TYPE_LIST);
            if (!strcmp(name, "map") || !strcmp(name, "map_new") || !strcmp(name, "imap")) return make_type(TYPE_MAP);
            if (!strcmp(name, "task")) return make_type(TYPE_TASK);
            // γ3: `assert(cond)` is a stdlib function (conceptually
            // in std/test). Type-check args; irgen lowers it to the
            // same _assert_fail-on-false path NODE_ASSERT used to
            // take. The line-number for the panic message comes
            // from the call node itself.
            if (!strcmp(name, "assert")) {
                if (n->call.arg_count != 1) {
                    fprintf(stderr,
                        "error:%d: assert() takes exactly 1 argument\n",
                        n->line);
                    exit(1);
                }
                check_expr(c, n->call.args[0]);
                return make_type(TYPE_VOID);
            }
            if (!strcmp(name, "yell")) {
                // γ1: resolve `yell(x)` to a concrete symbol at
                // compile time based on x's static type. The call
                // node's name slot is rewritten to the resolved
                // symbol so irgen + iremit just emit `bl <symbol>`.
                //   yell(int)    → yell_int  (built-in)
                //   yell(bool)   → yell_int  (built-in; 0/1)
                //   yell(String) → String_yell (user method via γ2)
                //   yell(T)      → <T>_yell   (user-defined method)
                // No match → error here at type-check time.
                if (n->call.arg_count != 1) {
                    fprintf(stderr,
                        "error:%d: yell() takes exactly 1 argument\n",
                        n->line);
                    exit(1);
                }
                Type arg_t = check_expr(c, n->call.args[0]);
                if (arg_t.kind == TYPE_INT || arg_t.kind == TYPE_BOOL ||
                    arg_t.kind == TYPE_BYTE) {
                    n->call.name = strdup("yell_int");
                } else if (arg_t.kind == TYPE_STR) {
                    n->call.name = strdup("String_yell");
                } else if (arg_t.kind == TYPE_STRUCT) {
                    char buf[256];
                    snprintf(buf, sizeof(buf), "%s_yell", arg_t.struct_name);
                    n->call.name = strdup(buf);
                } else if (arg_t.kind == TYPE_UNKNOWN) {
                    // Legacy / transitional: values flowing out of an
                    // untyped `list` element (no elem type tagged) or
                    // through chained legacy indexing land here. Keep
                    // the runtime magic-number dispatch (`_yell`) for
                    // these — once ε drops the legacy keyword forms
                    // every value carries a concrete type and this
                    // arm becomes the hard-error case below.
                    n->call.name = strdup("yell");
                } else {
                    fprintf(stderr,
                        "error:%d: yell() has no overload for this type\n",
                        n->line);
                    exit(1);
                }
                return make_type(TYPE_VOID);
            }
            if (!strcmp(name, "len")) {
                if (n->call.arg_count != 1) { fprintf(stderr, "error:%d: len() takes 1 argument\n", n->line); exit(1); }
                check_expr(c, n->call.args[0]);
                return make_type(TYPE_INT);
            }
            // γ4: free-function string builtins (str_len, str_eq,
            // str_concat, char_at, int_to_str, yell_str, panic_oob)
            // are no longer dispatched in the checker. User code uses
            // method form: `s.len()`, `s.equals(t)`, `s + t`,
            // `n.to_string()`, `s.char_at(i)`, `s.yell()`. Calls
            // using the old free-function names hit the generic
            // "unknown function" error path.
            // β5: ptr_of / as_string were type-system-only escape
            // hatches needed when std/map stored String values in
            // int slots. With β2/β3's `array of T` rewrite the
            // hatches are no longer needed — typed arrays carry
            // String directly — and the overhaul bans kernel-layer
            // names from being callable in any .ptt file. Removed.
            if (!strcmp(name, "list_set")) { for (int j=0;j<n->call.arg_count;j++) check_expr(c, n->call.args[j]); return make_type(TYPE_VOID); }
            if (!strcmp(name, "imap_set")) { for (int j=0;j<n->call.arg_count;j++) check_expr(c, n->call.args[j]); return make_type(TYPE_VOID); }
            if (!strcmp(name, "imap_get")) { for (int j=0;j<n->call.arg_count;j++) check_expr(c, n->call.args[j]); return make_type(TYPE_INT); }
            if (!strcmp(name, "imap_len")) { if (n->call.arg_count > 0) check_expr(c, n->call.args[0]); return make_type(TYPE_INT); }
            if (!strcmp(name, "map_get")) return make_type(TYPE_INT);
            if (!strcmp(name, "map_keys")) return make_type(TYPE_LIST);
            if (!strcmp(name, "map_set") || !strcmp(name, "map_len") || !strcmp(name, "list_len") || !strcmp(name, "list_push")) return make_type(TYPE_VOID);
            if (!strcmp(name, "list_pop")) return make_type(TYPE_INT);
            // γ4: str_concat / int_to_str dispatch dropped — user
            // code uses `a + b` and `n.to_string()`. Same machinery
            // resolves them to the existing _str_concat / _int_to_str
            // runtime symbols (binary op via NODE_BINARY tag,
            // method via NODE_METHOD_CALL on int receiver).
            // β5: raw memory primitives (heap_alloc, heap_free,
            // mem_load, mem_store, mem_load_byte, mem_store_byte)
            // are NOT user-callable. They were the bootstrap kernel
            // surface that pure-Potato std/list / std/map / std/string
            // used before the `array of T` primitive existed. With
            // α + β1..β4 they're never referenced from any .ptt file.
            // The compiler emits `bl _heap_alloc` / `bl _heap_free` /
            // `bl _panic_oob` directly when lowering language
            // constructs (array allocation, RAII drop, bounds-check
            // failure). User code that calls these names hits the
            // generic "unknown function" error below.
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
            // Check arg types and call-site `ref` markers (Codex P0-2).
            // Param has `ref` ⇒ call site MUST spell `ref`.
            // Param does NOT have `ref` ⇒ call site MUST NOT spell `ref`.
            // The rule keeps mutation visible at the call site.
            for (int j = 0; j < n->call.arg_count; j++) {
                Type arg_t = check_expr(c, n->call.args[j]);
                Type expected = parse_type_str(c, fi->param_types_str[j]);
                if (arg_t.kind != TYPE_UNKNOWN && expected.kind != TYPE_UNKNOWN && !types_equal(arg_t, expected)) {
                    fprintf(stderr, "error:%d: argument %d of '%s' expects '%s', got '%s'\n",
                        n->line, j + 1, name, type_name(expected), type_name(arg_t));
                    exit(1);
                }
                int param_ref = fi->param_is_ref ? fi->param_is_ref[j] : 0;
                int call_ref  = n->call.arg_is_ref ? n->call.arg_is_ref[j] : 0;
                if (param_ref && !call_ref) {
                    fprintf(stderr,
                        "error:%d: argument %d of '%s' is `ref` — "
                        "spell `ref` at the call site so the mutation "
                        "is visible\n",
                        n->line, j + 1, name);
                    exit(1);
                }
                if (!param_ref && call_ref) {
                    fprintf(stderr,
                        "error:%d: argument %d of '%s' is not `ref` — "
                        "remove the `ref` from the call site\n",
                        n->line, j + 1, name);
                    exit(1);
                }
            }
            return fi->return_type;
        }
        case NODE_METHOD_CALL: {
            const char *m = n->method_call.method;

            // Type-receiver method calls. Under the new language
            // law, a type expression is never a value, so user code
            // can't form a value with `Type.variant(...)`.
            //
            // Exception: the stdlib factory bodies (`none`, `some`,
            // `ok`, `err`) construct enum variants this way as
            // their canonical lowering. The checker permits the
            // form there and forwards the AST to irgen, which knows
            // how to emit enum variant materialization for an
            // IDENT-named-receiver method call. Every other source
            // position is rejected with a teaching diagnostic.
            if (n->method_call.object->type == NODE_IDENT) {
                const char *obj_name = n->method_call.object->ident.name;
                Type sym_t = get_sym(c, obj_name);
                // Allow-list for the legacy `Type.variant(...)` AST
                // is set ONLY by the parser, ONLY for nodes parsed
                // from std/option.ptt / std/result.ptt (verified
                // against the compiler's binary dir, not function
                // name). Every other source — including a user file
                // with a free function literally named `none` /
                // `some` / `ok` / `err` — produces nodes with
                // is_stdlib_enum_factory == 0, so the diagnostic
                // path below fires.
                int is_factory_body = n->method_call.is_stdlib_enum_factory;
                if (sym_t.kind == TYPE_UNKNOWN && is_enum_type(c, obj_name)) {
                    if (!is_factory_body) {
                        char display[256];
                        format_mangled_word_style(obj_name, display, sizeof(display));
                        // The space before `.` matches the source-code
                        // shape `Option of int .Some(...)`.
                        const char *dot_sp = strchr(display, ' ') ? " " : "";
                        fprintf(stderr,
                            "error:%d: enum values are formed with factories, not `%s%s.%s(...)`\n",
                            n->line, display, dot_sp, m);
                        fprintf(stderr,
                            "  help: use `none of T ()`, `some of T (v)`, "
                            "`ok of T, E (v)`, or `err of T, E (e)`\n");
                        exit(1);
                    }
                    // Inside a factory body: type-check args (best
                    // effort) and return the enum's named struct type.
                    for (int j = 0; j < n->method_call.arg_count; j++)
                        check_expr(c, n->method_call.args[j]);
                    return make_struct(obj_name);
                }
                if (sym_t.kind == TYPE_UNKNOWN && is_struct(c, obj_name)) {
                    char display[256];
                    format_mangled_word_style(obj_name, display, sizeof(display));
                    fprintf(stderr,
                        "error:%d: type expression `%s` is not a value\n",
                        n->line, display);
                    fprintf(stderr,
                        "  help: form a value with `%s()` or "
                        "`%s(field is value, ...)` first, then call `%s(...)` on it\n",
                        display, display, m);
                    exit(1);
                }
            }

            Type obj_t = check_expr(c, n->method_call.object);

            // Dispatch order:
            //   1. enum constructor (handled above)
            //   2. user method on the receiver's static struct/enum type
            //   3. built-in collection / task methods (push/pop/len/set/get/keys/fire/collapse)
            //   4. last-resort: same-name free function
            // Save whether we knew the receiver type — used for the
            // "no such method" diagnostic if no built-in matches.
            int receiver_was_struct = (obj_t.kind == TYPE_STRUCT && obj_t.struct_name != NULL);
            // P3.2: methods on primitive types (str, int, bool). We dispatch
            // via the same find_method machinery as struct receivers — the
            // receiver-type "name" is just the primitive's spelling.
            //
            // P6.0b: TYPE_STR receivers also look up methods under
            // "String" (the canonical stdlib struct name). This is what
            // lets `s.len()` resolve to `String.len` for any value of
            // type str — same in-memory layout post-P3.4, just two
            // spellings of the same thing.
            const char *receiver_type_name = NULL;
            if (receiver_was_struct) {
                receiver_type_name = obj_t.struct_name;
            } else if (obj_t.kind == TYPE_STR) {
                receiver_type_name = "str";
            } else if (obj_t.kind == TYPE_INT) {
                receiver_type_name = "int";
            } else if (obj_t.kind == TYPE_BOOL) {
                receiver_type_name = "bool";
            } else if (obj_t.kind == TYPE_UNKNOWN) {
                // γ4: legacy untyped collections produce TYPE_UNKNOWN
                // values on indexing. Try int methods first — that's
                // by far the most common case (`list of int`-shaped
                // data). If no int method matches, fall back to str /
                // String the same way TYPE_STR receivers do below.
                receiver_type_name = "int";
            }
            if (receiver_type_name) {
                FuncInfo *user_method = find_method(c, receiver_type_name, m);
                // For TYPE_STR receivers, fall through to the
                // String-named struct's methods if the primitive
                // didn't have a match.
                if (!user_method && obj_t.kind == TYPE_STR) {
                    user_method = find_method(c, "String", m);
                    if (user_method) receiver_type_name = "String";
                }
                // Codex P1-14: when the receiver is String and no
                // method matches, surface the most likely cause —
                // missing `use std/string`. Pre-fix this fell to
                // the legacy `len` built-in fallback (returns int
                // unconditionally) and miscompiled to bare `bl _len`,
                // surfacing as a cryptic linker error.
                if (!user_method &&
                    obj_t.kind == TYPE_STRUCT &&
                    obj_t.struct_name &&
                    !strcmp(obj_t.struct_name, "String")) {
                    fprintf(stderr,
                        "error:%d: String has no method '%s' in scope\n",
                        n->line, m);
                    fprintf(stderr,
                        "  help: add `use std/string` at the top of "
                        "the file (or `use std/basics` for the "
                        "String + List + Map bundle)\n");
                    exit(1);
                }
                if (user_method) {
                    // Implicit `self` is the receiver; user-declared params follow.
                    int expected = user_method->param_count - 1; // minus self
                    if (n->method_call.arg_count != expected) {
                        fprintf(stderr, "error:%d: method '%s.%s' expects %d arguments, got %d\n",
                            n->line, receiver_type_name, m, expected, n->method_call.arg_count);
                        exit(1);
                    }
                    // Codex review: the implicit `self` argument
                    // also needs ref enforcement. If the method
                    // declares `self ref T`, it mutates whatever
                    // storage the receiver expression points at.
                    // Walk the receiver to its root identifier:
                    //
                    //   c.bump()              root = c
                    //   h.counter.bump()      root = h
                    //   xs[0].bump()          root = xs
                    //   make_counter().bump() root = none (transient)
                    //
                    // If the root is a non-ref parameter, the call
                    // mutates caller-owned storage through a
                    // parameter the callee declared read-only.
                    // Reject. If the root is a local or a ref
                    // param, allow. Transient receivers (no IDENT
                    // root) don't alias caller storage; allow.
                    int self_is_ref = user_method->param_is_ref
                        ? user_method->param_is_ref[0] : 0;
                    if (self_is_ref) {
                        // Walk the receiver expression to its
                        // root identifier. Known accessor methods
                        // that return *aliased* caller storage
                        // (List.get, Map.get) are walked through
                        // the same way as field/index access —
                        // their result is a view into the
                        // receiver, so a `ref self` mutation on
                        // the result mutates the receiver's
                        // storage too. (Codex task #142.)
                        //
                        // Future-proofing: a real escape-analysis
                        // pass or a per-method "returns aliased
                        // storage" attribute would replace the
                        // hardcoded list. For today's stdlib the
                        // set is small and explicit; the gap was
                        // empirically demonstrated for `List.get`.
                        Node *root = n->method_call.object;
                        while (root && root->type != NODE_IDENT) {
                            if (root->type == NODE_FIELD_ACCESS) {
                                root = root->field_access.object;
                            } else if (root->type == NODE_INDEX) {
                                root = root->index_access.object;
                            } else if (root->type == NODE_METHOD_CALL &&
                                       root->method_call.method &&
                                       (!strcmp(root->method_call.method, "get"))) {
                                // List.get / Map.get / StringMap.get
                                // return a view into the receiver;
                                // continue walking through the
                                // receiver of `get`.
                                root = root->method_call.object;
                            } else {
                                root = NULL;   // transient — fresh value, no alias
                            }
                        }
                        if (root && root->type == NODE_IDENT) {
                            const char *root_name = root->ident.name;
                            int root_ref = get_sym_is_ref(c, root_name);
                            if (root_ref == 0) {
                                fprintf(stderr,
                                    "error:%d: method '%s.%s' takes "
                                    "`ref self` but the receiver is "
                                    "rooted at non-ref parameter "
                                    "'%s'\n",
                                    n->line, receiver_type_name, m,
                                    root_name);
                                fprintf(stderr,
                                    "  help: declare the parameter "
                                    "as `%s ref ...` so the caller "
                                    "sees the mutation\n",
                                    root_name);
                                exit(1);
                            }
                        }
                    }
                    for (int j = 0; j < n->method_call.arg_count; j++) {
                        Type arg_t = check_expr(c, n->method_call.args[j]);
                        Type expected_t = parse_type_str(c, user_method->param_types_str[j + 1]);
                        if (arg_t.kind != TYPE_UNKNOWN && expected_t.kind != TYPE_UNKNOWN &&
                            !types_equal(arg_t, expected_t)) {
                            fprintf(stderr, "error:%d: argument %d of '%s.%s' expects '%s', got '%s'\n",
                                n->line, j + 1, receiver_type_name, m,
                                type_name(expected_t), type_name(arg_t));
                            exit(1);
                        }
                    }
                    // Tag the call so codegen can emit the mangled symbol.
                    n->method_call.resolved_struct_name = (char *)receiver_type_name;
                    return user_method->return_type;
                }
            }

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
                // γ4: type-check args so any nested method call (e.g.
                // `m.get(i.to_string())`) gets resolved_struct_name set.
                for (int j = 0; j < n->method_call.arg_count; j++)
                    check_expr(c, n->method_call.args[j]);
                if (obj_t.val_type) return *obj_t.val_type;
                return make_type(TYPE_UNKNOWN);
            }
            if (!strcmp(m, "keys")) return make_type(TYPE_LIST);
            if (!strcmp(m, "set") || !strcmp(m, "fire") || !strcmp(m, "collapse")) {
                // γ4: same as get — recurse into args. Without this,
                // `m.set(i.to_string(), i * i)` left the inner method
                // call untagged and irgen emitted bare `bl _to_string`.
                for (int j = 0; j < n->method_call.arg_count; j++)
                    check_expr(c, n->method_call.args[j]);
                return make_type(TYPE_VOID);
            }
            // Import-alias call: `lib.make_list()` where `lib` is
            // an import alias. The imported function was renamed
            // to `<alias>_make_list` during merge; look it up by
            // that prefixed name. Validate arity and argument types
            // the same way direct function calls do — without this,
            // `math.max(1)` (1 arg, expects 2) compiles and reads
            // garbage. Codex audit P0-1.
            if (n->method_call.object->type == NODE_IDENT) {
                const char *obj_name = n->method_call.object->ident.name;
                // Codex P1-11 round 3: error messages prefer the
                // user-written alias over the canonical synthetic
                // (e.g. `math.max` not `m1.max`).
                const char *display_name = n->method_call.alias_display
                    ? n->method_call.alias_display
                    : obj_name;
                int is_alias = 0;
                for (int ai = 0; ai < c->import_count; ai++) {
                    if (c->import_aliases[ai] &&
                        !strcmp(c->import_aliases[ai], obj_name)) {
                        is_alias = 1;
                        break;
                    }
                }
                if (is_alias) {
                    char prefixed[256];
                    snprintf(prefixed, sizeof(prefixed),
                        "%s_%s", obj_name, m);
                    FuncInfo *afi = find_func(c, prefixed);
                    if (!afi) {
                        fprintf(stderr,
                            "error:%d: module '%s' has no function '%s' "
                            "(looked for symbol '%s')\n",
                            n->line, display_name, m, prefixed);
                        exit(1);
                    }
                    if (n->method_call.arg_count != afi->param_count) {
                        fprintf(stderr,
                            "error:%d: function '%s.%s' expects %d "
                            "arguments, got %d\n",
                            n->line, display_name, m,
                            afi->param_count, n->method_call.arg_count);
                        exit(1);
                    }
                    for (int j = 0; j < n->method_call.arg_count; j++) {
                        Type arg_t = check_expr(c, n->method_call.args[j]);
                        Type expected_t = parse_type_str(c,
                            afi->param_types_str[j]);
                        if (arg_t.kind != TYPE_UNKNOWN &&
                            expected_t.kind != TYPE_UNKNOWN &&
                            !types_equal(arg_t, expected_t)) {
                            fprintf(stderr,
                                "error:%d: argument %d of '%s.%s' "
                                "expects '%s', got '%s'\n",
                                n->line, j + 1, display_name, m,
                                type_name(expected_t), type_name(arg_t));
                            exit(1);
                        }
                        // Call-site `ref` enforcement for module-aliased
                        // calls (Codex P0-2). Same rule as direct calls.
                        int param_ref = afi->param_is_ref ? afi->param_is_ref[j] : 0;
                        int call_ref  = n->method_call.arg_is_ref ? n->method_call.arg_is_ref[j] : 0;
                        if (param_ref && !call_ref) {
                            fprintf(stderr,
                                "error:%d: argument %d of '%s.%s' is "
                                "`ref` — spell `ref` at the call site\n",
                                n->line, j + 1, display_name, m);
                            exit(1);
                        }
                        if (!param_ref && call_ref) {
                            fprintf(stderr,
                                "error:%d: argument %d of '%s.%s' is "
                                "not `ref` — remove the `ref` from the "
                                "call site\n",
                                n->line, j + 1, display_name, m);
                            exit(1);
                        }
                    }
                    return afi->return_type;
                }
            }

            // User method via free-function fallback
            FuncInfo *fi = find_func(c, m);
            if (fi) return fi->return_type;
            // If the receiver was a known struct/enum, we already tried to
            // resolve a user method above. None matched, none of the built-in
            // names matched, and there's no free function with this name.
            // Report a clear "no such method" rather than fall through.
            if (receiver_was_struct) {
                fprintf(stderr, "error:%d: type '%s' has no method '%s' (and no built-in or free function named '%s' is in scope)\n",
                    n->line, obj_t.struct_name, m, m);
                exit(1);
            }
            return make_type(TYPE_UNKNOWN);
        }
        case NODE_FIELD_ACCESS: {
            Type obj_t = check_expr(c, n->field_access.object);
            // β1: `arr.cap` for `arr : array of T` returns the
            // capacity at offset 0 of the runtime header. Marked
            // for irgen via a synthetic struct_name "array".
            if (obj_t.kind == TYPE_ARRAY && n->field_access.field &&
                !strcmp(n->field_access.field, "cap")) {
                n->field_access.struct_name = "array";
                return make_type(TYPE_INT);
            }
            if (obj_t.kind == TYPE_STRUCT && obj_t.struct_name) {
                n->field_access.struct_name = obj_t.struct_name;
                if (!struct_has_field(c, obj_t.struct_name, n->field_access.field)) {
                    fprintf(stderr, "error:%d: struct '%s' has no field '%s'\n",
                        n->line, obj_t.struct_name, n->field_access.field);
                    exit(1);
                }
                // Look up the field's declared type so that callers
                // (like the give/return-type check) see the right
                // type. Without this, every field access reports
                // TYPE_INT regardless of the struct's actual field
                // declaration, which silently miscompiled monomorph
                // outputs that store str/bool fields.
                StructInfo *si = find_struct(c, obj_t.struct_name);
                if (si) {
                    for (int i = 0; i < si->field_count; i++) {
                        if (!strcmp(si->field_names[i], n->field_access.field)) {
                            return parse_type_str(c, si->field_types[i]);
                        }
                    }
                }
            } else {
                n->field_access.struct_name = NULL;
                // Untyped receiver: same ambiguity check the direct
                // codegen does at emit time. If the field name maps
                // to two different offsets across the registered
                // structs, refuse to pick one — the user must annotate
                // the receiver.
                int unique_offset = -1;
                int matches = 0;
                const char *first = NULL;
                const char *second = NULL;
                const char *first_field_type = NULL;
                for (int i = 0; i < c->struct_count; i++) {
                    StructInfo *si = &c->structs[i];
                    for (int j = 0; j < si->field_count; j++) {
                        if (si->field_names[j] && !strcmp(si->field_names[j], n->field_access.field)) {
                            int off = j * 8;
                            if (matches == 0) {
                                unique_offset = off;
                                first = si->name;
                                first_field_type = si->field_types[j];
                            } else if (off != unique_offset && !second) {
                                second = si->name;
                            }
                            matches++;
                            break;
                        }
                    }
                }
                if (second) {
                    fprintf(stderr, "error:%d: ambiguous field '%s' on receiver of unknown type "
                        "(it could be '%s.%s' at one offset and '%s.%s' at another). "
                        "Annotate the receiver with an explicit type.\n",
                        n->line, n->field_access.field,
                        first, n->field_access.field,
                        second, n->field_access.field);
                    exit(1);
                }
                // Bug surfaced by spudlock #1: when the receiver's
                // type is unknown but the field name resolves to a
                // unique declared field, return that field's actual
                // type. Previously we returned TYPE_INT here, which
                // caused String fields read from `through (x in
                // list_of_struct)` to be miscategorized as int and
                // then fail later expressions like `x.name + "..."`
                // with a "String + int" type error.
                if (matches >= 1 && first_field_type) {
                    return parse_type_str(c, first_field_type);
                }
            }
            return make_type(TYPE_INT);
        }
        case NODE_INDEX: {
            Type obj_t = check_expr(c, n->index_access.object);
            check_expr(c, n->index_access.index);
            // α5: tag this access for irgen so it picks between the
            // array layout (cap@0, data@8) and the list layout
            // (cap@0, count@8, data@16).
            n->index_access.is_array = (obj_t.kind == TYPE_ARRAY);
            // α8: tag for byte-element arrays so irgen uses ldrb/strb
            // and element-size 1 instead of 8.
            n->index_access.is_byte = (obj_t.kind == TYPE_ARRAY &&
                obj_t.elem_type && obj_t.elem_type->kind == TYPE_BYTE);
            // ε5: stdlib container types (List of T, Map of K, V,
            // StringMap of V) are TYPE_STRUCT in the checker. Post-
            // monomorph their struct name is mangled, e.g.
            // "List__int" or "StringMap__int". Tag the index access
            // so irgen routes `xs[i]` to `<Type>_get(xs, i)` instead
            // of decoding the legacy list header layout. We match
            // by prefix on the mangled name — only the three known
            // stdlib container names get this treatment.
            if (obj_t.kind == TYPE_STRUCT && obj_t.struct_name) {
                const char *sn = obj_t.struct_name;
                if (!strncmp(sn, "List__", 6) || !strcmp(sn, "List") ||
                    !strncmp(sn, "Map__", 5) || !strcmp(sn, "Map")) {
                    n->index_access.method_struct = strdup(sn);
                }
            }
            // Return element type if known
            if (obj_t.elem_type) return *obj_t.elem_type;
            // ε5: for List of T, the element type is the type
            // parameter — read it off val_type when available.
            if (obj_t.kind == TYPE_STRUCT && obj_t.val_type)
                return *obj_t.val_type;
            // ε3 chained-index support: for monomorphised stdlib
            // structs (`List__int`, `Map__int__int`,
            // `StringMap__int`, etc.), infer the element type from
            // the `data` field of the struct definition. Without
            // this, `grid[0][0]` on `List of List of int` types
            // the inner index as TYPE_UNKNOWN and falls back to
            // legacy header decoding.
            if (obj_t.kind == TYPE_STRUCT && obj_t.struct_name) {
                StructInfo *si = find_struct(c, obj_t.struct_name);
                if (si) {
                    // Find the `data` field.
                    for (int fi = 0; fi < si->field_count; fi++) {
                        if (!strcmp(si->field_names[fi], "data") ||
                            !strcmp(si->field_names[fi], "vals")) {
                            const char *t = si->field_types[fi];
                            // Strip "array<" / "array__" prefix to get
                            // the element type spelling.
                            const char *inner = NULL;
                            if (!strncmp(t, "array<", 6) &&
                                t[strlen(t) - 1] == '>') {
                                int len = (int)strlen(t) - 7;
                                char buf[256];
                                if (len > 0 && len < (int)sizeof(buf)) {
                                    memcpy(buf, t + 6, len);
                                    buf[len] = '\0';
                                    return parse_type_str(c, buf);
                                }
                            } else if (!strncmp(t, "array__", 7)) {
                                inner = t + 7;
                                return parse_type_str(c, inner);
                            }
                        }
                    }
                }
            }
            return make_type(TYPE_UNKNOWN);
        }
        case NODE_INDEX_ASSIGN: {
            Type obj_t = check_expr(c, n->index_assign.object);
            check_expr(c, n->index_assign.index);
            Type val_t = check_expr(c, n->index_assign.value);
            n->index_assign.is_array = (obj_t.kind == TYPE_ARRAY);
            n->index_assign.is_byte = (obj_t.kind == TYPE_ARRAY &&
                obj_t.elem_type && obj_t.elem_type->kind == TYPE_BYTE);
            // F-001: when the array element type resolves to a
            // heap-shaped struct declared in this program, stash
            // its name so irgen can drop the previous occupant of
            // the slot before storing a new owner via
            // `arr[i] be now src` / `arr[i] be rep src`. Plain
            // `arr[i] be src` keeps the legacy raw-store semantics
            // (shift/swap loops rely on it).
            if (obj_t.kind == TYPE_ARRAY && obj_t.elem_type &&
                obj_t.elem_type->kind == TYPE_STRUCT &&
                obj_t.elem_type->struct_name &&
                find_struct(c, obj_t.elem_type->struct_name)) {
                n->index_assign.elem_struct_name =
                    strdup(obj_t.elem_type->struct_name);
            }
            // ε5: same struct-method routing as NODE_INDEX.
            if (obj_t.kind == TYPE_STRUCT && obj_t.struct_name) {
                const char *sn = obj_t.struct_name;
                if (!strncmp(sn, "List__", 6) || !strcmp(sn, "List") ||
                    !strncmp(sn, "Map__", 5) || !strcmp(sn, "Map")) {
                    n->index_assign.method_struct = strdup(sn);
                }
            }
            if (getenv("ERBOS_DBG_IDX")) {
                fprintf(stderr, "[IDX_ASSIGN line %d] obj=%s elem=%s val=%s\n",
                    n->line, type_name(obj_t),
                    obj_t.elem_type ? type_name(*obj_t.elem_type) : "null",
                    type_name(val_t));
            }
            // `arr[i] be now src` / `arr[i] be rep src`: the source
            // must be a bare identifier (the parser already enforces
            // this), and for `is_rep` we stash the source's static
            // struct name so irgen can emit `bl _clone_<X>`. Mirrors
            // the NODE_FIELD_ASSIGN handling.
            if (n->index_assign.is_rep && val_t.kind == TYPE_STRUCT &&
                val_t.struct_name && !n->index_assign.src_struct_name) {
                n->index_assign.src_struct_name = strdup(val_t.struct_name);
            }
            if (n->index_assign.is_move && n->index_assign.value->type == NODE_IDENT) {
                // Mark the source local moved so RAII at scope end
                // does not re-free what now lives in the array slot.
                const char *src_name = n->index_assign.value->ident.name;
                for (int si = c->count - 1; si >= 0; si--) {
                    if (!strcmp(c->syms[si].name, src_name)) {
                        c->syms[si].is_moved = 1;
                        break;
                    }
                }
            }
            // Element type compat (best-effort)
            if (obj_t.elem_type && val_t.kind != TYPE_UNKNOWN &&
                obj_t.elem_type->kind != TYPE_UNKNOWN &&
                !types_equal(*obj_t.elem_type, val_t)) {
                fprintf(stderr, "error:%d: cannot assign '%s' to element of '%s'\n",
                    n->line, type_name(val_t), type_name(obj_t));
                exit(1);
            }
            return make_type(TYPE_VOID);
        }
        case NODE_ARRAY_NEW: {
            // α3: `array of T with cap N`. Type-check the cap
            // expression (must be int). Return TYPE_ARRAY of the
            // declared element type.
            Type cap_t = check_expr(c, n->array_new.cap);
            if (cap_t.kind != TYPE_UNKNOWN && cap_t.kind != TYPE_INT) {
                fprintf(stderr, "error:%d: array constructor `cap` must be int, got '%s'\n",
                    n->line, type_name(cap_t));
                exit(1);
            }
            Type elem_t = parse_type_str(c, n->array_new.elem_type);
            return make_array_of(elem_t);
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
            // ε3: when the literal carries an elem_type_name tag
            // (set by the monomorph seed-literals pass when the
            // `List` template is in scope), type the literal as
            // the concrete `List__<elem>` struct so downstream
            // index / iteration / method dispatch picks the
            // stdlib path. Without the tag, fall back to TYPE_LIST.
            if (n->list_lit.elem_type_name) {
                char buf[256];
                snprintf(buf, sizeof(buf), "List__%s",
                    n->list_lit.elem_type_name);
                return make_struct(strdup(buf));
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
            // ε4: when val_type_name is set the literal lowers to
            // a `Map__String__<V>` struct in irgen; type accordingly.
            if (n->map_lit.val_type_name) {
                char buf[256];
                snprintf(buf, sizeof(buf), "Map__String__%s",
                    n->map_lit.val_type_name);
                return make_struct(strdup(buf));
            }
            if (key_t.kind != TYPE_UNKNOWN && val_t.kind != TYPE_UNKNOWN)
                return make_map_of(key_t, val_t);
            return make_type(TYPE_MAP);
        }
        default: return make_type(TYPE_UNKNOWN);
    }
}

static int stmt_returns(Node *stmt);
static int block_returns(Node *block);

// Codex P1-9: must-return analysis. A non-void function must
// return on every control-flow path, not "anywhere a give
// statement exists." The previous heuristic (has_give_in_block)
// accepted programs where give appeared in just one branch of
// an if and the no-return path produced 0.
//
// Returns 1 iff `stmt` guarantees control does not fall through.
static int stmt_returns(Node *stmt) {
    if (!stmt) return 0;
    switch (stmt->type) {
        case NODE_GIVE: return 1;
        case NODE_BLOCK: return block_returns(stmt);
        case NODE_IF: {
            // An if-chain returns iff every branch returns AND
            // there's a nah branch. Without nah, falling out of
            // every condition's else means continuing past the
            // if-chain — so it doesn't return.
            for (int b = 0; b < stmt->if_stmt.branch_count; b++) {
                if (!block_returns(stmt->if_stmt.bodies[b])) return 0;
            }
            if (!stmt->if_stmt.nah_body) return 0;
            return block_returns(stmt->if_stmt.nah_body);
        }
        case NODE_MATCH: {
            // A match returns iff every arm body returns. Match
            // exhaustiveness is a separate audit topic; today we
            // trust that the user covered the variants they care
            // about. A non-exhaustive match falls through on
            // unmatched values, so this is a conservative
            // approximation — an exhaustive match returns iff
            // every arm body returns.
            for (int ai = 0; ai < stmt->match_expr.arm_count; ai++) {
                Node *body = stmt->match_expr.arm_bodies[ai];
                if (!body) return 0;
                if (body->type == NODE_BLOCK) {
                    if (!block_returns(body)) return 0;
                } else {
                    if (!stmt_returns(body)) return 0;
                }
            }
            return stmt->match_expr.arm_count > 0;
        }
        // Loops, asserts, calls — control can fall through.
        // Asserts can panic-exit but the checker treats them as
        // ordinary statements. infi(true) { ... } without stop
        // is provably non-returning but we don't bother with that
        // analysis here; users put `give` after the loop or
        // structure their code so the loop isn't the last stmt.
        default:
            return 0;
    }
}

static int block_returns(Node *block) {
    if (!block) return 0;
    int count = block->block.stmts.count;
    if (count == 0) return 0;
    return stmt_returns(block->block.stmts.items[count - 1]);
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
                (!strcmp(n->var_decl.value->call.name, "map") || !strcmp(n->var_decl.value->call.name, "map_new") ||
                 !strcmp(n->var_decl.value->call.name, "imap"))) {
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
            // Codex audit P0-5: explicit type annotations must
            // constrain the initializer's type. The form
            // `x is int 10` parses with type_name="int" + value 10;
            // `x is int "hi"` should be rejected. The annotation is
            // a constraint, not a hint.
            //
            // Skip for is_now / is_rep / is_nomut because for those
            // forms the parser may set type_name from the parse path
            // (see var-decl auto-construct in parser.c) — that's a
            // different role (carry the source's struct name through
            // to irgen). And skip when the annotation is itself a
            // generic-stdlib name we already overwrote above.
            if (n->var_decl.type_name && !n->var_decl.is_move && !n->var_decl.is_rep) {
                // Parametric type names like "List__int" / "Map__String__int"
                // were already handled above via elem/key/val_type_name.
                // Skip those to avoid a redundant comparison that
                // would error on equivalent-but-non-identical Type
                // shapes.
                int is_parametric_handled =
                    (n->var_decl.elem_type_name &&
                     !strncmp(n->var_decl.type_name, "List", 4)) ||
                    (n->var_decl.key_type_name &&
                     !strncmp(n->var_decl.type_name, "Map", 3));
                if (!is_parametric_handled) {
                    Type ann = parse_type_str(c, n->var_decl.type_name);
                    if (t.kind != TYPE_UNKNOWN && ann.kind != TYPE_UNKNOWN &&
                        !types_equal(ann, t)) {
                        fprintf(stderr,
                            "error:%d: variable '%s' annotated as '%s' "
                            "but initializer has type '%s'\n",
                            n->line, n->var_decl.name,
                            type_name(ann), type_name(t));
                        exit(1);
                    }
                }
            }
            // `b is now a` transfers ownership: mark `a` as moved so any
            // subsequent `a` reference produces a use-after-move error.
            //
            // Borrow gate: a non-ref function parameter is a read-only
            // borrow of caller-owned data. Allowing `is now <param>`
            // would transfer the pointer into a callee-side binding
            // whose RAII would then race the caller's RAII for the
            // same heap block — a guaranteed double-free for nested-
            // generic owned fields (Box of List of int, Option of
            // List of int, etc.). The String case looks fine in
            // practice only because the literal `owned == 0` short-
            // circuit hides the same bug. Reject the move and tell
            // the caller to use `ref` if they actually wanted
            // mutation.
            if (n->var_decl.is_move && n->var_decl.value->type == NODE_IDENT) {
                const char *src = n->var_decl.value->ident.name;
                int src_is_ref = get_sym_is_ref(c, src);
                if (src_is_ref == 0) {
                    fprintf(stderr,
                        "error:%d: cannot move out of borrowed parameter '%s'\n",
                        n->line, src);
                    fprintf(stderr,
                        "  help: declare the parameter as `%s ref ...` "
                        "if the callee should take ownership, or use "
                        "`is rep %s` to deep-clone the borrow\n", src, src);
                    exit(1);
                }
                mark_moved_sym(c, src);
            }
            // F-002: `local is now arr[i]` extracts the slot's pointer
            // and nulls the slot in one step. Stash the element struct
            // name into type_name so irgen heap-tracks the new local
            // with the correct `_drop_<X>` symbol. The slot-null write
            // is emitted by irgen.
            if (n->var_decl.is_move &&
                n->var_decl.value->type == NODE_INDEX &&
                t.kind == TYPE_STRUCT && t.struct_name) {
                n->var_decl.type_name = (char *)t.struct_name;
            }
            // `b is rep a` deep-clones the source. Stash the source's
            // struct name (when known) into type_name so irgen can
            // emit `bl _clone_<StructName>`. Without this, irgen has
            // no way to recover the type of a moved-by-pointer value.
            if (n->var_decl.is_rep && t.kind == TYPE_STRUCT && t.struct_name) {
                n->var_decl.type_name = (char *)t.struct_name;
            }
            // Reject plain `q is p` for heap-shaped values. Without an
            // explicit `now` (move) or `rep` (deep clone), this would
            // produce a silent untracked alias: `q` and `p` would
            // share the same heap pointer, neither marked moved, and
            // when one goes out of scope the other dangles. The
            // language's only safe options for heap-shaped sources
            // are `is now` (transfer ownership) or `is rep` (allocate
            // an independent copy). Primitives (int/bool/byte) copy
            // by value, so plain `is` stays fine for them.
            if (!n->var_decl.is_move && !n->var_decl.is_rep &&
                n->var_decl.value->type == NODE_IDENT &&
                (t.kind == TYPE_STRUCT || t.kind == TYPE_LIST ||
                 t.kind == TYPE_MAP || t.kind == TYPE_ARRAY ||
                 t.kind == TYPE_STR)) {
                const char *src = n->var_decl.value->ident.name;
                fprintf(stderr,
                    "error:%d: ambiguous alias: `%s is %s` for a heap-shaped value\n",
                    n->line, n->var_decl.name, src);
                fprintf(stderr,
                    "  help: use `%s is now %s` to move ownership (source becomes inaccessible)\n",
                    n->var_decl.name, src);
                fprintf(stderr,
                    "  help: use `%s is rep %s` to deep-clone (independent copy)\n",
                    n->var_decl.name, src);
                exit(1);
            }
            // Bind the new variable; carry its `nomut` flag so future
            // assigns can be rejected.
            set_sym_with_flags(c, n->var_decl.name, t, n->var_decl.is_nomut);
            break;
        }
        case NODE_ASSIGN: {
            // Reject assignments to nomut bindings. The direct codegen
            // catches this at emit time; re-check here so the IR
            // backend rejects the same set of programs.
            if (is_nomut_sym(c, n->assign.name)) {
                fprintf(stderr, "error:%d: cannot reassign nomut variable '%s'\n",
                    n->line, n->assign.name);
                exit(1);
            }
            Type existing = get_sym(c, n->assign.name);
            Type new_t = check_expr(c, n->assign.value);
            // Codex P0-7: same heap-alias ban as on var-decl. Plain
            // `q be p` for heap-shaped values is silent untracked
            // aliasing — both bindings would hold the same pointer
            // with no move tracking. Force `now` (move) or `rep`
            // (deep clone). The RHS being a non-IDENT (constructor
            // call, function return, etc.) is OK — those are
            // owned-fresh values.
            int rhs_is_ident = (n->assign.value->type == NODE_IDENT);
            int heap_shaped =
                (new_t.kind == TYPE_STRUCT || new_t.kind == TYPE_LIST ||
                 new_t.kind == TYPE_MAP || new_t.kind == TYPE_ARRAY ||
                 new_t.kind == TYPE_STR);
            if (!n->assign.is_move && !n->assign.is_rep &&
                rhs_is_ident && heap_shaped) {
                fprintf(stderr,
                    "error:%d: ambiguous alias: `%s be %s` for a "
                    "heap-shaped value\n",
                    n->line, n->assign.name, n->assign.value->ident.name);
                fprintf(stderr,
                    "  help: use `%s be now %s` to move ownership "
                    "(source becomes inaccessible)\n",
                    n->assign.name, n->assign.value->ident.name);
                fprintf(stderr,
                    "  help: use `%s be rep %s` to deep-clone "
                    "(independent copy)\n",
                    n->assign.name, n->assign.value->ident.name);
                exit(1);
            }
            // `q be now p` marks p moved.
            if (n->assign.is_move && rhs_is_ident) {
                const char *src = n->assign.value->ident.name;
                int src_is_ref = get_sym_is_ref(c, src);
                if (src_is_ref == 0) {
                    fprintf(stderr,
                        "error:%d: cannot move out of borrowed parameter '%s'\n",
                        n->line, src);
                    fprintf(stderr,
                        "  help: declare the parameter as `%s ref ...` "
                        "if the callee should take ownership, or use "
                        "`be rep %s` to deep-clone the borrow\n", src, src);
                    exit(1);
                }
                mark_moved_sym(c, src);
            }
            // F-010: re-binding via `be now` / `be rep` re-establishes
            // the destination as a live owner, so clear its moved
            // flag. Without this, a previously-moved local that gets
            // reassigned via `dst be now src` is permanently treated
            // as moved by the checker, even though irgen's set_local
            // already clears the runtime moved flag and produces
            // correct code. Mirrors the var-decl re-binding behaviour
            // in set_sym (line 144).
            if (n->assign.is_move || n->assign.is_rep) {
                for (int si = c->count - 1; si >= 0; si--) {
                    if (!strcmp(c->syms[si].name, n->assign.name)) {
                        c->syms[si].is_moved = 0;
                        break;
                    }
                }
            }
            // F-002: `q be now arr[i]` extracts the slot's pointer
            // and nulls the slot. Stash the element struct name so
            // irgen tags the destination's heap-tracking with the
            // correct `_drop_<X>` symbol when re-marking after the
            // move. (Source slot has no symbol-level liveness state.)
            if (n->assign.is_move &&
                n->assign.value->type == NODE_INDEX &&
                new_t.kind == TYPE_STRUCT && new_t.struct_name) {
                n->assign.src_struct_name = (char *)new_t.struct_name;
            }
            // `q be rep p` stashes source struct name for irgen's
            // `_clone_<X>` dispatch.
            if (n->assign.is_rep && new_t.kind == TYPE_STRUCT && new_t.struct_name) {
                n->assign.src_struct_name = (char *)new_t.struct_name;
            }
            if (existing.kind != TYPE_UNKNOWN && new_t.kind != TYPE_UNKNOWN && !types_equal(existing, new_t)) {
                fprintf(stderr, "error:%d: cannot assign '%s' to variable '%s' of type '%s'\n",
                    n->line, type_name(new_t), n->assign.name, type_name(existing));
                exit(1);
            }
            break;
        }
        case NODE_FIELD_ASSIGN: {
            Type obj_t = check_expr(c, n->field_assign.object);
            Type val_t = check_expr(c, n->field_assign.value);
            // `field be now src` transfers ownership of `src` into
            // the field. When the source is a named local, mark the
            // symbol moved so subsequent reads produce a
            // use-after-move error (mirrors `is now`).
            //
            // `field be now obj.field2` (or any non-IDENT RHS) is
            // also accepted: it transfers the pointer but cannot
            // mark a struct field as moved (we only track moves at
            // the symbol level). This is the typical "drain a field
            // into another" pattern used at the end of a function
            // where the source struct is about to be RAII-freed
            // anyway. The cost: the source field is left dangling
            // until the parent goes out of scope; it shouldn't be
            // read.
            //
            // `field be rep src` deep-clones; src isn't marked
            // moved (both bindings remain alive).
            if (n->field_assign.is_move &&
                n->field_assign.value->type == NODE_IDENT) {
                const char *src = n->field_assign.value->ident.name;
                // Same borrow gate as the var-decl `is now`: moving
                // out of a non-ref parameter creates two owners of
                // the same heap block. Reject explicitly.
                int src_is_ref = get_sym_is_ref(c, src);
                if (src_is_ref == 0) {
                    fprintf(stderr,
                        "error:%d: cannot move out of borrowed parameter '%s'\n",
                        n->line, src);
                    fprintf(stderr,
                        "  help: declare the parameter as `%s ref ...` "
                        "if the callee should take ownership, or use "
                        "`be rep %s` to deep-clone the borrow\n", src, src);
                    exit(1);
                }
                mark_moved_sym(c, src);
            }
            // `field be rep src` deep-clones the source. Stash the
            // source's struct name (when known) so irgen can emit
            // `bl _clone_<StructName>`. Mirrors the var-decl `is rep`
            // path.
            if (n->field_assign.is_rep && val_t.kind == TYPE_STRUCT && val_t.struct_name) {
                n->field_assign.src_struct_name = (char *)val_t.struct_name;
            }
            // Tag the assignment with the receiver's struct name when known,
            // so codegen can pick the right field offset deterministically
            // instead of falling through to a name-based global search.
            if (obj_t.kind == TYPE_STRUCT && obj_t.struct_name) {
                n->field_assign.struct_name = (char *)obj_t.struct_name;
                if (!struct_has_field(c, obj_t.struct_name, n->field_assign.field)) {
                    fprintf(stderr, "error:%d: struct '%s' has no field '%s'\n",
                        n->line, obj_t.struct_name, n->field_assign.field);
                    exit(1);
                }
                // Codex audit P0-4: field reassignment must check
                // RHS type against the declared field type. Pre-fix
                // `p.x be "hi"` (int field) compiled silently and
                // miscompiled the int slot to a String pointer.
                //
                // For self-referential structs (`Node.next Node`),
                // declare the field with the proper struct type,
                // not `int`. The "store struct pointers as int"
                // idiom is no longer permitted.
                StructInfo *si = find_struct(c, obj_t.struct_name);
                if (si) {
                    for (int fi = 0; fi < si->field_count; fi++) {
                        if (!strcmp(si->field_names[fi], n->field_assign.field)) {
                            Type expected = parse_type_str(c, si->field_types[fi]);
                            if (val_t.kind != TYPE_UNKNOWN &&
                                expected.kind != TYPE_UNKNOWN &&
                                !types_equal(expected, val_t)) {
                                fprintf(stderr,
                                    "error:%d: field '%s.%s' has type "
                                    "'%s' but assigned value has type "
                                    "'%s'\n",
                                    n->line, obj_t.struct_name,
                                    n->field_assign.field,
                                    type_name(expected),
                                    type_name(val_t));
                                exit(1);
                            }
                            break;
                        }
                    }
                }
            }
            // Codex P0-7: heap alias ban for field-assign. Plain
            // `obj.field be src` for heap-shaped src (when src is a
            // bare ident) is silently moved at irgen — that's a
            // hidden ownership transfer. Force the user to spell
            // `now` (move) or `rep` (deep clone). Constructor calls,
            // function returns, etc. as RHS are exempt — they're
            // owned-fresh values, no aliasing risk.
            int field_rhs_ident = (n->field_assign.value->type == NODE_IDENT);
            int field_heap_shaped =
                (val_t.kind == TYPE_STRUCT || val_t.kind == TYPE_LIST ||
                 val_t.kind == TYPE_MAP || val_t.kind == TYPE_ARRAY ||
                 val_t.kind == TYPE_STR);
            if (!n->field_assign.is_move && !n->field_assign.is_rep &&
                field_rhs_ident && field_heap_shaped) {
                fprintf(stderr,
                    "error:%d: ambiguous alias: `obj.%s be %s` for a "
                    "heap-shaped value\n",
                    n->line, n->field_assign.field,
                    n->field_assign.value->ident.name);
                fprintf(stderr,
                    "  help: use `obj.%s be now %s` to move ownership\n",
                    n->field_assign.field,
                    n->field_assign.value->ident.name);
                fprintf(stderr,
                    "  help: use `obj.%s be rep %s` to deep-clone\n",
                    n->field_assign.field,
                    n->field_assign.value->ident.name);
                exit(1);
            }
            // Enforce ref: if object is a non-ref param, block mutation.
            // Also enforce nomut: a `nomut` binding cannot have its
            // fields directly mutated. Method-call validation lives
            // in the NODE_METHOD_CALL handler above — it checks the
            // ref-ness of the receiver against the method's
            // `self ref T` declaration the same way ordinary args
            // are validated against `param_is_ref`.
            if (n->field_assign.object->type == NODE_IDENT) {
                const char *obj_name = n->field_assign.object->ident.name;
                int ref_status = get_sym_is_ref(c, obj_name);
                Type obj_t2 = get_sym(c, obj_name);
                // ref_status: -1=local(ok), 0=param non-ref(error for structs), 1=param ref(ok)
                if (ref_status == 0 && obj_t2.kind == TYPE_STRUCT) {
                    fprintf(stderr, "error:%d: cannot mutate '%s' — parameter is not ref\n", n->line, obj_name);
                    exit(1);
                }
                if (is_nomut_sym(c, obj_name)) {
                    fprintf(stderr,
                        "error:%d: cannot mutate field '%s' of nomut variable '%s'\n",
                        n->line, n->field_assign.field, obj_name);
                    exit(1);
                }
            }
            break;
        }
        case NODE_INDEX_ASSIGN: {
            // α6: forward to the expression-level case which sets
            // is_array on the node.
            check_expr(c, n);
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
        case NODE_THROUGH_IN: {
            // γ1: thread the collection's element type through to
            // the loop variable when we know it.
            // ε6: stdlib container receivers (TYPE_STRUCT named
            // List / Map / StringMap, possibly mangled) tag the
            // node so irgen routes the iteration through
            // <Type>_len + <Type>_get instead of the legacy header
            // layout. Element type comes from the struct's val_type
            // (set by parse_type_str when the type was generic).
            Type col = check_expr(c, n->through_in.collection);
            Type elem = make_type(TYPE_UNKNOWN);
            if ((col.kind == TYPE_LIST || col.kind == TYPE_ARRAY) &&
                col.elem_type)
                elem = *col.elem_type;
            else if (col.kind == TYPE_STRUCT && col.val_type)
                elem = *col.val_type;
            if (col.kind == TYPE_STRUCT && col.struct_name) {
                const char *sn = col.struct_name;
                if (!strncmp(sn, "List__", 6) || !strcmp(sn, "List") ||
                    !strncmp(sn, "Map__", 5) || !strcmp(sn, "Map")) {
                    n->through_in.method_struct = strdup(sn);
                }
            }
            set_sym(c, n->through_in.var_name, elem);
            for (int j = 0; j < n->through_in.body->block.stmts.count; j++)
                check_stmt(c, n->through_in.body->block.stmts.items[j]);
            break;
        }
        case NODE_INFI:
            if (n->infi.cond) check_expr(c, n->infi.cond);
            for (int j = 0; j < n->infi.body->block.stmts.count; j++)
                check_stmt(c, n->infi.body->block.stmts.items[j]);
            break;
        case NODE_BLOCK: {
            // Codex audit P0-3: NODE_BLOCK must be a lexical scope
            // for the symbol table. Save the symbol count at block
            // entry; restore it on exit. Without this, locals
            // declared inside `{ ... }` remained visible to outer
            // code, contradicting the documented "freed when owner
            // goes out of scope" semantics — and at runtime irgen
            // already freed the heap, so the outer read was a
            // use-after-free hidden behind a successful checker
            // pass.
            int saved = c->count;
            for (int j = 0; j < n->block.stmts.count; j++)
                check_stmt(c, n->block.stmts.items[j]);
            c->count = saved;
            break;
        }
        case NODE_MATCH: {
            // Type-check the matched expression. The match-arm
            // bodies need their binding parameters registered with
            // the right types so subsequent expression-level checks
            // (especially method dispatch like `count.to_string()`)
            // pick the right symbol.
            //
            // Without this case, match arms ran through irgen and
            // emit but never through the checker, so binding-shadow
            // variables had no type and method dispatch fell back
            // to the unknown-receiver path — emitting bare `_to_string`
            // instead of `_int_to_string`. Surfaced by spudlock's
            // report.ptt match against ReportCase.{Success(int),
            // Failure(int, String)}.
            Type matched_t = check_expr(c, n->match_expr.expr);
            // Restrict variant lookup to the matched expression's
            // declared enum type when known. Without this scope,
            // multiple monomorphized enums sharing a variant name
            // (e.g. `Result__int__String` and `Result__String__int`
            // both have `Ok` and `Err`) would collide and pick the
            // first by definition order — binding the wrong payload
            // type to the arm parameter.
            const char *enum_scope = NULL;
            if (matched_t.kind == TYPE_STRUCT && matched_t.struct_name) {
                enum_scope = matched_t.struct_name;
                // Stash on the AST so irgen's tag-lookup uses the
                // same scope (without it, irgen's first-match walk
                // also collides on shared variant names).
                n->match_expr.enum_name = (char *)matched_t.struct_name;
            }
            for (int ai = 0; ai < n->match_expr.arm_count; ai++) {
                const char *vname = n->match_expr.arm_variant_names[ai];
                int saved = c->count;
                // Look up the variant's declared field types from
                // the program's enum table. When `enum_scope` is
                // known, only consult that specific enum; otherwise
                // fall back to the legacy first-match-wins walk so
                // older code without typed receivers still works.
                if (c->program) {
                    for (int ei = 0; ei < c->program->program.enums.count; ei++) {
                        Node *e = c->program->program.enums.items[ei];
                        if (enum_scope && strcmp(e->enum_def.name, enum_scope) != 0)
                            continue;
                        for (int vi = 0; vi < e->enum_def.variant_count; vi++) {
                            if (!vname) continue;
                            if (strcmp(e->enum_def.variant_names[vi], vname) != 0) continue;
                            // Found the variant. Bind each arm
                            // parameter with the declared field
                            // type at that position.
                            int decl_count = e->enum_def.variant_field_counts[vi];
                            int arm_count = n->match_expr.arm_binding_counts[ai];
                            int bind_count = decl_count < arm_count ? decl_count : arm_count;
                            for (int bi = 0; bi < bind_count; bi++) {
                                const char *bn = n->match_expr.arm_bindings[ai][bi];
                                const char *bt = e->enum_def.variant_field_types[vi][bi];
                                set_sym(c, bn, parse_type_str(c, bt));
                            }
                            goto done_arm_bindings;
                        }
                    }
                }
done_arm_bindings:;
                Node *body = n->match_expr.arm_bodies[ai];
                if (body && body->type == NODE_BLOCK) {
                    for (int j = 0; j < body->block.stmts.count; j++)
                        check_stmt(c, body->block.stmts.items[j]);
                } else if (body) {
                    // Single-statement arm: treat as one stmt.
                    check_stmt(c, body);
                }
                // Pop bindings.
                c->count = saved;
            }
            break;
        }
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
                    // Allow int<->struct compatibility for the
                    // legacy null-pointer-as-struct pattern: a
                    // function declared to return a struct can
                    // `give nil` (an int 0), and a function
                    // declared `int` can return a struct (its
                    // pointer value). γ7 narrows the latter to
                    // exclude `String` — returning a String from
                    // an int-typed function is a real type error.
                    int give_is_string = (give_t.kind == TYPE_STRUCT &&
                        give_t.struct_name && !strcmp(give_t.struct_name, "String"));
                    int compat = (give_t.kind == TYPE_INT && c->cur_return_type.kind == TYPE_STRUCT) ||
                                 (give_t.kind == TYPE_STRUCT && c->cur_return_type.kind == TYPE_INT && !give_is_string) ||
                                 (give_t.kind == TYPE_STRUCT && c->cur_return_type.kind == TYPE_STRUCT);
                    if (!compat) {
                        fprintf(stderr, "error:%d: return type mismatch: expected '%s', got '%s'\n",
                            n->line, type_name(c->cur_return_type), type_name(give_t));
                        exit(1);
                    }
                }
            }
            c->found_give = 1;
            break;
        }
        case NODE_CALL:
        case NODE_METHOD_CALL:
            check_expr(c, n);
            break;
        case NODE_ASSERT:
            if (n->assert_stmt.condition) check_expr(c, n->assert_stmt.condition);
            break;
        default:
            break;
    }
}

void checker_run(Node *program) {
    Checker c = {0};
    c.program = program;

    // Register import aliases so identifier checks know about them.
    c.import_aliases = program->program.use_aliases;
    c.import_count = program->program.use_count;

    // Register structs.
    // Type-name convention: every struct / enum name must start
    // with an uppercase letter (PascalCase). The grammar relies
    // on this to disambiguate `Foo()` (struct constructor) from
    // `foo()` (function call) at parse time, and the convention
    // matches every other modern statically-typed language.
    c.struct_count = program->program.structs.count;
    c.structs = malloc(c.struct_count * sizeof(StructInfo));
    for (int i = 0; i < c.struct_count; i++) {
        Node *s = program->program.structs.items[i];
        const char *nm = s->struct_def.name;
        if (nm && nm[0] && !(nm[0] >= 'A' && nm[0] <= 'Z')) {
            char cap = (nm[0] >= 'a' && nm[0] <= 'z')
                ? (char)(nm[0] - 32) : nm[0];
            fprintf(stderr,
                "error:%d: struct name '%s' must start with an "
                "uppercase letter (try '%c%s')\n",
                s->line, nm, cap, nm + 1);
            exit(1);
        }
        c.structs[i].name = s->struct_def.name;
        c.structs[i].field_names = s->struct_def.field_names;
        c.structs[i].field_types = s->struct_def.field_types;
        c.structs[i].field_count = s->struct_def.field_count;
    }

    // Register enum names as known types (treat like structs for
    // type resolution). Same uppercase-first convention.
    for (int i = 0; i < program->program.enums.count; i++) {
        Node *e = program->program.enums.items[i];
        const char *nm = e->enum_def.name;
        if (nm && nm[0] && !(nm[0] >= 'A' && nm[0] <= 'Z')) {
            char cap = (nm[0] >= 'a' && nm[0] <= 'z')
                ? (char)(nm[0] - 32) : nm[0];
            fprintf(stderr,
                "error:%d: enum name '%s' must start with an "
                "uppercase letter (try '%c%s')\n",
                e->line, nm, cap, nm + 1);
            exit(1);
        }
        c.struct_count++;
        c.structs = realloc(c.structs, c.struct_count * sizeof(StructInfo));
        c.structs[c.struct_count - 1].name = (char *)nm;
        c.structs[c.struct_count - 1].field_names = NULL;
        c.structs[c.struct_count - 1].field_types = NULL;
        c.structs[c.struct_count - 1].field_count = 0;
    }

    // Register functions
    if (program->program.funcs.count > MAX_FUNCS) {
        fprintf(stderr, "error: too many functions (%d), max is %d\n",
            program->program.funcs.count, MAX_FUNCS);
        exit(1);
    }
    c.func_count = program->program.funcs.count;

    // Multi-spark check: only one `spark { }` block per
    // compilation unit. Examples/library files should not
    // declare a spark — they're imported, not run directly.
    // Catches both in-source duplicates and accidental
    // double-import of a runnable program.
    {
        Node *first_spark = NULL;
        for (int i = 0; i < c.func_count; i++) {
            Node *f = program->program.funcs.items[i];
            if (!f->func_def.receiver_type &&
                f->func_def.name &&
                !strcmp(f->func_def.name, "spark")) {
                if (first_spark) {
                    fprintf(stderr,
                        "error:%d: multiple 'spark' blocks (only one entry point allowed); first defined at line %d\n",
                        f->line, first_spark->line);
                    exit(1);
                }
                first_spark = f;
            }
        }
    }

    for (int i = 0; i < c.func_count; i++) {
        Node *f = program->program.funcs.items[i];
        c.funcs[i].name = f->func_def.name;
        c.funcs[i].receiver_type = f->func_def.receiver_type;
        c.funcs[i].param_count = f->func_def.param_count;
        c.funcs[i].param_types_str = f->func_def.param_types;
        c.funcs[i].param_is_ref = f->func_def.param_is_ref;

        // Validate method shape: receiver type must exist as a struct,
        // an enum, or a primitive type (str / int / bool — methods on
        // primitives shipped in P3.2). Method must declare at least one
        // parameter (the receiver). First parameter's declared type
        // must match the receiver type.
        if (f->func_def.receiver_type) {
            int is_primitive_recv = (!strcmp(f->func_def.receiver_type, "str") ||
                                     !strcmp(f->func_def.receiver_type, "int") ||
                                     !strcmp(f->func_def.receiver_type, "bool"));
            if (!is_primitive_recv && !is_struct(&c, f->func_def.receiver_type)) {
                fprintf(stderr, "error:%d: method '%s.%s' attached to unknown type '%s'\n",
                    f->line, f->func_def.receiver_type, f->func_def.name,
                    f->func_def.receiver_type);
                exit(1);
            }
            if (f->func_def.param_count < 1) {
                fprintf(stderr, "error:%d: method '%s.%s' must declare a receiver parameter (e.g. 'self %s' or 'self ref %s')\n",
                    f->line, f->func_def.receiver_type, f->func_def.name,
                    f->func_def.receiver_type, f->func_def.receiver_type);
                exit(1);
            }
            const char *first_param_type = f->func_def.param_types[0];
            if (strcmp(first_param_type, f->func_def.receiver_type) != 0) {
                fprintf(stderr, "error:%d: method '%s.%s' receiver parameter must be of type '%s', got '%s'\n",
                    f->line, f->func_def.receiver_type, f->func_def.name,
                    f->func_def.receiver_type, first_param_type);
                exit(1);
            }
        }

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
            int is_ref = f->func_def.param_is_ref ? f->func_def.param_is_ref[j] : 0;
            set_sym_ref(&c, f->func_def.param_names[j], t, is_ref);
        }

        Node *body = f->func_def.body;
        for (int j = 0; j < body->block.stmts.count; j++)
            check_stmt(&c, body->block.stmts.items[j]);

        // Check that every control-flow path returns. Codex P1-9.
        if (c.cur_return_type.kind != TYPE_VOID) {
            if (!block_returns(body)) {
                fprintf(stderr,
                    "error: function '%s' must return '%s' on every "
                    "path; some path falls through without `give`\n",
                    f->func_def.name, type_name(c.cur_return_type));
                exit(1);
            }
        }
    }

    // Check test bodies
    for (int i = 0; i < program->program.tests.count; i++) {
        Node *t = program->program.tests.items[i];
        c.count = 0;
        c.cur_return_type = make_type(TYPE_VOID);
        c.found_give = 0;
        Node *body = t->test_def.body;
        for (int j = 0; j < body->block.stmts.count; j++)
            check_stmt(&c, body->block.stmts.items[j]);
    }
}
