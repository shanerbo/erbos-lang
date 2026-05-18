#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parser.h"

static Token *cur(Parser *p) { return &p->tokens[p->pos]; }
static Token *peek_at(Parser *p, int offset) { return &p->tokens[p->pos + offset]; }

static Token *eat(Parser *p, TokenType t) {
    if (cur(p)->type != t) {
        fprintf(stderr, "%s:%d: error: unexpected token\n", p->filename, cur(p)->line);
        exit(1);
    }
    return &p->tokens[p->pos++];
}

static void skip_newlines(Parser *p) {
    while (cur(p)->type == TOK_NEWLINE) p->pos++;
}

static int at(Parser *p, TokenType t) { return cur(p)->type == t; }

static Node *alloc_node(NodeType type, int line) {
    Node *n = calloc(1, sizeof(Node));
    n->type = type;
    n->line = line;
    return n;
}

// Render a type-name string from its internal `<>`-bracketed form
// back to the user-facing word-style form for diagnostics. The
// internal form is what parse_type_name produces (for the
// monomorphizer); the source uses `Type of T1, T2, ...`.
//
// "List<int>"           -> "List of int"
// "Map<String,int>"     -> "Map of String, int"
// "List<List<int>>"     -> "List of List of int"
// "Foo"                 -> "Foo"
//
// The output is malloc'd by the caller-controlled buffer; bounded.
static void format_type_word_style(const char *internal, char *out, int outsz) {
    if (!internal || outsz <= 0) { if (outsz > 0) out[0] = '\0'; return; }
    int oi = 0;
    int depth = 0;
    for (int i = 0; internal[i] && oi + 1 < outsz; i++) {
        char c = internal[i];
        if (c == '<') {
            if (oi + 4 < outsz) {
                memcpy(out + oi, " of ", 4);
                oi += 4;
            }
            depth++;
        } else if (c == '>') {
            depth--;
        } else if (c == ',' && depth > 0) {
            if (oi + 2 < outsz) {
                out[oi++] = ',';
                out[oi++] = ' ';
            }
        } else {
            out[oi++] = c;
        }
    }
    out[oi] = '\0';
}

static void list_push(NodeList *l, Node *n) {
    if (l->count >= l->cap) {
        l->cap = l->cap ? l->cap * 2 : 4;
        l->items = realloc(l->items, l->cap * sizeof(Node *));
    }
    l->items[l->count++] = n;
}

// Forward declarations
static Node *parse_expr(Parser *p);
static Node *parse_stmt(Parser *p);
static Node *parse_block(Parser *p);
static Node *parse_if_continuation(Parser *p, Node *first_cond, int line);
static char *parse_type_name(Parser *p);
static int parse_type_params(Parser *p, char ***out_names);

// Parse a single call argument. If the argument is in the named form
//   IDENT is EXPR
// (a struct-style named-field initializer, e.g. `Point(x is 1, y is 2)`),
// the caller-provided *out_name is set to the field name (heap-owned
// strdup'd copy, never NULL). Otherwise *out_name is set to NULL and
// the argument is parsed as a plain expression. Detection requires
// IDENT *immediately* followed by `is`; anything else (including
// `IDENT eq ...`, `IDENT.foo`, `IDENT(...)`) falls through to the
// plain-expression path. Caller is responsible for enforcing the
// "all-or-nothing" rule between named and positional args within the
// same call.
static Node *parse_call_arg(Parser *p, char **out_name) {
    *out_name = NULL;
    if (at(p, TOK_IDENT) && peek_at(p, 1)->type == TOK_IS) {
        char *fname = strdup(cur(p)->value);
        p->pos++;          // skip IDENT
        p->pos++;          // skip `is`
        Node *val = parse_expr(p);
        *out_name = fname;
        return val;
    }
    return parse_expr(p);
}

// --- Expression parsing ---

