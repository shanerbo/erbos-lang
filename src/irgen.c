#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "irgen.h"
#include "token.h"

typedef struct {
    IRFunc *func;
    IRBlock *block;
    int vreg_next;
    int label_next;
    char **local_names;
    VReg *local_vregs;
    int *local_is_mut;  // 1 if variable was reassigned (needs stack slot)
    int *local_slots;   // stack slot index for mutable vars
    int local_count;
    int local_cap;
    int slot_next;      // next stack slot
    // Loop context for stop/skip
    int loop_start;
    int loop_end;
    // Program ref for struct/enum info
    Node *program;
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
        if (!strcmp(c->local_names[i], name)) {
            c->local_vregs[i] = v;
            if (c->local_slots[i] < 0)
                c->local_slots[i] = c->slot_next++;
            emit(c, (IRInst){.op = IR_STORE_LOCAL, .a = v, .imm = c->local_slots[i]});
            return;
        }
    }
    if (c->local_count >= c->local_cap) {
        c->local_cap = c->local_cap ? c->local_cap * 2 : 16;
        c->local_names = realloc(c->local_names, c->local_cap * sizeof(char *));
        c->local_vregs = realloc(c->local_vregs, c->local_cap * sizeof(VReg));
        c->local_is_mut = realloc(c->local_is_mut, c->local_cap * sizeof(int));
        c->local_slots = realloc(c->local_slots, c->local_cap * sizeof(int));
    }
    c->local_names[c->local_count] = (char *)name;
    c->local_vregs[c->local_count] = v;
    c->local_is_mut[c->local_count] = 0;
    c->local_slots[c->local_count] = c->slot_next++;
    // Store initial value
    emit(c, (IRInst){.op = IR_STORE_LOCAL, .a = v, .imm = c->local_slots[c->local_count]});
    c->local_count++;
}

