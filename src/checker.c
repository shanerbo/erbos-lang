#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "checker.h"

#define MAX_SYMS 256
#define MAX_FUNCS 128

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
    // P3.4: TYPE_STR and TYPE_STRUCT("String") describe the same
    // in-memory representation (a String header). The language-
    // primitive type spelling and the user-declared struct
    // spelling are interchangeable so existing code that
    // declares `s str` keeps working alongside new code that
    // uses `s String`.
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
        case NODE_STR_LIT: return make_type(TYPE_STR);
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
            // Names that resolve to types (struct/enum) are fine in
            // expression position because they're constructor calls
            // dispatched in NODE_CALL / NODE_METHOD_CALL above. Only
            // bare identifier refs that match no symbol AND no type
            // get reported.
            if (is_struct(c, n->ident.name)) return make_struct(n->ident.name);
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
                Type arg_t = check_expr(c, n->call.args[0]);
                if (arg_t.kind == TYPE_MAP) n->call.args[0]->resolved_type = 4;
                else if (arg_t.kind == TYPE_STR) n->call.args[0]->resolved_type = 5;
                else n->call.args[0]->resolved_type = 3;
                return make_type(TYPE_INT);
            }
            if (!strcmp(name, "str_len")) { if (n->call.arg_count > 0) check_expr(c, n->call.args[0]); return make_type(TYPE_INT); }
            if (!strcmp(name, "str_eq")) { for (int j=0;j<n->call.arg_count;j++) check_expr(c, n->call.args[j]); return make_type(TYPE_INT); }
            if (!strcmp(name, "char_at")) { for (int j=0;j<n->call.arg_count;j++) check_expr(c, n->call.args[j]); return make_type(TYPE_STR); }
            if (!strcmp(name, "yell_str")) { for (int j=0;j<n->call.arg_count;j++) check_expr(c, n->call.args[j]); return make_type(TYPE_VOID); }
            if (!strcmp(name, "panic_oob")) { return make_type(TYPE_VOID); }
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
            // P3.4: these helpers return String headers now. We
            // type them as TYPE_STR for back-compat with the
            // existing dispatch (the operators + interpolation
            // path keys on TYPE_STR), but allow them to flow into
            // String-typed positions too. types_equal treats
            // TYPE_STR as compatible with TYPE_STRUCT("String")
            // for the same-bytes-different-spelling situation —
            // see types_equal in this file.
            if (!strcmp(name, "str_concat")) return make_type(TYPE_STR);
            if (!strcmp(name, "int_to_str")) return make_type(TYPE_STR);
            // β5: raw memory primitives (heap_alloc, heap_free,
            // mem_load, mem_store, mem_load_byte, mem_store_byte,
            // write_bytes) are NOT user-callable. They were the
            // bootstrap kernel surface that pure-Potato std/list /
            // std/map / std/string used before the `array of T`
            // primitive existed. With α + β1..β4 they're never
            // referenced from any .ptt file. The compiler emits
            // `bl _heap_alloc` / `bl _heap_free` / `bl _write_bytes`
            // / `bl _panic_oob` directly when lowering language
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

            // Detect an enum constructor BEFORE evaluating the object,
            // because the object is a bare identifier naming the enum
            // type (e.g. `Result.Ok(42)`), not a value. If we let
            // check_expr() of the object run first, get_sym() on the
            // type name would produce TYPE_UNKNOWN and the constructor
            // would lose its statically-known result type.
            if (n->method_call.object->type == NODE_IDENT) {
                const char *obj_name = n->method_call.object->ident.name;
                if (is_struct(c, obj_name)) {
                    // It's a known struct or enum name. If it has an enum
                    // entry registered, treat this as an enum constructor:
                    // type-check the args (best effort) and return the
                    // enum's named struct type.
                    for (int j = 0; j < n->method_call.arg_count; j++)
                        check_expr(c, n->method_call.args[j]);
                    return make_struct(obj_name);
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
                if (user_method) {
                    // Implicit `self` is the receiver; user-declared params follow.
                    int expected = user_method->param_count - 1; // minus self
                    if (n->method_call.arg_count != expected) {
                        fprintf(stderr, "error:%d: method '%s.%s' expects %d arguments, got %d\n",
                            n->line, receiver_type_name, m, expected, n->method_call.arg_count);
                        exit(1);
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
                if (obj_t.val_type) return *obj_t.val_type;
                return make_type(TYPE_UNKNOWN);
            }
            if (!strcmp(m, "keys")) return make_type(TYPE_LIST);
            if (!strcmp(m, "set") || !strcmp(m, "fire") || !strcmp(m, "collapse")) return make_type(TYPE_VOID);
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
                for (int i = 0; i < c->struct_count; i++) {
                    StructInfo *si = &c->structs[i];
                    for (int j = 0; j < si->field_count; j++) {
                        if (si->field_names[j] && !strcmp(si->field_names[j], n->field_access.field)) {
                            int off = j * 8;
                            if (matches == 0) {
                                unique_offset = off;
                                first = si->name;
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
            // Return element type if known
            if (obj_t.elem_type) return *obj_t.elem_type;
            return make_type(TYPE_UNKNOWN);
        }
        case NODE_INDEX_ASSIGN: {
            Type obj_t = check_expr(c, n->index_assign.object);
            check_expr(c, n->index_assign.index);
            Type val_t = check_expr(c, n->index_assign.value);
            n->index_assign.is_array = (obj_t.kind == TYPE_ARRAY);
            n->index_assign.is_byte = (obj_t.kind == TYPE_ARRAY &&
                obj_t.elem_type && obj_t.elem_type->kind == TYPE_BYTE);
            if (getenv("ERBOS_DBG_IDX")) {
                fprintf(stderr, "[IDX_ASSIGN line %d] obj=%s elem=%s val=%s\n",
                    n->line, type_name(obj_t),
                    obj_t.elem_type ? type_name(*obj_t.elem_type) : "null",
                    type_name(val_t));
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
            // `b is now a` transfers ownership: mark `a` as moved so any
            // subsequent `a` reference produces a use-after-move error.
            if (n->var_decl.is_move && n->var_decl.value->type == NODE_IDENT) {
                mark_moved_sym(c, n->var_decl.value->ident.name);
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
            if (existing.kind != TYPE_UNKNOWN && new_t.kind != TYPE_UNKNOWN && !types_equal(existing, new_t)) {
                fprintf(stderr, "error:%d: cannot assign '%s' to variable '%s' of type '%s'\n",
                    n->line, type_name(new_t), n->assign.name, type_name(existing));
                exit(1);
            }
            break;
        }
        case NODE_FIELD_ASSIGN: {
            Type obj_t = check_expr(c, n->field_assign.object);
            check_expr(c, n->field_assign.value);
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
            }
            // Enforce ref: if object is a non-ref param, block mutation
            if (n->field_assign.object->type == NODE_IDENT) {
                const char *obj_name = n->field_assign.object->ident.name;
                int ref_status = get_sym_is_ref(c, obj_name);
                Type obj_t2 = get_sym(c, obj_name);
                // ref_status: -1=local(ok), 0=param non-ref(error for structs), 1=param ref(ok)
                if (ref_status == 0 && obj_t2.kind == TYPE_STRUCT) {
                    fprintf(stderr, "error:%d: cannot mutate '%s' — parameter is not ref\n", n->line, obj_name);
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
            // the loop variable when we know it. For TYPE_LIST or
            // TYPE_ARRAY with elem_type set we propagate the
            // element type; otherwise the var lands as TYPE_UNKNOWN
            // and yell-on-element falls through to the runtime
            // magic-number shim (the historic behaviour). Once ε
            // drops the legacy keyword forms every collection
            // carries an elem type and TYPE_UNKNOWN goes away.
            Type col = check_expr(c, n->through_in.collection);
            Type elem = make_type(TYPE_UNKNOWN);
            if ((col.kind == TYPE_LIST || col.kind == TYPE_ARRAY) &&
                col.elem_type)
                elem = *col.elem_type;
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
                    // Allow int<->struct compatibility (null pointers, pointer returns)
                    int compat = (give_t.kind == TYPE_INT && c->cur_return_type.kind == TYPE_STRUCT) ||
                                 (give_t.kind == TYPE_STRUCT && c->cur_return_type.kind == TYPE_INT) ||
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

    // Register import aliases so identifier checks know about them.
    c.import_aliases = program->program.use_aliases;
    c.import_count = program->program.use_count;

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

    // Register enum names as known types (treat like structs for type resolution)
    for (int i = 0; i < program->program.enums.count; i++) {
        // Add enum names to struct list so is_struct/parse_type_str recognizes them
        c.struct_count++;
        c.structs = realloc(c.structs, c.struct_count * sizeof(StructInfo));
        c.structs[c.struct_count - 1].name = program->program.enums.items[i]->enum_def.name;
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

        // Check for missing return
        if (c.cur_return_type.kind != TYPE_VOID && !c.found_give) {
            if (!has_give_in_block(body)) {
                fprintf(stderr, "error: function '%s' must return '%s' but has no give statement\n",
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
