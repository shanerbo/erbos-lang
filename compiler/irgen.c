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
    // RAII bookkeeping (P4.3e). Mirrors src/codegen.c's per-local
    // is_heap / is_moved / alloc_sizes parallel arrays; populated when
    // a NODE_VAR_DECL's RHS is a heap-producing expression (struct
    // constructor, list/map/imap literal, or one of the *_new helpers).
    // Cleared on `give expr` for the returned name and on `is now` for
    // the source name, so moved variables are never double-freed.
    int *local_is_heap;
    int *local_is_moved;
    int *local_alloc_sizes;
    int *local_is_array;     // α7: 1 if the local holds an `array of T`
                             // header. RAII has to free both the header
                             // (16 bytes) AND the data buffer (cap*esz
                             // bytes loaded from header[8]) at scope exit.
    int *local_array_esz;    // α8: element size for the array local (8
                             // for `array of int`, 1 for `array of byte`).
                             // Used by emit_scope_cleanup to compute the
                             // data-buffer size for _heap_free.
    char **local_struct_names; // P0-8: when the local is a struct, its
                             // monomorphised type name. Lets
                             // emit_scope_cleanup call _drop_<X>
                             // (recursive drop) instead of bare
                             // _heap_free, so owned heap fields like
                             // List.data and Map.keys are freed too.
                             // NULL when the local isn't a struct
                             // (primitives, raw arrays).
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
            // Reassigning a moved-out local re-binds it; clear the
            // moved flag so it can participate in the next scope's
            // RAII cleanup naturally.
            c->local_is_moved[i] = 0;
            return;
        }
    }
    if (c->local_count >= c->local_cap) {
        c->local_cap = c->local_cap ? c->local_cap * 2 : 16;
        c->local_names = realloc(c->local_names, c->local_cap * sizeof(char *));
        c->local_vregs = realloc(c->local_vregs, c->local_cap * sizeof(VReg));
        c->local_is_mut = realloc(c->local_is_mut, c->local_cap * sizeof(int));
        c->local_slots = realloc(c->local_slots, c->local_cap * sizeof(int));
        c->local_is_heap = realloc(c->local_is_heap, c->local_cap * sizeof(int));
        c->local_is_moved = realloc(c->local_is_moved, c->local_cap * sizeof(int));
        c->local_alloc_sizes = realloc(c->local_alloc_sizes, c->local_cap * sizeof(int));
        c->local_is_array = realloc(c->local_is_array, c->local_cap * sizeof(int));
        c->local_array_esz = realloc(c->local_array_esz, c->local_cap * sizeof(int));
        c->local_struct_names = realloc(c->local_struct_names, c->local_cap * sizeof(char *));
    }
    c->local_names[c->local_count] = (char *)name;
    c->local_vregs[c->local_count] = v;
    c->local_is_mut[c->local_count] = 0;
    c->local_slots[c->local_count] = c->slot_next++;
    c->local_is_heap[c->local_count] = 0;
    c->local_is_moved[c->local_count] = 0;
    c->local_alloc_sizes[c->local_count] = 0;
    c->local_is_array[c->local_count] = 0;
    c->local_array_esz[c->local_count] = 8;
    c->local_struct_names[c->local_count] = NULL;
    // Store initial value
    emit(c, (IRInst){.op = IR_STORE_LOCAL, .a = v, .imm = c->local_slots[c->local_count]});
    c->local_count++;
}

// Mark a named local as heap-allocated (the most recent definition
// wins). Sizes follow the same conventions src/codegen.c uses:
// struct_field_count*8 (min 8) for struct allocations, 520 for list
// headers, 152 for map/imap headers. The exact size only matters for
// the free-list metadata; the allocator rounds up to 16-byte alignment
// so any conservative over-estimate is safe.
// α7/α8: mark a local as holding an array header. RAII frees both
// the data buffer (cap*esz bytes loaded from header[8]) and the
// header itself (16 bytes). esz is 8 for `array of int` etc., 1
// for `array of byte`.
static void mark_array_local(IRGenCtx *c, const char *name, int esz) {
    for (int i = c->local_count - 1; i >= 0; i--) {
        if (!strcmp(c->local_names[i], name)) {
            c->local_is_heap[i] = 1;
            c->local_is_array[i] = 1;
            c->local_array_esz[i] = esz;
            c->local_alloc_sizes[i] = 16;  // header size; data freed separately
            return;
        }
    }
}

static void mark_heap_size(IRGenCtx *c, const char *name, int size) {
    for (int i = c->local_count - 1; i >= 0; i--) {
        if (!strcmp(c->local_names[i], name)) {
            c->local_is_heap[i] = 1;
            c->local_alloc_sizes[i] = size;
            return;
        }
    }
}

// P0-8: tag a heap local with the struct type name it holds.
// emit_scope_cleanup uses this to call _drop_<struct_name> instead
// of bare _heap_free, so owned heap fields (List.data, Map.keys,
// nested struct fields) are recursively freed.
static void mark_struct_local(IRGenCtx *c, const char *name, const char *struct_name) {
    for (int i = c->local_count - 1; i >= 0; i--) {
        if (!strcmp(c->local_names[i], name)) {
            c->local_struct_names[i] = strdup(struct_name);
            return;
        }
    }
}

// Mark a named local as moved-out. Subsequent emit_scope_cleanup
// sweeps will skip it so the new owner's free is the only one.
static void mark_moved_local(IRGenCtx *c, const char *name) {
    for (int i = c->local_count - 1; i >= 0; i--) {
        if (!strcmp(c->local_names[i], name)) {
            c->local_is_moved[i] = 1;
            return;
        }
    }
}

// Codex review (heap replacement leaks old owner): drop the
// current contents of a local slot before overwriting it. Called
// from NODE_ASSIGN's `is_move` / `is_rep` paths so that
//   a is List of int; a.push(1)
//   a be now b   // a's old List header + array drops here, not leak
// Only fires when the local is heap-marked and not already moved.
// Picks _drop_<X> if the struct name is known, falls back to
// _heap_free / array-cleanup otherwise.
static void emit_drop_local_slot(IRGenCtx *c, int idx) {
    if (idx < 0 || idx >= c->local_count) return;
    if (!c->local_is_heap[idx] || c->local_is_moved[idx]) return;
    VReg ptr = new_vreg(c);
    emit(c, (IRInst){.op = IR_LOAD_LOCAL, .dst = ptr,
                     .imm = c->local_slots[idx]});
    if (c->local_is_array[idx]) {
        // Array layout: [cap @ 0, data @ 8]. Free data, then header.
        VReg cap = new_vreg(c);
        emit(c, (IRInst){.op = IR_LOAD, .dst = cap, .a = ptr, .imm = 0});
        VReg esz = new_vreg(c);
        emit(c, (IRInst){.op = IR_CONST, .dst = esz,
                         .imm = c->local_array_esz[idx]});
        VReg data_sz = new_vreg(c);
        emit(c, (IRInst){.op = IR_MUL, .dst = data_sz, .a = cap, .b = esz});
        VReg data = new_vreg(c);
        emit(c, (IRInst){.op = IR_LOAD, .dst = data, .a = ptr, .imm = 8});
        VReg *fa1 = malloc(2 * sizeof(VReg));
        fa1[0] = data; fa1[1] = data_sz;
        VReg ig1 = new_vreg(c);
        emit(c, (IRInst){.op = IR_CALL, .dst = ig1, .str = "heap_free",
                         .args = fa1, .arg_count = 2});
        VReg sz16 = new_vreg(c);
        emit(c, (IRInst){.op = IR_CONST, .dst = sz16, .imm = 16});
        VReg *fa2 = malloc(2 * sizeof(VReg));
        fa2[0] = ptr; fa2[1] = sz16;
        VReg ig2 = new_vreg(c);
        emit(c, (IRInst){.op = IR_CALL, .dst = ig2, .str = "heap_free",
                         .args = fa2, .arg_count = 2});
        return;
    }
    if (c->local_struct_names && c->local_struct_names[idx]) {
        char drop_sym[256];
        snprintf(drop_sym, sizeof(drop_sym), "drop_%s",
                 c->local_struct_names[idx]);
        VReg *args = malloc(sizeof(VReg));
        args[0] = ptr;
        VReg ignored = new_vreg(c);
        emit(c, (IRInst){.op = IR_CALL, .dst = ignored,
                         .str = strdup(drop_sym),
                         .args = args, .arg_count = 1});
        return;
    }
    // Legacy fallback: bare _heap_free with the recorded size.
    VReg sz = new_vreg(c);
    int s = c->local_alloc_sizes[idx] ? c->local_alloc_sizes[idx] : 16;
    emit(c, (IRInst){.op = IR_CONST, .dst = sz, .imm = s});
    VReg *args = malloc(2 * sizeof(VReg));
    args[0] = ptr;
    args[1] = sz;
    VReg ignored = new_vreg(c);
    emit(c, (IRInst){.op = IR_CALL, .dst = ignored, .str = "heap_free",
                     .args = args, .arg_count = 2});
}

