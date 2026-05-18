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
    NODE_ENUM_DEF,
    NODE_MATCH,
    NODE_TEST_DEF,
    NODE_ASSERT,
    NODE_ARRAY_NEW,        // α2: `array of T with cap N`
    NODE_INDEX_ASSIGN,     // α6: `arr[i] be v`
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
        struct { NodeList funcs; NodeList structs; NodeList enums; NodeList tests; char **use_paths; char **use_aliases; int use_count; } program;

        // NODE_FUNC_DEF
        struct {
            char *name;             // bare method name when receiver_type != NULL
            char *receiver_type;    // NULL for free functions; struct name for methods
            char **param_names;
            char **param_types;
            int *param_is_ref;
            int param_count;
            char *return_type;
            Node *body;

            // Generic-method support (P3 foundation):
            //   - When a method is declared on a generic receiver
            //     (`Map.set(self ref Map of K, V, ...)`),
            //     receiver_type_args carries ["K","V"].
            //   - Free functions and methods on non-generic types leave
            //     receiver_type_args=NULL/0.
            //   - The monomorphization pass clones the AST per concrete
            //     instantiation and substitutes type-vars; no codegen
            //     change is required after that pass.
            char **receiver_type_args;
            int receiver_type_arg_count;

            // Generic free-function support (Phase ζ): when a free
            // function is declared with `name of T (...)` /
            // `name of K, V (...)`, type_params carries the bound
            // type variable names and type_param_count is > 0.
            // Free functions without `of ...` leave both at zero.
            // The monomorphization pass clones such templates per
            // call-site instantiation. Methods use receiver_type_args
            // for the same role (extracted from the receiver type
            // string instead of declared explicitly), so the two
            // fields stay independent.
            char **type_params;
            int type_param_count;
        } func_def;

        // NODE_STRUCT_DEF
        struct {
            char *name;
            char **field_names;
            char **field_types;
            int field_count;

            // Generic-struct support (P3 foundation):
            //   - For a non-generic struct (`Point is { x int, y int }`),
            //     type_params is NULL and type_param_count is 0.
            //   - For `Map of K, V is { ... }`, type_params=["K","V"].
            //   - Field types may textually reference these names; the
            //     monomorphization pass substitutes them per concrete
            //     instantiation.
            char **type_params;
            int type_param_count;
        } struct_def;

        // NODE_BLOCK
        struct { NodeList stmts; } block;

        // NODE_VAR_DECL
        struct {
            char *name;
            char *type_name;
            char *elem_type_name;  // for list of X
            char *key_type_name;   // for map of X, Y
            char *val_type_name;   // for map of X, Y
            int is_nomut;
            int is_move;
            int is_rep;
            Node *value;
        } var_decl;

        // NODE_ASSIGN
        // is_move / is_rep: same role as on NODE_VAR_DECL — `q be now p`
        // moves ownership; `q be rep p` deep-clones. src_struct_name is
        // populated by the checker for `is_rep` so irgen can emit
        // `bl _clone_<X>`. All zero for plain `q be p` (which is itself
        // restricted to primitives or owned-fresh RHS — Codex P0-7).
        struct {
            char *name;
            Node *value;
            int is_move;
            int is_rep;
            char *src_struct_name;
        } assign;

        // NODE_FIELD_ASSIGN (obj.field = value)
        // is_move/is_rep: set when the assignment uses `now`/`rep`
        // ownership-transfer forms: `field be now src` / `field be rep src`.
        // Mirrors the same flags on NODE_VAR_DECL so the checker and
        // irgen can apply identical move/clone semantics at field
        // position. Both default to 0 for plain `field be expr`.
        //
        // src_struct_name: when is_rep is set, the checker stashes the
        // *source's* struct type here so irgen can emit `bl
        // _clone_<src_struct_name>`. Different from struct_name (which
        // is the *receiver's* struct type, used for field offset).
        struct {
            Node *object;
            char *field;
            Node *value;
            char *struct_name;
            char *src_struct_name;
            int is_move;
            int is_rep;
        } field_assign;

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
        // method_struct (ε6): if set, the iteration uses
        // <method_struct>_len + <method_struct>_get instead of the
        // legacy header-layout decode (count@8, data@16). Set by
        // the checker for stdlib container types (List, Map,
        // StringMap) whose backing storage is an `array of T` field.
        struct {
            char *var_name;
            Node *collection;
            Node *body;
            char *method_struct;
        } through_in;

        // NODE_INFI
        struct {
            Node *cond; // NULL = infinite
            Node *body;
        } infi;

        // NODE_GIVE
        struct { Node *value; } give;

        // NODE_CALL
        // arg_names (named-arg constructor form): parallel to args.
        // When non-NULL, arg_names[i] is the declared field name for
        // args[i]; the call is interpreted as `Foo(field is value, ...)`.
        // When NULL, the call is positional.
        //
        // arg_is_ref: parallel to args. arg_is_ref[i] == 1 iff the
        // call-site spelled `ref` for that argument. The checker
        // validates this matches the callee's param_is_ref. Codex
        // audit P0-2 — without preserving the marker, `f(ref x)`
        // and `f(x)` were indistinguishable downstream.
        //
        // (Removed: `explicit_generic_zero_ctor` was a parser-only
        // marker used to reject `List of T()` / `Box of int()` in
        // favour of the bare auto-construct form. Under the new
        // language law, `TypeExpr()` is the canonical zero-value
        // formation; bare type expressions are not values.)
        struct {
            char *name;
            Node **args;
            char **arg_names;
            int *arg_is_ref;
            int arg_count;
            // Generic free-function call sites carry explicit type
            // arguments via `name of T (args)` /
            // `name of K, V (args)`. The monomorphization pass
            // uses these to materialize the right specialization.
            // NULL/0 for non-parametric calls and for ordinary
            // struct constructor / method-name calls.
            char **type_args;
            int type_arg_count;
        } call;

        // NODE_METHOD_CALL (obj.method(args))
        // arg_is_ref: same as NODE_CALL — call-site `ref` markers,
        // preserved for checker validation against callee params.
        // Note: applies to non-receiver args only. For real method
        // calls (`c.bump()`), the receiver's ref-ness is inferred
        // from the method's `self` declaration via dot-syntax. For
        // alias-qualified calls (`mod.func(ref x)`), the entire
        // arg list is non-receiver and validated like NODE_CALL.
        struct {
            Node *object;
            char *method;
            Node **args;
            int *arg_is_ref;
            int arg_count;
            char *resolved_struct_name; // set by checker when this is a user method on a struct
            // Codex P1-11 round 3: when an alias-qualified call like
            // `math.max(...)` has its receiver IDENT rewritten to a
            // canonical alias (e.g. `m1`) for symbol-prefix safety,
            // the user-written alias is preserved here so checker
            // diagnostics report `math.max`, not `m1.max`.
            char *alias_display;
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
        struct { Node *object; char *field; char *struct_name; } field_access;

        // NODE_INDEX (arr[idx]). is_array set by checker when the
        // indexed object's static type is `array of T`. is_byte
        // set when T == byte (α8). Used by irgen to pick between
        // the array layout (cap at offset 0, data at offset 8)
        // and the legacy list layout (cap/count/data at offsets
        // 0/8/16); is_byte selects the element-size 1 path with
        // ldrb/strb instead of the default 8-byte ldr/str.
        // method_struct (ε5): if set, the index access dispatches
        // to `<method_struct>_get(obj, idx)` instead of using the
        // header layout — used for stdlib `List of T` /
        // `Map of K, V` / `StringMap of V` whose backing storage is itself
        // an `array of T` field, not a flat header.
        struct { Node *object; Node *index; int is_array; int is_byte; char *method_struct; } index_access;

        // NODE_LIST_LIT
        // NODE_LIST_LIT — `[a, b, c]`. ε3 adds elem_type_name:
        // a pre-monomorph pass infers the element type from the
        // first item and stores its monomorph-mangled spelling
        // (e.g. "int" or "String") here. The monomorph collector
        // then seeds `List<elem_type_name>`; irgen routes the
        // literal through `_alloc_List__<elem>` + per-item
        // `_List__<elem>_push`. Empty literals (`[]`) default to
        // `int`.
        struct { Node **items; int count; char *elem_type_name; } list_lit;

        // NODE_MAP_LIT
        // NODE_MAP_LIT — `["k" to v, ...]`. ε4 adds val_type_name:
        // a pre-monomorph pass infers the value type from the
        // first pair (keys are always String for the literal
        // syntax) and stores it here. Irgen routes the literal
        // through `_alloc_StringMap__<V>` + per-pair
        // `_StringMap__<V>_set`.
        struct { Node **keys; Node **values; int count; char *val_type_name; } map_lit;

        // NODE_ARRAY_NEW (α2): `array of T with cap N` constructor.
        // `elem_type` is the legacy <>-bracketed type string the
        // monomorphizer/checker pipeline uses; for `array of int`
        // this is "int". `cap` is any int expression.
        struct { char *elem_type; Node *cap; } array_new;

        // NODE_INDEX_ASSIGN (α6/α8): `arr[i] be v` / `xs[i] = v`.
        // Same shape as NODE_INDEX but with a value to write.
        // method_struct (ε5): if set, `obj[i] be v` dispatches to
        // `<method_struct>_set(obj, i, v)`.
        struct {
            Node *object;
            Node *index;
            Node *value;
            int is_array;
            int is_byte;
            char *method_struct;
        } index_assign;

        // NODE_ENUM_DEF
        //
        // Generic-enum support mirrors the struct path:
        //   - For a non-generic enum (`Result is Ok(int) | Err(String)`),
        //     type_params is NULL and type_param_count is 0.
        //   - For `Option of T is Some(value T) | None`,
        //     type_params=["T"]. Variant field types may textually
        //     reference these names; the monomorphization pass
        //     substitutes them per concrete instantiation.
        struct {
            char *name;
            char **variant_names;
            char ***variant_field_names;
            char ***variant_field_types;
            int *variant_field_counts;
            int variant_count;
            char **type_params;
            int type_param_count;
        } enum_def;

        // NODE_MATCH
        //
        // enum_name (set by checker): the concrete enum type
        // the scrutinee resolves to. Without this, irgen's
        // variant-tag lookup walks every enum first-match-wins,
        // which is wrong when multiple monomorphized enums share
        // a variant name (e.g. `Result__int__String` and
        // `Result__String__int` both have `Ok` and `Err`).
        struct {
            Node *expr;
            char **arm_variant_names;
            char ***arm_bindings;
            int *arm_binding_counts;
            Node **arm_bodies;
            int arm_count;
            char *enum_name;
        } match_expr;

        // NODE_TEST_DEF
        struct {
            char *name;
            Node *body;
        } test_def;

        // NODE_ASSERT
        struct {
            Node *condition;
        } assert_stmt;
    };
};

#endif
