#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "optimizer.h"

// === Function Inlining ===
// Inline small leaf functions (single give expression, no loops, no calls)

static int is_inlinable(Node *func) {
    if (!func || func->type != NODE_FUNC_DEF) return 0;
    Node *body = func->func_def.body;
    if (body->block.stmts.count != 1) return 0;
    Node *stmt = body->block.stmts.items[0];
    // Must be a single give with an expression
    if (stmt->type != NODE_GIVE || !stmt->give.value) return 0;
    // The give expression must be simple (binary, unary, literal, ident)
    Node *expr = stmt->give.value;
    if (expr->type == NODE_BINARY || expr->type == NODE_UNARY ||
        expr->type == NODE_INT_LIT || expr->type == NODE_IDENT ||
        expr->type == NODE_BOOL_LIT) return 1;
    return 0;
}

// Deep copy a node (for inlining — need fresh copy per call site)
static Node *copy_node(Node *n);

static Node *copy_node(Node *n) {
    if (!n) return NULL;
    Node *c = calloc(1, sizeof(Node));
    *c = *n;
    switch (n->type) {
        case NODE_BINARY:
            c->binary.left = copy_node(n->binary.left);
            c->binary.right = copy_node(n->binary.right);
            break;
        case NODE_UNARY:
            c->unary.operand = copy_node(n->unary.operand);
            break;
        case NODE_CALL:
            c->call.args = malloc(n->call.arg_count * sizeof(Node *));
            for (int i = 0; i < n->call.arg_count; i++)
                c->call.args[i] = copy_node(n->call.args[i]);
            break;
        default: break;
    }
    return c;
}

// Substitute parameter names with argument expressions
static Node *substitute(Node *expr, char **param_names, Node **args, int count) {
    if (!expr) return NULL;
    switch (expr->type) {
        case NODE_IDENT:
            for (int i = 0; i < count; i++) {
                if (!strcmp(expr->ident.name, param_names[i]))
                    return copy_node(args[i]);
            }
            return expr;
        case NODE_BINARY:
            expr->binary.left = substitute(expr->binary.left, param_names, args, count);
            expr->binary.right = substitute(expr->binary.right, param_names, args, count);
            return expr;
        case NODE_UNARY:
            expr->unary.operand = substitute(expr->unary.operand, param_names, args, count);
            return expr;
        default:
            return expr;
    }
}

// Try to inline a call node — returns inlined expression or NULL
static Node *try_inline(Node *call, Node **funcs, int func_count) {
    if (call->type != NODE_CALL) return NULL;
    for (int i = 0; i < func_count; i++) {
        if (!funcs[i] || funcs[i]->type != NODE_FUNC_DEF) continue;
        if (strcmp(funcs[i]->func_def.name, call->call.name) != 0) continue;
        if (!is_inlinable(funcs[i])) return NULL;
        if (call->call.arg_count != funcs[i]->func_def.param_count) return NULL;
        // Inline: copy the give expression and substitute params with args
        Node *give_expr = funcs[i]->func_def.body->block.stmts.items[0]->give.value;
        Node *inlined = copy_node(give_expr);
        inlined = substitute(inlined, funcs[i]->func_def.param_names, call->call.args, call->call.arg_count);
        return inlined;
    }
    return NULL;
}

static Node *inline_expr(Node *n, Node **funcs, int func_count);

static Node *inline_expr(Node *n, Node **funcs, int func_count) {
    if (!n) return n;
    switch (n->type) {
        case NODE_CALL: {
            // First inline args
            for (int i = 0; i < n->call.arg_count; i++)
                n->call.args[i] = inline_expr(n->call.args[i], funcs, func_count);
            // Try to inline this call
            Node *inlined = try_inline(n, funcs, func_count);
            if (inlined) return inline_expr(inlined, funcs, func_count); // recurse on result
            return n;
        }
        case NODE_BINARY:
            n->binary.left = inline_expr(n->binary.left, funcs, func_count);
            n->binary.right = inline_expr(n->binary.right, funcs, func_count);
            return n;
        case NODE_UNARY:
            n->unary.operand = inline_expr(n->unary.operand, funcs, func_count);
            return n;
        case NODE_METHOD_CALL:
            n->method_call.object = inline_expr(n->method_call.object, funcs, func_count);
            for (int i = 0; i < n->method_call.arg_count; i++)
                n->method_call.args[i] = inline_expr(n->method_call.args[i], funcs, func_count);
            return n;
        case NODE_INDEX:
            n->index_access.object = inline_expr(n->index_access.object, funcs, func_count);
            n->index_access.index = inline_expr(n->index_access.index, funcs, func_count);
            return n;
        default:
            return n;
    }
}