static Node *parse_primary(Parser *p) {
    int line = cur(p)->line;

    // α2: `array of T with cap N` constructor expression. Recognised
    // as a primary expression so it can be the right-hand side of
    // `is` declarations and any other expression position.
    //
    // `array` is a regular IDENT (no reserved token) — we look at
    // the IDENT's spelling and require the followup `of <type> with
    // cap <expr>` shape. If anything mismatches, fall through to
    // ordinary IDENT-as-variable handling below so existing code
    // that has a variable named `array` keeps working.
    if (at(p, TOK_IDENT) && cur(p)->value && !strcmp(cur(p)->value, "array") &&
        peek_at(p, 1)->type == TOK_OF) {
        // Lookahead: must be `array of <type> with cap <expr>`.
        // We commit to this branch only after seeing the full prefix
        // up to `with`; otherwise, restore position and let the
        // generic IDENT path handle whatever this turns out to be.
        int saved = p->pos;
        p->pos++;            // consume `array`
        p->pos++;            // consume `of`
        char *elem_type = parse_type_name(p);
        // Expect IDENT "with".
        if (!(at(p, TOK_IDENT) && cur(p)->value && !strcmp(cur(p)->value, "with"))) {
            // Not the constructor form. Roll back; the generic path
            // below will report whatever the right error is.
            p->pos = saved;
            free(elem_type);
        } else {
            p->pos++;        // consume `with`
            // Expect IDENT "cap".
            if (!(at(p, TOK_IDENT) && cur(p)->value && !strcmp(cur(p)->value, "cap"))) {
                fprintf(stderr, "%s:%d: error: expected `cap` after `with` in array constructor\n",
                    p->filename, cur(p)->line);
                exit(1);
            }
            p->pos++;        // consume `cap`
            Node *cap_expr = parse_expr(p);
            Node *n = alloc_node(NODE_ARRAY_NEW, line);
            n->array_new.elem_type = elem_type;
            n->array_new.cap = cap_expr;
            return n;
        }
    }

    if (at(p, TOK_INT_LIT)) {
        Node *n = alloc_node(NODE_INT_LIT, line);
        n->int_lit.value = atol(cur(p)->value);
        p->pos++;
        return n;
    }
    if (at(p, TOK_STR_LIT)) {
        Node *n = alloc_node(NODE_STR_LIT, line);
        n->str_lit.value = cur(p)->value;
        p->pos++;
        // Allow postfix method calls on a string literal:
        //   "hello".len(), "abc".equals("abc"), etc.
        // Same chain logic as the IDENT-postfix path below.
        Node *base = n;
        while (at(p, TOK_DOT)) {
            p->pos++;
            char *field = eat(p, TOK_IDENT)->value;
            if (at(p, TOK_LPAREN)) {
                p->pos++;
                Node *mc = alloc_node(NODE_METHOD_CALL, line);
                mc->method_call.object = base;
                mc->method_call.method = field;
                int cap = 4;
                mc->method_call.args = malloc(cap * sizeof(Node *));
                mc->method_call.arg_is_ref = malloc(cap * sizeof(int));
                mc->method_call.arg_count = 0;
                skip_newlines(p);
                if (!at(p, TOK_RPAREN)) {
                    int is_ref = 0;
                    if (at(p, TOK_REF)) { p->pos++; is_ref = 1; }
                    mc->method_call.args[mc->method_call.arg_count] = parse_expr(p);
                    mc->method_call.arg_is_ref[mc->method_call.arg_count] = is_ref;
                    mc->method_call.arg_count++;
                    skip_newlines(p);
                    while (at(p, TOK_COMMA)) {
                        p->pos++;
                        skip_newlines(p);
                        if (mc->method_call.arg_count >= cap) {
                            cap *= 2;
                            mc->method_call.args = realloc(mc->method_call.args, cap * sizeof(Node *));
                            mc->method_call.arg_is_ref = realloc(mc->method_call.arg_is_ref, cap * sizeof(int));
                        }
                        is_ref = 0;
                        if (at(p, TOK_REF)) { p->pos++; is_ref = 1; }
                        mc->method_call.args[mc->method_call.arg_count] = parse_expr(p);
                        mc->method_call.arg_is_ref[mc->method_call.arg_count] = is_ref;
                        mc->method_call.arg_count++;
                        skip_newlines(p);
                    }
                }
                eat(p, TOK_RPAREN);
                base = mc;
            } else {
                Node *fa = alloc_node(NODE_FIELD_ACCESS, line);
                fa->field_access.object = base;
                fa->field_access.field = field;
                base = fa;
            }
        }
        return base;
    }
    if (at(p, TOK_TRUE) || at(p, TOK_FALSE)) {
        Node *n = alloc_node(NODE_BOOL_LIT, line);
        n->bool_lit.value = at(p, TOK_TRUE);
        p->pos++;
        return n;
    }
    if (at(p, TOK_NIL)) {
        Node *n = alloc_node(NODE_INT_LIT, line);
        n->int_lit.value = 0;
        p->pos++;
        return n;
    }
    if (at(p, TOK_NOT)) {
        p->pos++;
        Node *n = alloc_node(NODE_UNARY, line);
        n->unary.op = TOK_NOT;
        n->unary.operand = parse_primary(p);
        return n;
    }
    if (at(p, TOK_MINUS)) {
        p->pos++;
        Node *n = alloc_node(NODE_UNARY, line);
        n->unary.op = TOK_MINUS;
        n->unary.operand = parse_primary(p);
        return n;
    }
    if (at(p, TOK_LPAREN)) {
        p->pos++;
        Node *n = parse_expr(p);
        eat(p, TOK_RPAREN);
        return n;
    }
    // list(), map(), task() constructors
    if ((at(p, TOK_LIST) || at(p, TOK_MAP) || at(p, TOK_IMAP) || at(p, TOK_TASK)) && peek_at(p, 1)->type == TOK_LPAREN) {
        char *name = at(p, TOK_LIST) ? "list" : at(p, TOK_IMAP) ? "imap" : at(p, TOK_MAP) ? "map" : "task";
        p->pos++;
        p->pos++; // skip (
        eat(p, TOK_RPAREN);
        Node *n = alloc_node(NODE_CALL, line);
        n->call.name = name;
        n->call.args = NULL;
        n->call.arg_count = 0;
        return n;
    }
    // List literal [1, 2, 3] or Map literal ["a" to 1, "b" to 2]
    if (at(p, TOK_LBRACKET)) {        p->pos++;
        if (at(p, TOK_RBRACKET)) {
            p->pos++;
            Node *n = alloc_node(NODE_LIST_LIT, line);
            n->list_lit.items = NULL;
            n->list_lit.count = 0;
            return n;
        }
        Node *first = parse_expr(p);
        if (at(p, TOK_TO)) {
            // Map literal: ["a" to 1, "b" to 2]
            p->pos++;
            Node *first_val = parse_expr(p);
            int cap = 8;
            Node *n = alloc_node(NODE_MAP_LIT, line);
            n->map_lit.keys = malloc(cap * sizeof(Node *));
            n->map_lit.values = malloc(cap * sizeof(Node *));
            n->map_lit.keys[0] = first;
            n->map_lit.values[0] = first_val;
            n->map_lit.count = 1;
            while (at(p, TOK_COMMA)) {
                p->pos++;
                if (n->map_lit.count >= cap) { cap *= 2; n->map_lit.keys = realloc(n->map_lit.keys, cap * sizeof(Node *)); n->map_lit.values = realloc(n->map_lit.values, cap * sizeof(Node *)); }
                n->map_lit.keys[n->map_lit.count] = parse_expr(p);
                eat(p, TOK_TO);
                n->map_lit.values[n->map_lit.count] = parse_expr(p);
                n->map_lit.count++;
            }
            eat(p, TOK_RBRACKET);
            return n;
        }
        // List literal
        int cap = 8;
        Node *n = alloc_node(NODE_LIST_LIT, line);
        n->list_lit.items = malloc(cap * sizeof(Node *));
        n->list_lit.items[0] = first;
        n->list_lit.count = 1;
        while (at(p, TOK_COMMA)) {
            p->pos++;
            if (n->list_lit.count >= cap) { cap *= 2; n->list_lit.items = realloc(n->list_lit.items, cap * sizeof(Node *)); }
            n->list_lit.items[n->list_lit.count++] = parse_expr(p);
        }
        eat(p, TOK_RBRACKET);
        return n;
    }
    if (at(p, TOK_IDENT)) {
        char *name = cur(p)->value;

        // Parametric value formation: `Box of int(value is 7)`,
        // `Map of String, int()`, or enum factory `some of int (7)`.
        //
        // Language law: a type expression names a type only. A bare
        // type expression like `List of int` is never a value. A
        // value is formed only by:
        //   - TypeExpr()
        //   - TypeExpr(field is value, ...)
        //   - enum factories: none/some/ok/err
        //
        // We disambiguate by looking ahead: a value-formation
        // expression's type-arg list is always followed by '('. The
        // shape is IDENT `of` TYPE [`,` TYPE]* `(`, with the type
        // args themselves possibly parametric (recursing through `of`).
        //
        // Lookahead walker: starting at peek(1), consume only
        // type-position tokens (IDENT / primitive keywords / `of` /
        // comma). Stop at the first token that can't be in a type
        // position. If that token is `(`, it's a value-formation call.
        // If it's anything else (including `.`), the bare type
        // expression is rejected as a value below.
        if (peek_at(p, 1)->type == TOK_OF) {
            int look = 1;
            int ok = 1;
            int saw_type_token = 0;
            while (ok) {
                TokenType t = p->tokens[p->pos + look].type;
                if (t == TOK_OF || t == TOK_COMMA) {
                    look++;
                    saw_type_token = 0;  // need a type ident next
                    continue;
                }
                if (t == TOK_IDENT || t == TOK_INT || t == TOK_STR_TYPE ||
                    t == TOK_BOOL || t == TOK_LIST || t == TOK_MAP ||
                    t == TOK_IMAP || t == TOK_TASK || t == TOK_VOID) {
                    look++;
                    saw_type_token = 1;
                    continue;
                }
                break;
            }
            // Parametric type used as a method receiver (legacy
            // `Option of T .Some(value)` shape). The parser still
            // produces this AST shape so the stdlib factory bodies
            // can lower enum variant materialization through it,
            // but every node it produces is stamped with
            // `is_stdlib_enum_factory` only when the parser is
            // reading a vetted stdlib factory source file (set by
            // main.c after `parser_init`). The checker uses that
            // flag — and only that flag — as the allow-list for the
            // legacy form. User code cannot set the flag, so a
            // user file whose function happens to be named
            // `none` / `some` / `ok` / `err` cannot bypass the law.
            if (ok && saw_type_token && p->tokens[p->pos + look].type == TOK_DOT) {
                char *full = parse_type_name(p);
                Node *base = alloc_node(NODE_IDENT, line);
                base->ident.name = full;
                while (at(p, TOK_DOT) || at(p, TOK_LBRACKET)) {
                    if (at(p, TOK_DOT)) {
                        p->pos++;
                        char *field = eat(p, TOK_IDENT)->value;
                        if (at(p, TOK_LPAREN)) {
                            p->pos++;
                            Node *mc = alloc_node(NODE_METHOD_CALL, line);
                            mc->method_call.object = base;
                            mc->method_call.method = field;
                            mc->method_call.is_stdlib_enum_factory =
                                p->is_stdlib_enum_factory_file;
                            int mcap = 4;
                            mc->method_call.args = malloc(mcap * sizeof(Node *));
                            mc->method_call.arg_is_ref = malloc(mcap * sizeof(int));
                            mc->method_call.arg_count = 0;
                            skip_newlines(p);
                            if (!at(p, TOK_RPAREN)) {
                                int mis_ref = 0;
                                if (at(p, TOK_REF)) { p->pos++; mis_ref = 1; }
                                mc->method_call.args[mc->method_call.arg_count] = parse_expr(p);
                                mc->method_call.arg_is_ref[mc->method_call.arg_count] = mis_ref;
                                mc->method_call.arg_count++;
                                skip_newlines(p);
                                while (at(p, TOK_COMMA)) {
                                    p->pos++;
                                    skip_newlines(p);
                                    if (mc->method_call.arg_count >= mcap) {
                                        mcap *= 2;
                                        mc->method_call.args = realloc(mc->method_call.args, mcap * sizeof(Node *));
                                        mc->method_call.arg_is_ref = realloc(mc->method_call.arg_is_ref, mcap * sizeof(int));
                                    }
                                    mis_ref = 0;
                                    if (at(p, TOK_REF)) { p->pos++; mis_ref = 1; }
                                    mc->method_call.args[mc->method_call.arg_count] = parse_expr(p);
                                    mc->method_call.arg_is_ref[mc->method_call.arg_count] = mis_ref;
                                    mc->method_call.arg_count++;
                                    skip_newlines(p);
                                }
                            }
                            eat(p, TOK_RPAREN);
                            base = mc;
                        } else {
                            Node *fa = alloc_node(NODE_FIELD_ACCESS, line);
                            fa->field_access.object = base;
                            fa->field_access.field = field;
                            base = fa;
                        }
                    } else {
                        p->pos++;
                        Node *idx = parse_expr(p);
                        eat(p, TOK_RBRACKET);
                        Node *ia = alloc_node(NODE_INDEX, line);
                        ia->index_access.object = base;
                        ia->index_access.index = idx;
                        base = ia;
                    }
                }
                return base;
            }
            // Bare type expression in value position. `List of int`,
            // `Map of String, int`, `Box of Foo` etc. are types, not
            // values. To form a value, write `TypeExpr()` or
            // `TypeExpr(field is value, ...)`.
            if (ok && saw_type_token && p->tokens[p->pos + look].type != TOK_LPAREN) {
                // Render the type expression in the user-facing
                // word-style form for the diagnostic.
                char *full = parse_type_name(p);
                char display[512];
                format_type_word_style(full ? full : "", display, sizeof(display));
                fprintf(stderr,
                    "error:%d: type expression `%s` is not a value\n",
                    line, display);
                fprintf(stderr,
                    "  help: form a value with `%s()` or "
                    "`%s(field is value, ...)`\n",
                    display, display);
                if (full) free(full);
                exit(1);
            }
            if (ok && saw_type_token && p->tokens[p->pos + look].type == TOK_LPAREN) {
                // Parametric constructor confirmed. parse_type_name reads
                // `Box of int` as `Box<int>` (legacy internal form) for
                // the monomorphizer, then we consume `(` and arguments.
                // The checker later rejects the redundant empty-paren
                // form `Box of int()`; only named-arg generic
                // constructors pay rent.
                char *full = parse_type_name(p);
                eat(p, TOK_LPAREN);
                Node *n = alloc_node(NODE_CALL, line);
                n->call.name = full;
                int cap = 4;
                n->call.args = malloc(cap * sizeof(Node *));
                n->call.arg_names = malloc(cap * sizeof(char *));
                n->call.arg_is_ref = malloc(cap * sizeof(int));
                n->call.arg_count = 0;
                int saw_named = 0;
                int saw_positional = 0;
                // Multiline argument lists: newlines inside (...) are
                // ignored. Without these skips, programs that format
                // long struct constructors across lines would die with
                // "unexpected token in expression."
                skip_newlines(p);
                if (!at(p, TOK_RPAREN)) {
                    int is_ref = 0;
                    if (at(p, TOK_REF)) { p->pos++; is_ref = 1; }
                    char *nm = NULL;
                    n->call.args[n->call.arg_count] = parse_call_arg(p, &nm);
                    n->call.arg_names[n->call.arg_count] = nm;
                    n->call.arg_is_ref[n->call.arg_count] = is_ref;
                    if (nm) saw_named = 1; else saw_positional = 1;
                    n->call.arg_count++;
                    skip_newlines(p);
                    while (at(p, TOK_COMMA)) {
                        p->pos++;
                        skip_newlines(p);
                        if (n->call.arg_count >= cap) {
                            cap *= 2;
                            n->call.args = realloc(n->call.args, cap * sizeof(Node *));
                            n->call.arg_names = realloc(n->call.arg_names, cap * sizeof(char *));
                            n->call.arg_is_ref = realloc(n->call.arg_is_ref, cap * sizeof(int));
                        }
                        is_ref = 0;
                        if (at(p, TOK_REF)) { p->pos++; is_ref = 1; }
                        nm = NULL;
                        n->call.args[n->call.arg_count] = parse_call_arg(p, &nm);
                        n->call.arg_names[n->call.arg_count] = nm;
                        n->call.arg_is_ref[n->call.arg_count] = is_ref;
                        if (nm) saw_named = 1; else saw_positional = 1;
                        n->call.arg_count++;
                        skip_newlines(p);
                    }
                }
                if (saw_named && saw_positional) {
                    fprintf(stderr, "%s:%d: error: cannot mix named and positional arguments in '%s'\n",
                        p->filename, line, full);
                    exit(1);
                }
                if (!saw_named) {
                    free(n->call.arg_names);
                    n->call.arg_names = NULL;
                }
                eat(p, TOK_RPAREN);
                Node *base = n;
                // Continue with the same chain logic the normal IDENT
                // path uses below.
                while (at(p, TOK_DOT) || at(p, TOK_LBRACKET)) {
                    if (at(p, TOK_DOT)) {
                        p->pos++;
                        char *field = eat(p, TOK_IDENT)->value;
                        if (at(p, TOK_LPAREN)) {
                            p->pos++;
                            Node *mc = alloc_node(NODE_METHOD_CALL, line);
                            mc->method_call.object = base;
                            mc->method_call.method = field;
                            int mcap = 4;
                            mc->method_call.args = malloc(mcap * sizeof(Node *));
                            mc->method_call.arg_is_ref = malloc(mcap * sizeof(int));
                            mc->method_call.arg_count = 0;
                            skip_newlines(p);
                            if (!at(p, TOK_RPAREN)) {
                                int mis_ref = 0;
                                if (at(p, TOK_REF)) { p->pos++; mis_ref = 1; }
                                mc->method_call.args[mc->method_call.arg_count] = parse_expr(p);
                                mc->method_call.arg_is_ref[mc->method_call.arg_count] = mis_ref;
                                mc->method_call.arg_count++;
                                skip_newlines(p);
                                while (at(p, TOK_COMMA)) {
                                    p->pos++;
                                    skip_newlines(p);
                                    if (mc->method_call.arg_count >= mcap) {
                                        mcap *= 2;
                                        mc->method_call.args = realloc(mc->method_call.args, mcap * sizeof(Node *));
                                        mc->method_call.arg_is_ref = realloc(mc->method_call.arg_is_ref, mcap * sizeof(int));
                                    }
                                    mis_ref = 0;
                                    if (at(p, TOK_REF)) { p->pos++; mis_ref = 1; }
                                    mc->method_call.args[mc->method_call.arg_count] = parse_expr(p);
                                    mc->method_call.arg_is_ref[mc->method_call.arg_count] = mis_ref;
                                    mc->method_call.arg_count++;
                                    skip_newlines(p);
                                }
                            }
                            eat(p, TOK_RPAREN);
                            base = mc;
                        } else {
                            Node *fa = alloc_node(NODE_FIELD_ACCESS, line);
                            fa->field_access.object = base;
                            fa->field_access.field = field;
                            base = fa;
                        }
                    } else {
                        p->pos++;
                        Node *idx = parse_expr(p);
                        eat(p, TOK_RBRACKET);
                        Node *ia = alloc_node(NODE_INDEX, line);
                        ia->index_access.object = base;
                        ia->index_access.index = idx;
                        base = ia;
                    }
                }
                return base;
            }
        }

        p->pos++;
        Node *base;

        // Function call
        if (at(p, TOK_LPAREN)) {
            p->pos++;
            Node *n = alloc_node(NODE_CALL, line);
            n->call.name = name;
            int cap = 4;
            n->call.args = malloc(cap * sizeof(Node *));
            n->call.arg_names = malloc(cap * sizeof(char *));
            n->call.arg_is_ref = malloc(cap * sizeof(int));
            n->call.arg_count = 0;
            int saw_named = 0;
            int saw_positional = 0;
            // Multiline argument lists: newlines inside (...) are ignored.
            skip_newlines(p);
            if (!at(p, TOK_RPAREN)) {
                int is_ref = 0;
                if (at(p, TOK_REF)) { p->pos++; is_ref = 1; }
                char *nm = NULL;
                n->call.args[n->call.arg_count] = parse_call_arg(p, &nm);
                n->call.arg_names[n->call.arg_count] = nm;
                n->call.arg_is_ref[n->call.arg_count] = is_ref;
                if (nm) saw_named = 1; else saw_positional = 1;
                n->call.arg_count++;
                skip_newlines(p);
                while (at(p, TOK_COMMA)) {
                    p->pos++;
                    skip_newlines(p);
                    if (n->call.arg_count >= cap) {
                        cap *= 2;
                        n->call.args = realloc(n->call.args, cap * sizeof(Node *));
                        n->call.arg_names = realloc(n->call.arg_names, cap * sizeof(char *));
                        n->call.arg_is_ref = realloc(n->call.arg_is_ref, cap * sizeof(int));
                    }
                    is_ref = 0;
                    if (at(p, TOK_REF)) { p->pos++; is_ref = 1; }
                    nm = NULL;
                    n->call.args[n->call.arg_count] = parse_call_arg(p, &nm);
                    n->call.arg_names[n->call.arg_count] = nm;
                    n->call.arg_is_ref[n->call.arg_count] = is_ref;
                    if (nm) saw_named = 1; else saw_positional = 1;
                    n->call.arg_count++;
                    skip_newlines(p);
                }
            }
            if (saw_named && saw_positional) {
                fprintf(stderr, "%s:%d: error: cannot mix named and positional arguments in '%s'\n",
                    p->filename, line, name);
                exit(1);
            }
            if (!saw_named) {
                free(n->call.arg_names);
                n->call.arg_names = NULL;
            }
            eat(p, TOK_RPAREN);
            base = n;
        } else {
            base = alloc_node(NODE_IDENT, line);
            base->ident.name = name;
        }

        // Chain: .field, .method(), [index]
        while (at(p, TOK_DOT) || at(p, TOK_LBRACKET)) {
            if (at(p, TOK_DOT)) {
                p->pos++;
                char *field = eat(p, TOK_IDENT)->value;
                if (at(p, TOK_LPAREN)) {
                    // method call
                    p->pos++;
                    Node *mc = alloc_node(NODE_METHOD_CALL, line);
                    mc->method_call.object = base;
                    mc->method_call.method = field;
                    int cap = 4;
                    mc->method_call.args = malloc(cap * sizeof(Node *));
                    mc->method_call.arg_is_ref = malloc(cap * sizeof(int));
                    mc->method_call.arg_count = 0;
                    skip_newlines(p);
                    if (!at(p, TOK_RPAREN)) {
                        // Preserve call-site `ref` markers. Same shape
                        // as NODE_CALL — used for module-aliased free
                        // function calls and for non-self method args.
                        int is_ref = 0;
                        if (at(p, TOK_REF)) { p->pos++; is_ref = 1; }
                        mc->method_call.args[mc->method_call.arg_count] = parse_expr(p);
                        mc->method_call.arg_is_ref[mc->method_call.arg_count] = is_ref;
                        mc->method_call.arg_count++;
                        skip_newlines(p);
                        while (at(p, TOK_COMMA)) {
                            p->pos++;
                            skip_newlines(p);
                            if (mc->method_call.arg_count >= cap) {
                                cap *= 2;
                                mc->method_call.args = realloc(mc->method_call.args, cap * sizeof(Node *));
                                mc->method_call.arg_is_ref = realloc(mc->method_call.arg_is_ref, cap * sizeof(int));
                            }
                            is_ref = 0;
                            if (at(p, TOK_REF)) { p->pos++; is_ref = 1; }
                            mc->method_call.args[mc->method_call.arg_count] = parse_expr(p);
                            mc->method_call.arg_is_ref[mc->method_call.arg_count] = is_ref;
                            mc->method_call.arg_count++;
                            skip_newlines(p);
                        }
                    }
                    eat(p, TOK_RPAREN);
                    base = mc;
                } else {
                    // field access
                    Node *fa = alloc_node(NODE_FIELD_ACCESS, line);
                    fa->field_access.object = base;
                    fa->field_access.field = field;
                    base = fa;
                }
            } else {
                // index
                p->pos++;
                Node *idx = parse_expr(p);
                eat(p, TOK_RBRACKET);
                Node *ia = alloc_node(NODE_INDEX, line);
                ia->index_access.object = base;
                ia->index_access.index = idx;
                base = ia;
            }
        }
        return base;
    }
    fprintf(stderr, "%s:%d: error: unexpected token in expression\n", p->filename, line);
    exit(1);
}

