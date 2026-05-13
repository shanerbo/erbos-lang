#include <stdlib.h>
#include <string.h>
#include "optimizer.h"

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
    // Optimize functions
    for (int i = 0; i < program->program.funcs.count; i++) {
        Node *f = program->program.funcs.items[i];
        Node *body = f->func_def.body;
        // Constant folding
        for (int j = 0; j < body->block.stmts.count; j++)
            fold_stmt(body->block.stmts.items[j]);
        // Dead code elimination
        eliminate_dead_code(&body->block.stmts);
    }
    // Optimize test bodies
    for (int i = 0; i < program->program.tests.count; i++) {
        Node *t = program->program.tests.items[i];
        Node *body = t->test_def.body;
        for (int j = 0; j < body->block.stmts.count; j++)
            fold_stmt(body->block.stmts.items[j]);
        eliminate_dead_code(&body->block.stmts);
    }
}