static VReg get_local(IRGenCtx *c, const char *name) {
    for (int i = c->local_count - 1; i >= 0; i--) {
        if (!strcmp(c->local_names[i], name)) {
            // Always load from stack — safe across blocks
            VReg dst = new_vreg(c);
            emit(c, (IRInst){.op = IR_LOAD_LOCAL, .dst = dst, .imm = c->local_slots[i]});
            return dst;
        }
    }
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

static void switch_block(IRGenCtx *c, IRBlock *b) { c->block = b; }

static VReg gen_expr(IRGenCtx *c, Node *n);
static void gen_stmt(IRGenCtx *c, Node *n);
static void gen_block(IRGenCtx *c, Node *block);

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
            // Check if it's a struct constructor
            int is_struct = 0;
            int field_count = 0;
            if (c->program) {
                for (int si = 0; si < c->program->program.structs.count; si++) {
                    Node *s = c->program->program.structs.items[si];
                    if (!strcmp(s->struct_def.name, n->call.name)) {
                        is_struct = 1;
                        field_count = s->struct_def.field_count;
                        break;
                    }
                }
            }
            if (is_struct) {
                // Call _alloc_StructName
                char alloc_name[128];
                snprintf(alloc_name, sizeof(alloc_name), "alloc_%s", n->call.name);
                VReg ptr = new_vreg(c);
                emit(c, (IRInst){.op = IR_CALL, .dst = ptr, .str = strdup(alloc_name), .args = NULL, .arg_count = 0});
                // Store args as fields
                for (int i = 0; i < n->call.arg_count && i < field_count; i++) {
                    VReg val = gen_expr(c, n->call.args[i]);
                    emit(c, (IRInst){.op = IR_STORE, .a = ptr, .b = val, .imm = i * 8});
                }
                return ptr;
            }
            // Remap built-in calls to their real emitted symbols.
            // Mirrors the dispatch in src/codegen.c so the IR backend
            // produces calls to symbols that actually exist.
            const char *call_name = n->call.name;
            if (!strcmp(call_name, "list"))      call_name = "list_new";
            else if (!strcmp(call_name, "map"))  call_name = "map_new";
            else if (!strcmp(call_name, "imap")) call_name = "imap_new";
            else if (!strcmp(call_name, "task")) {
                // task() returns a placeholder handle (0). No bl needed.
                VReg dst = new_vreg(c);
                emit(c, (IRInst){.op = IR_CONST, .dst = dst, .imm = 0});
                return dst;
            }
            // Universal len(): for strings call _str_len; for lists/maps
            // load count from offset 8 (their shared header layout).
            // We can't always know the runtime type here at the IR level
            // (the checker tagged resolved_type on the arg, but it isn't
            // re-read here), so dispatch via a single _len helper which
            // does the same heuristic as direct codegen at runtime.
            else if (!strcmp(call_name, "len")) {
                if (n->call.arg_count == 1) {
                    Node *arg = n->call.args[0];
                    // Heuristic match on resolved_type set by the checker:
                    //   3 = list/map (load offset 8)
                    //   5 = string (call _str_len)
                    if (arg->resolved_type == 5) {
                        call_name = "str_len";
                    } else {
                        // Treat as a generic header-load.
                        VReg argv = gen_expr(c, arg);
                        VReg dst = new_vreg(c);
                        emit(c, (IRInst){.op = IR_LOAD, .dst = dst, .a = argv, .imm = 8});
                        return dst;
                    }
                }
            }
            VReg *args = NULL;
            if (n->call.arg_count > 0)
                args = malloc(n->call.arg_count * sizeof(VReg));
            for (int i = 0; i < n->call.arg_count; i++)
                args[i] = gen_expr(c, n->call.args[i]);
            VReg dst = new_vreg(c);
            emit(c, (IRInst){.op = IR_CALL, .dst = dst, .str = (char *)call_name, .args = args, .arg_count = n->call.arg_count});
            return dst;
        }
        case NODE_METHOD_CALL: {
            // Mirror the dispatch order in src/codegen.c so the IR
            // backend produces the same observable behaviour as the
            // direct codegen:
            //   1. enum constructor (object IDENT names a registered enum)
            //   2. user method on a typed struct receiver (resolved_struct_name)
            //   3. built-in collection / task method (push/pop/get/set/keys/len/fire/collapse)
            //   4. import-alias call (module.func)
            //   5. generic same-name fallback
            const char *method = n->method_call.method;

            // 1. Enum constructor: EnumName.Variant(args...)
            if (n->method_call.object->type == NODE_IDENT && c->program) {
                const char *obj_name = n->method_call.object->ident.name;
                for (int ei = 0; ei < c->program->program.enums.count; ei++) {
                    Node *e = c->program->program.enums.items[ei];
                    if (strcmp(e->enum_def.name, obj_name) != 0) continue;
                    int variant_idx = -1;
                    for (int vi = 0; vi < e->enum_def.variant_count; vi++) {
                        if (!strcmp(e->enum_def.variant_names[vi], method)) {
                            variant_idx = vi; break;
                        }
                    }
                    if (variant_idx < 0) break; // fall through to other dispatch
                    int nfields = n->method_call.arg_count;
                    int size = (nfields + 1) * 8;
                    if (size < 16) size = 16;
                    VReg sz = new_vreg(c);
                    emit(c, (IRInst){.op = IR_CONST, .dst = sz, .imm = size});
                    VReg *aa = malloc(sizeof(VReg));
                    aa[0] = sz;
                    VReg ptr = new_vreg(c);
                    emit(c, (IRInst){.op = IR_CALL, .dst = ptr, .str = "heap_alloc", .args = aa, .arg_count = 1});
                    // tag at offset 0
                    VReg tag = new_vreg(c);
                    emit(c, (IRInst){.op = IR_CONST, .dst = tag, .imm = variant_idx});
                    emit(c, (IRInst){.op = IR_STORE, .a = ptr, .b = tag, .imm = 0});
                    // fields at offsets 8, 16, ...
                    for (int fi = 0; fi < nfields; fi++) {
                        VReg val = gen_expr(c, n->method_call.args[fi]);
                        emit(c, (IRInst){.op = IR_STORE, .a = ptr, .b = val, .imm = (fi + 1) * 8});
                    }
                    return ptr;
                }
            }

            // 2. User method on a typed struct receiver
            // 3. Built-in list/map/imap/task method
            // 4. Import alias
            // 5. Fallback
            // For all four, the call shape is: emit receiver as args[0],
            // then the user-supplied args as args[1..N], then `bl _<sym>`.
            // The symbol depends on which branch matches.
            VReg receiver = gen_expr(c, n->method_call.object);
            int total_args = n->method_call.arg_count + 1;
            VReg *args = malloc(total_args * sizeof(VReg));
            args[0] = receiver;
            for (int i = 0; i < n->method_call.arg_count; i++)
                args[i + 1] = gen_expr(c, n->method_call.args[i]);

            const char *sym = NULL;
            char sym_buf[256];

            // 2. resolved_struct_name set by the checker
            if (n->method_call.resolved_struct_name) {
                snprintf(sym_buf, sizeof(sym_buf), "%s_%s",
                         n->method_call.resolved_struct_name, method);
                sym = strdup(sym_buf);
            }
            // 3. built-in collection methods
            if (!sym) {
                if (!strcmp(method, "push") || !strcmp(method, "pop") || !strcmp(method, "len")) {
                    snprintf(sym_buf, sizeof(sym_buf), "list_%s", method);
                    sym = strdup(sym_buf);
                } else if (!strcmp(method, "set") || !strcmp(method, "get") || !strcmp(method, "keys")) {
                    snprintf(sym_buf, sizeof(sym_buf), "map_%s", method);
                    sym = strdup(sym_buf);
                } else if (!strcmp(method, "fire") || !strcmp(method, "collapse")) {
                    snprintf(sym_buf, sizeof(sym_buf), "task_%s", method);
                    sym = strdup(sym_buf);
                }
            }
            // 4. import-alias call (alias.func)
            if (!sym && n->method_call.object->type == NODE_IDENT && c->program) {
                const char *obj_name = n->method_call.object->ident.name;
                for (int ai = 0; ai < c->program->program.use_count; ai++) {
                    if (!strcmp(c->program->program.use_aliases[ai], obj_name)) {
                        snprintf(sym_buf, sizeof(sym_buf), "%s_%s", obj_name, method);
                        sym = strdup(sym_buf);
                        // Drop the receiver — it's an alias, not a value.
                        for (int i = 0; i < n->method_call.arg_count; i++)
                            args[i] = args[i + 1];
                        total_args = n->method_call.arg_count;
                        break;
                    }
                }
            }
            // 5. fallback
            if (!sym) sym = strdup(method);

            VReg dst = new_vreg(c);
            emit(c, (IRInst){.op = IR_CALL, .dst = dst, .str = (char *)sym,
                             .args = args, .arg_count = total_args});
            return dst;
        }

        case NODE_FIELD_ACCESS: {
            VReg obj = gen_expr(c, n->field_access.object);
            // Find field offset
            int offset = 0;
            if (c->program && n->field_access.struct_name) {
                for (int si = 0; si < c->program->program.structs.count; si++) {
                    Node *s = c->program->program.structs.items[si];
                    if (!strcmp(s->struct_def.name, n->field_access.struct_name)) {
                        for (int fi = 0; fi < s->struct_def.field_count; fi++) {
                            if (!strcmp(s->struct_def.field_names[fi], n->field_access.field)) {
                                offset = fi * 8;
                                break;
                            }
                        }
                        break;
                    }
                }
            }
            VReg dst = new_vreg(c);
            emit(c, (IRInst){.op = IR_LOAD, .dst = dst, .a = obj, .imm = offset});
            return dst;
        }
        default: {
            VReg dst = new_vreg(c);
            emit(c, (IRInst){.op = IR_CONST, .dst = dst, .imm = 0});
            return dst;
        }
    }
}