static int precedence(TokenType t) {
    switch (t) {
        case TOK_OR: return 1;
        case TOK_AND: return 2;
        case TOK_EQ: case TOK_NEQ: case TOK_EQ_WORD: case TOK_NE_WORD: return 3;
        case TOK_LT: case TOK_GT: case TOK_LTE: case TOK_GTE:
        case TOK_LT_WORD: case TOK_GT_WORD: case TOK_LE_WORD: case TOK_GE_WORD: return 4;
        case TOK_PLUS: case TOK_MINUS: return 5;
        case TOK_STAR: case TOK_SLASH: case TOK_PERCENT: case TOK_MOD_WORD: return 6;
        default: return -1;
    }
}

static Node *parse_expr_prec(Parser *p, int min_prec) {
    Node *left = parse_primary(p);
    while (1) {
        TokenType op = cur(p)->type;
        int prec = precedence(op);
        if (prec < min_prec) break;
        int line = cur(p)->line;
        p->pos++;
        Node *right = parse_expr_prec(p, prec + 1);
        Node *bin = alloc_node(NODE_BINARY, line);
        bin->binary.op = op;
        bin->binary.left = left;
        bin->binary.right = right;
        left = bin;
    }
    return left;
}

static Node *parse_expr(Parser *p) {
    return parse_expr_prec(p, 1);
}

