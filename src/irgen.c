#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "irgen.h"
#include "token.h"

typedef struct {
    IRFunc *func;       // current function being generated
    IRBlock *block;     // current basic block
    int vreg_next;      // next virtual register number
    int label_next;     // next label number
    // Local variable → vreg mapping
    char **local_names;
    VReg *local_vregs;
    int local_count;
    int local_cap;
} IRGenCtx;

static VReg new_vreg(IRGenCtx *c) { return c->vreg_next++; }
static int new_label(IRGenCtx *c) { return c->label_next++; }

static void emit(IRGenCtx *c, IRInst inst) {
    IRBlock *b = c->block;
    if (b->count >= b->cap) {
        b->cap = b->cap ? b->cap * 2 : 16;
        b->insts = realloc(b->insts, b->cap * sizeof(IRInst));
    }
    b->insts[b->count++] = inst;
}

static void set_local(IRGenCtx *c, const char *name, VReg v) {
    for (int i = 0; i < c->local_count; i++) {
        if (!strcmp(c->local_names[i], name)) { c->local_vregs[i] = v; return; }
    }
    if (c->local_count >= c->local_cap) {
        c->local_cap = c->local_cap ? c->local_cap * 2 : 16;
        c->local_names = realloc(c->local_names, c->local_cap * sizeof(char *));
        c->local_vregs = realloc(c->local_vregs, c->local_cap * sizeof(VReg));
    }
    c->local_names[c->local_count] = (char *)name;
    c->local_vregs[c->local_count] = v;
    c->local_count++;
}

static VReg get_local(IRGenCtx *c, const char *name) {
    for (int i = c->local_count - 1; i >= 0; i--)
        if (!strcmp(c->local_names[i], name)) return c->local_vregs[i];
    return -1;
}

static IRBlock *new_block(IRGenCtx *c) {
    IRFunc *f = c->func;
    if (f->block_count >= f->block_cap) {
        f->block_cap = f->block_cap ? f->block_cap * 2 : 8;
        f->blocks = realloc(f->blocks, f->block_cap * sizeof(IRBlock));
    }
    IRBlock *b = &f->blocks[f->block_count++];
    b->label = new_label(c);
    b->insts = NULL;
    b->count = 0;
    b->cap = 0;
    return b;
}

// Forward declarations
static VReg gen_expr(IRGenCtx *c, Node *n);
static void gen_stmt(IRGenCtx *c, Node *n);

