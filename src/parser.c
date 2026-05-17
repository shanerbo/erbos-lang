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
                mc->method_call.arg_count = 0;
                if (!at(p, TOK_RPAREN)) {
                    mc->method_call.args[mc->method_call.arg_count++] = parse_expr(p);
                    while (at(p, TOK_COMMA)) {
                        p->pos++;
                        if (mc->method_call.arg_count >= cap) {
                            cap *= 2;
                            mc->method_call.args = realloc(mc->method_call.args, cap * sizeof(Node *));
                        }
                        mc->method_call.args[mc->method_call.arg_count++] = parse_expr(p);
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

        // Parametric constructor: `Box of int ()` or `Map of str to int ()`.
        // We disambiguate from a generic non-call expression with `of`
        // inside (e.g. a hypothetical `xs is List of int` in a place
        // that expects an expression) by looking ahead: a parametric
        // constructor's type-arg list is followed by '('. The shape
        // is IDENT `of` IDENT [`to` IDENT] `(`, optionally with the
        // type args themselves being parametric (recursing through
        // more `of` / `to`).
        //
        // Lookahead walker: starting at peek(1), consume only
        // type-position tokens (IDENT / primitive keywords / `of` /
        // `to`). Stop at the first token that can't be in a type
        // position. If that token is `(`, it's a constructor call.
        if (peek_at(p, 1)->type == TOK_OF) {
            int look = 1;
            int ok = 1;
            int saw_type_token = 0;
            while (ok) {
                TokenType t = p->tokens[p->pos + look].type;
                if (t == TOK_OF || t == TOK_TO) {
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
            if (ok && saw_type_token && p->tokens[p->pos + look].type == TOK_LPAREN) {
                // Parametric constructor confirmed. parse_type_name reads
                // `Box of int` as `Box<int>` (legacy internal form) for
                // the monomorphizer, then we consume `(` and arguments.
                char *full = parse_type_name(p);
                eat(p, TOK_LPAREN);
                Node *n = alloc_node(NODE_CALL, line);
                n->call.name = full;
                int cap = 4;
                n->call.args = malloc(cap * sizeof(Node *));
                n->call.arg_count = 0;
                if (!at(p, TOK_RPAREN)) {
                    if (at(p, TOK_REF)) p->pos++;
                    n->call.args[n->call.arg_count++] = parse_expr(p);
                    while (at(p, TOK_COMMA)) {
                        p->pos++;
                        if (n->call.arg_count >= cap) {
                            cap *= 2;
                            n->call.args = realloc(n->call.args, cap * sizeof(Node *));
                        }
                        if (at(p, TOK_REF)) p->pos++;
                        n->call.args[n->call.arg_count++] = parse_expr(p);
                    }
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
                            mc->method_call.arg_count = 0;
                            if (!at(p, TOK_RPAREN)) {
                                mc->method_call.args[mc->method_call.arg_count++] = parse_expr(p);
                                while (at(p, TOK_COMMA)) {
                                    p->pos++;
                                    if (mc->method_call.arg_count >= mcap) {
                                        mcap *= 2;
                                        mc->method_call.args = realloc(mc->method_call.args, mcap * sizeof(Node *));
                                    }
                                    mc->method_call.args[mc->method_call.arg_count++] = parse_expr(p);
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
            n->call.arg_count = 0;
            if (!at(p, TOK_RPAREN)) {
                if (at(p, TOK_REF)) p->pos++; // skip ref at call site
                n->call.args[n->call.arg_count++] = parse_expr(p);
                while (at(p, TOK_COMMA)) {
                    p->pos++;
                    if (n->call.arg_count >= cap) {
                        cap *= 2;
                        n->call.args = realloc(n->call.args, cap * sizeof(Node *));
                    }
                    if (at(p, TOK_REF)) p->pos++; // skip ref at call site
                    n->call.args[n->call.arg_count++] = parse_expr(p);
                }
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
                    mc->method_call.arg_count = 0;
                    if (!at(p, TOK_RPAREN)) {
                        mc->method_call.args[mc->method_call.arg_count++] = parse_expr(p);
                        while (at(p, TOK_COMMA)) {
                            p->pos++;
                            if (mc->method_call.arg_count >= cap) {
                                cap *= 2;
                                mc->method_call.args = realloc(mc->method_call.args, cap * sizeof(Node *));
                            }
                            mc->method_call.args[mc->method_call.arg_count++] = parse_expr(p);
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

    // Assert statement
    if (at(p, TOK_ASSERT)) {
        p->pos++;
        eat(p, TOK_LPAREN);
        Node *n = alloc_node(NODE_ASSERT, line);
        n->assert_stmt.condition = parse_expr(p);
        eat(p, TOK_RPAREN);
        return n;
    }
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

    if (at(p, TOK_NOMUT)) {
        p->pos++;
        char *name = eat(p, TOK_IDENT)->value;
        eat(p, TOK_IS);
        Node *n = alloc_node(NODE_VAR_DECL, line);
        n->var_decl.name = name;
        n->var_decl.is_nomut = 1;
        n->var_decl.type_name = NULL;
        if (at(p, TOK_INT) || at(p, TOK_STR_TYPE) || at(p, TOK_BOOL) || at(p, TOK_IDENT)) {
            // Check if next is a type keyword or struct name followed by value
            if (at(p, TOK_INT) || at(p, TOK_STR_TYPE) || at(p, TOK_BOOL)) {
                n->var_decl.type_name = cur(p)->value;
                p->pos++;
            }
        }
        n->var_decl.value = parse_expr(p);
        return n;
    }

    if (at(p, TOK_IDENT)) {
        // var decl: IDENT IS ...
        if (peek_at(p, 1)->type == TOK_IS) {
            char *name = cur(p)->value;
            p->pos++;
            eat(p, TOK_IS);
            Node *n = alloc_node(NODE_VAR_DECL, line);
            n->var_decl.name = name;
            n->var_decl.is_nomut = 0;
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
            // (a) Lowercase legacy keywords (`list of T`, `map of K to V`,
            //     `imap of int to V`, `task`) — paths through the
            //     existing branches below; auto-constructed if no value
            //     follows.
            //
            // (b) Primitive-named types (`int`, `str`, `bool`).
            //
            // (c) User-generic / capitalized struct types in the
            //     word-style form: `IDENT [of TYPE [to TYPE]]`. With no
            //     value expression after, this auto-constructs to
            //     `IDENT(...)` — same convenience as (a), so users
            //     can write `xs is List of int` without trailing `()`.
            //     Triggered only when the IDENT isn't immediately
            //     followed by `(` (which would be a constructor call
            //     parsed as a regular expression).
            int explicit_type_consumed = 0;
            if (at(p, TOK_INT) || at(p, TOK_STR_TYPE) || at(p, TOK_BOOL) ||
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
                        if (at(p, TOK_TO)) {
                            p->pos++; // skip 'to'
                            n->var_decl.val_type_name = cur(p)->value;
                            p->pos++; // skip val type
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
                //   m  is Map of int to int
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
                if (at(p, TOK_LPAREN)) {
                    // Explicit constructor call follows; rewind so
                    // parse_expr below sees the full
                    // `IDENT of ... (...)` shape.
                    p->pos = saved_pos;
                    free(parsed_type);
                } else {
                    n->var_decl.type_name = parsed_type;
                    explicit_type_consumed = 1;
                }
            }
            // If no value expression — auto-construct.
            if (at(p, TOK_NEWLINE) || at(p, TOK_EOF)) {
                if (explicit_type_consumed && n->var_decl.type_name) {
                    // Auto-create the constructor call. For lowercase
                    // legacy keywords (list / map / imap / task) the
                    // call name is the keyword spelling (irgen remaps
                    // them to `_list_new` / `_map_new` etc.). For user
                    // types the call name is the parsed type string,
                    // which goes through monomorphization just like
                    // any other constructor call.
                    Node *call = alloc_node(NODE_CALL, line);
                    call->call.name = n->var_decl.type_name;
                    call->call.args = NULL;
                    call->call.arg_count = 0;
                    n->var_decl.value = call;
                } else {
                    fprintf(stderr, "%s:%d: error: variable '%s' must be initialized with a value\n", p->filename, line, name);
                    exit(1);
                }
            } else {
                n->var_decl.value = parse_expr(p);
            }
            return n;
        }
        // assignment: IDENT be ...
        if (peek_at(p, 1)->type == TOK_BE) {
            char *name = cur(p)->value;
            p->pos++;
            p->pos++; // skip be
            Node *n = alloc_node(NODE_ASSIGN, line);
            n->assign.name = name;
            n->assign.value = parse_expr(p);
            return n;
        }
        // field assign: IDENT.field = ... 
        if (peek_at(p, 1)->type == TOK_DOT) {
            // Could be field assign or method call or just expr
            // Parse as expression first
            Node *expr = parse_expr(p);
            if (at(p, TOK_ASSIGN) || at(p, TOK_BE)) {
                // field assignment — verify expr is actually a field access
                if (expr->type != NODE_FIELD_ACCESS) {
                    fprintf(stderr, "%s:%d: error: invalid assignment target\n", p->filename, line);
                    exit(1);
                }
                p->pos++; // skip = or be
                Node *n = alloc_node(NODE_FIELD_ASSIGN, line);
                n->field_assign.object = expr->field_access.object;
                n->field_assign.field = expr->field_access.field;
                n->field_assign.value = parse_expr(p);
                return n;
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
    eat(p, TOK_IS);
    skip_newlines(p);

    Node *n = alloc_node(NODE_ENUM_DEF, line);
    n->enum_def.name = name;
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
                n->enum_def.variant_field_types[vi][fc] = cur(p)->value;
                p->pos++;
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
// Handles bare names ("int", "str", "Counter") and parametric types
// ("Box of int", "Map of str to int", "List of List of int").
//
// Word-style generics (P3.1):
//   - `of T` introduces a single type parameter: "Box of int".
//   - `of K to V` introduces a key->value pair: "Map of str to int".
//   - Both `of` and `to` are right-associative, so
//     "List of List of int" parses as "List of (List of int)" and
//     "Map of str to List of int" parses as "Map of str to (List of int)".
//   - There are NO commas, NO parens, and NO `<>` in type position.
//
// The output uses the legacy `<>` internal representation that the
// monomorphizer pass parses (it splits on `<`, top-level `,`, and
// matching `>`). Word-style "Box of int" emits "Box<int>"; "Map of
// str to int" emits "Map<str,int>"; "List of List of int" emits
// "List<List<int>>". This keeps the monomorph + mangling code below
// unchanged — only the surface syntax differs from the original P3.
static char *parse_type_name(Parser *p) {
    if (!at(p, TOK_IDENT) && !at(p, TOK_INT) && !at(p, TOK_STR_TYPE) &&
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
    // Word-style chain: optional `of T [to V]` continuations.
    //   "Box of int"          -> "Box<int>"
    //   "Map of str to int"   -> "Map<str,int>"
    //   "List of List of int" -> "List<List<int>>"
    // The first `of` opens a `<...>`; subsequent `to` separators
    // become `,`; nested `of` recurses into another bracket pair.
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
        if (at(p, TOK_TO)) {
            p->pos++; // consume `to`
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
        if (len + 1 < (int)sizeof(buf)) buf[len++] = '>';
        buf[len] = '\0';
    }
    return strdup(buf);
}

// Parse an optional `of T [to V]` type-parameter list immediately
// after a type name in a struct or method declaration head.
// Examples:
//   Box of T is { ... }              -> ["T"]
//   Map of K to V is { ... }         -> ["K", "V"]
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
    if (at(p, TOK_TO)) {
        p->pos++; // consume `to`
        if (count >= cap) {
            cap *= 2;
            names = realloc(names, cap * sizeof(char *));
        }
        names[count++] = eat(p, TOK_IDENT)->value;
    }
    *out_names = names;
    return count;
}

static Node *parse_struct_def(Parser *p) {
    int line = cur(p)->line;
    char *name = eat(p, TOK_IDENT)->value;

    // Optional generic-parameter list: `Map<K, V> is { ... }`.
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
        // primitive type keyword (str / int / bool) instead of an
        // identifier. We accept those and synthesize the receiver_type
        // string from the keyword spelling. This is what allows
        // `str.len(self str) int { ... }` to register `_str_len` as a
        // method that `"hello".len()` dispatches to.
        else if ((at(p, TOK_IDENT) || at(p, TOK_STR_TYPE) || at(p, TOK_INT) ||
                  at(p, TOK_BOOL)) &&
                 peek_at(p, 1)->type == TOK_DOT &&
                 peek_at(p, 2)->type == TOK_IDENT && peek_at(p, 3)->type == TOK_LPAREN) {
            char *recv;
            if (at(p, TOK_STR_TYPE))      { recv = strdup("str");  p->pos++; }
            else if (at(p, TOK_INT))      { recv = strdup("int");  p->pos++; }
            else if (at(p, TOK_BOOL))     { recv = strdup("bool"); p->pos++; }
            else                          { recv = eat(p, TOK_IDENT)->value; }
            eat(p, TOK_DOT);
            // parse_func_def reads from the method-name token onward.
            Node *m = parse_func_def(p);
            m->func_def.receiver_type = recv;
            // Extract receiver_type_args from the first param's type
            // string. parse_type_name emits the legacy <>-bracketed
            // form for the monomorphizer's benefit; we re-parse it
            // here to lift the type variable names out.
            //
            // Examples:
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
                        // receiver is always `Type of T` or `Type of K to V`
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
        // Generic struct: IDENT OF IDENT [TO IDENT] IS {
        //   Box of T is { ... }            -> name="Box", params=["T"]
        //   Map of K to V is { ... }       -> name="Map", params=["K", "V"]
        // parse_struct_def handles the params via parse_type_params
        // (which now consumes `of T [to V]` instead of `<T,V>`).
        else if (at(p, TOK_IDENT) && peek_at(p, 1)->type == TOK_OF &&
                 peek_at(p, 2)->type == TOK_IDENT) {
            // Look past the params to confirm `is {`. Shape:
            //   IDENT of IDENT is {                      (1 param)
            //   IDENT of IDENT to IDENT is {             (2 params)
            int look = 3;
            if (p->tokens[p->pos + look].type == TOK_TO) {
                look++;
                if (p->tokens[p->pos + look].type == TOK_IDENT) look++;
                else { list_push(&program->program.funcs, parse_func_def(p)); skip_newlines(p); continue; }
            }
            if (p->tokens[p->pos + look].type == TOK_IS &&
                p->tokens[p->pos + look + 1].type == TOK_LBRACE) {
                list_push(&program->program.structs, parse_struct_def(p));
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