// --- Statement parsing ---

static Node *parse_block(Parser *p) {
    int line = cur(p)->line;
    eat(p, TOK_LBRACE);
    skip_newlines(p);
    Node *block = alloc_node(NODE_BLOCK, line);
    block->block.stmts = (NodeList){0};
    while (!at(p, TOK_RBRACE) && !at(p, TOK_EOF)) {
        list_push(&block->block.stmts, parse_stmt(p));
        skip_newlines(p);
    }
    eat(p, TOK_RBRACE);
    return block;
}

static Node *parse_through(Parser *p) {
    int line = cur(p)->line;
    eat(p, TOK_THROUGH);
    eat(p, TOK_LPAREN);
    char *var_name = eat(p, TOK_IDENT)->value;

    if (at(p, TOK_FROM)) {
        p->pos++;
        Node *n = alloc_node(NODE_THROUGH_RANGE, line);
        n->through_range.var_name = var_name;
        n->through_range.from = parse_expr(p);
        eat(p, TOK_TO);
        n->through_range.to = parse_expr(p);
        n->through_range.by = NULL;
        if (at(p, TOK_BY)) { p->pos++; n->through_range.by = parse_expr(p); }
        eat(p, TOK_RPAREN);
        n->through_range.body = parse_block(p);
        return n;
    } else {
        eat(p, TOK_IN);
        Node *n = alloc_node(NODE_THROUGH_IN, line);
        n->through_in.var_name = var_name;
        n->through_in.collection = parse_expr(p);
        eat(p, TOK_RPAREN);
        n->through_in.body = parse_block(p);
        return n;
    }
}

// Parse an `?{ ... }` block plus zero or more `<expr> ?{ ... }` else-ifs and an
// optional trailing `nah { ... }`. Caller is expected to have already parsed
// the leading condition expression (`first_cond`) and the `?` token has not yet
// been consumed.
static Node *parse_if_continuation(Parser *p, Node *first_cond, int line) {
    eat(p, TOK_QUESTION);
    Node *n = alloc_node(NODE_IF, line);
    int cap = 4;
    n->if_stmt.conds = malloc(cap * sizeof(Node *));
    n->if_stmt.bodies = malloc(cap * sizeof(Node *));
    n->if_stmt.branch_count = 0;
    n->if_stmt.nah_body = NULL;
    n->if_stmt.conds[0] = first_cond;
    n->if_stmt.bodies[0] = parse_block(p);
    n->if_stmt.branch_count = 1;
    while (1) {
        int saved = p->pos;
        skip_newlines(p);
        if (at(p, TOK_NAH)) { p->pos++; n->if_stmt.nah_body = parse_block(p); break; }
        if (at(p, TOK_EOF) || at(p, TOK_RBRACE)) { p->pos = saved; break; }
        // Heuristic: this is an else-if only if a `?` appears before the next newline
        int test_pos = p->pos;
        int found_q = 0;
        while (p->tokens[test_pos].type != TOK_NEWLINE && p->tokens[test_pos].type != TOK_EOF && p->tokens[test_pos].type != TOK_RBRACE) {
            if (p->tokens[test_pos].type == TOK_QUESTION) { found_q = 1; break; }
            test_pos++;
        }
        if (!found_q) { p->pos = saved; break; }
        Node *c = parse_expr(p);
        eat(p, TOK_QUESTION);
        Node *b = parse_block(p);
        if (n->if_stmt.branch_count >= cap) {
            cap *= 2;
            n->if_stmt.conds = realloc(n->if_stmt.conds, cap * sizeof(Node *));
            n->if_stmt.bodies = realloc(n->if_stmt.bodies, cap * sizeof(Node *));
        }
        n->if_stmt.conds[n->if_stmt.branch_count] = c;
        n->if_stmt.bodies[n->if_stmt.branch_count] = b;
        n->if_stmt.branch_count++;
    }
    return n;
}

