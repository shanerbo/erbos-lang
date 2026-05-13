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

// --- Expression parsing ---

static Node *parse_primary(Parser *p) {
    int line = cur(p)->line;

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
        return n;
    }
    if (at(p, TOK_TRUE) || at(p, TOK_FALSE)) {
        Node *n = alloc_node(NODE_BOOL_LIT, line);
        n->bool_lit.value = at(p, TOK_TRUE);
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

            // Explicit type: check for type keywords or list/map (but NOT if followed by '(' — that's a constructor)
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
            }
            // If no value expression — auto-construct for list/map/task, error for others
            if (at(p, TOK_NEWLINE) || at(p, TOK_EOF)) {
                if (n->var_decl.type_name && (!strcmp(n->var_decl.type_name, "list") ||
                    !strcmp(n->var_decl.type_name, "map") || !strcmp(n->var_decl.type_name, "imap") || !strcmp(n->var_decl.type_name, "task"))) {
                    // Auto-create constructor: list() / map() / task()
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
                // field assignment
                p->pos++; // skip = or be
                Node *n = alloc_node(NODE_FIELD_ASSIGN, line);
                n->field_assign.object = expr->field_access.object;
                n->field_assign.field = expr->field_access.field;
                n->field_assign.value = parse_expr(p);
                return n;
            }
            // Check if it's a condition
            if (at(p, TOK_QUESTION)) {
                goto parse_if_from_expr;
            }
            return expr;
            parse_if_from_expr: (void)0;
            {
                eat(p, TOK_QUESTION);
                Node *n = alloc_node(NODE_IF, line);
                int cap = 4;
                n->if_stmt.conds = malloc(cap * sizeof(Node *));
                n->if_stmt.bodies = malloc(cap * sizeof(Node *));
                n->if_stmt.branch_count = 0;
                n->if_stmt.nah_body = NULL;
                n->if_stmt.conds[0] = expr;
                n->if_stmt.bodies[0] = parse_block(p);
                n->if_stmt.branch_count = 1;
                // Check for else-if or nah (may be after newlines)
                while (1) {
                    int saved = p->pos;
                    skip_newlines(p);
                    if (at(p, TOK_NAH)) { p->pos++; n->if_stmt.nah_body = parse_block(p); break; }
                    // Check if next is a condition ending in ?{ (else-if)
                    if (at(p, TOK_EOF) || at(p, TOK_RBRACE)) { p->pos = saved; break; }
                    // Try to see if this looks like an else-if: parse expr, expect ?
                    // If not, restore position and break
                    int test_pos = p->pos;
                    // Heuristic: if we can't find ? before newline/rbrace, it's not else-if
                    int found_q = 0;
                    while (p->tokens[test_pos].type != TOK_NEWLINE && p->tokens[test_pos].type != TOK_EOF && p->tokens[test_pos].type != TOK_RBRACE) {
                        if (p->tokens[test_pos].type == TOK_QUESTION) { found_q = 1; break; }
                        test_pos++;
                    }
                    if (!found_q) { p->pos = saved; break; }
                    Node *c = parse_expr(p);
                    eat(p, TOK_QUESTION);
                    Node *b = parse_block(p);
                    if (n->if_stmt.branch_count >= cap) { cap *= 2; n->if_stmt.conds = realloc(n->if_stmt.conds, cap * sizeof(Node *)); n->if_stmt.bodies = realloc(n->if_stmt.bodies, cap * sizeof(Node *)); }
                    n->if_stmt.conds[n->if_stmt.branch_count] = c;
                    n->if_stmt.bodies[n->if_stmt.branch_count] = b;
                    n->if_stmt.branch_count++;
                }
                return n;
            }
        }
        // Otherwise parse as expression
        Node *expr = parse_expr(p);
        if (at(p, TOK_QUESTION)) {
            eat(p, TOK_QUESTION);
            Node *n = alloc_node(NODE_IF, line);
            int cap = 4;
            n->if_stmt.conds = malloc(cap * sizeof(Node *));
            n->if_stmt.bodies = malloc(cap * sizeof(Node *));
            n->if_stmt.branch_count = 0;
            n->if_stmt.nah_body = NULL;
            n->if_stmt.conds[0] = expr;
            n->if_stmt.bodies[0] = parse_block(p);
            n->if_stmt.branch_count = 1;
            while (1) {
                int saved = p->pos;
                skip_newlines(p);
                if (at(p, TOK_NAH)) { p->pos++; n->if_stmt.nah_body = parse_block(p); break; }
                if (at(p, TOK_EOF) || at(p, TOK_RBRACE)) { p->pos = saved; break; }
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
                if (n->if_stmt.branch_count >= cap) { cap *= 2; n->if_stmt.conds = realloc(n->if_stmt.conds, cap * sizeof(Node *)); n->if_stmt.bodies = realloc(n->if_stmt.bodies, cap * sizeof(Node *)); }
                n->if_stmt.conds[n->if_stmt.branch_count] = c;
                n->if_stmt.bodies[n->if_stmt.branch_count] = b;
                n->if_stmt.branch_count++;
            }
            return n;
        }
        return expr;
    }

    // Bare expression
    Node *expr = parse_expr(p);
    if (at(p, TOK_QUESTION)) {
        eat(p, TOK_QUESTION);
        Node *n = alloc_node(NODE_IF, line);
        int cap = 4;
        n->if_stmt.conds = malloc(cap * sizeof(Node *));
        n->if_stmt.bodies = malloc(cap * sizeof(Node *));
        n->if_stmt.branch_count = 0;
        n->if_stmt.nah_body = NULL;
        n->if_stmt.conds[0] = expr;
        n->if_stmt.bodies[0] = parse_block(p);
        n->if_stmt.branch_count = 1;
        while (1) {
            int saved = p->pos;
            skip_newlines(p);
            if (at(p, TOK_NAH)) { p->pos++; n->if_stmt.nah_body = parse_block(p); break; }
            if (at(p, TOK_EOF) || at(p, TOK_RBRACE)) { p->pos = saved; break; }
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
            if (n->if_stmt.branch_count >= cap) { cap *= 2; n->if_stmt.conds = realloc(n->if_stmt.conds, cap * sizeof(Node *)); n->if_stmt.bodies = realloc(n->if_stmt.bodies, cap * sizeof(Node *)); }
            n->if_stmt.conds[n->if_stmt.branch_count] = c;
            n->if_stmt.bodies[n->if_stmt.branch_count] = b;
            n->if_stmt.branch_count++;
        }
        return n;
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

static Node *parse_struct_def(Parser *p) {
    int line = cur(p)->line;
    char *name = eat(p, TOK_IDENT)->value;
    eat(p, TOK_IS);
    eat(p, TOK_LBRACE);
    skip_newlines(p);

    Node *n = alloc_node(NODE_STRUCT_DEF, line);
    n->struct_def.name = name;
    int cap = 8;
    n->struct_def.field_names = malloc(cap * sizeof(char *));
    n->struct_def.field_types = malloc(cap * sizeof(char *));
    n->struct_def.field_count = 0;

    while (!at(p, TOK_RBRACE) && !at(p, TOK_EOF)) {
        skip_newlines(p);
        if (at(p, TOK_RBRACE)) break;
        char *fname = eat(p, TOK_IDENT)->value;
        char *ftype = cur(p)->value;
        p->pos++;
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
            char *ptype = cur(p)->value;
            p->pos++;
            n->func_def.param_names[n->func_def.param_count] = pname;
            n->func_def.param_types[n->func_def.param_count] = ptype;
            n->func_def.param_is_ref[n->func_def.param_count] = is_ref;
            n->func_def.param_count++;
            if (at(p, TOK_COMMA)) p->pos++;
        }
        eat(p, TOK_RPAREN);
    }

    // Return type
    if (is_type_token(p) && !at(p, TOK_LBRACE)) {
        n->func_def.return_type = cur(p)->value;
        p->pos++;
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

    skip_newlines(p);
    while (!at(p, TOK_EOF)) {
        // Struct: IDENT IS {
        if (at(p, TOK_IDENT) && peek_at(p, 1)->type == TOK_IS && peek_at(p, 2)->type == TOK_LBRACE) {
            list_push(&program->program.structs, parse_struct_def(p));
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