static void gen_block(IRGenCtx *c, Node *block) {
    for (int i = 0; i < block->block.stmts.count; i++)
        gen_stmt(c, block->block.stmts.items[i]);
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

        case NODE_FIELD_ASSIGN: {
            VReg val = gen_expr(c, n->field_assign.value);
            VReg obj = gen_expr(c, n->field_assign.object);
            // Find field offset (search all structs)
            int offset = 0;
            if (c->program) {
                for (int si = 0; si < c->program->program.structs.count; si++) {
                    Node *s = c->program->program.structs.items[si];
                    int found = 0;
                    for (int fi = 0; fi < s->struct_def.field_count; fi++) {
                        if (!strcmp(s->struct_def.field_names[fi], n->field_assign.field)) {
                            offset = fi * 8;
                            found = 1;
                            break;
                        }
                    }
                    if (found) break;
                }
            }
            emit(c, (IRInst){.op = IR_STORE, .a = obj, .b = val, .imm = offset});
            break;
        }

        // === IF / NAH ===
        case NODE_IF: {
            int end_lbl = new_label(c);
            for (int i = 0; i < n->if_stmt.branch_count; i++) {
                VReg cond = gen_expr(c, n->if_stmt.conds[i]);
                int then_lbl = new_label(c);
                int next_lbl = new_label(c);
                emit(c, (IRInst){.op = IR_BR_COND, .a = cond, .label = then_lbl, .label2 = next_lbl});
                // Then block
                IRBlock *then_b = new_block(c);
                then_b->label = then_lbl;
                switch_block(c, then_b);
                gen_block(c, n->if_stmt.bodies[i]);
                emit(c, (IRInst){.op = IR_BR, .label = end_lbl});
                // Next (else-if or else or end)
                IRBlock *next_b = new_block(c);
                next_b->label = next_lbl;
                switch_block(c, next_b);
            }
            // Nah (else) block
            if (n->if_stmt.nah_body) {
                gen_block(c, n->if_stmt.nah_body);
            }
            emit(c, (IRInst){.op = IR_BR, .label = end_lbl});
            IRBlock *end_b = new_block(c);
            end_b->label = end_lbl;
            switch_block(c, end_b);
            break;
        }

        // === THROUGH RANGE (for i in start..end) ===
        case NODE_THROUGH_RANGE: {
            VReg from = gen_expr(c, n->through_range.from);
            VReg to = gen_expr(c, n->through_range.to);
            VReg step;
            if (n->through_range.by)
                step = gen_expr(c, n->through_range.by);
            else {
                step = new_vreg(c);
                emit(c, (IRInst){.op = IR_CONST, .dst = step, .imm = 1});
            }
            // Store to/step as hidden locals so they survive across blocks
            int to_slot = c->slot_next++;
            int step_slot = c->slot_next++;
            emit(c, (IRInst){.op = IR_STORE_LOCAL, .a = to, .imm = to_slot});
            emit(c, (IRInst){.op = IR_STORE_LOCAL, .a = step, .imm = step_slot});

            // Init loop var
            set_local(c, n->through_range.var_name, from);

            int cond_lbl = new_label(c);
            int body_lbl = new_label(c);
            int inc_lbl = new_label(c);
            int end_lbl = new_label(c);

            int prev_start = c->loop_start;
            int prev_end = c->loop_end;
            c->loop_start = inc_lbl;
            c->loop_end = end_lbl;

            emit(c, (IRInst){.op = IR_BR, .label = cond_lbl});

            // Condition
            IRBlock *cond_b = new_block(c);
            cond_b->label = cond_lbl;
            switch_block(c, cond_b);
            VReg iter = get_local(c, n->through_range.var_name);
            VReg to_load = new_vreg(c);
            emit(c, (IRInst){.op = IR_LOAD_LOCAL, .dst = to_load, .imm = to_slot});
            VReg cmp = new_vreg(c);
            emit(c, (IRInst){.op = IR_CMP_LT, .dst = cmp, .a = iter, .b = to_load});
            emit(c, (IRInst){.op = IR_BR_COND, .a = cmp, .label = body_lbl, .label2 = end_lbl});

            // Body
            IRBlock *body_b = new_block(c);
            body_b->label = body_lbl;
            switch_block(c, body_b);
            gen_block(c, n->through_range.body);
            emit(c, (IRInst){.op = IR_BR, .label = inc_lbl});

            // Increment
            IRBlock *inc_b = new_block(c);
            inc_b->label = inc_lbl;
            switch_block(c, inc_b);
            VReg cur = get_local(c, n->through_range.var_name);
            VReg step_load = new_vreg(c);
            emit(c, (IRInst){.op = IR_LOAD_LOCAL, .dst = step_load, .imm = step_slot});
            VReg next = new_vreg(c);
            emit(c, (IRInst){.op = IR_ADD, .dst = next, .a = cur, .b = step_load});
            set_local(c, n->through_range.var_name, next);
            emit(c, (IRInst){.op = IR_BR, .label = cond_lbl});

            // End
            IRBlock *end_b = new_block(c);
            end_b->label = end_lbl;
            switch_block(c, end_b);

            c->loop_start = prev_start;
            c->loop_end = prev_end;
            break;
        }

        // === INFI (while/infinite loop) ===
        case NODE_INFI: {
            int cond_lbl = new_label(c);
            int body_lbl = new_label(c);
            int end_lbl = new_label(c);

            int prev_start = c->loop_start;
            int prev_end = c->loop_end;
            c->loop_start = cond_lbl;
            c->loop_end = end_lbl;

            emit(c, (IRInst){.op = IR_BR, .label = cond_lbl});

            IRBlock *cond_b = new_block(c);
            cond_b->label = cond_lbl;
            switch_block(c, cond_b);

            if (n->infi.cond) {
                VReg cond = gen_expr(c, n->infi.cond);
                emit(c, (IRInst){.op = IR_BR_COND, .a = cond, .label = body_lbl, .label2 = end_lbl});
            } else {
                emit(c, (IRInst){.op = IR_BR, .label = body_lbl});
            }

            IRBlock *body_b = new_block(c);
            body_b->label = body_lbl;
            switch_block(c, body_b);
            gen_block(c, n->infi.body);
            emit(c, (IRInst){.op = IR_BR, .label = cond_lbl});

            IRBlock *end_b = new_block(c);
            end_b->label = end_lbl;
            switch_block(c, end_b);

            c->loop_start = prev_start;
            c->loop_end = prev_end;
            break;
        }

        // === STOP (break) ===
        case NODE_STOP:
            if (c->loop_end >= 0)
                emit(c, (IRInst){.op = IR_BR, .label = c->loop_end});
            break;

        // === SKIP (continue) ===
        case NODE_SKIP:
            if (c->loop_start >= 0)
                emit(c, (IRInst){.op = IR_BR, .label = c->loop_start});
            break;

        case NODE_BLOCK:
            gen_block(c, n);
            break;

        default:
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
        // Mangle methods as `<ReceiverType>_<method>` so the emitted
        // symbol matches what the call-site emitter produces. Free
        // functions keep their original name.
        if (f->func_def.receiver_type) {
            char buf[256];
            snprintf(buf, sizeof(buf), "%s_%s",
                     f->func_def.receiver_type, f->func_def.name);
            irf->name = strdup(buf);
        } else {
            irf->name = f->func_def.name;
        }
        irf->param_count = f->func_def.param_count;

        IRGenCtx ctx = {0};
        ctx.func = irf;
        ctx.block = new_block(&ctx);
        ctx.loop_start = -1;
        ctx.loop_end = -1;
        ctx.program = program;

        // Register params as locals
        for (int j = 0; j < f->func_def.param_count; j++) {
            VReg pv = new_vreg(&ctx);
            set_local(&ctx, f->func_def.param_names[j], pv);
        }

        // Generate body
        gen_block(&ctx, f->func_def.body);

        irf->vreg_count = ctx.vreg_next;
        irf->local_slots = ctx.slot_next;
        free(ctx.local_names);
        free(ctx.local_vregs);
        free(ctx.local_is_mut);
        free(ctx.local_slots);
    }

    return ir;
}