static Node *parse_stmt(Parser *p) {
    skip_newlines(p);
    int line = cur(p)->line;

    if (at(p, TOK_GIVE)) {
        p->pos++;
        Node *n = alloc_node(NODE_GIVE, line);
        // give with no value = void return
        if (at(p, TOK_NEWLINE) || at(p, TOK_RBRACE) || at(p, TOK_EOF)) {
            n->give.value = NULL;
        } else {
            n->give.value = parse_expr(p);
        }
        return n;
    }
    if (at(p, TOK_STOP)) { p->pos++; return alloc_node(NODE_STOP, line); }
    if (at(p, TOK_SKIP)) { p->pos++; return alloc_node(NODE_SKIP, line); }

    // γ3: TOK_ASSERT no longer exists; `assert(cond)` is a plain
    // NODE_CALL parsed through the call path below. The checker
    // recognises the name and emits the same `_assert_fail` lowering.
    if (at(p, TOK_THROUGH)) { return parse_through(p); }

    // Match expression
    if (at(p, TOK_MATCH)) {
        p->pos++;
        Node *expr = parse_expr(p);
        eat(p, TOK_LBRACE);
        skip_newlines(p);

        Node *n = alloc_node(NODE_MATCH, line);
        n->match_expr.expr = expr;
        int cap = 8;
        n->match_expr.arm_variant_names = malloc(cap * sizeof(char *));
        n->match_expr.arm_bindings = malloc(cap * sizeof(char **));
        n->match_expr.arm_binding_counts = malloc(cap * sizeof(int));
        n->match_expr.arm_bodies = malloc(cap * sizeof(Node *));
        n->match_expr.arm_count = 0;

        while (!at(p, TOK_RBRACE) && !at(p, TOK_EOF)) {
            skip_newlines(p);
            if (at(p, TOK_RBRACE)) break;
            int ai = n->match_expr.arm_count;
            if (ai >= cap) { cap *= 2; n->match_expr.arm_variant_names = realloc(n->match_expr.arm_variant_names, cap * sizeof(char *)); n->match_expr.arm_bindings = realloc(n->match_expr.arm_bindings, cap * sizeof(char **)); n->match_expr.arm_binding_counts = realloc(n->match_expr.arm_binding_counts, cap * sizeof(int)); n->match_expr.arm_bodies = realloc(n->match_expr.arm_bodies, cap * sizeof(Node *)); }
            n->match_expr.arm_variant_names[ai] = eat(p, TOK_IDENT)->value;
            n->match_expr.arm_bindings[ai] = NULL;
            n->match_expr.arm_binding_counts[ai] = 0;
            if (at(p, TOK_LPAREN)) {
                p->pos++;
                int bcap = 4;
                n->match_expr.arm_bindings[ai] = malloc(bcap * sizeof(char *));
                int bc = 0;
                while (!at(p, TOK_RPAREN)) {
                    if (bc >= bcap) { bcap *= 2; n->match_expr.arm_bindings[ai] = realloc(n->match_expr.arm_bindings[ai], bcap * sizeof(char *)); }
                    n->match_expr.arm_bindings[ai][bc++] = eat(p, TOK_IDENT)->value;
                    if (at(p, TOK_COMMA)) p->pos++;
                }
                eat(p, TOK_RPAREN);
                n->match_expr.arm_binding_counts[ai] = bc;
            }
            eat(p, TOK_ARROW);
            if (at(p, TOK_LBRACE)) {
                n->match_expr.arm_bodies[ai] = parse_block(p);
            } else {
                Node *body = alloc_node(NODE_BLOCK, cur(p)->line);
                body->block.stmts = (NodeList){0};
                list_push(&body->block.stmts, parse_stmt(p));
                n->match_expr.arm_bodies[ai] = body;
            }
            n->match_expr.arm_count++;
            skip_newlines(p);
        }
        eat(p, TOK_RBRACE);
        return n;
    }

    // Bare block: { ... } for scoped lifetimes
    if (at(p, TOK_LBRACE)) {
        return parse_block(p);
    }

    if (at(p, TOK_INFI)) {        p->pos++;
        Node *n = alloc_node(NODE_INFI, line);
        if (at(p, TOK_LPAREN)) {
            p->pos++;
            n->infi.cond = parse_expr(p);
            eat(p, TOK_RPAREN);
        } else {
            n->infi.cond = NULL;
        }
        n->infi.body = parse_block(p);
        return n;
    }

    // `nomut` is a leading prefix on `IDENT is ...`. Rather than
    // duplicate the entire is-decl parsing path here (which has
    // `is now`, `is rep`, primitive types, generic auto-construct,
    // etc.), we consume the `nomut` keyword and let the IDENT path
    // below handle the rest, then mark the resulting NODE_VAR_DECL
    // as nomut. This keeps every shape `is` accepts available to
    // `nomut` for free — including `nomut xs is List of int`,
    // `nomut p is Point(x is 1, y is 2)`, etc.
    int leading_nomut = 0;
    if (at(p, TOK_NOMUT)) {
        p->pos++;
        leading_nomut = 1;
    }

    if (at(p, TOK_IDENT)) {
        // var decl: IDENT IS ...
        if (peek_at(p, 1)->type == TOK_IS) {
            char *name = cur(p)->value;
            p->pos++;
            eat(p, TOK_IS);
            Node *n = alloc_node(NODE_VAR_DECL, line);
            n->var_decl.name = name;
            n->var_decl.is_nomut = leading_nomut;
            n->var_decl.is_move = 0;
            n->var_decl.is_rep = 0;
            n->var_decl.type_name = NULL;

            // Check for "is now X" (move) or "is rep X" (clone)
            if (at(p, TOK_NOW)) {
                p->pos++;
                n->var_decl.is_move = 1;
                // Must be an identifier (can only move a variable, not a literal)
                if (!at(p, TOK_IDENT)) {
                    fprintf(stderr, "%s:%d: error: can only move a variable, not a value\n", p->filename, line);
                    exit(1);
                }
                n->var_decl.value = parse_expr(p);
                return n;
            }
            if (at(p, TOK_REP)) {
                p->pos++;
                n->var_decl.is_rep = 1;
                if (!at(p, TOK_IDENT)) {
                    fprintf(stderr, "%s:%d: error: can only clone a variable, not a value\n", p->filename, line);
                    exit(1);
                }
                n->var_decl.value = parse_expr(p);
                return n;
            }

            // Explicit type. Several shapes are accepted:
            //
            // (a) Lowercase legacy keywords (`list of T`, `map of K, V`,
            //     `imap of int, V`, `task`) — paths through the
            //     existing branches below; auto-constructed if no value
            //     follows.
            //
            // (b) Primitive-named types (`int`, `str`, `bool`).
            //
            // (c) Type annotation followed by an initializer:
            //     `xs is int 5` / `flag is bool true`. The
            //     bare-type-without-initializer auto-construct form
            //     (e.g. `xs is List of int`) is rejected under the
            //     new language law — a value is formed only by
            //     `TypeExpr()`, `TypeExpr(field is value, ...)`, or
            //     enum factories.
            int explicit_type_consumed = 0;
            // Codex P2-16: `x str` variable annotations are also
            // rejected — same teaching error as type positions
            // elsewhere. Without this guard the var-decl path would
            // silently treat `str` as `String`.
            if (at(p, TOK_STR_TYPE)) {
                fprintf(stderr,
                    "error:%d: `str` is no longer a type — use `String` "
                    "(from `std/string`) instead\n", cur(p)->line);
                fprintf(stderr,
                    "  help: replace `str` with `String` and add "
                    "`use std/string` at the top of the file\n");
                exit(1);
            }
            if (at(p, TOK_INT) || at(p, TOK_BOOL) ||
                ((at(p, TOK_LIST) || at(p, TOK_MAP) || at(p, TOK_IMAP) || at(p, TOK_TASK)) && peek_at(p, 1)->type != TOK_LPAREN)) {
                if (at(p, TOK_TASK)) {
                    n->var_decl.type_name = "task";
                    p->pos++;
                } else if (at(p, TOK_LIST)) {
                    n->var_decl.type_name = "list";
                    p->pos++;
                    if (at(p, TOK_OF)) {
                        p->pos++; // skip 'of'
                        n->var_decl.elem_type_name = cur(p)->value;
                        p->pos++; // skip type
                    }
                } else if (at(p, TOK_MAP) || at(p, TOK_IMAP)) {
                    n->var_decl.type_name = at(p, TOK_IMAP) ? "imap" : "map";
                    p->pos++;
                    if (at(p, TOK_OF)) {
                        p->pos++; // skip 'of'
                        n->var_decl.key_type_name = cur(p)->value;
                        p->pos++; // skip key type
                        if (at(p, TOK_COMMA)) {
                            p->pos++; // skip comma
                            n->var_decl.val_type_name = cur(p)->value;
                            p->pos++; // skip val type
                        }
                        if (at(p, TOK_TO)) {
                            fprintf(stderr,
                                "error:%d: `to` is no longer used between "
                                "generic type arguments — use a comma\n",
                                cur(p)->line);
                            fprintf(stderr,
                                "  help: write `map of K, V`\n");
                            exit(1);
                        }
                    }
                } else {
                    n->var_decl.type_name = cur(p)->value;
                    p->pos++;
                }
                explicit_type_consumed = 1;
            } else if (at(p, TOK_IDENT) && peek_at(p, 1)->type == TOK_OF &&
                       peek_at(p, 2)->type != TOK_LPAREN &&
                       !(cur(p)->value && !strcmp(cur(p)->value, "array"))) {
                // `array of T with cap N` is a constructor expression
                // (handled in parse_primary), not a type-name auto-
                // construct. Skip this branch when the head is
                // `array` so the right-hand side falls through to
                // parse_expr below.
                //
                // Capitalised user-generic type form, no parens:
                //   xs is List of int
                //   m  is Map of int, int
                //   sm is StringMap of int
                // parse_type_name reads it as a single legacy
                // <>-bracketed string for the monomorphizer ("List<int>"
                // etc.), which we stash in type_name. Auto-construct
                // below builds the constructor call.
                //
                // Restriction: only fires when there's no `(` after
                // the type expression (would be the explicit-
                // constructor form `Box of int (...)` which is
                // already handled as a regular expression elsewhere).
                // We snapshot the position, parse the type, and
                // back out if a `(` shows up — letting parse_expr
                // handle the constructor-call form.
                int saved_pos = p->pos;
                char *parsed_type = parse_type_name(p);
                if (at(p, TOK_LPAREN) || at(p, TOK_DOT)) {
                    // Explicit constructor call (`Box of int (...)`)
                    // or parametric type used as an enum-variant
                    // receiver (`Option of int .Some(7)`) follows;
                    // rewind so parse_expr below sees the full
                    // expression.
                    p->pos = saved_pos;
                    free(parsed_type);
                } else {
                    n->var_decl.type_name = parsed_type;
                    explicit_type_consumed = 1;
                }
            }
            // Under the new language law, a bare type expression is
            // not a value. `xs is List of int` (no initializer) is
            // rejected; the user must write `xs is List of int()`
            // for zero-value formation or `xs is List of int(...)`
            // for named-field formation.
            if (at(p, TOK_NEWLINE) || at(p, TOK_EOF)) {
                if (explicit_type_consumed && n->var_decl.type_name) {
                    char display[512];
                    format_type_word_style(n->var_decl.type_name, display, sizeof(display));
                    fprintf(stderr,
                        "error:%d: type expression `%s` is not a value\n",
                        line, display);
                    fprintf(stderr,
                        "  help: form a value with `%s()` or "
                        "`%s(field is value, ...)`\n",
                        display, display);
                    exit(1);
                }
                fprintf(stderr, "%s:%d: error: variable '%s' must be initialized with a value\n", p->filename, line, name);
                exit(1);
            } else {
                n->var_decl.value = parse_expr(p);
            }
            return n;
        }
        // assignment: IDENT be ...
        // Codex P0-7: also accept `IDENT be now src` and `IDENT be
        // rep src` for explicit heap reassignment, mirroring the
        // forms on NODE_VAR_DECL and NODE_FIELD_ASSIGN.
        if (peek_at(p, 1)->type == TOK_BE) {
            char *name = cur(p)->value;
            p->pos++;
            p->pos++; // skip be
            Node *n = alloc_node(NODE_ASSIGN, line);
            n->assign.name = name;
            n->assign.is_move = 0;
            n->assign.is_rep = 0;
            n->assign.src_struct_name = NULL;
            if (at(p, TOK_NOW)) {
                p->pos++;
                n->assign.is_move = 1;
            } else if (at(p, TOK_REP)) {
                p->pos++;
                n->assign.is_rep = 1;
            }
            n->assign.value = parse_expr(p);
            return n;
        }
        // field assign: IDENT.field = ...
        // index assign: IDENT[i] be ...   (α6)
        if (peek_at(p, 1)->type == TOK_DOT || peek_at(p, 1)->type == TOK_LBRACKET) {
            // Could be field/index assign, method call, or just expr.
            // Parse as expression first.
            Node *expr = parse_expr(p);
            if (at(p, TOK_ASSIGN) || at(p, TOK_BE)) {
                // Assignment — figure out the LHS shape.
                if (expr->type == NODE_FIELD_ACCESS) {
                    p->pos++; // skip = or be
                    Node *n = alloc_node(NODE_FIELD_ASSIGN, line);
                    n->field_assign.object = expr->field_access.object;
                    n->field_assign.field = expr->field_access.field;
                    n->field_assign.is_move = 0;
                    n->field_assign.is_rep = 0;
                    // `field be now src` — transfer ownership of `src`
                    // into the field. `field be rep src` — deep-clone.
                    // Same surface as `is now` / `is rep` for var
                    // declarations. RHS must be an identifier; the
                    // checker rejects non-IDENT RHS.
                    if (at(p, TOK_NOW)) {
                        p->pos++;
                        n->field_assign.is_move = 1;
                    } else if (at(p, TOK_REP)) {
                        p->pos++;
                        n->field_assign.is_rep = 1;
                    }
                    n->field_assign.value = parse_expr(p);
                    return n;
                }
                if (expr->type == NODE_INDEX) {
                    // α6: arr[i] be v
                    p->pos++; // skip = or be
                    Node *n = alloc_node(NODE_INDEX_ASSIGN, line);
                    n->index_assign.object = expr->index_access.object;
                    n->index_assign.index = expr->index_access.index;
                    n->index_assign.is_array = 0; // checker fills in
                    n->index_assign.is_move = 0;
                    n->index_assign.is_rep = 0;
                    // Optional `now` / `rep` modifiers, mirroring
                    // var-decl and field-assign. `arr[i] be now src`
                    // moves ownership of src into the slot;
                    // `arr[i] be rep src` deep-clones src into the
                    // slot. Both restrict src to a bare identifier
                    // (irgen marks it moved or emits _clone_<X>).
                    if (at(p, TOK_NOW)) {
                        p->pos++;
                        n->index_assign.is_move = 1;
                        if (!at(p, TOK_IDENT)) {
                            fprintf(stderr,
                                "%s:%d: error: can only move a variable into an array slot, not a value\n",
                                p->filename, line);
                            exit(1);
                        }
                    } else if (at(p, TOK_REP)) {
                        p->pos++;
                        n->index_assign.is_rep = 1;
                        if (!at(p, TOK_IDENT)) {
                            fprintf(stderr,
                                "%s:%d: error: can only clone a variable into an array slot, not a value\n",
                                p->filename, line);
                            exit(1);
                        }
                    }
                    n->index_assign.value = parse_expr(p);
                    return n;
                }
                fprintf(stderr, "%s:%d: error: invalid assignment target\n", p->filename, line);
                exit(1);
            }
            // Check if it's a condition
            if (at(p, TOK_QUESTION)) {
                return parse_if_continuation(p, expr, line);
            }
            return expr;
        }
        // Otherwise parse as expression
        Node *expr = parse_expr(p);
        if (at(p, TOK_QUESTION)) {
            return parse_if_continuation(p, expr, line);
        }
        return expr;
    }

    // Bare expression
    Node *expr = parse_expr(p);
    if (at(p, TOK_QUESTION)) {
        return parse_if_continuation(p, expr, line);
    }
    return expr;
}