static void inline_stmt(Node *n, Node **funcs, int func_count) {
    if (!n) return;
    switch (n->type) {
        case NODE_VAR_DECL:
            n->var_decl.value = inline_expr(n->var_decl.value, funcs, func_count);
            break;
        case NODE_ASSIGN:
            n->assign.value = inline_expr(n->assign.value, funcs, func_count);
            break;
        case NODE_GIVE:
            if (n->give.value) n->give.value = inline_expr(n->give.value, funcs, func_count);
            break;
        case NODE_IF:
            for (int i = 0; i < n->if_stmt.branch_count; i++) {
                n->if_stmt.conds[i] = inline_expr(n->if_stmt.conds[i], funcs, func_count);
                for (int j = 0; j < n->if_stmt.bodies[i]->block.stmts.count; j++)
                    inline_stmt(n->if_stmt.bodies[i]->block.stmts.items[j], funcs, func_count);
            }
            if (n->if_stmt.nah_body)
                for (int j = 0; j < n->if_stmt.nah_body->block.stmts.count; j++)
                    inline_stmt(n->if_stmt.nah_body->block.stmts.items[j], funcs, func_count);
            break;
        case NODE_THROUGH_RANGE:
            n->through_range.from = inline_expr(n->through_range.from, funcs, func_count);
            n->through_range.to = inline_expr(n->through_range.to, funcs, func_count);
            if (n->through_range.by) n->through_range.by = inline_expr(n->through_range.by, funcs, func_count);
            for (int j = 0; j < n->through_range.body->block.stmts.count; j++)
                inline_stmt(n->through_range.body->block.stmts.items[j], funcs, func_count);
            break;
        case NODE_INFI:
            if (n->infi.cond) n->infi.cond = inline_expr(n->infi.cond, funcs, func_count);
            for (int j = 0; j < n->infi.body->block.stmts.count; j++)
                inline_stmt(n->infi.body->block.stmts.items[j], funcs, func_count);
            break;
        case NODE_BLOCK:
            for (int j = 0; j < n->block.stmts.count; j++)
                inline_stmt(n->block.stmts.items[j], funcs, func_count);
            break;
        case NODE_CALL:
        case NODE_METHOD_CALL:
            inline_expr(n, funcs, func_count);
            break;
        case NODE_ASSERT:
            n->assert_stmt.condition = inline_expr(n->assert_stmt.condition, funcs, func_count);
            break;
        default: break;
    }
}

// === Constant Folding ===
// Evaluate constant expressions at compile time

static Node *fold_expr(Node *n) {
    if (!n) return n;

    switch (n->type) {
        case NODE_BINARY: {
            n->binary.left = fold_expr(n->binary.left);
            n->binary.right = fold_expr(n->binary.right);
            // If both sides are int literals, compute at compile time
            if (n->binary.left->type == NODE_INT_LIT && n->binary.right->type == NODE_INT_LIT) {
                long l = n->binary.left->int_lit.value;
                long r = n->binary.right->int_lit.value;
                long result = 0;
                int folded = 1;
                switch (n->binary.op) {
                    case TOK_PLUS: result = l + r; break;
                    case TOK_MINUS: result = l - r; break;
                    case TOK_STAR: result = l * r; break;
                    case TOK_SLASH: if (r != 0) result = l / r; else folded = 0; break;
                    case TOK_PERCENT: case TOK_MOD_WORD: if (r != 0) result = l % r; else folded = 0; break;
                    default: folded = 0; break;
                }
                if (folded) {
                    n->type = NODE_INT_LIT;
                    n->int_lit.value = result;
                }
            }
            return n;
        }
        case NODE_UNARY: {
            n->unary.operand = fold_expr(n->unary.operand);
            if (n->unary.operand->type == NODE_INT_LIT && n->unary.op == TOK_MINUS) {
                n->type = NODE_INT_LIT;
                n->int_lit.value = -n->unary.operand->int_lit.value;
            }
            return n;
        }
        case NODE_CALL: {
            for (int i = 0; i < n->call.arg_count; i++)
                n->call.args[i] = fold_expr(n->call.args[i]);
            return n;
        }
        case NODE_METHOD_CALL: {
            n->method_call.object = fold_expr(n->method_call.object);
            for (int i = 0; i < n->method_call.arg_count; i++)
                n->method_call.args[i] = fold_expr(n->method_call.args[i]);
            return n;
        }
        case NODE_INDEX: {
            n->index_access.object = fold_expr(n->index_access.object);
            n->index_access.index = fold_expr(n->index_access.index);
            return n;
        }
        default:
            return n;
    }
}