static VReg gen_expr(IRGenCtx *c, Node *n) {
    if (!n) return -1;
    switch (n->type) {
        case NODE_INT_LIT: {
            VReg dst = new_vreg(c);
            emit(c, (IRInst){.op = IR_CONST, .dst = dst, .imm = n->int_lit.value});
            return dst;
        }
        case NODE_STR_LIT: {
            VReg dst = new_vreg(c);
            emit(c, (IRInst){.op = IR_LOAD_STR, .dst = dst, .str = n->str_lit.value});
            return dst;
        }
        case NODE_BOOL_LIT: {
            VReg dst = new_vreg(c);
            emit(c, (IRInst){.op = IR_CONST, .dst = dst, .imm = n->bool_lit.value});
            return dst;
        }
        case NODE_IDENT: {
            VReg v = get_local(c, n->ident.name);
            if (v >= 0) return v;
            // Unknown — return a placeholder
            VReg dst = new_vreg(c);
            emit(c, (IRInst){.op = IR_CONST, .dst = dst, .imm = 0});
            return dst;
        }
        case NODE_BINARY: {
            VReg a = gen_expr(c, n->binary.left);
            VReg b = gen_expr(c, n->binary.right);
            VReg dst = new_vreg(c);
            IROp op;
            switch (n->binary.op) {
                case TOK_PLUS: op = IR_ADD; break;
                case TOK_MINUS: op = IR_SUB; break;
                case TOK_STAR: op = IR_MUL; break;
                case TOK_SLASH: op = IR_DIV; break;
                case TOK_PERCENT: case TOK_MOD_WORD: op = IR_MOD; break;
                case TOK_EQ: case TOK_EQ_WORD: op = IR_CMP_EQ; break;
                case TOK_NEQ: case TOK_NE_WORD: op = IR_CMP_NE; break;
                case TOK_LT: case TOK_LT_WORD: op = IR_CMP_LT; break;
                case TOK_GT: case TOK_GT_WORD: op = IR_CMP_GT; break;
                case TOK_LTE: case TOK_LE_WORD: op = IR_CMP_LE; break;
                case TOK_GTE: case TOK_GE_WORD: op = IR_CMP_GE; break;
                case TOK_AND: op = IR_AND; break;
                case TOK_OR: op = IR_OR; break;
                default: op = IR_ADD; break;
            }
            emit(c, (IRInst){.op = op, .dst = dst, .a = a, .b = b});
            return dst;
        }
        case NODE_UNARY: {
            VReg a = gen_expr(c, n->unary.operand);
            VReg dst = new_vreg(c);
            if (n->unary.op == TOK_MINUS)
                emit(c, (IRInst){.op = IR_NEG, .dst = dst, .a = a});
            else
                emit(c, (IRInst){.op = IR_NOT, .dst = dst, .a = a});
            return dst;
        }
        case NODE_CALL: {
            // Generate args
            VReg *args = NULL;
            if (n->call.arg_count > 0)
                args = malloc(n->call.arg_count * sizeof(VReg));
            for (int i = 0; i < n->call.arg_count; i++)
                args[i] = gen_expr(c, n->call.args[i]);
            VReg dst = new_vreg(c);
            emit(c, (IRInst){.op = IR_CALL, .dst = dst, .str = (char *)n->call.name, .args = args, .arg_count = n->call.arg_count});
            return dst;
        }
        default: {
            // Fallback for complex nodes — emit as call to codegen later
            VReg dst = new_vreg(c);
            emit(c, (IRInst){.op = IR_CONST, .dst = dst, .imm = 0});
            return dst;
        }
    }
}

static void gen_stmt(IRGenCtx *c, Node *n) {
    if (!n) return;
    switch (n->type) {
        case NODE_VAR_DECL: {
            VReg v = gen_expr(c, n->var_decl.value);
            set_local(c, n->var_decl.name, v);
            break;
        }
        case NODE_ASSIGN: {
            VReg v = gen_expr(c, n->assign.value);
            set_local(c, n->assign.name, v);
            break;
        }
        case NODE_GIVE: {
            if (n->give.value) {
                VReg v = gen_expr(c, n->give.value);
                emit(c, (IRInst){.op = IR_RET, .a = v});
            } else {
                emit(c, (IRInst){.op = IR_RET_VOID});
            }
            break;
        }
        case NODE_CALL:
        case NODE_METHOD_CALL:
            gen_expr(c, n);
            break;
        default:
            // TODO: control flow (if, loops, match)
            break;
    }
}

IRProgram *irgen_generate(Node *program) {
    IRProgram *ir = calloc(1, sizeof(IRProgram));

    for (int i = 0; i < program->program.funcs.count; i++) {
        Node *f = program->program.funcs.items[i];

        if (ir->func_count >= ir->func_cap) {
            ir->func_cap = ir->func_cap ? ir->func_cap * 2 : 8;
            ir->funcs = realloc(ir->funcs, ir->func_cap * sizeof(IRFunc));
        }
        IRFunc *irf = &ir->funcs[ir->func_count++];
        memset(irf, 0, sizeof(IRFunc));
        irf->name = f->func_def.name;
        irf->param_count = f->func_def.param_count;

        IRGenCtx ctx = {0};
        ctx.func = irf;
        ctx.block = new_block(&ctx);

        // Register params as locals
        for (int j = 0; j < f->func_def.param_count; j++) {
            VReg pv = new_vreg(&ctx);
            set_local(&ctx, f->func_def.param_names[j], pv);
        }

        // Generate body
        Node *body = f->func_def.body;
        for (int j = 0; j < body->block.stmts.count; j++)
            gen_stmt(&ctx, body->block.stmts.items[j]);

        irf->vreg_count = ctx.vreg_next;
        free(ctx.local_names);
        free(ctx.local_vregs);
    }

    return ir;
}