// --- Top-level ---

static int is_type_token(Parser *p) {
    return at(p, TOK_INT) || at(p, TOK_STR_TYPE) || at(p, TOK_BOOL) || at(p, TOK_VOID) || at(p, TOK_IDENT);
}

static Node *parse_enum_def(Parser *p) {
    int line = cur(p)->line;
    char *name = eat(p, TOK_IDENT)->value;

    // Optional generic-parameter list, mirroring parse_struct_def:
    //   `Option of T is Some(value T) | None`
    //   `Either of L, R is Left(value L) | Right(value R)`
    char **type_params = NULL;
    int type_param_count = parse_type_params(p, &type_params);

    eat(p, TOK_IS);
    skip_newlines(p);

    Node *n = alloc_node(NODE_ENUM_DEF, line);
    n->enum_def.name = name;
    n->enum_def.type_params = type_params;
    n->enum_def.type_param_count = type_param_count;
    int cap = 8;
    n->enum_def.variant_names = malloc(cap * sizeof(char *));
    n->enum_def.variant_field_names = malloc(cap * sizeof(char **));
    n->enum_def.variant_field_types = malloc(cap * sizeof(char **));
    n->enum_def.variant_field_counts = malloc(cap * sizeof(int));
    n->enum_def.variant_count = 0;

    // Parse first variant (no leading |)
    while (1) {
        skip_newlines(p);
        if (at(p, TOK_PIPE)) p->pos++; // skip optional |
        skip_newlines(p);
        if (!at(p, TOK_IDENT)) break;

        int vi = n->enum_def.variant_count;
        if (vi >= cap) {
            cap *= 2;
            n->enum_def.variant_names = realloc(n->enum_def.variant_names, cap * sizeof(char *));
            n->enum_def.variant_field_names = realloc(n->enum_def.variant_field_names, cap * sizeof(char **));
            n->enum_def.variant_field_types = realloc(n->enum_def.variant_field_types, cap * sizeof(char **));
            n->enum_def.variant_field_counts = realloc(n->enum_def.variant_field_counts, cap * sizeof(int));
        }
        n->enum_def.variant_names[vi] = eat(p, TOK_IDENT)->value;
        n->enum_def.variant_field_names[vi] = NULL;
        n->enum_def.variant_field_types[vi] = NULL;
        n->enum_def.variant_field_counts[vi] = 0;

        // Optional fields: Variant(name type, name type)
        if (at(p, TOK_LPAREN)) {
            p->pos++;
            int fcap = 4;
            n->enum_def.variant_field_names[vi] = malloc(fcap * sizeof(char *));
            n->enum_def.variant_field_types[vi] = malloc(fcap * sizeof(char *));
            int fc = 0;
            while (!at(p, TOK_RPAREN)) {
                if (fc >= fcap) { fcap *= 2; n->enum_def.variant_field_names[vi] = realloc(n->enum_def.variant_field_names[vi], fcap * sizeof(char *)); n->enum_def.variant_field_types[vi] = realloc(n->enum_def.variant_field_types[vi], fcap * sizeof(char *)); }
                n->enum_def.variant_field_names[vi][fc] = eat(p, TOK_IDENT)->value;
                // Use the full type-name parser so generic / nested
                // types like `List of T`, `Map of K, V` are accepted
                // in variant payloads. The internal <>-bracketed form
                // is what monomorph expects.
                n->enum_def.variant_field_types[vi][fc] = parse_type_name(p);
                fc++;
                if (at(p, TOK_COMMA)) p->pos++;
            }
            eat(p, TOK_RPAREN);
            n->enum_def.variant_field_counts[vi] = fc;
        }
        n->enum_def.variant_count++;

        // Check if next line has | for another variant
        skip_newlines(p);
        if (!at(p, TOK_PIPE)) break;
    }
    return n;
}

// Read a single type expression and return it as one allocated string.
// Handles bare names ("int", "String", "Counter") and parametric types
// ("Box of int", "Map of String, int", "List of List of int").
//
// Word-style generics (P3.1):
//   - `of T` introduces a single type parameter: "Box of int".
//   - `of A, B, C` introduces multiple parameters.
//   - `of` is right-associative, so
//     "List of List of int" parses as "List of (List of int)" and
//     "Map of String, List of int" parses as "Map of String, (List of int)".
//   - There are NO `to` generic separators, NO parens, and NO `<>` in type
//     position.
//
// The output uses the legacy `<>` internal representation that the
// monomorphizer pass parses (it splits on `<`, top-level `,`, and
// matching `>`). Word-style "Box of int" emits "Box<int>"; "Map of
// String, int" emits "Map<String,int>"; "List of List of int" emits
// "List<List<int>>". This keeps the monomorph + mangling code below
// unchanged — only the surface syntax differs from the original P3.
static char *parse_type_name(Parser *p) {
    // Codex P2-16: `str` was a transitional alias for the
    // stdlib `String` struct during the str→String rewrite.
    // CLAUDE.md and the design docs are explicit that there is
    // no `str → String` sugar; the lexer still produces
    // TOK_STR_TYPE so we can surface a teaching error here.
    // Any code using `str` in a type position must migrate to
    // `String` and `use std/string`.
    if (at(p, TOK_STR_TYPE)) {
        fprintf(stderr,
            "error:%d: `str` is no longer a type — use `String` "
            "(from `std/string`) instead\n", cur(p)->line);
        fprintf(stderr,
            "  help: replace `str` with `String` and add "
            "`use std/string` at the top of the file\n");
        exit(1);
    }
    if (!at(p, TOK_IDENT) && !at(p, TOK_INT) &&
        !at(p, TOK_BOOL) && !at(p, TOK_VOID) && !at(p, TOK_LIST) &&
        !at(p, TOK_MAP) && !at(p, TOK_IMAP) && !at(p, TOK_TASK)) {
        // Caller's responsibility to surface a useful error; just take
        // whatever token's `value` is and advance like the old code did.
        char *v = cur(p)->value;
        p->pos++;
        return v ? strdup(v) : strdup("");
    }
    // Buffer up the head identifier; if it's a primitive keyword the
    // token's `value` may be NULL — fall back to the keyword spelling.
    const char *head = cur(p)->value;
    if (!head) {
        switch (cur(p)->type) {
            case TOK_INT: head = "int"; break;
            case TOK_STR_TYPE: head = "str"; break;
            case TOK_BOOL: head = "bool"; break;
            case TOK_VOID: head = "void"; break;
            case TOK_LIST: head = "list"; break;
            case TOK_MAP: head = "map"; break;
            case TOK_IMAP: head = "imap"; break;
            case TOK_TASK: head = "task"; break;
            default: head = ""; break;
        }
    }
    // Most type expressions are short.
    char buf[512];
    int len = (int)strlen(head);
    if (len >= (int)sizeof(buf)) len = (int)sizeof(buf) - 1;
    memcpy(buf, head, len);
    buf[len] = '\0';
    p->pos++;
    // Word-style chain: optional `of T [, U ...]` continuations.
    //   "Box of int"          -> "Box<int>"
    //   "Map of String, int"  -> "Map<String,int>"
    //   "List of List of int" -> "List<List<int>>"
    // The first `of` opens a `<...>`; subsequent top-level comma
    // separators remain commas; nested `of` recurses into another
    // bracket pair.
    if (at(p, TOK_OF)) {
        p->pos++; // consume `of`
        if (len + 1 < (int)sizeof(buf)) buf[len++] = '<';
        char *inner = parse_type_name(p);
        if (inner) {
            int plen = (int)strlen(inner);
            if (len + plen < (int)sizeof(buf)) {
                memcpy(buf + len, inner, plen);
                len += plen;
            }
            free(inner);
        }
        while (at(p, TOK_COMMA)) {
            // Comma is both the generic-arg separator and the surrounding
            // syntax separator. Treat it as a type-arg separator only when
            // the following token can start a type and does not look like
            // the start of the next named function parameter (`name Type`).
            TokenType nt = peek_at(p, 1)->type;
            TokenType nt2 = peek_at(p, 2)->type;
            int next_starts_type =
                nt == TOK_IDENT || nt == TOK_INT || nt == TOK_BOOL ||
                nt == TOK_VOID || nt == TOK_LIST || nt == TOK_MAP ||
                nt == TOK_IMAP || nt == TOK_TASK || nt == TOK_STR_TYPE;
            int looks_like_param_boundary =
                nt == TOK_IDENT &&
                (nt2 == TOK_IDENT || nt2 == TOK_INT || nt2 == TOK_BOOL ||
                 nt2 == TOK_VOID || nt2 == TOK_LIST || nt2 == TOK_MAP ||
                 nt2 == TOK_IMAP || nt2 == TOK_TASK || nt2 == TOK_STR_TYPE);
            if (!next_starts_type || looks_like_param_boundary) break;
            p->pos++; // consume comma
            if (len + 1 < (int)sizeof(buf)) buf[len++] = ',';
            char *vinner = parse_type_name(p);
            if (vinner) {
                int plen = (int)strlen(vinner);
                if (len + plen < (int)sizeof(buf)) {
                    memcpy(buf + len, vinner, plen);
                    len += plen;
                }
                free(vinner);
            }
        }
        if (at(p, TOK_TO)) {
            fprintf(stderr,
                "error:%d: `to` is no longer used between generic type "
                "arguments — use a comma\n", cur(p)->line);
            fprintf(stderr,
                "  help: write `Map of K, V` or `Result of T, E`\n");
            exit(1);
        }
        if (len + 1 < (int)sizeof(buf)) buf[len++] = '>';
        buf[len] = '\0';
    }
    return strdup(buf);
}