static void fold_stmt(Node *n) {
    if (!n) return;
    switch (n->type) {
        case NODE_VAR_DECL:
            n->var_decl.value = fold_expr(n->var_decl.value);
            break;
        case NODE_ASSIGN:
            n->assign.value = fold_expr(n->assign.value);
            break;
        case NODE_GIVE:
            if (n->give.value) n->give.value = fold_expr(n->give.value);
            break;
        case NODE_IF:
            for (int i = 0; i < n->if_stmt.branch_count; i++) {
                n->if_stmt.conds[i] = fold_expr(n->if_stmt.conds[i]);
                Node *body = n->if_stmt.bodies[i];
                for (int j = 0; j < body->block.stmts.count; j++)
                    fold_stmt(body->block.stmts.items[j]);
            }
            if (n->if_stmt.nah_body) {
                Node *body = n->if_stmt.nah_body;
                for (int j = 0; j < body->block.stmts.count; j++)
                    fold_stmt(body->block.stmts.items[j]);
            }
            break;
        case NODE_THROUGH_RANGE:
            n->through_range.from = fold_expr(n->through_range.from);
            n->through_range.to = fold_expr(n->through_range.to);
            if (n->through_range.by) n->through_range.by = fold_expr(n->through_range.by);
            for (int j = 0; j < n->through_range.body->block.stmts.count; j++)
                fold_stmt(n->through_range.body->block.stmts.items[j]);
            break;
        case NODE_THROUGH_IN:
            n->through_in.collection = fold_expr(n->through_in.collection);
            for (int j = 0; j < n->through_in.body->block.stmts.count; j++)
                fold_stmt(n->through_in.body->block.stmts.items[j]);
            break;
        case NODE_INFI:
            if (n->infi.cond) n->infi.cond = fold_expr(n->infi.cond);
            for (int j = 0; j < n->infi.body->block.stmts.count; j++)
                fold_stmt(n->infi.body->block.stmts.items[j]);
            break;
        case NODE_BLOCK:
            for (int j = 0; j < n->block.stmts.count; j++)
                fold_stmt(n->block.stmts.items[j]);
            break;
        case NODE_MATCH:
            n->match_expr.expr = fold_expr(n->match_expr.expr);
            for (int i = 0; i < n->match_expr.arm_count; i++) {
                Node *body = n->match_expr.arm_bodies[i];
                for (int j = 0; j < body->block.stmts.count; j++)
                    fold_stmt(body->block.stmts.items[j]);
            }
            break;
        case NODE_CALL:
        case NODE_METHOD_CALL:
            fold_expr(n);
            break;
        case NODE_ASSERT:
            n->assert_stmt.condition = fold_expr(n->assert_stmt.condition);
            break;
        default:
            break;
    }
}

// === Dead Code Elimination ===
// Remove statements after give/stop in a block

static void eliminate_dead_code(NodeList *stmts) {
    for (int i = 0; i < stmts->count; i++) {
        Node *s = stmts->items[i];
        if (s->type == NODE_GIVE || s->type == NODE_STOP) {
            // Everything after this is dead
            stmts->count = i + 1;
            break;
        }
        // Recurse into sub-blocks
        if (s->type == NODE_IF) {
            for (int b = 0; b < s->if_stmt.branch_count; b++)
                eliminate_dead_code(&s->if_stmt.bodies[b]->block.stmts);
            if (s->if_stmt.nah_body)
                eliminate_dead_code(&s->if_stmt.nah_body->block.stmts);
        }
        if (s->type == NODE_THROUGH_RANGE)
            eliminate_dead_code(&s->through_range.body->block.stmts);
        if (s->type == NODE_THROUGH_IN)
            eliminate_dead_code(&s->through_in.body->block.stmts);
        if (s->type == NODE_INFI)
            eliminate_dead_code(&s->infi.body->block.stmts);
        if (s->type == NODE_BLOCK)
            eliminate_dead_code(&s->block.stmts);
        if (s->type == NODE_MATCH) {
            for (int a = 0; a < s->match_expr.arm_count; a++)
                eliminate_dead_code(&s->match_expr.arm_bodies[a]->block.stmts);
        }
    }
}

// === Public API ===

void optimizer_run(Node *program) {
    Node **funcs = program->program.funcs.items;
    int func_count = program->program.funcs.count;

    // Pass 1: Inline small functions
    for (int i = 0; i < func_count; i++) {
        Node *f = program->program.funcs.items[i];
        Node *body = f->func_def.body;
        for (int j = 0; j < body->block.stmts.count; j++)
            inline_stmt(body->block.stmts.items[j], funcs, func_count);
    }
    for (int i = 0; i < program->program.tests.count; i++) {
        Node *t = program->program.tests.items[i];
        Node *body = t->test_def.body;
        for (int j = 0; j < body->block.stmts.count; j++)
            inline_stmt(body->block.stmts.items[j], funcs, func_count);
    }

    // Pass 2: Constant folding (benefits from inlining — inlined code may have foldable constants)
    for (int i = 0; i < func_count; i++) {
        Node *f = program->program.funcs.items[i];
        Node *body = f->func_def.body;
        for (int j = 0; j < body->block.stmts.count; j++)
            fold_stmt(body->block.stmts.items[j]);
        eliminate_dead_code(&body->block.stmts);
    }
    for (int i = 0; i < program->program.tests.count; i++) {
        Node *t = program->program.tests.items[i];
        Node *body = t->test_def.body;
        for (int j = 0; j < body->block.stmts.count; j++)
            fold_stmt(body->block.stmts.items[j]);
        eliminate_dead_code(&body->block.stmts);
    }
}