// Find the index of a named local, or -1 if not declared yet.
static int find_local_index(IRGenCtx *c, const char *name) {
    for (int i = c->local_count - 1; i >= 0; i--) {
        if (!strcmp(c->local_names[i], name)) return i;
    }
    return -1;
}

// Emit `_heap_free(ptr, size)` for every local in [from, local_count)
// that is heap-allocated and not moved. Used at scope end (NODE_BLOCK),
// at returns (NODE_GIVE), and at function fall-off.
//
// Array locals (α7) get a two-step cleanup: free the data buffer
// (size = cap * 8, where cap is loaded from header[0]) THEN free
// the 16-byte header itself.
static void emit_scope_cleanup(IRGenCtx *c, int from) {
    for (int i = from; i < c->local_count; i++) {
        if (!c->local_is_heap[i] || c->local_is_moved[i]) continue;
        c->local_is_moved[i] = 1;
        VReg header = new_vreg(c);
        emit(c, (IRInst){.op = IR_LOAD_LOCAL, .dst = header, .imm = c->local_slots[i]});

        if (c->local_is_array[i]) {
            // Array layout: header[0]=cap, header[8]=data.
            // Free data first (size = cap * elem_size), then header
            // (16 bytes). elem_size: 8 for `array of int`, 1 for
            // `array of byte`.
            VReg cap = new_vreg(c);
            emit(c, (IRInst){.op = IR_LOAD, .dst = cap, .a = header, .imm = 0});
            VReg esz = new_vreg(c);
            emit(c, (IRInst){.op = IR_CONST, .dst = esz, .imm = c->local_array_esz[i]});
            VReg data_sz = new_vreg(c);
            emit(c, (IRInst){.op = IR_MUL, .dst = data_sz, .a = cap, .b = esz});
            VReg data = new_vreg(c);
            emit(c, (IRInst){.op = IR_LOAD, .dst = data, .a = header, .imm = 8});
            VReg *fa1 = malloc(2 * sizeof(VReg));
            fa1[0] = data; fa1[1] = data_sz;
            VReg ig1 = new_vreg(c);
            emit(c, (IRInst){.op = IR_CALL, .dst = ig1, .str = "heap_free",
                             .args = fa1, .arg_count = 2});
            // Now free the header.
            VReg sz16 = new_vreg(c);
            emit(c, (IRInst){.op = IR_CONST, .dst = sz16, .imm = 16});
            VReg *fa2 = malloc(2 * sizeof(VReg));
            fa2[0] = header; fa2[1] = sz16;
            VReg ig2 = new_vreg(c);
            emit(c, (IRInst){.op = IR_CALL, .dst = ig2, .str = "heap_free",
                             .args = fa2, .arg_count = 2});
            continue;
        }

        // P0-8: struct locals call _drop_<X> for recursive cleanup
        // of owned heap fields. Falls through to plain heap_free
        // for locals whose struct type isn't tracked (legacy
        // collection allocators, untyped pointers).
        if (c->local_struct_names && c->local_struct_names[i]) {
            char drop_sym[256];
            snprintf(drop_sym, sizeof(drop_sym), "drop_%s",
                c->local_struct_names[i]);
            VReg *args = malloc(sizeof(VReg));
            args[0] = header;
            VReg ignored = new_vreg(c);
            emit(c, (IRInst){.op = IR_CALL, .dst = ignored,
                             .str = strdup(drop_sym),
                             .args = args, .arg_count = 1});
            continue;
        }

        VReg sz = new_vreg(c);
        int s = c->local_alloc_sizes[i] ? c->local_alloc_sizes[i] : 16;
        emit(c, (IRInst){.op = IR_CONST, .dst = sz, .imm = s});
        VReg *args = malloc(2 * sizeof(VReg));
        args[0] = header;
        args[1] = sz;
        VReg ignored = new_vreg(c);
        emit(c, (IRInst){.op = IR_CALL, .dst = ignored, .str = "heap_free",
                         .args = args, .arg_count = 2});
    }
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
            const char *s = n->str_lit.value;
            // Detect interpolation: any `{` triggers the segmented build.
            int has_interp = 0;
            for (int k = 0; s[k]; k++) {
                if (s[k] == '{') { has_interp = 1; break; }
            }
            if (!has_interp) {
                VReg dst = new_vreg(c);
                emit(c, (IRInst){.op = IR_LOAD_STR, .dst = dst, .str = (char *)s});
                return dst;
            }
            // Interpolation. Build the result by walking the literal,
            // alternating between fixed segments (loaded as strs and
            // concatenated onto the accumulator) and `{var}` slots
            // (looked up, optionally _int_to_str-converted using the
            // same `>0x100000 means already-str pointer` heuristic the
            // direct codegen uses, then concatenated).
            //
            // Mirror of src/codegen.c:153-212.
            VReg acc = new_vreg(c);
            emit(c, (IRInst){.op = IR_LOAD_STR, .dst = acc, .str = ""});

            int i = 0;
            while (s[i]) {
                if (s[i] == '{') {
                    // {varname}
                    i++;
                    char varname[64];
                    int vi = 0;
                    while (s[i] && s[i] != '}' && vi < 63) { varname[vi++] = s[i++]; }
                    varname[vi] = '\0';
                    if (s[i] == '}') i++;
                    VReg v = get_local(c, varname);
                    // String-or-int heuristic: cmp v >= 0x100000.
                    VReg threshold = new_vreg(c);
                    emit(c, (IRInst){.op = IR_CONST, .dst = threshold, .imm = 0x100000});
                    VReg is_str = new_vreg(c);
                    emit(c, (IRInst){.op = IR_CMP_GE, .dst = is_str, .a = v, .b = threshold});
                    // If int, call _int_to_str; else use v as-is.
                    int skip_lbl = new_label(c);
                    int call_lbl = new_label(c);
                    int join_lbl = new_label(c);
                    int converted_slot = c->slot_next++;
                    emit(c, (IRInst){.op = IR_BR_COND, .a = is_str, .label = skip_lbl, .label2 = call_lbl});

                    // call_lbl: convert int -> str via _int_to_str(v)
                    IRBlock *call_b = new_block(c);
                    call_b->label = call_lbl;
                    switch_block(c, call_b);
                    VReg *aa1 = malloc(sizeof(VReg));
                    aa1[0] = v;
                    VReg conv = new_vreg(c);
                    emit(c, (IRInst){.op = IR_CALL, .dst = conv, .str = "int_to_str",
                                     .args = aa1, .arg_count = 1});
                    emit(c, (IRInst){.op = IR_STORE_LOCAL, .a = conv, .imm = converted_slot});
                    emit(c, (IRInst){.op = IR_BR, .label = join_lbl});

                    // skip_lbl: already a string pointer
                    IRBlock *skip_b = new_block(c);
                    skip_b->label = skip_lbl;
                    switch_block(c, skip_b);
                    emit(c, (IRInst){.op = IR_STORE_LOCAL, .a = v, .imm = converted_slot});
                    emit(c, (IRInst){.op = IR_BR, .label = join_lbl});

                    // join: load the converted/passed-through string and concat with acc.
                    IRBlock *join_b = new_block(c);
                    join_b->label = join_lbl;
                    switch_block(c, join_b);
                    VReg piece = new_vreg(c);
                    emit(c, (IRInst){.op = IR_LOAD_LOCAL, .dst = piece, .imm = converted_slot});
                    VReg *cargs = malloc(2 * sizeof(VReg));
                    cargs[0] = acc;
                    cargs[1] = piece;
                    VReg new_acc = new_vreg(c);
                    emit(c, (IRInst){.op = IR_CALL, .dst = new_acc, .str = "str_concat",
                                     .args = cargs, .arg_count = 2});
                    acc = new_acc;
                } else {
                    // Literal segment up to the next `{`
                    char seg[256];
                    int si = 0;
                    while (s[i] && s[i] != '{' && si < 255) { seg[si++] = s[i++]; }
                    seg[si] = '\0';
                    VReg lit = new_vreg(c);
                    emit(c, (IRInst){.op = IR_LOAD_STR, .dst = lit, .str = strdup(seg)});
                    VReg *cargs = malloc(2 * sizeof(VReg));
                    cargs[0] = acc;
                    cargs[1] = lit;
                    VReg new_acc = new_vreg(c);
                    emit(c, (IRInst){.op = IR_CALL, .dst = new_acc, .str = "str_concat",
                                     .args = cargs, .arg_count = 2});
                    acc = new_acc;
                }
            }
            return acc;
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
            // String operators: `+` -> _str_concat, `eq`/`ne` -> _str_eq
            // (with optional inversion). Triggered by the checker's
            // resolved_type == 2 ("this binary op is on strings") tag.
            // Mirrors src/codegen.c:237-269.
            if (n->resolved_type == 2) {
                if (n->binary.op == TOK_PLUS || n->binary.op == TOK_ADD_WORD) {
                    VReg *args = malloc(2 * sizeof(VReg));
                    args[0] = a; args[1] = b;
                    VReg dst = new_vreg(c);
                    emit(c, (IRInst){.op = IR_CALL, .dst = dst, .str = "str_concat",
                                     .args = args, .arg_count = 2});
                    return dst;
                }
                if (n->binary.op == TOK_EQ || n->binary.op == TOK_EQ_WORD) {
                    VReg *args = malloc(2 * sizeof(VReg));
                    args[0] = a; args[1] = b;
                    VReg dst = new_vreg(c);
                    emit(c, (IRInst){.op = IR_CALL, .dst = dst, .str = "str_eq",
                                     .args = args, .arg_count = 2});
                    return dst;
                }
                if (n->binary.op == TOK_NEQ || n->binary.op == TOK_NE_WORD) {
                    VReg *args = malloc(2 * sizeof(VReg));
                    args[0] = a; args[1] = b;
                    VReg eq_v = new_vreg(c);
                    emit(c, (IRInst){.op = IR_CALL, .dst = eq_v, .str = "str_eq",
                                     .args = args, .arg_count = 2});
                    // Invert: dst = !eq_v
                    VReg dst = new_vreg(c);
                    emit(c, (IRInst){.op = IR_NOT, .dst = dst, .a = eq_v});
                    return dst;
                }
            }
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
            if (c->program) {
                for (int si = 0; si < c->program->program.structs.count; si++) {
                    Node *s = c->program->program.structs.items[si];
                    if (!strcmp(s->struct_def.name, n->call.name)) {
                        is_struct = 1;
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
                // Named-arg form: look up each arg's field name to find
                // the declared field index → emit a store at that
                // offset. Order in source is free; the checker has
                // already validated that every field is present exactly
                // once and that types match. The positional form (with
                // args) is rejected by the checker.
                if (n->call.arg_names) {
                    Node *struct_def = NULL;
                    for (int si = 0; si < c->program->program.structs.count; si++) {
                        Node *s = c->program->program.structs.items[si];
                        if (!strcmp(s->struct_def.name, n->call.name)) {
                            struct_def = s;
                            break;
                        }
                    }
                    for (int i = 0; i < n->call.arg_count; i++) {
                        const char *fname = n->call.arg_names[i];
                        int field_idx = -1;
                        for (int j = 0; j < struct_def->struct_def.field_count; j++) {
                            if (!strcmp(struct_def->struct_def.field_names[j], fname)) {
                                field_idx = j;
                                break;
                            }
                        }
                        // Defensive: checker guarantees field_idx >= 0.
                        VReg val = gen_expr(c, n->call.args[i]);
                        emit(c, (IRInst){.op = IR_STORE, .a = ptr, .b = val,
                                         .imm = field_idx * 8});
                    }
                }
                // Zero-default constructor: nothing else to do — the
                // _alloc_<X> call returns a zeroed struct.
                return ptr;
            }
            // Remap built-in calls to their real emitted symbols.
            // Mirrors the dispatch in src/codegen.c so the IR backend
            // produces calls to symbols that actually exist.
            const char *call_name = n->call.name;
            // γ3: `assert(cond)` lowers to the same conditional
            // _assert_fail-with-line path that NODE_ASSERT used.
            // Inlined here so we don't have to keep the special
            // statement node alive once the parser drops TOK_ASSERT.
            if (!strcmp(call_name, "assert") && n->call.arg_count == 1) {
                VReg cond = gen_expr(c, n->call.args[0]);
                int ok_lbl = new_label(c);
                int fail_lbl = new_label(c);
                emit(c, (IRInst){.op = IR_BR_COND, .a = cond,
                                 .label = ok_lbl, .label2 = fail_lbl});
                IRBlock *fail_b = new_block(c);
                fail_b->label = fail_lbl;
                switch_block(c, fail_b);
                VReg line_no = new_vreg(c);
                emit(c, (IRInst){.op = IR_CONST, .dst = line_no, .imm = n->line});
                VReg *aa = malloc(sizeof(VReg));
                aa[0] = line_no;
                VReg ignored = new_vreg(c);
                emit(c, (IRInst){.op = IR_CALL, .dst = ignored, .str = "assert_fail",
                                 .args = aa, .arg_count = 1});
                emit(c, (IRInst){.op = IR_BR, .label = ok_lbl});
                IRBlock *ok_b = new_block(c);
                ok_b->label = ok_lbl;
                switch_block(c, ok_b);
                VReg dummy = new_vreg(c);
                emit(c, (IRInst){.op = IR_CONST, .dst = dummy, .imm = 0});
                return dummy;
            }
            // ε1+ζ1: `list` / `map` / `imap` constructors are gone.
            // User code now writes `List of T` / `Map of K to V` /
            // `StringMap of V` (or the literal forms) and the
            // monomorphizer + irgen route them through the stdlib
            // method symbols. The remap below is dead.
            if (!strcmp(call_name, "task")) {
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
            // β5: raw memory primitives (mem_load / mem_store /
            // mem_load_byte / mem_store_byte / ptr_of / as_string)
            // are no longer user-callable. The checker rejects them
            // before we get here. The compiler still emits the
            // underlying IR opcodes (IR_LOAD / IR_STORE /
            // IR_LOAD_BYTE / IR_STORE_BYTE) directly when lowering
            // language constructs — typed array indexing, struct
            // field access, etc.
            else if (!strcmp(call_name, "len")) {
                // Free-function `len()` survives only on legacy
                // `list`/`map`/`imap` keyword-form receivers (where
                // count lives at offset 8 of a 24-byte header).
                // String receivers route via `s.len()` (γ4); the
                // string-specific `_str_len` symbol was dropped in
                // ζ2. Once ε retires the legacy collection keywords
                // this entire builtin goes away.
                if (n->call.arg_count == 1) {
                    Node *arg = n->call.args[0];
                    VReg argv = gen_expr(c, arg);
                    VReg dst = new_vreg(c);
                    emit(c, (IRInst){.op = IR_LOAD, .dst = dst, .a = argv, .imm = 8});
                    return dst;
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
            // 3. built-in task methods (the green-thread runtime
            // is a separate effort — not yet wired into compiled
            // output but the symbols are no-op stubs from
            // emit_task_builtins). The legacy list/map collection
            // method dispatch was retired in ε1+ζ1 — `xs.push(v)`
            // etc. now resolve to the user-method symbols emitted
            // by std/list / std/map / std/string_map through the
            // resolved_struct_name path above.
            if (!sym) {
                if (!strcmp(method, "fire") || !strcmp(method, "collapse")) {
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

        case NODE_INDEX: {
            // xs[i]. Three layouts:
            //   list  (legacy keyword-form): cap@0, count@8, data@16.
            //   array (α5):                  cap@0, data@8.
            //   stdlib container (ε5):        dispatch to <Type>_get(xs, i).
            // Selected by the checker via index_access.is_array
            // (legacy/array decision) and index_access.method_struct
            // (stdlib container — when set, supersedes the layout
            // decoding entirely).
            //
            // The two layout paths bounds-check inline (panic if
            // i < 0 or i >= bound). The stdlib method path delegates
            // bounds checking to the user-method body.
            if (n->index_access.method_struct) {
                VReg obj = gen_expr(c, n->index_access.object);
                VReg idx = gen_expr(c, n->index_access.index);
                VReg *args = malloc(2 * sizeof(VReg));
                args[0] = obj;
                args[1] = idx;
                char sym[256];
                snprintf(sym, sizeof(sym), "%s_get",
                    n->index_access.method_struct);
                VReg dst = new_vreg(c);
                emit(c, (IRInst){.op = IR_CALL, .dst = dst,
                    .str = strdup(sym), .args = args, .arg_count = 2});
                return dst;
            }
            VReg obj = gen_expr(c, n->index_access.object);
            VReg idx = gen_expr(c, n->index_access.index);

            int is_arr = n->index_access.is_array;
            int bound_off = is_arr ? 0 : 8;     // array: cap@0; list: count@8
            int data_off  = is_arr ? 8 : 16;    // array: data@8; list: data@16
            // bound = obj[bound_off]
            VReg count = new_vreg(c);
            emit(c, (IRInst){.op = IR_LOAD, .dst = count, .a = obj, .imm = bound_off});
            // negative-index check: idx < 0 ?
            VReg zero = new_vreg(c);
            emit(c, (IRInst){.op = IR_CONST, .dst = zero, .imm = 0});
            VReg is_neg = new_vreg(c);
            emit(c, (IRInst){.op = IR_CMP_LT, .dst = is_neg, .a = idx, .b = zero});
            int neg_panic_lbl = new_label(c);
            int neg_ok_lbl = new_label(c);
            emit(c, (IRInst){.op = IR_BR_COND, .a = is_neg,
                             .label = neg_panic_lbl, .label2 = neg_ok_lbl});

            IRBlock *neg_panic_b = new_block(c);
            neg_panic_b->label = neg_panic_lbl;
            switch_block(c, neg_panic_b);
            VReg ignored1 = new_vreg(c);
            emit(c, (IRInst){.op = IR_CALL, .dst = ignored1, .str = "panic_oob",
                             .args = NULL, .arg_count = 0});
            emit(c, (IRInst){.op = IR_BR, .label = neg_ok_lbl});

            IRBlock *neg_ok_b = new_block(c);
            neg_ok_b->label = neg_ok_lbl;
            switch_block(c, neg_ok_b);

            // upper-bound check: idx >= count ?
            VReg is_oob = new_vreg(c);
            emit(c, (IRInst){.op = IR_CMP_GE, .dst = is_oob, .a = idx, .b = count});
            int oob_panic_lbl = new_label(c);
            int oob_ok_lbl = new_label(c);
            emit(c, (IRInst){.op = IR_BR_COND, .a = is_oob,
                             .label = oob_panic_lbl, .label2 = oob_ok_lbl});

            IRBlock *oob_panic_b = new_block(c);
            oob_panic_b->label = oob_panic_lbl;
            switch_block(c, oob_panic_b);
            VReg ignored2 = new_vreg(c);
            emit(c, (IRInst){.op = IR_CALL, .dst = ignored2, .str = "panic_oob",
                             .args = NULL, .arg_count = 0});
            emit(c, (IRInst){.op = IR_BR, .label = oob_ok_lbl});

            IRBlock *oob_ok_b = new_block(c);
            oob_ok_b->label = oob_ok_lbl;
            switch_block(c, oob_ok_b);

            // data_ptr = obj[data_off]; addr = data_ptr + idx*esz;
            // load.  data_off and elem-size set above based on layout
            // and element type.
            int is_byte_elem = n->index_access.is_byte;
            int elem_sz = is_byte_elem ? 1 : 8;
            VReg data_ptr = new_vreg(c);
            emit(c, (IRInst){.op = IR_LOAD, .dst = data_ptr, .a = obj, .imm = data_off});
            VReg byte_off;
            if (elem_sz == 1) {
                byte_off = idx;          // index IS the byte offset
            } else {
                VReg esz = new_vreg(c);
                emit(c, (IRInst){.op = IR_CONST, .dst = esz, .imm = elem_sz});
                byte_off = new_vreg(c);
                emit(c, (IRInst){.op = IR_MUL, .dst = byte_off, .a = idx, .b = esz});
            }
            VReg addr = new_vreg(c);
            emit(c, (IRInst){.op = IR_ADD, .dst = addr, .a = data_ptr, .b = byte_off});
            VReg dst = new_vreg(c);
            emit(c, (IRInst){
                .op = is_byte_elem ? IR_LOAD_BYTE : IR_LOAD,
                .dst = dst, .a = addr, .imm = 0
            });
            return dst;
        }

        case NODE_ARRAY_NEW: {
            // α4/α8: `array of T with cap N` — typed-storage primitive.
            //
            // Runtime layout (16 bytes):
            //   [0]  cap         (int)
            //   [8]  data_ptr    (pointer to cap * sizeof(T) bytes)
            //
            // Two heap allocations: the header (16 bytes) and the
            // data buffer (cap * sizeof(T) bytes). The user-Potato
            // code never sees these calls — the compiler synthesises
            // them. Element size is 8 (one machine word) by default,
            // 1 for `array of byte`.
            int elem_sz = (n->array_new.elem_type &&
                           !strcmp(n->array_new.elem_type, "byte")) ? 1 : 8;
            VReg cap_v = gen_expr(c, n->array_new.cap);
            // data_size = cap * elem_sz
            VReg esz = new_vreg(c);
            emit(c, (IRInst){.op = IR_CONST, .dst = esz, .imm = elem_sz});
            VReg data_size = new_vreg(c);
            emit(c, (IRInst){.op = IR_MUL, .dst = data_size, .a = cap_v, .b = esz});
            // data = _heap_alloc(data_size)
            VReg *da = malloc(sizeof(VReg));
            da[0] = data_size;
            VReg data = new_vreg(c);
            emit(c, (IRInst){.op = IR_CALL, .dst = data, .str = "heap_alloc",
                             .args = da, .arg_count = 1});
            // header = _heap_alloc(16)
            VReg sz16 = new_vreg(c);
            emit(c, (IRInst){.op = IR_CONST, .dst = sz16, .imm = 16});
            VReg *ha = malloc(sizeof(VReg));
            ha[0] = sz16;
            VReg header = new_vreg(c);
            emit(c, (IRInst){.op = IR_CALL, .dst = header, .str = "heap_alloc",
                             .args = ha, .arg_count = 1});
            // header[0] = cap; header[8] = data
            emit(c, (IRInst){.op = IR_STORE, .a = header, .b = cap_v, .imm = 0});
            emit(c, (IRInst){.op = IR_STORE, .a = header, .b = data, .imm = 8});
            return header;
        }
        case NODE_LIST_LIT: {
            // ε3: when the literal carries an elem_type_name tag
            // (set by the monomorph seed-literals pass when the
            // `List` template is in scope), route through the
            // stdlib `List of T` constructor + per-item push:
            //   tmp is List of <inferred-elem-type>
            //   tmp.push(items[0]); tmp.push(items[1]); ...
            //
            // Otherwise (no `use std/list`), fall through to the
            // legacy 24-byte header form so existing programs keep
            // running until ε1 retires the legacy keyword forms.
            if (n->list_lit.elem_type_name) {
                char alloc_sym[256];
                char push_sym[256];
                snprintf(alloc_sym, sizeof(alloc_sym),
                    "alloc_List__%s", n->list_lit.elem_type_name);
                snprintf(push_sym, sizeof(push_sym),
                    "List__%s_push", n->list_lit.elem_type_name);
                VReg list_obj = new_vreg(c);
                emit(c, (IRInst){.op = IR_CALL, .dst = list_obj,
                    .str = strdup(alloc_sym),
                    .args = NULL, .arg_count = 0});
                for (int i = 0; i < n->list_lit.count; i++) {
                    VReg val = gen_expr(c, n->list_lit.items[i]);
                    VReg *aa = malloc(2 * sizeof(VReg));
                    aa[0] = list_obj;
                    aa[1] = val;
                    VReg ignored = new_vreg(c);
                    emit(c, (IRInst){.op = IR_CALL, .dst = ignored,
                        .str = strdup(push_sym),
                        .args = aa, .arg_count = 2});
                }
                return list_obj;
            }
            // Legacy 24-byte header form: cap@0, count@8, data@16.
            int count = n->list_lit.count;
            int data_size = count * 8 > 64 ? count * 8 : 64;
            int cap = count > 8 ? count : 8;
            VReg sz_header = new_vreg(c);
            emit(c, (IRInst){.op = IR_CONST, .dst = sz_header, .imm = 24});
            VReg *aa1 = malloc(sizeof(VReg));
            aa1[0] = sz_header;
            VReg header = new_vreg(c);
            emit(c, (IRInst){.op = IR_CALL, .dst = header, .str = "heap_alloc",
                             .args = aa1, .arg_count = 1});
            VReg sz_data = new_vreg(c);
            emit(c, (IRInst){.op = IR_CONST, .dst = sz_data, .imm = data_size});
            VReg *aa2 = malloc(sizeof(VReg));
            aa2[0] = sz_data;
            VReg data = new_vreg(c);
            emit(c, (IRInst){.op = IR_CALL, .dst = data, .str = "heap_alloc",
                             .args = aa2, .arg_count = 1});
            VReg vcap = new_vreg(c);
            emit(c, (IRInst){.op = IR_CONST, .dst = vcap, .imm = cap});
            emit(c, (IRInst){.op = IR_STORE, .a = header, .b = vcap, .imm = 0});
            VReg vcnt = new_vreg(c);
            emit(c, (IRInst){.op = IR_CONST, .dst = vcnt, .imm = count});
            emit(c, (IRInst){.op = IR_STORE, .a = header, .b = vcnt, .imm = 8});
            emit(c, (IRInst){.op = IR_STORE, .a = header, .b = data, .imm = 16});
            for (int i = 0; i < count; i++) {
                VReg val = gen_expr(c, n->list_lit.items[i]);
                emit(c, (IRInst){.op = IR_STORE, .a = data, .b = val, .imm = i * 8});
            }
            return header;
        }

        case NODE_MAP_LIT: {
            // ε4: when val_type_name is set (Map template is in
            // scope), route through `Map of String to V` —
            // _alloc_Map__String__<V> + per-pair _Map__String__<V>_set.
            // The map literal `["k" to v]` is always String-keyed
            // (the key syntax requires a string literal); the
            // monomorph instantiation seeds `Map<String,V>`.
            if (n->map_lit.val_type_name) {
                char alloc_sym[256];
                char set_sym[256];
                snprintf(alloc_sym, sizeof(alloc_sym),
                    "alloc_Map__String__%s", n->map_lit.val_type_name);
                snprintf(set_sym, sizeof(set_sym),
                    "Map__String__%s_set", n->map_lit.val_type_name);
                VReg map = new_vreg(c);
                emit(c, (IRInst){.op = IR_CALL, .dst = map,
                    .str = strdup(alloc_sym),
                    .args = NULL, .arg_count = 0});
                for (int i = 0; i < n->map_lit.count; i++) {
                    VReg key = gen_expr(c, n->map_lit.keys[i]);
                    VReg val = gen_expr(c, n->map_lit.values[i]);
                    VReg *aa = malloc(3 * sizeof(VReg));
                    aa[0] = map; aa[1] = key; aa[2] = val;
                    VReg ignored = new_vreg(c);
                    emit(c, (IRInst){.op = IR_CALL, .dst = ignored,
                        .str = strdup(set_sym),
                        .args = aa, .arg_count = 3});
                }
                return map;
            }
            // Legacy: _map_new + _map_set per pair.
            VReg map = new_vreg(c);
            emit(c, (IRInst){.op = IR_CALL, .dst = map, .str = "map_new",
                             .args = NULL, .arg_count = 0});
            for (int i = 0; i < n->map_lit.count; i++) {
                VReg key = gen_expr(c, n->map_lit.keys[i]);
                VReg val = gen_expr(c, n->map_lit.values[i]);
                VReg *args = malloc(3 * sizeof(VReg));
                args[0] = map;
                args[1] = key;
                args[2] = val;
                VReg ignored = new_vreg(c);
                emit(c, (IRInst){.op = IR_CALL, .dst = ignored, .str = "map_set",
                                 .args = args, .arg_count = 3});
            }
            return map;
        }

        case NODE_FIELD_ACCESS: {
            VReg obj = gen_expr(c, n->field_access.object);
            // Find field offset
            int offset = 0;
            // β1: `arr.cap` for `arr : array of T` — the checker
            // tags struct_name = "array" for this case. Cap is at
            // offset 0 in the runtime header.
            if (n->field_access.struct_name &&
                !strcmp(n->field_access.struct_name, "array") &&
                n->field_access.field && !strcmp(n->field_access.field, "cap")) {
                VReg dst = new_vreg(c);
                emit(c, (IRInst){.op = IR_LOAD, .dst = dst, .a = obj, .imm = 0});
                return dst;
            }
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
            // `is rep` deep-clones the source rather than just rebinding.
            // The checker stashed the source's struct name in
            // var_decl.type_name when the source had a known struct
            // type; we route through `_clone_<StructName>` to allocate
            // a fresh block and recursively copy fields. The result is
            // the new pointer; the source is unchanged and still owns
            // its original block.
            //
            // Without type_name (source type wasn't a struct, or
            // checker couldn't determine it) we fall through to the
            // legacy shallow rebind. That path is the latent UAF bug;
            // the checker always sets type_name for struct-shaped
            // sources today, so the legacy path should be unreachable.
            VReg v;
            if (n->var_decl.is_rep && n->var_decl.type_name) {
                VReg src = gen_expr(c, n->var_decl.value);
                v = new_vreg(c);
                char clone_sym[256];
                snprintf(clone_sym, sizeof(clone_sym), "clone_%s",
                    n->var_decl.type_name);
                VReg *args = malloc(sizeof(VReg));
                args[0] = src;
                emit(c, (IRInst){.op = IR_CALL, .dst = v,
                                 .str = strdup(clone_sym),
                                 .args = args, .arg_count = 1});
            } else {
                v = gen_expr(c, n->var_decl.value);
            }
            set_local(c, n->var_decl.name, v);
            // RAII: mirror src/codegen.c's heap-marking. `is now`
            // transfers ownership: the source is dead, the target is
            // heap. `is rep` returns a fresh independent heap block
            // that the new local owns. Plain decls of struct
            // constructors and collection allocators are heap. Sizes
            // follow the same conventions the direct codegen uses.
            if (n->var_decl.is_move) {
                if (n->var_decl.value->type == NODE_IDENT)
                    mark_moved_local(c, n->var_decl.value->ident.name);
                mark_heap_size(c, n->var_decl.name, 0); // size unknown — use default 16
                // P0-8: carry the struct name through `is now` so
                // the new owner gets recursive drop. Source's struct
                // name was stashed by checker for is_rep; for is_now
                // it's not stashed but we can read it from the
                // moved-from local.
                if (n->var_decl.value->type == NODE_IDENT) {
                    for (int i = c->local_count - 1; i >= 0; i--) {
                        if (!strcmp(c->local_names[i],
                                    n->var_decl.value->ident.name) &&
                            c->local_struct_names[i]) {
                            mark_struct_local(c, n->var_decl.name,
                                c->local_struct_names[i]);
                            break;
                        }
                    }
                }
            } else if (n->var_decl.is_rep) {
                mark_heap_size(c, n->var_decl.name, 0);
                if (n->var_decl.type_name) {
                    mark_struct_local(c, n->var_decl.name, n->var_decl.type_name);
                }
            } else if (n->var_decl.value->type == NODE_CALL) {
                const char *fn = n->var_decl.value->call.name;
                int size = 0;
                if (c->program) {
                    for (int si = 0; si < c->program->program.structs.count; si++) {
                        Node *s = c->program->program.structs.items[si];
                        if (!strcmp(s->struct_def.name, fn)) {
                            size = s->struct_def.field_count * 8;
                            if (size == 0) size = 8;
                            break;
                        }
                    }
                }
                if (size > 0) {
                    mark_heap_size(c, n->var_decl.name, size);
                    // P0-8: tag the local with its struct type so
                    // emit_scope_cleanup can call _drop_<X> instead
                    // of bare _heap_free, recursively freeing
                    // owned heap fields (List.data, Map.keys,
                    // nested struct fields).
                    mark_struct_local(c, n->var_decl.name, fn);
                } else if (!strcmp(fn, "list") || !strcmp(fn, "list_new")) {
                    mark_heap_size(c, n->var_decl.name, 520);
                } else if (!strcmp(fn, "map") || !strcmp(fn, "map_new")
                        || !strcmp(fn, "imap") || !strcmp(fn, "imap_new")) {
                    mark_heap_size(c, n->var_decl.name, 152);
                } else if (!strncmp(fn, "alloc_", 6)) {
                    mark_heap_size(c, n->var_decl.name, 520);
                }
            } else if (n->var_decl.value->type == NODE_LIST_LIT) {
                // ε3: tagged list literals lower to a 16-byte
                // `List of T` struct (count + array-ptr); legacy
                // 24-byte list header is the untagged fallback.
                int sz = n->var_decl.value->list_lit.elem_type_name ? 16 : 520;
                mark_heap_size(c, n->var_decl.name, sz);
                if (n->var_decl.value->list_lit.elem_type_name) {
                    char struct_name[256];
                    snprintf(struct_name, sizeof(struct_name), "List__%s",
                        n->var_decl.value->list_lit.elem_type_name);
                    mark_struct_local(c, n->var_decl.name, struct_name);
                }
            } else if (n->var_decl.value->type == NODE_MAP_LIT) {
                // ε4: tagged map literals lower to a 24-byte
                // `StringMap of V` struct (count + 2 array-ptrs);
                // legacy 152-byte map_new is the untagged fallback.
                int sz = n->var_decl.value->map_lit.val_type_name ? 24 : 152;
                mark_heap_size(c, n->var_decl.name, sz);
                if (n->var_decl.value->map_lit.val_type_name) {
                    char struct_name[256];
                    snprintf(struct_name, sizeof(struct_name),
                        "Map__String__%s",
                        n->var_decl.value->map_lit.val_type_name);
                    mark_struct_local(c, n->var_decl.name, struct_name);
                }
            } else if (n->var_decl.value->type == NODE_ARRAY_NEW) {
                // α7/α8: array gets two-step RAII free (data + header).
                // Element size is 1 for `array of byte`, 8 for
                // everything else (default machine word).
                int esz = (n->var_decl.value->array_new.elem_type &&
                           !strcmp(n->var_decl.value->array_new.elem_type, "byte")) ? 1 : 8;
                mark_array_local(c, n->var_decl.name, esz);
            }
            break;
        }
        case NODE_ASSIGN: {
            // Codex P0-7: handle `q be now p` (mark source moved)
            // and `q be rep p` (deep-clone via _clone_<X>). Plain
            // `q be p` for heap-shaped values is rejected by the
            // checker; here we only see the explicit forms.
            //
            // Codex review (heap replacement leak): for the
            // explicit-form cases, drop the destination's previous
            // owned value before overwriting. Without this, the
            // old List/Map/struct leaked because set_local just
            // overwrites the slot.
            if (n->assign.is_move || n->assign.is_rep) {
                int dst_idx = find_local_index(c, n->assign.name);
                emit_drop_local_slot(c, dst_idx);
            }
            VReg v;
            if (n->assign.is_rep && n->assign.src_struct_name) {
                VReg src = gen_expr(c, n->assign.value);
                v = new_vreg(c);
                char clone_sym[256];
                snprintf(clone_sym, sizeof(clone_sym),
                    "clone_%s", n->assign.src_struct_name);
                VReg *args = malloc(sizeof(VReg));
                args[0] = src;
                emit(c, (IRInst){.op = IR_CALL, .dst = v,
                                 .str = strdup(clone_sym),
                                 .args = args, .arg_count = 1});
            } else {
                if (n->assign.is_move &&
                    n->assign.value->type == NODE_IDENT) {
                    mark_moved_local(c, n->assign.value->ident.name);
                }
                v = gen_expr(c, n->assign.value);
            }
            set_local(c, n->assign.name, v);
            break;
        }
        case NODE_GIVE: {
            if (n->give.value) {
                VReg v = gen_expr(c, n->give.value);
                // Returning a named local transfers ownership: skip its
                // free in the cleanup sweep below. Mirrors how the
                // direct codegen handles `give x` for heap x.
                if (n->give.value->type == NODE_IDENT)
                    mark_moved_local(c, n->give.value->ident.name);
                emit_scope_cleanup(c, 0);
                emit(c, (IRInst){.op = IR_RET, .a = v});
            } else {
                emit_scope_cleanup(c, 0);
                emit(c, (IRInst){.op = IR_RET_VOID});
            }
            break;
        }
        case NODE_CALL:
        case NODE_METHOD_CALL:
            gen_expr(c, n);
            break;

        case NODE_FIELD_ASSIGN: {
            // β1: when the RHS is a named local that owns heap
            // storage (array, struct), assigning it into a field
            // transfers ownership. The local is "moved out" — RAII
            // cleanup must skip it on scope exit, otherwise we
            // double-free (the field still points at the buffer
            // that the local also pointed at).
            //
            // `field be rep src` deep-clones the source; both source
            // and field end up alive with independent blocks, so the
            // source is NOT marked moved.
            if (n->field_assign.value->type == NODE_IDENT && !n->field_assign.is_rep) {
                mark_moved_local(c, n->field_assign.value->ident.name);
            }
            VReg val;
            if (n->field_assign.is_rep && n->field_assign.src_struct_name) {
                // Eval source pointer, then `bl _clone_<SrcStruct>`.
                // The result is the new independent block.
                VReg src = gen_expr(c, n->field_assign.value);
                val = new_vreg(c);
                char clone_sym[256];
                snprintf(clone_sym, sizeof(clone_sym),
                    "clone_%s", n->field_assign.src_struct_name);
                VReg *args = malloc(sizeof(VReg));
                args[0] = src;
                emit(c, (IRInst){.op = IR_CALL, .dst = val,
                                 .str = strdup(clone_sym),
                                 .args = args, .arg_count = 1});
            } else {
                val = gen_expr(c, n->field_assign.value);
            }
            VReg obj = gen_expr(c, n->field_assign.object);
            // Resolve the field offset using the same per-struct policy
            // P2 introduced for the direct codegen: when the checker
            // tagged the assign with a struct name, look up the offset
            // in that exact struct; otherwise fall back to a global
            // search (used by the BST/linked-list pattern that stores
            // struct pointers as int).
            int offset = 0;
            int found = 0;
            const char *field_type = NULL;
            if (c->program && n->field_assign.struct_name) {
                for (int si = 0; si < c->program->program.structs.count && !found; si++) {
                    Node *s = c->program->program.structs.items[si];
                    if (strcmp(s->struct_def.name, n->field_assign.struct_name) != 0) continue;
                    for (int fi = 0; fi < s->struct_def.field_count; fi++) {
                        if (!strcmp(s->struct_def.field_names[fi], n->field_assign.field)) {
                            offset = fi * 8;
                            field_type = s->struct_def.field_types[fi];
                            found = 1;
                            break;
                        }
                    }
                }
            }
            if (!found && c->program) {
                for (int si = 0; si < c->program->program.structs.count; si++) {
                    Node *s = c->program->program.structs.items[si];
                    int local_found = 0;
                    for (int fi = 0; fi < s->struct_def.field_count; fi++) {
                        if (!strcmp(s->struct_def.field_names[fi], n->field_assign.field)) {
                            offset = fi * 8;
                            field_type = s->struct_def.field_types[fi];
                            local_found = 1;
                            break;
                        }
                    }
                    if (local_found) break;
                }
            }
            // Codex review (heap replacement leak): for `field be
            // now src` and `field be rep src`, drop the previous
            // owned value before storing the new pointer. Without
            // this, the field's old contents leak — auto-init'd
            // empty Lists/Strings, or any value the field was
            // earlier set to, accumulates dead memory until the
            // parent struct goes out of scope.
            //
            // Only fires for the explicit move/rep forms with a
            // heap-shaped declared field type. Plain
            // `field be primitive_value` doesn't need a drop.
            if ((n->field_assign.is_move || n->field_assign.is_rep) &&
                field_type) {
                int field_is_struct = 0;
                if (c->program) {
                    for (int si = 0; si < c->program->program.structs.count; si++) {
                        if (!strcmp(c->program->program.structs.items[si]->struct_def.name, field_type)) {
                            field_is_struct = 1;
                            break;
                        }
                    }
                }
                int field_is_array = !strncmp(field_type, "array__", 7);
                int field_is_byte_array = !strcmp(field_type, "array__byte");
                if (field_is_struct) {
                    // Load old field value; null-guard; bl _drop_<FieldType>.
                    VReg old = new_vreg(c);
                    emit(c, (IRInst){.op = IR_LOAD, .dst = old,
                                     .a = obj, .imm = offset});
                    char drop_sym[256];
                    snprintf(drop_sym, sizeof(drop_sym), "drop_%s", field_type);
                    VReg *args = malloc(sizeof(VReg));
                    args[0] = old;
                    VReg ignored = new_vreg(c);
                    emit(c, (IRInst){.op = IR_CALL, .dst = ignored,
                                     .str = strdup(drop_sym),
                                     .args = args, .arg_count = 1});
                } else if (field_is_array) {
                    // Mirror the array two-step cleanup: free data
                    // (cap*esz) then header (16 bytes). Null-guard
                    // the whole thing; if the field was nil we
                    // skip the frees.
                    int esz = field_is_byte_array ? 1 : 8;
                    VReg old = new_vreg(c);
                    emit(c, (IRInst){.op = IR_LOAD, .dst = old,
                                     .a = obj, .imm = offset});
                    int skip_label = new_label(c);
                    int hdr_label = new_label(c);
                    int after_label = new_label(c);
                    // if (old == 0) goto skip_label
                    VReg zero_v = new_vreg(c);
                    emit(c, (IRInst){.op = IR_CONST, .dst = zero_v, .imm = 0});
                    VReg is_null = new_vreg(c);
                    emit(c, (IRInst){.op = IR_CMP_EQ, .dst = is_null, .a = old, .b = zero_v});
                    emit(c, (IRInst){.op = IR_BR_COND, .a = is_null,
                                     .label = after_label, .label2 = hdr_label});
                    // hdr_label: load cap, free data buffer, then header
                    IRBlock *hb = new_block(c); hb->label = hdr_label;
                    switch_block(c, hb);
                    VReg cap = new_vreg(c);
                    emit(c, (IRInst){.op = IR_LOAD, .dst = cap, .a = old, .imm = 0});
                    VReg esz_v = new_vreg(c);
                    emit(c, (IRInst){.op = IR_CONST, .dst = esz_v, .imm = esz});
                    VReg data_sz = new_vreg(c);
                    emit(c, (IRInst){.op = IR_MUL, .dst = data_sz,
                                     .a = cap, .b = esz_v});
                    VReg data = new_vreg(c);
                    emit(c, (IRInst){.op = IR_LOAD, .dst = data, .a = old, .imm = 8});
                    VReg *fa1 = malloc(2 * sizeof(VReg));
                    fa1[0] = data; fa1[1] = data_sz;
                    VReg ig1 = new_vreg(c);
                    emit(c, (IRInst){.op = IR_CALL, .dst = ig1, .str = "heap_free",
                                     .args = fa1, .arg_count = 2});
                    VReg sz16 = new_vreg(c);
                    emit(c, (IRInst){.op = IR_CONST, .dst = sz16, .imm = 16});
                    VReg *fa2 = malloc(2 * sizeof(VReg));
                    fa2[0] = old; fa2[1] = sz16;
                    VReg ig2 = new_vreg(c);
                    emit(c, (IRInst){.op = IR_CALL, .dst = ig2, .str = "heap_free",
                                     .args = fa2, .arg_count = 2});
                    emit(c, (IRInst){.op = IR_BR, .label = after_label});
                    // after_label: continue
                    IRBlock *ab = new_block(c); ab->label = after_label;
                    switch_block(c, ab);
                    (void)skip_label; // kept for symmetry; same as after
                }
            }
            emit(c, (IRInst){.op = IR_STORE, .a = obj, .b = val, .imm = offset});
            break;
        }
        case NODE_INDEX_ASSIGN: {
            // α6: arr[i] be v / xs[i] be v.
            // Same shape as NODE_INDEX read: bounds-checked typed
            // store. Layout differs by is_array tag (set by checker).
            // ε5: when index_assign.method_struct is set the access
            // dispatches to <Type>_set(obj, idx, val) instead.
            // This sits in gen_stmt (statement context) so we just
            // emit the call and break; no value is returned.
            if (n->index_assign.method_struct) {
                VReg obj = gen_expr(c, n->index_assign.object);
                VReg idx = gen_expr(c, n->index_assign.index);
                VReg val = gen_expr(c, n->index_assign.value);
                VReg *args = malloc(3 * sizeof(VReg));
                args[0] = obj;
                args[1] = idx;
                args[2] = val;
                char sym[256];
                snprintf(sym, sizeof(sym), "%s_set",
                    n->index_assign.method_struct);
                VReg dummy = new_vreg(c);
                emit(c, (IRInst){.op = IR_CALL, .dst = dummy,
                    .str = strdup(sym), .args = args, .arg_count = 3});
                break;
            }
            VReg val = gen_expr(c, n->index_assign.value);
            VReg obj = gen_expr(c, n->index_assign.object);
            VReg idx = gen_expr(c, n->index_assign.index);

            int is_arr = n->index_assign.is_array;
            int bound_off = is_arr ? 0 : 8;
            int data_off  = is_arr ? 8 : 16;

            // bound = obj[bound_off]
            VReg bound = new_vreg(c);
            emit(c, (IRInst){.op = IR_LOAD, .dst = bound, .a = obj, .imm = bound_off});
            // negative-index check
            VReg zero = new_vreg(c);
            emit(c, (IRInst){.op = IR_CONST, .dst = zero, .imm = 0});
            VReg is_neg = new_vreg(c);
            emit(c, (IRInst){.op = IR_CMP_LT, .dst = is_neg, .a = idx, .b = zero});
            int neg_panic_lbl = new_label(c);
            int neg_ok_lbl = new_label(c);
            emit(c, (IRInst){.op = IR_BR_COND, .a = is_neg,
                             .label = neg_panic_lbl, .label2 = neg_ok_lbl});
            IRBlock *neg_panic_b = new_block(c);
            neg_panic_b->label = neg_panic_lbl;
            switch_block(c, neg_panic_b);
            VReg ignored1 = new_vreg(c);
            emit(c, (IRInst){.op = IR_CALL, .dst = ignored1, .str = "panic_oob",
                             .args = NULL, .arg_count = 0});
            emit(c, (IRInst){.op = IR_BR, .label = neg_ok_lbl});
            IRBlock *neg_ok_b = new_block(c);
            neg_ok_b->label = neg_ok_lbl;
            switch_block(c, neg_ok_b);
            // upper-bound check
            VReg is_oob = new_vreg(c);
            emit(c, (IRInst){.op = IR_CMP_GE, .dst = is_oob, .a = idx, .b = bound});
            int oob_panic_lbl = new_label(c);
            int oob_ok_lbl = new_label(c);
            emit(c, (IRInst){.op = IR_BR_COND, .a = is_oob,
                             .label = oob_panic_lbl, .label2 = oob_ok_lbl});
            IRBlock *oob_panic_b = new_block(c);
            oob_panic_b->label = oob_panic_lbl;
            switch_block(c, oob_panic_b);
            VReg ignored2 = new_vreg(c);
            emit(c, (IRInst){.op = IR_CALL, .dst = ignored2, .str = "panic_oob",
                             .args = NULL, .arg_count = 0});
            emit(c, (IRInst){.op = IR_BR, .label = oob_ok_lbl});
            IRBlock *oob_ok_b = new_block(c);
            oob_ok_b->label = oob_ok_lbl;
            switch_block(c, oob_ok_b);
            // data_ptr = obj[data_off]; addr = data_ptr + idx*esz; store.
            int is_byte_elem_w = n->index_assign.is_byte;
            int elem_sz_w = is_byte_elem_w ? 1 : 8;
            VReg data_ptr = new_vreg(c);
            emit(c, (IRInst){.op = IR_LOAD, .dst = data_ptr, .a = obj, .imm = data_off});
            VReg byte_off_w;
            if (elem_sz_w == 1) {
                byte_off_w = idx;
            } else {
                VReg esz_w = new_vreg(c);
                emit(c, (IRInst){.op = IR_CONST, .dst = esz_w, .imm = elem_sz_w});
                byte_off_w = new_vreg(c);
                emit(c, (IRInst){.op = IR_MUL, .dst = byte_off_w, .a = idx, .b = esz_w});
            }
            VReg addr = new_vreg(c);
            emit(c, (IRInst){.op = IR_ADD, .dst = addr, .a = data_ptr, .b = byte_off_w});
            emit(c, (IRInst){
                .op = is_byte_elem_w ? IR_STORE_BYTE : IR_STORE,
                .a = addr, .b = val, .imm = 0
            });
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

        // === through (x in collection) ===
        case NODE_THROUGH_IN: {
            VReg coll = gen_expr(c, n->through_in.collection);
            // Stash collection ptr + index in hidden locals so the
            // values survive across basic blocks. Same pattern as
            // NODE_THROUGH_RANGE's to/step slots.
            int coll_slot = c->slot_next++;
            int idx_slot = c->slot_next++;
            emit(c, (IRInst){.op = IR_STORE_LOCAL, .a = coll, .imm = coll_slot});
            VReg zero = new_vreg(c);
            emit(c, (IRInst){.op = IR_CONST, .dst = zero, .imm = 0});
            emit(c, (IRInst){.op = IR_STORE_LOCAL, .a = zero, .imm = idx_slot});

            int cond_lbl = new_label(c);
            int body_lbl = new_label(c);
            int inc_lbl = new_label(c);
            int end_lbl = new_label(c);

            int prev_start = c->loop_start;
            int prev_end = c->loop_end;
            c->loop_start = inc_lbl;
            c->loop_end = end_lbl;

            emit(c, (IRInst){.op = IR_BR, .label = cond_lbl});

            // Condition: idx < count(coll). Two paths:
            //   ε6 stdlib container: count = <Type>_len(coll).
            //   legacy:              count = coll[8] (header layout).
            const char *ms = n->through_in.method_struct;
            IRBlock *cond_b = new_block(c);
            cond_b->label = cond_lbl;
            switch_block(c, cond_b);
            VReg coll_v = new_vreg(c);
            emit(c, (IRInst){.op = IR_LOAD_LOCAL, .dst = coll_v, .imm = coll_slot});
            VReg count = new_vreg(c);
            if (ms) {
                VReg *args = malloc(sizeof(VReg));
                args[0] = coll_v;
                char sym[256];
                snprintf(sym, sizeof(sym), "%s_len", ms);
                emit(c, (IRInst){.op = IR_CALL, .dst = count,
                    .str = strdup(sym), .args = args, .arg_count = 1});
            } else {
                emit(c, (IRInst){.op = IR_LOAD, .dst = count, .a = coll_v, .imm = 8});
            }
            VReg idx_v = new_vreg(c);
            emit(c, (IRInst){.op = IR_LOAD_LOCAL, .dst = idx_v, .imm = idx_slot});
            VReg cmp = new_vreg(c);
            emit(c, (IRInst){.op = IR_CMP_LT, .dst = cmp, .a = idx_v, .b = count});
            emit(c, (IRInst){.op = IR_BR_COND, .a = cmp, .label = body_lbl, .label2 = end_lbl});

            // Body: load element. Two paths:
            //   ε6 stdlib container: elem = <Type>_get(coll, idx).
            //   legacy:              elem = data_ptr[idx*8] from header.
            IRBlock *body_b = new_block(c);
            body_b->label = body_lbl;
            switch_block(c, body_b);
            VReg coll_v2 = new_vreg(c);
            emit(c, (IRInst){.op = IR_LOAD_LOCAL, .dst = coll_v2, .imm = coll_slot});
            VReg idx_v2 = new_vreg(c);
            emit(c, (IRInst){.op = IR_LOAD_LOCAL, .dst = idx_v2, .imm = idx_slot});
            VReg elem = new_vreg(c);
            if (ms) {
                VReg *args = malloc(2 * sizeof(VReg));
                args[0] = coll_v2;
                args[1] = idx_v2;
                char sym[256];
                snprintf(sym, sizeof(sym), "%s_get", ms);
                emit(c, (IRInst){.op = IR_CALL, .dst = elem,
                    .str = strdup(sym), .args = args, .arg_count = 2});
            } else {
                VReg data_ptr = new_vreg(c);
                emit(c, (IRInst){.op = IR_LOAD, .dst = data_ptr, .a = coll_v2, .imm = 16});
                VReg eight = new_vreg(c);
                emit(c, (IRInst){.op = IR_CONST, .dst = eight, .imm = 8});
                VReg byte_off = new_vreg(c);
                emit(c, (IRInst){.op = IR_MUL, .dst = byte_off, .a = idx_v2, .b = eight});
                VReg elem_addr = new_vreg(c);
                emit(c, (IRInst){.op = IR_ADD, .dst = elem_addr, .a = data_ptr, .b = byte_off});
                emit(c, (IRInst){.op = IR_LOAD, .dst = elem, .a = elem_addr, .imm = 0});
            }
            set_local(c, n->through_in.var_name, elem);
            gen_block(c, n->through_in.body);
            emit(c, (IRInst){.op = IR_BR, .label = inc_lbl});

            // Increment idx by 1.
            IRBlock *inc_b = new_block(c);
            inc_b->label = inc_lbl;
            switch_block(c, inc_b);
            VReg idx_v3 = new_vreg(c);
            emit(c, (IRInst){.op = IR_LOAD_LOCAL, .dst = idx_v3, .imm = idx_slot});
            VReg one = new_vreg(c);
            emit(c, (IRInst){.op = IR_CONST, .dst = one, .imm = 1});
            VReg next = new_vreg(c);
            emit(c, (IRInst){.op = IR_ADD, .dst = next, .a = idx_v3, .b = one});
            emit(c, (IRInst){.op = IR_STORE_LOCAL, .a = next, .imm = idx_slot});
            emit(c, (IRInst){.op = IR_BR, .label = cond_lbl});

            // End.
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

        case NODE_BLOCK: {
            // RAII for nested `{ ... }` scopes: locals declared inside
            // the block get freed when the block ends, mirroring
            // src/codegen.c's emit_scope_cleanup(scope_start). We
            // remember local_count at entry and only sweep what the
            // block itself added.
            int scope_start = c->local_count;
            gen_block(c, n);
            emit_scope_cleanup(c, scope_start);
            break;
        }

        case NODE_ASSERT: {
            // Mirror src/codegen.c:869-877. Evaluate the condition; if
            // non-zero, fall through. If zero, call _assert_fail with
            // the source line as x0; that helper prints the line, then
            // " assertion failed", then exits 1 (it never returns).
            VReg cond = gen_expr(c, n->assert_stmt.condition);
            int ok_lbl = new_label(c);
            int fail_lbl = new_label(c);
            emit(c, (IRInst){.op = IR_BR_COND, .a = cond, .label = ok_lbl, .label2 = fail_lbl});

            IRBlock *fail_b = new_block(c);
            fail_b->label = fail_lbl;
            switch_block(c, fail_b);
            VReg line_no = new_vreg(c);
            emit(c, (IRInst){.op = IR_CONST, .dst = line_no, .imm = n->line});
            VReg *aa = malloc(sizeof(VReg));
            aa[0] = line_no;
            VReg ignored = new_vreg(c);
            emit(c, (IRInst){.op = IR_CALL, .dst = ignored, .str = "assert_fail",
                             .args = aa, .arg_count = 1});
            // _assert_fail exits, but emit a branch anyway so the
            // basic-block graph is well-formed (every block ends with
            // either a branch or a return).
            emit(c, (IRInst){.op = IR_BR, .label = ok_lbl});

            IRBlock *ok_b = new_block(c);
            ok_b->label = ok_lbl;
            switch_block(c, ok_b);
            break;
        }

        case NODE_MATCH: {
            // Stash the match scrutinee in a hidden local so each arm
            // can re-read it (each arm reloads to extract its bindings
            // and to read the tag). Direct codegen does the equivalent
            // by pushing onto the stack (`str x0, [sp, #-16]!`).
            VReg scrutinee = gen_expr(c, n->match_expr.expr);
            int scrut_slot = c->slot_next++;
            emit(c, (IRInst){.op = IR_STORE_LOCAL, .a = scrutinee, .imm = scrut_slot});

            int end_lbl = new_label(c);

            for (int i = 0; i < n->match_expr.arm_count; i++) {
                int hit_lbl = new_label(c);   // arm body
                int miss_lbl = new_label(c);  // try next arm

                // Resolve the variant's tag index by name (enum order
                // determines tag numbering). Falls back to arm order
                // if the variant name isn't found in any registered
                // enum — same conservative behaviour as direct codegen.
                int tag = i;
                const char *vname = n->match_expr.arm_variant_names[i];
                if (c->program) {
                    for (int ei = 0; ei < c->program->program.enums.count; ei++) {
                        Node *e = c->program->program.enums.items[ei];
                        int found = 0;
                        for (int vi = 0; vi < e->enum_def.variant_count; vi++) {
                            if (!strcmp(e->enum_def.variant_names[vi], vname)) {
                                tag = vi;
                                found = 1;
                                break;
                            }
                        }
                        if (found) break;
                    }
                }

                // Load scrutinee, then tag (offset 0).
                VReg scrut = new_vreg(c);
                emit(c, (IRInst){.op = IR_LOAD_LOCAL, .dst = scrut, .imm = scrut_slot});
                VReg actual_tag = new_vreg(c);
                emit(c, (IRInst){.op = IR_LOAD, .dst = actual_tag, .a = scrut, .imm = 0});

                // Compare actual_tag == expected_tag.
                VReg expected_tag = new_vreg(c);
                emit(c, (IRInst){.op = IR_CONST, .dst = expected_tag, .imm = tag});
                VReg matches = new_vreg(c);
                emit(c, (IRInst){.op = IR_CMP_EQ, .dst = matches,
                                 .a = actual_tag, .b = expected_tag});
                emit(c, (IRInst){.op = IR_BR_COND, .a = matches,
                                 .label = hit_lbl, .label2 = miss_lbl});

                // hit_lbl: bind variant fields then run body.
                IRBlock *hit_b = new_block(c);
                hit_b->label = hit_lbl;
                switch_block(c, hit_b);
                for (int b = 0; b < n->match_expr.arm_binding_counts[i]; b++) {
                    VReg s2 = new_vreg(c);
                    emit(c, (IRInst){.op = IR_LOAD_LOCAL, .dst = s2, .imm = scrut_slot});
                    VReg field = new_vreg(c);
                    emit(c, (IRInst){.op = IR_LOAD, .dst = field, .a = s2,
                                     .imm = (b + 1) * 8});
                    set_local(c, n->match_expr.arm_bindings[i][b], field);
                }
                gen_block(c, n->match_expr.arm_bodies[i]);
                emit(c, (IRInst){.op = IR_BR, .label = end_lbl});

                // miss_lbl: try the next arm (or fall through to end_lbl
                // if this was the last one).
                IRBlock *miss_b = new_block(c);
                miss_b->label = miss_lbl;
                switch_block(c, miss_b);
            }
            // No arm matched: continue past end. (Direct codegen falls
            // through similarly; an explicit panic could be added later.)
            emit(c, (IRInst){.op = IR_BR, .label = end_lbl});

            IRBlock *end_b = new_block(c);
            end_b->label = end_lbl;
            switch_block(c, end_b);
            break;
        }

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
        // Function fall-off cleanup: any heap locals still live at the
        // end of the body must be freed before we return. Mirrors
        // src/codegen.c::emit_func's terminal emit_scope_cleanup. If
        // the body already ended in IR_RET/IR_RET_VOID (which call
        // emit_scope_cleanup themselves), this sweep finds nothing
        // because everything is marked moved.
        emit_scope_cleanup(&ctx, 0);

        irf->vreg_count = ctx.vreg_next;
        irf->local_slots = ctx.slot_next;
        free(ctx.local_names);
        free(ctx.local_vregs);
        free(ctx.local_is_mut);
        free(ctx.local_slots);
        free(ctx.local_is_heap);
        free(ctx.local_is_moved);
        free(ctx.local_alloc_sizes);
        free(ctx.local_is_array);
        free(ctx.local_array_esz);
    }

    // Synthesize one IRFunc per `test "name" { ... }` block, named
    // `_test_<N>` to mirror what the direct codegen emits. main.c's IR
    // mode then emits a multi-test _start that announces each test by
    // name and invokes _test_<N>; if the body contains a failing
    // assert, _assert_fail exits the process.
    for (int i = 0; i < program->program.tests.count; i++) {
        Node *t = program->program.tests.items[i];

        if (ir->func_count >= ir->func_cap) {
            ir->func_cap = ir->func_cap ? ir->func_cap * 2 : 8;
            ir->funcs = realloc(ir->funcs, ir->func_cap * sizeof(IRFunc));
        }
        IRFunc *irf = &ir->funcs[ir->func_count++];
        memset(irf, 0, sizeof(IRFunc));
        char buf[64];
        snprintf(buf, sizeof(buf), "test_%d", i);
        irf->name = strdup(buf);
        irf->param_count = 0;

        IRGenCtx ctx = {0};
        ctx.func = irf;
        ctx.block = new_block(&ctx);
        ctx.loop_start = -1;
        ctx.loop_end = -1;
        ctx.program = program;

        gen_block(&ctx, t->test_def.body);
        // Test bodies don't have an explicit return; emit cleanup +
        // RET_VOID. The cleanup matches what NODE_GIVE would have
        // emitted if the test ended in `give`.
        emit_scope_cleanup(&ctx, 0);
        emit(&ctx, (IRInst){.op = IR_RET_VOID});

        irf->vreg_count = ctx.vreg_next;
        irf->local_slots = ctx.slot_next;
        free(ctx.local_names);
        free(ctx.local_vregs);
        free(ctx.local_is_mut);
        free(ctx.local_slots);
        free(ctx.local_is_heap);
        free(ctx.local_is_moved);
        free(ctx.local_alloc_sizes);
        free(ctx.local_is_array);
        free(ctx.local_array_esz);
    }

    return ir;
}