// Parse an optional `of T [, U ...]` type-parameter list immediately
// after a type name in a struct or method declaration head.
// Examples:
//   Box of T is { ... }              -> ["T"]
//   Map of K, V is { ... }           -> ["K", "V"]
//   Counter is { ... }               -> []  (no params)
//   Box.set(self ref Box of T, ...)  -> ["T"] when called on the receiver
//
// Caller is positioned just after the IDENT; if the next token is
// not TOK_OF this is a no-op and returns 0/NULL via out params.
//
// Returns the number of type parameters parsed (0 if none).
static int parse_type_params(Parser *p, char ***out_names) {
    *out_names = NULL;
    if (!at(p, TOK_OF)) return 0;
    p->pos++; // consume `of`
    int cap = 2, count = 0;
    char **names = malloc(cap * sizeof(char *));
    names[count++] = eat(p, TOK_IDENT)->value;
    while (at(p, TOK_COMMA)) {
        p->pos++; // consume comma
        if (count >= cap) {
            cap *= 2;
            names = realloc(names, cap * sizeof(char *));
        }
        names[count++] = eat(p, TOK_IDENT)->value;
    }
    if (at(p, TOK_TO)) {
        fprintf(stderr,
            "error:%d: `to` is no longer used between generic type "
            "parameters — use a comma\n", cur(p)->line);
        fprintf(stderr,
            "  help: write `Map of K, V is { ... }`\n");
        exit(1);
    }
    *out_names = names;
    return count;
}

static Node *parse_struct_def(Parser *p) {
    int line = cur(p)->line;
    char *name = eat(p, TOK_IDENT)->value;

    // Optional generic-parameter list: word-style
    // `Map of K, V is { ... }` (the legacy `<>` form is gone).
    char **type_params = NULL;
    int type_param_count = parse_type_params(p, &type_params);

    eat(p, TOK_IS);
    eat(p, TOK_LBRACE);
    skip_newlines(p);

    Node *n = alloc_node(NODE_STRUCT_DEF, line);
    n->struct_def.name = name;
    n->struct_def.type_params = type_params;
    n->struct_def.type_param_count = type_param_count;
    int cap = 8;
    n->struct_def.field_names = malloc(cap * sizeof(char *));
    n->struct_def.field_types = malloc(cap * sizeof(char *));
    n->struct_def.field_count = 0;

    while (!at(p, TOK_RBRACE) && !at(p, TOK_EOF)) {
        skip_newlines(p);
        if (at(p, TOK_RBRACE)) break;
        char *fname = eat(p, TOK_IDENT)->value;
        char *ftype = parse_type_name(p);
        if (n->struct_def.field_count >= cap) {
            cap *= 2;
            n->struct_def.field_names = realloc(n->struct_def.field_names, cap * sizeof(char *));
            n->struct_def.field_types = realloc(n->struct_def.field_types, cap * sizeof(char *));
        }
        n->struct_def.field_names[n->struct_def.field_count] = fname;
        n->struct_def.field_types[n->struct_def.field_count] = ftype;
        n->struct_def.field_count++;
        // Either separator works between fields: a comma (matches
        // function-param syntax) or a newline (the canonical multi-
        // line form). Trailing separators before `}` are also fine
        // because skip_newlines + the loop's RBRACE check handle the
        // empty case at the top of the next iteration.
        if (at(p, TOK_COMMA)) p->pos++;
        skip_newlines(p);
    }
    eat(p, TOK_RBRACE);
    return n;
}

static Node *parse_func_def(Parser *p) {
    int line = cur(p)->line;
    char *name = eat(p, TOK_IDENT)->value;

    Node *n = alloc_node(NODE_FUNC_DEF, line);
    n->func_def.name = name;
    n->func_def.param_names = NULL;
    n->func_def.param_types = NULL;
    n->func_def.param_count = 0;
    n->func_def.return_type = NULL;
    n->func_def.type_params = NULL;
    n->func_def.type_param_count = 0;

    // Optional generic-parameter list, mirroring parse_struct_def
    // and parse_enum_def: `name of T (...)` / `name of K, V (...)`.
    // Only fires for free functions; methods get their type-param
    // names from the receiver type string (set by the caller after
    // parse_func_def returns).
    {
        char **tp = NULL;
        int tpc = parse_type_params(p, &tp);
        if (tpc > 0) {
            n->func_def.type_params = tp;
            n->func_def.type_param_count = tpc;
        }
    }

    if (at(p, TOK_LPAREN)) {
        p->pos++;
        int cap = 4;
        n->func_def.param_names = malloc(cap * sizeof(char *));
        n->func_def.param_types = malloc(cap * sizeof(char *));
        n->func_def.param_is_ref = malloc(cap * sizeof(int));
        while (!at(p, TOK_RPAREN)) {
            if (n->func_def.param_count >= cap) {
                cap *= 2;
                n->func_def.param_names = realloc(n->func_def.param_names, cap * sizeof(char *));
                n->func_def.param_types = realloc(n->func_def.param_types, cap * sizeof(char *));
                n->func_def.param_is_ref = realloc(n->func_def.param_is_ref, cap * sizeof(int));
            }
            char *pname = eat(p, TOK_IDENT)->value;
            // Check for "ref" before type
            int is_ref = 0;
            if (at(p, TOK_REF)) { is_ref = 1; p->pos++; }
            char *ptype = parse_type_name(p);
            n->func_def.param_names[n->func_def.param_count] = pname;
            n->func_def.param_types[n->func_def.param_count] = ptype;
            n->func_def.param_is_ref[n->func_def.param_count] = is_ref;
            n->func_def.param_count++;
            if (at(p, TOK_COMMA)) p->pos++;
        }
        eat(p, TOK_RPAREN);
    }

    // Return type — supports parametric types (e.g. `Box<int>`).
    if (is_type_token(p) && !at(p, TOK_LBRACE)) {
        n->func_def.return_type = parse_type_name(p);
    }

    n->func_def.body = parse_block(p);
    return n;
}

void parser_init(Parser *p, Lexer *l) {
    p->tokens = l->tokens;
    p->count = l->count;
    p->pos = 0;
    p->filename = "unknown";
    // Default: not a stdlib factory. main.c flips this on for
    // sources whose absolute path is the bundled-stdlib
    // std/option.ptt / std/result.ptt.
    p->is_stdlib_enum_factory_file = 0;
}

Node *parser_parse(Parser *p) {
    Node *program = alloc_node(NODE_PROGRAM, 1);
    program->program.funcs = (NodeList){0};
    program->program.structs = (NodeList){0};
    program->program.enums = (NodeList){0};
    program->program.tests = (NodeList){0};
    program->program.use_paths = NULL;
    program->program.use_aliases = NULL;
    program->program.use_count = 0;

    skip_newlines(p);

    // Parse use declarations first
    while (at(p, TOK_USE)) {
        p->pos++;
        // Build path: segment/segment/segment, where each segment
        // is an identifier OR a type-name token (`list`, `map`,
        // `imap`, `task`, `int`, `str`, `bool`, `void`). The
        // type-name keywords also serve as legal stdlib filenames
        // (e.g. `std/list`, `std/map`) — accept them in path
        // position and capture their literal spelling.
        char path[256] = {0};
        const char *seg;
        if (at(p, TOK_IDENT)) {
            seg = eat(p, TOK_IDENT)->value;
        } else {
            switch (cur(p)->type) {
                case TOK_LIST: seg = "list"; break;
                case TOK_MAP:  seg = "map";  break;
                case TOK_IMAP: seg = "imap"; break;
                case TOK_TASK: seg = "task"; break;
                case TOK_INT:  seg = "int";  break;
                case TOK_STR_TYPE: seg = "str"; break;
                case TOK_BOOL: seg = "bool"; break;
                default: seg = eat(p, TOK_IDENT)->value; break;
            }
            p->pos++;
        }
        strcat(path, seg);
        while (at(p, TOK_SLASH)) {
            p->pos++;
            strcat(path, "/");
            const char *seg2;
            if (at(p, TOK_IDENT)) {
                seg2 = eat(p, TOK_IDENT)->value;
            } else {
                switch (cur(p)->type) {
                    case TOK_LIST: seg2 = "list"; break;
                    case TOK_MAP:  seg2 = "map";  break;
                    case TOK_IMAP: seg2 = "imap"; break;
                    case TOK_TASK: seg2 = "task"; break;
                    case TOK_INT:  seg2 = "int";  break;
                    case TOK_STR_TYPE: seg2 = "str"; break;
                    case TOK_BOOL: seg2 = "bool"; break;
                    default: seg2 = eat(p, TOK_IDENT)->value; break;
                }
                p->pos++;
            }
            strcat(path, seg2);
        }
        // Optional alias: as name
        char *alias = NULL;
        if (at(p, TOK_AS)) {
            p->pos++;
            alias = eat(p, TOK_IDENT)->value;
        } else {
            // Default alias = last segment of path
            alias = strrchr(path, '/');
            alias = alias ? alias + 1 : path;
        }
        int idx = program->program.use_count++;
        program->program.use_paths = realloc(program->program.use_paths, program->program.use_count * sizeof(char *));
        program->program.use_aliases = realloc(program->program.use_aliases, program->program.use_count * sizeof(char *));
        program->program.use_paths[idx] = strdup(path);
        program->program.use_aliases[idx] = strdup(alias);
        skip_newlines(p);
    }

    while (!at(p, TOK_EOF)) {
        // Entry point: spark { ... } — α0.2.
        // `spark` is now a reserved keyword (TOK_SPARK); user code
        // can't shadow the entry-point name. The shape is fixed:
        // exactly `spark` followed by `{ body }`. No parameters, no
        // return type. Synthesised into a regular NODE_FUNC_DEF
        // with name="spark" so the rest of the pipeline (checker,
        // monomorph, irgen) treats it like any free function. The
        // runtime entry point in main.c emits `bl _spark`, which
        // matches the mangled name.
        if (at(p, TOK_SPARK)) {
            int line = cur(p)->line;
            p->pos++; // consume `spark`
            Node *n = alloc_node(NODE_FUNC_DEF, line);
            n->func_def.name = "spark";
            n->func_def.param_names = NULL;
            n->func_def.param_types = NULL;
            n->func_def.param_is_ref = NULL;
            n->func_def.param_count = 0;
            n->func_def.return_type = NULL;
            n->func_def.receiver_type = NULL;
            n->func_def.receiver_type_args = NULL;
            n->func_def.receiver_type_arg_count = 0;
            n->func_def.body = parse_block(p);
            list_push(&program->program.funcs, n);
        }
        // Test block: test "name" { }
        else if (at(p, TOK_TEST)) {
            p->pos++;
            Node *t = alloc_node(NODE_TEST_DEF, cur(p)->line);
            t->test_def.name = eat(p, TOK_STR_LIT)->value;
            t->test_def.body = parse_block(p);
            list_push(&program->program.tests, t);
        }
        // Method def: IDENT . IDENT ( ... )   e.g.  Counter.bump(self ref Counter) { ... }
        // Generic methods (Box.set, Map.set) take the same form — the
        // receiver type's `of T` clause appears inside the first param's
        // type string (`self ref Box of T`), and we recover the bound
        // type-parameter names from that string after parsing.
        //
        // Methods on primitive types (P3.2): the receiver token is a
        // primitive type keyword (int / bool) instead of an
        // identifier. We accept those and synthesize the receiver_type
        // string from the keyword spelling.
        //
        // Codex P2-16: `str.foo(self str)` method-def headers are
        // rejected here for the same reason `str` is rejected as a
        // type — the canonical text type is the `String` struct in
        // std/string.ptt. Methods on String are written with
        // `String.foo(self String)` and live alongside the struct
        // definition.
        else if ((at(p, TOK_IDENT) || at(p, TOK_STR_TYPE) || at(p, TOK_INT) ||
                  at(p, TOK_BOOL)) &&
                 peek_at(p, 1)->type == TOK_DOT &&
                 peek_at(p, 2)->type == TOK_IDENT && peek_at(p, 3)->type == TOK_LPAREN) {
            if (at(p, TOK_STR_TYPE)) {
                fprintf(stderr,
                    "error:%d: `str` is no longer a type — methods on "
                    "text values go on `String` (from `std/string`)\n",
                    cur(p)->line);
                fprintf(stderr,
                    "  help: change `str.foo(self str)` to "
                    "`String.foo(self String)`\n");
                exit(1);
            }
            char *recv;
            if (at(p, TOK_INT))           { recv = strdup("int");  p->pos++; }
            else if (at(p, TOK_BOOL))     { recv = strdup("bool"); p->pos++; }
            else                          { recv = eat(p, TOK_IDENT)->value; }
            eat(p, TOK_DOT);
            // parse_func_def reads from the method-name token onward.
            Node *m = parse_func_def(p);
            m->func_def.receiver_type = recv;
            // Extract receiver_type_args from the first param's type
            // string. parse_type_name emits the internal <>-bracketed
            // form for the monomorphizer's benefit (user-facing syntax
            // is word-style `of T` / `of K, V`); we re-parse the
            // internal form here to lift the type variable names out.
            //
            // Examples (internal mangled form, NOT source syntax):
            //   first param type = "Box<T>"           -> ["T"]
            //   first param type = "Map<K,V>"         -> ["K", "V"]
            //   first param type = "Counter"          -> []
            if (m->func_def.param_count > 0 && m->func_def.param_types[0]) {
                const char *pt = m->func_def.param_types[0];
                const char *lt = strchr(pt, '<');
                const char *gt = strrchr(pt, '>');
                if (lt && gt && gt > lt) {
                    int cap = 2, count = 0;
                    char **names = malloc(cap * sizeof(char *));
                    const char *p2 = lt + 1;
                    while (p2 < gt) {
                        // Skip whitespace.
                        while (p2 < gt && (*p2 == ' ' || *p2 == ',')) p2++;
                        const char *start = p2;
                        // Token is bounded by ',' or '>' or '<' (nested
                        // arg start, e.g. "List<T>" — but for receiver
                        // type-param extraction we only care about the
                        // top-level idents, which are the type variables
                        // bound by the receiver). Nested generics never
                        // appear in a method's receiver-type clause: the
                        // receiver is always `Type of T` or `Type of K, V`
                        // with bare ident type variables.
                        while (p2 < gt && *p2 != ',' && *p2 != '<' && *p2 != '>') p2++;
                        int len2 = (int)(p2 - start);
                        if (len2 > 0) {
                            char *name2 = malloc(len2 + 1);
                            memcpy(name2, start, len2);
                            name2[len2] = '\0';
                            if (count >= cap) { cap *= 2; names = realloc(names, cap * sizeof(char *)); }
                            names[count++] = name2;
                        }
                    }
                    if (count > 0) {
                        m->func_def.receiver_type_args = names;
                        m->func_def.receiver_type_arg_count = count;
                    } else {
                        free(names);
                    }
                }
            }
            list_push(&program->program.funcs, m);
        }
        // Struct: IDENT IS {
        else if (at(p, TOK_IDENT) && peek_at(p, 1)->type == TOK_IS && peek_at(p, 2)->type == TOK_LBRACE) {
            list_push(&program->program.structs, parse_struct_def(p));
        }
        // Generic struct: IDENT OF IDENT [, IDENT ...] IS {
        //   Box of T is { ... }            -> name="Box", params=["T"]
        //   Map of K, V is { ... }         -> name="Map", params=["K", "V"]
        // parse_struct_def handles the params via parse_type_params
        // (which now consumes `of T [, V ...]` instead of `<T,V>`).
        else if (at(p, TOK_IDENT) && peek_at(p, 1)->type == TOK_OF &&
                 peek_at(p, 2)->type == TOK_IDENT) {
            // Shape:
            //   IDENT of IDENT is {                       (1 param, struct)
            //   IDENT of IDENT, IDENT is {                (2 params, struct)
            //   IDENT of IDENT is NEWLINE IDENT           (1 param, enum)
            //   IDENT of IDENT, IDENT is NEWLINE IDENT    (2 params, enum)
            //   IDENT of IDENT is IDENT(                  (1 param, enum)
            int look = 3;
            while (p->tokens[p->pos + look].type == TOK_COMMA) {
                look++;
                if (p->tokens[p->pos + look].type == TOK_IDENT) look++;
                else { list_push(&program->program.funcs, parse_func_def(p)); skip_newlines(p); continue; }
            }
            if (p->tokens[p->pos + look].type == TOK_IS &&
                p->tokens[p->pos + look + 1].type == TOK_LBRACE) {
                list_push(&program->program.structs, parse_struct_def(p));
            } else if (p->tokens[p->pos + look].type == TOK_IS) {
                // Possible generic enum. Skip past newlines after `is`
                // and look for an IDENT (variant). Same disambiguation
                // logic as the non-generic enum path below.
                int probe = look + 1;
                int is_enum = 0;
                if (p->tokens[p->pos + probe].type == TOK_NEWLINE) {
                    while (p->tokens[p->pos + probe].type == TOK_NEWLINE) probe++;
                    if (p->tokens[p->pos + probe].type == TOK_IDENT) is_enum = 1;
                } else if (p->tokens[p->pos + probe].type == TOK_IDENT &&
                           (p->tokens[p->pos + probe + 1].type == TOK_LPAREN ||
                            p->tokens[p->pos + probe + 1].type == TOK_NEWLINE ||
                            p->tokens[p->pos + probe + 1].type == TOK_PIPE)) {
                    is_enum = 1;
                }
                if (is_enum) {
                    list_push(&program->program.enums, parse_enum_def(p));
                } else {
                    list_push(&program->program.funcs, parse_func_def(p));
                }
            } else {
                // Shape didn't match — fall through to free-function path.
                list_push(&program->program.funcs, parse_func_def(p));
            }
        }
        // Enum: IDENT IS NEWLINE IDENT or IDENT IS IDENT( — not followed by {
        else if (at(p, TOK_IDENT) && peek_at(p, 1)->type == TOK_IS &&
                 peek_at(p, 2)->type != TOK_LBRACE &&
                 (peek_at(p, 2)->type == TOK_NEWLINE || peek_at(p, 2)->type == TOK_IDENT)) {
            // Could be enum or could be var decl in a function — only at top level it's enum
            // Check: if peek(2) is NEWLINE and peek(3) is IDENT, or peek(2) is IDENT and peek(3) is ( or NEWLINE
            int is_enum = 0;
            if (peek_at(p, 2)->type == TOK_NEWLINE) {
                // Skip newlines to find first variant
                int look = 3;
                while (p->tokens[p->pos + look].type == TOK_NEWLINE) look++;
                if (p->tokens[p->pos + look].type == TOK_IDENT) is_enum = 1;
            } else if (peek_at(p, 2)->type == TOK_IDENT &&
                       (peek_at(p, 3)->type == TOK_LPAREN || peek_at(p, 3)->type == TOK_NEWLINE || peek_at(p, 3)->type == TOK_PIPE)) {
                is_enum = 1;
            }
            if (is_enum) {
                list_push(&program->program.enums, parse_enum_def(p));
            } else {
                list_push(&program->program.funcs, parse_func_def(p));
            }
        } else {
            list_push(&program->program.funcs, parse_func_def(p));
        }
        skip_newlines(p);
    }
    return program;
}
