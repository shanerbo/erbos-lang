#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "codegen.h"

typedef struct {
    FILE *out;
    int label_count;
    // String literals
    char **strings;
    int string_count;
    int string_cap;
    // Locals
    char **locals;
    int *offsets;
    int *nomut_flags;
    int *is_heap;       // 1 if heap-allocated (needs free at scope end)
    int *is_moved;      // 1 if ownership was moved out
    int *alloc_sizes;   // size of heap allocation (for free)
    int local_count;
    int local_cap;
    int stack_size;
    // Loop labels for stop/skip
    int loop_start_label;
    int loop_end_label;
    int loop_continue_label;  // jumps to increment
    // Struct info
    char **struct_names;
    char ***struct_field_names;
    int *struct_field_counts;
    int struct_count;
    // Enum info
    char **enum_names;
    char ***enum_variant_names;
    int *enum_variant_counts;
    int enum_count;
    // Import aliases
    char **import_aliases;
    int import_count;
} Gen;

static int new_label(Gen *g) { return g->label_count++; }

static int add_string(Gen *g, const char *s) {
    if (g->string_count >= g->string_cap) {
        g->string_cap = g->string_cap ? g->string_cap * 2 : 8;
        g->strings = realloc(g->strings, g->string_cap * sizeof(char *));
    }
    g->strings[g->string_count] = (char *)s;
    return g->string_count++;
}

static int find_local(Gen *g, const char *name) {
    for (int i = g->local_count - 1; i >= 0; i--)
        if (!strcmp(g->locals[i], name)) return g->offsets[i];
    return -1;
}

static int is_nomut(Gen *g, const char *name) {
    for (int i = g->local_count - 1; i >= 0; i--)
        if (!strcmp(g->locals[i], name)) return g->nomut_flags[i];
    return 0;
}

static int add_local(Gen *g, const char *name, int nomut) {
    if (g->local_count >= g->local_cap) {
        g->local_cap = g->local_cap ? g->local_cap * 2 : 8;
        g->locals = realloc(g->locals, g->local_cap * sizeof(char *));
        g->offsets = realloc(g->offsets, g->local_cap * sizeof(int));
        g->nomut_flags = realloc(g->nomut_flags, g->local_cap * sizeof(int));
        g->is_heap = realloc(g->is_heap, g->local_cap * sizeof(int));
        g->is_moved = realloc(g->is_moved, g->local_cap * sizeof(int));
        g->alloc_sizes = realloc(g->alloc_sizes, g->local_cap * sizeof(int));
    }
    g->stack_size += 16;
    g->locals[g->local_count] = (char *)name;
    g->offsets[g->local_count] = g->stack_size;
    g->nomut_flags[g->local_count] = nomut;
    g->is_heap[g->local_count] = 0;
    g->is_moved[g->local_count] = 0;
    g->alloc_sizes[g->local_count] = 0;
    g->local_count++;
    return g->stack_size;
}

static void emit_expr(Gen *g, Node *n);
static void emit_stmt(Gen *g, Node *n);

// Emit load/store that handles offsets > 255 (ARM64 limitation)
static void emit_store_local(Gen *g, int reg, int offset) {
    if (offset <= 255) {
        fprintf(g->out, "    str x%d, [x29, #-%d]\n", reg, offset);
    } else {
        fprintf(g->out, "    sub x9, x29, #%d\n", offset);
        fprintf(g->out, "    str x%d, [x9]\n", reg);
    }
}

static void emit_load_local(Gen *g, int reg, int offset) {
    if (offset <= 255) {
        fprintf(g->out, "    ldr x%d, [x29, #-%d]\n", reg, offset);
    } else {
        fprintf(g->out, "    sub x9, x29, #%d\n", offset);
        fprintf(g->out, "    ldr x%d, [x9]\n", reg);
    }
}

static void mark_heap(Gen *g, const char *name) {
    for (int i = 0; i < g->local_count; i++)
        if (!strcmp(g->locals[i], name)) { g->is_heap[i] = 1; return; }
}

static void mark_heap_size(Gen *g, const char *name, int size) {
    for (int i = 0; i < g->local_count; i++)
        if (!strcmp(g->locals[i], name)) { g->is_heap[i] = 1; g->alloc_sizes[i] = size; return; }
}

static void mark_moved(Gen *g, const char *name) {
    for (int i = 0; i < g->local_count; i++)
        if (!strcmp(g->locals[i], name)) { g->is_moved[i] = 1; return; }
}

static int check_moved(Gen *g, const char *name) {
    for (int i = g->local_count - 1; i >= 0; i--)
        if (!strcmp(g->locals[i], name)) return g->is_moved[i];
    return 0;
}

// Emit frees for all heap locals from index 'from' to current that haven't been moved
static void emit_scope_cleanup(Gen *g, int from) {
    for (int i = from; i < g->local_count; i++) {
        if (g->is_heap[i] && !g->is_moved[i]) {
            emit_load_local(g, 0, g->offsets[i]); // load ptr into x0
            fprintf(g->out, "    mov x1, #%d\n", g->alloc_sizes[i] ? g->alloc_sizes[i] : 16);
            fprintf(g->out, "    bl _heap_free\n");
        }
    }
}

static void emit_expr(Gen *g, Node *n) {
    switch (n->type) {
        case NODE_INT_LIT:
            if (n->int_lit.value >= 0 && n->int_lit.value < 65536)
                fprintf(g->out, "    mov x0, #%ld\n", n->int_lit.value);
            else if (n->int_lit.value < 0 && n->int_lit.value > -65536)
                fprintf(g->out, "    mov x0, #%ld\n", n->int_lit.value);
            else
                fprintf(g->out, "    mov x0, #%ld\n", n->int_lit.value);
            break;
        case NODE_BOOL_LIT:
            fprintf(g->out, "    mov x0, #%d\n", n->bool_lit.value);
            break;
        case NODE_STR_LIT: {
            // Check for interpolation: {varname}
            const char *s = n->str_lit.value;
            int has_interp = 0;
            for (int i = 0; s[i]; i++) { if (s[i] == '{') { has_interp = 1; break; } }

            if (!has_interp) {
                int idx = add_string(g, s);
                fprintf(g->out, "    adrp x0, _str%d@PAGE\n", idx);
                fprintf(g->out, "    add x0, x0, _str%d@PAGEOFF\n", idx);
            } else {
                // Build string by concatenating parts
                // First part: emit empty string as accumulator
                int empty_idx = add_string(g, "");
                fprintf(g->out, "    adrp x0, _str%d@PAGE\n", empty_idx);
                fprintf(g->out, "    add x0, x0, _str%d@PAGEOFF\n", empty_idx);

                int i = 0;
                while (s[i]) {
                    if (s[i] == '{') {
                        // Extract variable name
                        i++;
                        char varname[64];
                        int vi = 0;
                        while (s[i] && s[i] != '}' && vi < 63) { varname[vi++] = s[i++]; }
                        varname[vi] = '\0';
                        if (s[i] == '}') i++;
                        // Save accumulator
                        fprintf(g->out, "    str x0, [sp, #-16]!\n");
                        // Load variable and convert to string if needed
                        int off = find_local(g, varname);
                        if (off < 0) { fprintf(stderr, "error line %d: undefined variable '%s' in interpolation\n", n->line, varname); exit(1); }
                        emit_load_local(g, 0, off);
                        // If value > 0x100000, it's already a string pointer — skip conversion
                        fprintf(g->out, "    mov x1, #0x100000\n");
                        int skip = new_label(g);
                        fprintf(g->out, "    cmp x0, x1\n");
                        fprintf(g->out, "    b.ge _L%d\n", skip);
                        fprintf(g->out, "    bl _int_to_str\n");
                        fprintf(g->out, "_L%d:\n", skip);
                        // Concat: accumulator + var_str
                        fprintf(g->out, "    mov x1, x0\n");
                        fprintf(g->out, "    ldr x0, [sp], #16\n");
                        fprintf(g->out, "    bl _str_concat\n");
                    } else {
                        // Extract literal segment
                        char seg[256];
                        int si = 0;
                        while (s[i] && s[i] != '{' && si < 255) { seg[si++] = s[i++]; }
                        seg[si] = '\0';
                        // Save accumulator, concat segment
                        fprintf(g->out, "    str x0, [sp, #-16]!\n");
                        int seg_idx = add_string(g, strdup(seg));
                        fprintf(g->out, "    adrp x1, _str%d@PAGE\n", seg_idx);
                        fprintf(g->out, "    add x1, x1, _str%d@PAGEOFF\n", seg_idx);
                        fprintf(g->out, "    ldr x0, [sp], #16\n");
                        fprintf(g->out, "    bl _str_concat\n");
                    }
                }
            }
            break;
        }
        case NODE_IDENT: {
            if (check_moved(g, n->ident.name)) {
                // Only error if it's a heap-allocated variable
                int is_heap_var = 0;
                for (int i = 0; i < g->local_count; i++)
                    if (!strcmp(g->locals[i], n->ident.name)) { is_heap_var = g->is_heap[i]; break; }
                if (is_heap_var) {
                    fprintf(stderr, "error:%d: use of moved variable '%s'\n", n->line, n->ident.name);
                    exit(1);
                }
            }
            int off = find_local(g, n->ident.name);
            if (off < 0) { fprintf(stderr, "error: undefined variable '%s'\n", n->ident.name); exit(1); }
            emit_load_local(g, 0, off);
            break;
        }
        case NODE_BINARY: {
            emit_expr(g, n->binary.right);
            fprintf(g->out, "    str x0, [sp, #-16]!\n");
            emit_expr(g, n->binary.left);
            fprintf(g->out, "    ldr x1, [sp], #16\n");
            switch (n->binary.op) {
                case TOK_PLUS:
                    if (n->resolved_type == 2) {
                        // str + str → _str_concat(left, right)
                        // left in x0, right in x1
                        fprintf(g->out, "    bl _str_concat\n");
                    } else {
                        fprintf(g->out, "    add x0, x0, x1\n");
                    }
                    break;
                case TOK_MINUS:
                    fprintf(g->out, "    sub x0, x0, x1\n"); break;
                case TOK_STAR:
                    fprintf(g->out, "    mul x0, x0, x1\n"); break;
                case TOK_SLASH:
                    fprintf(g->out, "    sdiv x0, x0, x1\n"); break;
                case TOK_PERCENT: case TOK_MOD_WORD:                    fprintf(g->out, "    sdiv x2, x0, x1\n");
                    fprintf(g->out, "    msub x0, x2, x1, x0\n");
                    break;
                case TOK_EQ: case TOK_EQ_WORD:
                    if (n->resolved_type == 2) {
                        fprintf(g->out, "    bl _str_eq\n");
                    } else {
                        fprintf(g->out, "    cmp x0, x1\n    cset x0, eq\n");
                    }
                    break;
                case TOK_NEQ: case TOK_NE_WORD:
                    if (n->resolved_type == 2) {
                        fprintf(g->out, "    bl _str_eq\n");
                        fprintf(g->out, "    eor x0, x0, #1\n");
                    } else {
                        fprintf(g->out, "    cmp x0, x1\n    cset x0, ne\n");
                    }
                    break;
                case TOK_LT: case TOK_LT_WORD:
                    fprintf(g->out, "    cmp x0, x1\n    cset x0, lt\n"); break;
                case TOK_GT: case TOK_GT_WORD:
                    fprintf(g->out, "    cmp x0, x1\n    cset x0, gt\n"); break;
                case TOK_LTE: case TOK_LE_WORD:
                    fprintf(g->out, "    cmp x0, x1\n    cset x0, le\n"); break;
                case TOK_GTE: case TOK_GE_WORD:
                    fprintf(g->out, "    cmp x0, x1\n    cset x0, ge\n"); break;
                case TOK_AND: {
                    // Short-circuit: if left is 0, skip right
                    int skip_lbl = new_label(g);
                    fprintf(g->out, "    cbz x0, _L%d\n", skip_lbl); // left false → result 0
                    fprintf(g->out, "    mov x0, x1\n"); // left true → result = right
                    fprintf(g->out, "_L%d:\n", skip_lbl);
                    break;
                }
                case TOK_OR: {
                    // Short-circuit: if left is 1, skip right
                    int skip_lbl = new_label(g);
                    fprintf(g->out, "    cbnz x0, _L%d\n", skip_lbl); // left true → result = left (1)
                    fprintf(g->out, "    mov x0, x1\n"); // left false → result = right
                    fprintf(g->out, "_L%d:\n", skip_lbl);
                    break;
                }
                default: break;
            }
            break;
        }
        case NODE_UNARY:
            emit_expr(g, n->unary.operand);
            if (n->unary.op == TOK_MINUS) fprintf(g->out, "    neg x0, x0\n");
            else if (n->unary.op == TOK_NOT) fprintf(g->out, "    eor x0, x0, #1\n");
            break;
        case NODE_CALL: {
            // Rewrite constructors: Point() → _alloc_Point, list() → _list_new, map() → _map_new
            const char *call_name = n->call.name;
            char actual_name[128];

            // Check if it's a struct constructor
            int is_struct = 0;
            for (int i = 0; i < g->struct_count; i++) {
                if (!strcmp(g->struct_names[i], call_name)) { is_struct = 1; break; }
            }
            if (is_struct) {
                snprintf(actual_name, sizeof(actual_name), "_alloc_%s", call_name);
            } else if (!strcmp(call_name, "list")) {
                snprintf(actual_name, sizeof(actual_name), "_list_new");
            } else if (!strcmp(call_name, "map")) {
                snprintf(actual_name, sizeof(actual_name), "_map_new");
            } else if (!strcmp(call_name, "imap")) {
                snprintf(actual_name, sizeof(actual_name), "_imap_new");
            } else if (!strcmp(call_name, "task")) {
                // task() just returns 0 — placeholder handle
                fprintf(g->out, "    mov x0, #0\n");
                break;
            } else if (!strcmp(call_name, "len")) {
                // Universal len(): list count at [8], map count at [8], string via _str_len
                emit_expr(g, n->call.args[0]);
                if (n->call.args[0]->resolved_type == 5) {
                    fprintf(g->out, "    bl _str_len\n"); // string
                } else {
                    fprintf(g->out, "    ldr x0, [x0, #8]\n"); // list or map (both at offset 8)
                }
                break;
            } else if (!strcmp(call_name, "str_len")) {
                emit_expr(g, n->call.args[0]);
                fprintf(g->out, "    bl _str_len\n");
                break;
            } else if (!strcmp(call_name, "char_at")) {
                // char_at(str, index) → 1-char string on heap
                emit_expr(g, n->call.args[1]); // index
                fprintf(g->out, "    str x0, [sp, #-16]!\n");
                emit_expr(g, n->call.args[0]); // string
                fprintf(g->out, "    ldr x1, [sp], #16\n"); // x1 = index
                fprintf(g->out, "    bl _char_at\n");
                break;
            } else {
                snprintf(actual_name, sizeof(actual_name), "_%s", call_name);
            }

            for (int i = n->call.arg_count - 1; i >= 0; i--) {
                emit_expr(g, n->call.args[i]);
                fprintf(g->out, "    str x0, [sp, #-16]!\n");
            }
            for (int i = 0; i < n->call.arg_count && i < 8; i++)
                fprintf(g->out, "    ldr x%d, [sp], #16\n", i);
            fprintf(g->out, "    bl %s\n", actual_name);
            break;
        }
        case NODE_METHOD_CALL: {
            // Check if this is an enum constructor: EnumName.Variant(args)
            if (n->method_call.object->type == NODE_IDENT) {
                const char *obj_name = n->method_call.object->ident.name;
                // Check if obj_name is an enum
                int enum_idx = -1;
                int variant_idx = -1;
                for (int ei = 0; ei < g->enum_count; ei++) {
                    if (!strcmp(g->enum_names[ei], obj_name)) {
                        enum_idx = ei;
                        for (int vi = 0; vi < g->enum_variant_counts[ei]; vi++) {
                            if (!strcmp(g->enum_variant_names[ei][vi], n->method_call.method)) {
                                variant_idx = vi;
                                break;
                            }
                        }
                        break;
                    }
                }
                if (enum_idx >= 0 && variant_idx >= 0) {
                    // Emit enum construction: alloc [tag | field0 | field1 | ...]
                    int nfields = n->method_call.arg_count;
                    int size = (nfields + 1) * 8;
                    fprintf(g->out, "    mov x0, #%d\n", size > 16 ? size : 16);
                    fprintf(g->out, "    bl _heap_alloc\n");
                    fprintf(g->out, "    mov x1, #%d\n", variant_idx); // tag
                    fprintf(g->out, "    str x1, [x0]\n");
                    for (int fi = 0; fi < nfields; fi++) {
                        fprintf(g->out, "    str x0, [sp, #-16]!\n");
                        emit_expr(g, n->method_call.args[fi]);
                        fprintf(g->out, "    mov x1, x0\n");
                        fprintf(g->out, "    ldr x0, [sp], #16\n");
                        fprintf(g->out, "    str x1, [x0, #%d]\n", (fi + 1) * 8);
                    }
                    break;
                }
            }
            // Regular method call (existing code)
            const char *method = n->method_call.method;

            // User method on a struct: tagged by the checker as resolved_struct_name.
            // Lower as `_<Type>_<method>(self, args...)` with the receiver becoming x0
            // and the user-declared args going into x1, x2, ... in source order.
            if (n->method_call.resolved_struct_name) {
                emit_expr(g, n->method_call.object);
                fprintf(g->out, "    str x0, [sp, #-16]!\n"); // save receiver
                for (int i = n->method_call.arg_count - 1; i >= 0; i--) {
                    emit_expr(g, n->method_call.args[i]);
                    fprintf(g->out, "    str x0, [sp, #-16]!\n");
                }
                for (int i = 0; i < n->method_call.arg_count; i++)
                    fprintf(g->out, "    ldr x%d, [sp], #16\n", i + 1);
                fprintf(g->out, "    ldr x0, [sp], #16\n"); // receiver into x0
                fprintf(g->out, "    bl _%s_%s\n", n->method_call.resolved_struct_name, method);
                break;
            }

            // List methods: push, pop, len
            if (!strcmp(method, "push") || !strcmp(method, "pop") || !strcmp(method, "len")) {
                emit_expr(g, n->method_call.object);
                if (n->method_call.arg_count == 1) {
                    fprintf(g->out, "    str x0, [sp, #-16]!\n");
                    emit_expr(g, n->method_call.args[0]);
                    fprintf(g->out, "    mov x1, x0\n");
                    fprintf(g->out, "    ldr x0, [sp], #16\n");
                }
                fprintf(g->out, "    bl _list_%s\n", method);
                break;
            }
            // Map methods: set, get, len, keys
            if (!strcmp(method, "set") || !strcmp(method, "get") || !strcmp(method, "keys")) {
                emit_expr(g, n->method_call.object);
                fprintf(g->out, "    str x0, [sp, #-16]!\n");
                for (int i = n->method_call.arg_count - 1; i >= 0; i--) {
                    emit_expr(g, n->method_call.args[i]);
                    fprintf(g->out, "    str x0, [sp, #-16]!\n");
                }
                for (int i = 0; i < n->method_call.arg_count; i++)
                    fprintf(g->out, "    ldr x%d, [sp], #16\n", i + 1);
                fprintf(g->out, "    ldr x0, [sp], #16\n");
                fprintf(g->out, "    bl _map_%s\n", method);
                break;
            }
            // Task methods
            if (!strcmp(method, "fire") || !strcmp(method, "collapse")) {
                // fire: the arg is already called as a function expression
                for (int i = 0; i < n->method_call.arg_count; i++)
                    emit_expr(g, n->method_call.args[i]);
                fprintf(g->out, "    bl _task_%s\n", method);
                break;
            }
            // Check if object is an import alias (module.function() call)
            if (n->method_call.object->type == NODE_IDENT) {
                const char *obj_name = n->method_call.object->ident.name;
                for (int ai = 0; ai < g->import_count; ai++) {
                    if (!strcmp(g->import_aliases[ai], obj_name)) {
                        // Module call: alias.func(args) → _alias_func(args)
                        for (int i = n->method_call.arg_count - 1; i >= 0; i--) {
                            emit_expr(g, n->method_call.args[i]);
                            fprintf(g->out, "    str x0, [sp, #-16]!\n");
                        }
                        for (int i = 0; i < n->method_call.arg_count && i < 8; i++)
                            fprintf(g->out, "    ldr x%d, [sp], #16\n", i);
                        fprintf(g->out, "    bl _%s_%s\n", obj_name, method);
                        goto method_done;
                    }
                }
            }
            // User-defined method: obj.method(args) → _method(obj, args)
            emit_expr(g, n->method_call.object);
            fprintf(g->out, "    str x0, [sp, #-16]!\n");
            for (int i = n->method_call.arg_count - 1; i >= 0; i--) {
                emit_expr(g, n->method_call.args[i]);
                fprintf(g->out, "    str x0, [sp, #-16]!\n");
            }
            for (int i = 0; i < n->method_call.arg_count; i++)
                fprintf(g->out, "    ldr x%d, [sp], #16\n", i + 1);
            fprintf(g->out, "    ldr x0, [sp], #16\n");
            fprintf(g->out, "    bl _%s\n", method);
            method_done:
            break;
        }
        case NODE_FIELD_ACCESS: {
            emit_expr(g, n->field_access.object);
            fprintf(g->out, "    // field access .%s\n", n->field_access.field);
            // Type-aware: use struct_name if available
            int found = 0;
            if (n->field_access.struct_name) {
                for (int i = 0; i < g->struct_count && !found; i++) {
                    if (!strcmp(g->struct_names[i], n->field_access.struct_name)) {
                        for (int j = 0; j < g->struct_field_counts[i]; j++) {
                            if (!strcmp(g->struct_field_names[i][j], n->field_access.field)) {
                                fprintf(g->out, "    ldr x0, [x0, #%d]\n", j * 8);
                                found = 1;
                                break;
                            }
                        }
                    }
                }
            }
            if (!found) {
                // Untyped receiver (BST/linked-list "struct stored as int" pattern).
                // Accept the global search ONLY if the field name has a unique
                // offset across every struct; otherwise the access is ambiguous
                // and the user must annotate the receiver with an explicit type.
                int unique_offset = -1;
                int matches = 0;
                const char *first_match_struct = NULL;
                const char *second_match_struct = NULL;
                for (int i = 0; i < g->struct_count; i++) {
                    for (int j = 0; j < g->struct_field_counts[i]; j++) {
                        if (!strcmp(g->struct_field_names[i][j], n->field_access.field)) {
                            int off = j * 8;
                            if (matches == 0) {
                                unique_offset = off;
                                first_match_struct = g->struct_names[i];
                            } else if (off != unique_offset) {
                                if (!second_match_struct) second_match_struct = g->struct_names[i];
                            }
                            matches++;
                            break;
                        }
                    }
                }
                if (matches == 0) {
                    // No struct has this field at all — emit a single load at
                    // offset 0 to preserve historical behaviour for synthetic
                    // accesses (NODE_FIELD_ACCESS appearing in interpolation
                    // resolution before any struct context is established).
                    fprintf(g->out, "    ldr x0, [x0]\n");
                } else if (second_match_struct) {
                    fprintf(stderr, "error:%d: ambiguous field '%s' on receiver of unknown type "
                        "(it could be '%s.%s' at one offset and '%s.%s' at another). "
                        "Annotate the receiver with an explicit type.\n",
                        n->line, n->field_access.field,
                        first_match_struct, n->field_access.field,
                        second_match_struct, n->field_access.field);
                    exit(1);
                } else {
                    fprintf(g->out, "    ldr x0, [x0, #%d]\n", unique_offset);
                }
            }
            break;
        }
        case NODE_INDEX: {
            emit_expr(g, n->index_access.index);
            fprintf(g->out, "    str x0, [sp, #-16]!\n"); // save index
            emit_expr(g, n->index_access.object);
            fprintf(g->out, "    ldr x1, [sp], #16\n");   // x1 = index
            // Header format: [cap | count | data_ptr]
            // Bounds check
            fprintf(g->out, "    cmp x1, #0\n");
            fprintf(g->out, "    b.lt _panic_oob\n");
            fprintf(g->out, "    ldr x2, [x0, #8]\n");    // count
            fprintf(g->out, "    cmp x1, x2\n");
            fprintf(g->out, "    b.ge _panic_oob\n");
            fprintf(g->out, "    ldr x0, [x0, #16]\n");   // data_ptr
            fprintf(g->out, "    ldr x0, [x0, x1, lsl #3]\n");
            break;
        }
        case NODE_LIST_LIT: {
            // Allocate as header format (same as list()) for consistency
            int count = n->list_lit.count;
            // Alloc header
            fprintf(g->out, "    mov x0, #24\n");
            fprintf(g->out, "    bl _heap_alloc\n");
            fprintf(g->out, "    str x0, [sp, #-16]!\n"); // save header
            // Alloc data
            fprintf(g->out, "    mov x0, #%d\n", count * 8 > 64 ? count * 8 : 64);
            fprintf(g->out, "    bl _heap_alloc\n");
            fprintf(g->out, "    mov x1, x0\n");          // x1 = data
            fprintf(g->out, "    ldr x0, [sp], #16\n");   // x0 = header
            fprintf(g->out, "    mov x2, #%d\n", count > 8 ? count : 8);
            fprintf(g->out, "    str x2, [x0]\n");        // capacity
            fprintf(g->out, "    mov x2, #%d\n", count);
            fprintf(g->out, "    str x2, [x0, #8]\n");    // count
            fprintf(g->out, "    str x1, [x0, #16]\n");   // data_ptr
            // Fill elements
            for (int i = 0; i < count; i++) {
                fprintf(g->out, "    str x0, [sp, #-16]!\n");
                fprintf(g->out, "    str x1, [sp, #-16]!\n");
                emit_expr(g, n->list_lit.items[i]);
                fprintf(g->out, "    mov x2, x0\n");
                fprintf(g->out, "    ldr x1, [sp], #16\n");
                fprintf(g->out, "    ldr x0, [sp], #16\n");
                fprintf(g->out, "    str x2, [x1, #%d]\n", i * 8);
            }
            break;
        }
        case NODE_MAP_LIT: {
            // Create map then set each entry
            fprintf(g->out, "    bl _map_new\n");
            for (int i = 0; i < n->map_lit.count; i++) {
                fprintf(g->out, "    str x0, [sp, #-16]!\n"); // save map
                emit_expr(g, n->map_lit.values[i]);
                fprintf(g->out, "    str x0, [sp, #-16]!\n"); // save value
                emit_expr(g, n->map_lit.keys[i]);
                fprintf(g->out, "    mov x1, x0\n");          // key in x1
                fprintf(g->out, "    ldr x2, [sp], #16\n");   // value in x2
                fprintf(g->out, "    ldr x0, [sp], #16\n");   // map in x0
                fprintf(g->out, "    str x0, [sp, #-16]!\n"); // save map again
                fprintf(g->out, "    bl _map_set\n");
                fprintf(g->out, "    ldr x0, [sp], #16\n");   // restore map
            }
            break;
        }
        default:
            fprintf(stderr, "error: cannot emit expression node type %d\n", n->type);
            exit(1);
    }
}

static void emit_stmt(Gen *g, Node *n) {
    switch (n->type) {
        case NODE_VAR_DECL: {
            int off = add_local(g, n->var_decl.name, n->var_decl.is_nomut);

            if (n->var_decl.is_move) {
                // Move: copy value, mark source as moved
                emit_expr(g, n->var_decl.value);
                emit_store_local(g, 0, off);
                // Mark source as moved (if it's an ident)
                if (n->var_decl.value->type == NODE_IDENT) {
                    mark_moved(g, n->var_decl.value->ident.name);
                }
                // New owner is heap
                mark_heap(g, n->var_decl.name);
            } else if (n->var_decl.is_rep) {
                // Clone: call _heap_alloc + memcpy (simplified: just copy pointer for now)
                // TODO: deep clone. For now, shallow copy (same as regular assign)
                emit_expr(g, n->var_decl.value);
                emit_store_local(g, 0, off);
                mark_heap(g, n->var_decl.name);
            } else {
                emit_expr(g, n->var_decl.value);
                emit_store_local(g, 0, off);
                // Detect heap allocations: struct constructors, list(), map()
                if (n->var_decl.value->type == NODE_CALL) {
                    const char *fn = n->var_decl.value->call.name;
                    int is_struct_alloc = 0;
                    int struct_size = 16;
                    for (int si = 0; si < g->struct_count; si++) {
                        if (!strcmp(g->struct_names[si], fn)) {
                            is_struct_alloc = 1;
                            struct_size = g->struct_field_counts[si] * 8;
                            if (struct_size == 0) struct_size = 8;
                            break;
                        }
                    }
                    if (is_struct_alloc) {
                        mark_heap_size(g, n->var_decl.name, struct_size);
                    } else if (!strcmp(fn, "list")) {
                        mark_heap_size(g, n->var_decl.name, 520);
                    } else if (!strcmp(fn, "map") || !strcmp(fn, "imap")) {
                        mark_heap_size(g, n->var_decl.name, 152); // 24 header + 128 data
                    } else if (strncmp(fn, "alloc_", 6) == 0 || !strcmp(fn, "list_new") || !strcmp(fn, "map_new")) {
                        mark_heap_size(g, n->var_decl.name, 520);
                    }
                }
            }
            break;
        }
        case NODE_ASSIGN: {
            if (is_nomut(g, n->assign.name)) {
                fprintf(stderr, "error:%d: cannot reassign nomut variable '%s'\n", n->line, n->assign.name);
                exit(1);
            }
            int off = find_local(g, n->assign.name);
            if (off < 0) { fprintf(stderr, "error:%d: undefined variable '%s'\n", n->line, n->assign.name); exit(1); }
            emit_expr(g, n->assign.value);
            emit_store_local(g, 0, off);
            break;
        }
        case NODE_FIELD_ASSIGN: {
            emit_expr(g, n->field_assign.value);
            fprintf(g->out, "    str x0, [sp, #-16]!\n");
            emit_expr(g, n->field_assign.object);
            fprintf(g->out, "    ldr x1, [sp], #16\n");
            int found = 0;
            // Type-aware: when the checker resolved a static struct type,
            // pick that struct's field offset deterministically.
            if (n->field_assign.struct_name) {
                for (int i = 0; i < g->struct_count && !found; i++) {
                    if (!strcmp(g->struct_names[i], n->field_assign.struct_name)) {
                        for (int j = 0; j < g->struct_field_counts[i]; j++) {
                            if (!strcmp(g->struct_field_names[i][j], n->field_assign.field)) {
                                fprintf(g->out, "    str x1, [x0, #%d]\n", j * 8);
                                found = 1;
                                break;
                            }
                        }
                    }
                }
            }
            if (!found) {
                // Untyped receiver (e.g. struct stored as int for BST/linked-list
                // patterns). Accept the global search ONLY if the field name
                // resolves to a single offset across all structs; otherwise
                // it is ambiguous and the user must annotate the receiver.
                int unique_offset = -1;
                int matches = 0;
                const char *first_match_struct = NULL;
                const char *second_match_struct = NULL;
                for (int i = 0; i < g->struct_count; i++) {
                    for (int j = 0; j < g->struct_field_counts[i]; j++) {
                        if (!strcmp(g->struct_field_names[i][j], n->field_assign.field)) {
                            int off = j * 8;
                            if (matches == 0) {
                                unique_offset = off;
                                first_match_struct = g->struct_names[i];
                            } else if (off != unique_offset) {
                                if (!second_match_struct) second_match_struct = g->struct_names[i];
                            }
                            matches++;
                            break;
                        }
                    }
                }
                if (matches == 0) {
                    fprintf(stderr, "error:%d: assignment to unknown field '%s'\n",
                        n->line, n->field_assign.field);
                    exit(1);
                }
                if (second_match_struct) {
                    fprintf(stderr, "error:%d: ambiguous field '%s' on receiver of unknown type "
                        "(it could be '%s.%s' at one offset and '%s.%s' at another). "
                        "Annotate the receiver with an explicit type.\n",
                        n->line, n->field_assign.field,
                        first_match_struct, n->field_assign.field,
                        second_match_struct, n->field_assign.field);
                    exit(1);
                }
                fprintf(g->out, "    str x1, [x0, #%d]\n", unique_offset);
            }
            break;
        }
        case NODE_GIVE:
            if (n->give.value) {
                emit_expr(g, n->give.value);
                // After emitting, mark as moved (ownership transfers out)
                if (n->give.value->type == NODE_IDENT) {
                    mark_moved(g, n->give.value->ident.name);
                }
            } else {
                fprintf(g->out, "    mov x0, #0\n");
            }
            // Save return value before cleanup
            fprintf(g->out, "    str x0, [sp, #-16]!\n");
            // Emit RAII cleanup for everything NOT moved
            emit_scope_cleanup(g, 0);
            // Restore return value
            fprintf(g->out, "    ldr x0, [sp], #16\n");
            fprintf(g->out, "    mov sp, x29\n");
            fprintf(g->out, "    ldp x29, x30, [sp], #16\n");
            fprintf(g->out, "    ret\n");
            break;
        case NODE_IF: {
            int end_label = new_label(g);
            for (int i = 0; i < n->if_stmt.branch_count; i++) {
                int next = new_label(g);
                emit_expr(g, n->if_stmt.conds[i]);
                fprintf(g->out, "    cbz x0, _L%d\n", next);
                Node *body = n->if_stmt.bodies[i];
                for (int j = 0; j < body->block.stmts.count; j++)
                    emit_stmt(g, body->block.stmts.items[j]);
                fprintf(g->out, "    b _L%d\n", end_label);
                fprintf(g->out, "_L%d:\n", next);
            }
            if (n->if_stmt.nah_body) {
                Node *body = n->if_stmt.nah_body;
                for (int j = 0; j < body->block.stmts.count; j++)
                    emit_stmt(g, body->block.stmts.items[j]);
            }
            fprintf(g->out, "_L%d:\n", end_label);
            break;
        }
        case NODE_THROUGH_RANGE: {
            int off = add_local(g, n->through_range.var_name, 0);
            emit_expr(g, n->through_range.from);
            emit_store_local(g, 0, off);
            int loop_start = new_label(g);
            int loop_end = new_label(g);
            int loop_cont = new_label(g);
            int prev_start = g->loop_start_label;
            int prev_end = g->loop_end_label;
            int prev_cont = g->loop_continue_label;
            g->loop_start_label = loop_start;
            g->loop_end_label = loop_end;
            g->loop_continue_label = loop_cont;
            fprintf(g->out, "_L%d:\n", loop_start);
            emit_load_local(g, 0, off);
            fprintf(g->out, "    str x0, [sp, #-16]!\n");
            emit_expr(g, n->through_range.to);
            fprintf(g->out, "    mov x1, x0\n");
            fprintf(g->out, "    ldr x0, [sp], #16\n");
            fprintf(g->out, "    cmp x0, x1\n");
            fprintf(g->out, "    b.ge _L%d\n", loop_end);
            Node *body = n->through_range.body;
            for (int j = 0; j < body->block.stmts.count; j++)
                emit_stmt(g, body->block.stmts.items[j]);
            // Increment (skip jumps here)
            fprintf(g->out, "_L%d:\n", loop_cont);
            emit_load_local(g, 0, off);
            if (n->through_range.by) {
                fprintf(g->out, "    str x0, [sp, #-16]!\n");
                emit_expr(g, n->through_range.by);
                fprintf(g->out, "    mov x1, x0\n");
                fprintf(g->out, "    ldr x0, [sp], #16\n");
                fprintf(g->out, "    add x0, x0, x1\n");
            } else {
                fprintf(g->out, "    add x0, x0, #1\n");
            }
            emit_store_local(g, 0, off);
            fprintf(g->out, "    b _L%d\n", loop_start);
            fprintf(g->out, "_L%d:\n", loop_end);
            g->loop_start_label = prev_start;
            g->loop_end_label = prev_end;
            g->loop_continue_label = prev_cont;
            break;
        }
        case NODE_THROUGH_IN: {
            int list_off = add_local(g, "__list_ptr", 0);
            int idx_off = add_local(g, "__idx", 0);
            int var_off = add_local(g, n->through_in.var_name, 0);
            emit_expr(g, n->through_in.collection);
            emit_store_local(g, 0, list_off);
            fprintf(g->out, "    mov x0, #0\n");
            emit_store_local(g, 0, idx_off);
            int loop_start = new_label(g);
            int loop_end = new_label(g);
            int loop_cont = new_label(g);
            int prev_start = g->loop_start_label;
            int prev_end = g->loop_end_label;
            int prev_cont = g->loop_continue_label;
            g->loop_start_label = loop_start;
            g->loop_end_label = loop_end;
            g->loop_continue_label = loop_cont;
            fprintf(g->out, "_L%d:\n", loop_start);
            // Header format: [cap | count | data_ptr]
            emit_load_local(g, 0, list_off);
            fprintf(g->out, "    ldr x1, [x0, #8]\n");    // count
            emit_load_local(g, 2, idx_off);
            fprintf(g->out, "    cmp x2, x1\n");
            fprintf(g->out, "    b.ge _L%d\n", loop_end);
            // Load element: data_ptr[idx]
            fprintf(g->out, "    ldr x3, [x0, #16]\n");   // data_ptr
            fprintf(g->out, "    ldr x0, [x3, x2, lsl #3]\n");
            emit_store_local(g, 0, var_off);
            Node *body2 = n->through_in.body;
            for (int j = 0; j < body2->block.stmts.count; j++)
                emit_stmt(g, body2->block.stmts.items[j]);
            fprintf(g->out, "_L%d:\n", loop_cont);
            emit_load_local(g, 0, idx_off);
            fprintf(g->out, "    add x0, x0, #1\n");
            emit_store_local(g, 0, idx_off);
            fprintf(g->out, "    b _L%d\n", loop_start);
            fprintf(g->out, "_L%d:\n", loop_end);
            g->loop_start_label = prev_start;
            g->loop_end_label = prev_end;
            g->loop_continue_label = prev_cont;
            break;
        }
        case NODE_STOP:
            if (g->loop_end_label < 0) { fprintf(stderr, "error:%d: 'stop' used outside of a loop\n", n->line); exit(1); }
            fprintf(g->out, "    b _L%d\n", g->loop_end_label);
            break;
        case NODE_SKIP:
            if (g->loop_continue_label < 0) { fprintf(stderr, "error:%d: 'skip' used outside of a loop\n", n->line); exit(1); }
            fprintf(g->out, "    b _L%d\n", g->loop_continue_label);
            break;
        case NODE_ASSERT: {
            emit_expr(g, n->assert_stmt.condition);
            int ok_label = new_label(g);
            fprintf(g->out, "    cbnz x0, _L%d\n", ok_label);
            // Assertion failed: print line number and exit
            fprintf(g->out, "    mov x0, #%d\n", n->line);
            fprintf(g->out, "    bl _assert_fail\n");
            fprintf(g->out, "_L%d:\n", ok_label);
            break;
        }
        case NODE_BLOCK: {
            int scope_start = g->local_count;
            for (int j = 0; j < n->block.stmts.count; j++)
                emit_stmt(g, n->block.stmts.items[j]);
            emit_scope_cleanup(g, scope_start);
            break;
        }
        case NODE_MATCH: {
            emit_expr(g, n->match_expr.expr);
            fprintf(g->out, "    str x0, [sp, #-16]!\n");
            int end_label = new_label(g);
            for (int i = 0; i < n->match_expr.arm_count; i++) {
                int next_label = new_label(g);
                fprintf(g->out, "    ldr x0, [sp]\n");
                fprintf(g->out, "    ldr x1, [x0]\n"); // tag
                // Look up actual tag index by variant name
                int tag = i; // default: use arm order
                const char *vname = n->match_expr.arm_variant_names[i];
                for (int ei = 0; ei < g->enum_count; ei++) {
                    for (int vi = 0; vi < g->enum_variant_counts[ei]; vi++) {
                        if (!strcmp(g->enum_variant_names[ei][vi], vname)) {
                            tag = vi;
                            goto found_tag;
                        }
                    }
                }
                found_tag:
                fprintf(g->out, "    cmp x1, #%d\n", tag);
                fprintf(g->out, "    b.ne _L%d\n", next_label);
                for (int b = 0; b < n->match_expr.arm_binding_counts[i]; b++) {
                    int off = add_local(g, n->match_expr.arm_bindings[i][b], 0);
                    fprintf(g->out, "    ldr x0, [sp]\n");
                    fprintf(g->out, "    ldr x0, [x0, #%d]\n", (b + 1) * 8);
                    emit_store_local(g, 0, off);
                }
                Node *body = n->match_expr.arm_bodies[i];
                for (int j = 0; j < body->block.stmts.count; j++)
                    emit_stmt(g, body->block.stmts.items[j]);
                fprintf(g->out, "    b _L%d\n", end_label);
                fprintf(g->out, "_L%d:\n", next_label);
            }
            fprintf(g->out, "_L%d:\n", end_label);
            fprintf(g->out, "    add sp, sp, #16\n");
            break;
        }
        case NODE_INFI: {
            int loop_start = new_label(g);
            int loop_end = new_label(g);
            int prev_start = g->loop_start_label;
            int prev_end = g->loop_end_label;
            int prev_cont = g->loop_continue_label;
            g->loop_start_label = loop_start;
            g->loop_end_label = loop_end;
            g->loop_continue_label = loop_start; // skip = back to top
            fprintf(g->out, "_L%d:\n", loop_start);
            if (n->infi.cond) {
                emit_expr(g, n->infi.cond);
                fprintf(g->out, "    cbz x0, _L%d\n", loop_end);
            }
            Node *body = n->infi.body;
            for (int j = 0; j < body->block.stmts.count; j++)
                emit_stmt(g, body->block.stmts.items[j]);
            fprintf(g->out, "    b _L%d\n", loop_start);
            fprintf(g->out, "_L%d:\n", loop_end);
            g->loop_start_label = prev_start;
            g->loop_end_label = prev_end;
            g->loop_continue_label = prev_cont;
            break;
        }
        default:
            emit_expr(g, n);
            break;
    }
}

static void emit_func(Gen *g, Node *n) {
    g->local_count = 0;
    g->stack_size = 0;
    g->loop_start_label = -1;
    g->loop_end_label = -1;
    g->loop_continue_label = -1;

    for (int i = 0; i < n->func_def.param_count; i++)
        add_local(g, n->func_def.param_names[i], 0);

    // Methods are emitted as `_<ReceiverType>_<method>`; free functions stay `_<name>`.
    if (n->func_def.receiver_type) {
        fprintf(g->out, ".globl _%s_%s\n.p2align 2\n_%s_%s:\n",
            n->func_def.receiver_type, n->func_def.name,
            n->func_def.receiver_type, n->func_def.name);
    } else {
        fprintf(g->out, ".globl _%s\n.p2align 2\n_%s:\n", n->func_def.name, n->func_def.name);
    }
    fprintf(g->out, "    stp x29, x30, [sp, #-16]!\n");
    fprintf(g->out, "    mov x29, sp\n");
    fprintf(g->out, "    sub sp, sp, #1024\n");

    for (int i = 0; i < n->func_def.param_count && i < 8; i++)
        emit_store_local(g, i, g->offsets[i]);

    Node *body = n->func_def.body;
    for (int i = 0; i < body->block.stmts.count; i++)
        emit_stmt(g, body->block.stmts.items[i]);

    // RAII: free all heap locals that weren't moved
    emit_scope_cleanup(g, 0);

    fprintf(g->out, "    mov x0, #0\n");
    fprintf(g->out, "    mov sp, x29\n");
    fprintf(g->out, "    ldp x29, x30, [sp], #16\n");
    fprintf(g->out, "    ret\n\n");
}

static void emit_yell_int(Gen *g) {
    fprintf(g->out, "// built-in: _yell_int (signed)\n.globl _yell_int\n.p2align 2\n_yell_int:\n");
    fprintf(g->out, "    stp x29, x30, [sp, #-16]!\n    mov x29, sp\n    sub sp, sp, #64\n");
    fprintf(g->out, "    mov x2, x0\n    add x1, sp, #32\n    mov x3, #0\n");
    // Handle negative
    fprintf(g->out, "    cmp x2, #0\n    b.ge _yi_pos\n");
    fprintf(g->out, "    mov w4, #45\n    strb w4, [x1]\n    mov x3, #1\n"); // '-'
    fprintf(g->out, "    neg x2, x2\n");
    fprintf(g->out, "_yi_pos:\n");
    fprintf(g->out, "    cmp x2, #0\n    b.ne _yi_loop\n");
    fprintf(g->out, "    mov w4, #48\n    strb w4, [x1, x3]\n    add x3, x3, #1\n    b _yi_write\n");
    fprintf(g->out, "_yi_loop:\n    cbz x2, _yi_reverse\n");
    fprintf(g->out, "    mov x4, #10\n    udiv x5, x2, x4\n    msub x6, x5, x4, x2\n");
    fprintf(g->out, "    add w6, w6, #48\n    strb w6, [x1, x3]\n    add x3, x3, #1\n    mov x2, x5\n    b _yi_loop\n");
    // Reverse only the digit portion (after sign if present)
    fprintf(g->out, "_yi_reverse:\n");
    fprintf(g->out, "    ldrb w7, [x1]\n    cmp w7, #45\n"); // check if '-'
    fprintf(g->out, "    mov x4, #0\n    b.ne _yi_revstart\n    mov x4, #1\n");
    fprintf(g->out, "_yi_revstart:\n    sub x5, x3, #1\n");
    fprintf(g->out, "_yi_rev:\n    cmp x4, x5\n    b.ge _yi_write\n");
    fprintf(g->out, "    ldrb w6, [x1, x4]\n    ldrb w7, [x1, x5]\n    strb w7, [x1, x4]\n    strb w6, [x1, x5]\n");
    fprintf(g->out, "    add x4, x4, #1\n    sub x5, x5, #1\n    b _yi_rev\n");
    fprintf(g->out, "_yi_write:\n    mov w4, #10\n    strb w4, [x1, x3]\n    add x3, x3, #1\n");
    fprintf(g->out, "    mov x16, #4\n    mov x0, #1\n    mov x2, x3\n    svc #0x80\n");
    fprintf(g->out, "    mov sp, x29\n    ldp x29, x30, [sp], #16\n    ret\n\n");
}

static void emit_yell_str(Gen *g) {
    fprintf(g->out, "// built-in: _yell_str (x0 = null-terminated string ptr)\n");
    fprintf(g->out, ".globl _yell_str\n.p2align 2\n_yell_str:\n");
    fprintf(g->out, "    stp x29, x30, [sp, #-16]!\n    mov x29, sp\n");
    fprintf(g->out, "    mov x1, x0\n    mov x3, #0\n");
    fprintf(g->out, "_ys_len:\n    ldrb w4, [x1, x3]\n    cbz w4, _ys_write\n    add x3, x3, #1\n    b _ys_len\n");
    fprintf(g->out, "_ys_write:\n");
    fprintf(g->out, "    mov x16, #4\n    mov x0, #1\n    mov x2, x3\n    svc #0x80\n");
    // Print newline
    fprintf(g->out, "    sub sp, sp, #16\n    mov w4, #10\n    strb w4, [sp]\n");
    fprintf(g->out, "    mov x16, #4\n    mov x0, #1\n    mov x1, sp\n    mov x2, #1\n    svc #0x80\n");
    fprintf(g->out, "    add sp, sp, #16\n");
    fprintf(g->out, "    mov sp, x29\n    ldp x29, x30, [sp], #16\n    ret\n\n");
}

static void emit_yell_dispatch(Gen *g) {
    // _yell: dispatcher — for MVP we'll use _yell_int as default
    // String detection: if value looks like a pointer (high bits set), call _yell_str
    fprintf(g->out, "// built-in: _yell (auto-dispatch int/str)\n");
    fprintf(g->out, ".globl _yell\n.p2align 2\n_yell:\n");
    fprintf(g->out, "    stp x29, x30, [sp, #-16]!\n    mov x29, sp\n");
    // Heuristic: if x0 > 0x100000, it's probably a pointer (string)
    fprintf(g->out, "    mov x1, #0x100000\n");
    fprintf(g->out, "    cmp x0, x1\n");
    fprintf(g->out, "    b.ge _yell_is_str\n");
    fprintf(g->out, "    bl _yell_int\n");
    fprintf(g->out, "    b _yell_done\n");
    fprintf(g->out, "_yell_is_str:\n");
    fprintf(g->out, "    bl _yell_str\n");
    fprintf(g->out, "_yell_done:\n");
    fprintf(g->out, "    mov sp, x29\n    ldp x29, x30, [sp], #16\n    ret\n\n");
}

static void emit_task_builtins(Gen *g) {
    // In single-threaded compiled mode, task.fire(fn()) already calls fn
    // So _task_fire and _task_collapse are no-ops
    fprintf(g->out, ".globl _task_fire\n.p2align 2\n_task_fire:\n    ret\n\n");
    fprintf(g->out, ".globl _task_collapse\n.p2align 2\n_task_collapse:\n    ret\n\n");
}

static void emit_heap_alloc(Gen *g) {
    // Free list: singly-linked list of freed blocks
    // Each free block: [next_ptr(8) | size(8) | ... unused space ...]
    // _heap_free_list points to first free block (or 0)

    // _heap_free(ptr=x0, size=x1): add block to free list
    fprintf(g->out, "// built-in: _heap_free(ptr=x0, size=x1)\n");
    fprintf(g->out, ".globl _heap_free\n.p2align 2\n_heap_free:\n");
    fprintf(g->out, "    cbz x0, _hf_ret\n"); // null check
    fprintf(g->out, "    adrp x2, _heap_free_list@PAGE\n");
    fprintf(g->out, "    add x2, x2, _heap_free_list@PAGEOFF\n");
    fprintf(g->out, "    ldr x3, [x2]\n");       // x3 = current head
    fprintf(g->out, "    str x3, [x0]\n");       // block->next = head
    fprintf(g->out, "    str x1, [x0, #8]\n");   // block->size = size
    fprintf(g->out, "    str x0, [x2]\n");       // head = block
    fprintf(g->out, "_hf_ret:\n    ret\n\n");

    // _heap_alloc: check free list first, then bump
    fprintf(g->out, "// built-in: _heap_alloc(size in x0) -> ptr in x0\n");
    fprintf(g->out, ".globl _heap_alloc\n.p2align 2\n_heap_alloc:\n");
    fprintf(g->out, "    stp x29, x30, [sp, #-16]!\n    mov x29, sp\n");
    // Align size to 16 bytes (minimum block size for free list metadata)
    fprintf(g->out, "    add x0, x0, #15\n");
    fprintf(g->out, "    and x0, x0, #-16\n");
    fprintf(g->out, "    mov x9, x0\n"); // x9 = aligned size
    // Check free list
    fprintf(g->out, "    adrp x10, _heap_free_list@PAGE\n");
    fprintf(g->out, "    add x10, x10, _heap_free_list@PAGEOFF\n");
    fprintf(g->out, "    ldr x11, [x10]\n");     // x11 = head
    fprintf(g->out, "    cbz x11, _ha_bump\n");  // empty free list -> bump
    // Walk free list for first-fit
    fprintf(g->out, "    mov x12, x10\n");       // x12 = prev_ptr (points to head slot)
    fprintf(g->out, "_ha_search:\n");
    fprintf(g->out, "    cbz x11, _ha_bump\n");
    fprintf(g->out, "    ldr x13, [x11, #8]\n"); // x13 = block size
    fprintf(g->out, "    cmp x13, x9\n");
    fprintf(g->out, "    b.ge _ha_found\n");     // block big enough
    fprintf(g->out, "    mov x12, x11\n");       // prev = current
    fprintf(g->out, "    ldr x11, [x11]\n");     // current = current->next
    fprintf(g->out, "    b _ha_search\n");
    // Found a block: unlink and return
    fprintf(g->out, "_ha_found:\n");
    fprintf(g->out, "    ldr x14, [x11]\n");     // next = block->next
    fprintf(g->out, "    str x14, [x12]\n");     // prev->next = next (unlink)
    fprintf(g->out, "    mov x0, x11\n");        // return block ptr
    fprintf(g->out, "    mov sp, x29\n    ldp x29, x30, [sp], #16\n    ret\n");
    // Bump allocate
    fprintf(g->out, "_ha_bump:\n");
    fprintf(g->out, "    adrp x10, _heap_ptr@PAGE\n");
    fprintf(g->out, "    add x10, x10, _heap_ptr@PAGEOFF\n");
    fprintf(g->out, "    ldr x11, [x10]\n");
    fprintf(g->out, "    adrp x12, _heap_end@PAGE\n");
    fprintf(g->out, "    add x12, x12, _heap_end@PAGEOFF\n");
    fprintf(g->out, "    ldr x13, [x12]\n");
    fprintf(g->out, "    add x14, x11, x9\n");
    fprintf(g->out, "    cmp x14, x13\n");
    fprintf(g->out, "    b.le _ha_ok\n");
    // mmap new page
    fprintf(g->out, "    mov x0, #0\n");
    fprintf(g->out, "    mov x1, #0x10000\n");
    fprintf(g->out, "    mov x2, #3\n");
    fprintf(g->out, "    mov x3, #0x1002\n");
    fprintf(g->out, "    mov x4, #-1\n");
    fprintf(g->out, "    mov x5, #0\n");
    fprintf(g->out, "    mov x16, #197\n");
    fprintf(g->out, "    svc #0x80\n");
    fprintf(g->out, "    mov x11, x0\n");
    fprintf(g->out, "    add x13, x0, #0x10000\n");
    fprintf(g->out, "    add x14, x11, x9\n");
    fprintf(g->out, "_ha_ok:\n");
    fprintf(g->out, "    mov x0, x11\n");
    fprintf(g->out, "    str x14, [x10]\n");
    fprintf(g->out, "    str x13, [x12]\n");
    fprintf(g->out, "    mov sp, x29\n    ldp x29, x30, [sp], #16\n    ret\n\n");
}

static void emit_str_eq(Gen *g) {
    // _str_eq(x0=str1, x1=str2) -> x0=1 if equal, 0 if not
    fprintf(g->out, "// built-in: _str_eq\n.globl _str_eq\n.p2align 2\n_str_eq:\n");
    fprintf(g->out, "_se_loop:\n");
    fprintf(g->out, "    ldrb w2, [x0], #1\n");
    fprintf(g->out, "    ldrb w3, [x1], #1\n");
    fprintf(g->out, "    cmp w2, w3\n");
    fprintf(g->out, "    b.ne _se_no\n");
    fprintf(g->out, "    cbz w2, _se_yes\n");
    fprintf(g->out, "    b _se_loop\n");
    fprintf(g->out, "_se_yes:\n    mov x0, #1\n    ret\n");
    fprintf(g->out, "_se_no:\n    mov x0, #0\n    ret\n\n");
}

static void emit_str_concat(Gen *g) {
    // _str_concat(x0=str1, x1=str2) -> x0=new string on heap
    fprintf(g->out, "// built-in: _str_concat(x0=s1, x1=s2) -> new str\n");
    fprintf(g->out, ".globl _str_concat\n.p2align 2\n_str_concat:\n");
    fprintf(g->out, "    stp x29, x30, [sp, #-16]!\n    mov x29, sp\n");
    fprintf(g->out, "    stp x19, x20, [sp, #-16]!\n");
    fprintf(g->out, "    stp x21, x22, [sp, #-16]!\n");
    fprintf(g->out, "    mov x19, x0\n    mov x20, x1\n");
    // Get len of s1
    fprintf(g->out, "    mov x21, #0\n");
    fprintf(g->out, "_sc_l1:\n    ldrb w2, [x19, x21]\n    cbz w2, _sc_l2s\n    add x21, x21, #1\n    b _sc_l1\n");
    // Get len of s2
    fprintf(g->out, "_sc_l2s:\n    mov x22, #0\n");
    fprintf(g->out, "_sc_l2:\n    ldrb w2, [x20, x22]\n    cbz w2, _sc_alloc\n    add x22, x22, #1\n    b _sc_l2\n");
    // Alloc len1+len2+1
    fprintf(g->out, "_sc_alloc:\n    add x0, x21, x22\n    add x0, x0, #1\n    bl _heap_alloc\n");
    fprintf(g->out, "    mov x3, x0\n"); // x3 = dest
    // Copy s1
    fprintf(g->out, "    mov x4, #0\n");
    fprintf(g->out, "_sc_c1:\n    cmp x4, x21\n    b.ge _sc_c2s\n    ldrb w5, [x19, x4]\n    strb w5, [x3, x4]\n    add x4, x4, #1\n    b _sc_c1\n");
    // Copy s2
    fprintf(g->out, "_sc_c2s:\n    mov x5, #0\n");
    fprintf(g->out, "_sc_c2:\n    cmp x5, x22\n    b.ge _sc_end\n    ldrb w6, [x20, x5]\n    add x7, x4, x5\n    strb w6, [x3, x7]\n    add x5, x5, #1\n    b _sc_c2\n");
    // Null terminate
    fprintf(g->out, "_sc_end:\n    add x7, x21, x22\n    strb wzr, [x3, x7]\n    mov x0, x3\n");
    fprintf(g->out, "    ldp x21, x22, [sp], #16\n    ldp x19, x20, [sp], #16\n");
    fprintf(g->out, "    mov sp, x29\n    ldp x29, x30, [sp], #16\n    ret\n\n");
}

static void emit_int_to_str(Gen *g) {
    // _int_to_str(x0=int) -> x0=heap string.
    //
    // ABI note: this helper uses x19/x20 to bridge a `bl _heap_alloc`
    // call (preserving the temp-buffer pointer + length across the
    // call). Earlier versions failed to save them in the prologue,
    // which silently corrupted any caller that legitimately held a
    // value in x19/x20 — fine for the original direct codegen (which
    // never put values in callee-save regs across calls), but a
    // crash for the IR backend's call-aware register allocator that
    // does. Always save and restore.
    fprintf(g->out, "// built-in: _int_to_str(x0) -> str\n");
    fprintf(g->out, ".globl _int_to_str\n.p2align 2\n_int_to_str:\n");
    fprintf(g->out, "    stp x29, x30, [sp, #-16]!\n    mov x29, sp\n");
    fprintf(g->out, "    stp x19, x20, [sp, #-16]!\n");
    fprintf(g->out, "    sub sp, sp, #32\n");
    fprintf(g->out, "    mov x2, x0\n    add x1, sp, #0\n    mov x3, #0\n");
    fprintf(g->out, "    cmp x2, #0\n    b.ne _its_loop\n");
    fprintf(g->out, "    mov w4, #48\n    strb w4, [x1]\n    mov x3, #1\n    b _its_alloc\n");
    fprintf(g->out, "_its_loop:\n    cbz x2, _its_rev\n");
    fprintf(g->out, "    mov x4, #10\n    udiv x5, x2, x4\n    msub x6, x5, x4, x2\n");
    fprintf(g->out, "    add w6, w6, #48\n    strb w6, [x1, x3]\n    add x3, x3, #1\n    mov x2, x5\n    b _its_loop\n");
    fprintf(g->out, "_its_rev:\n    mov x4, #0\n    sub x5, x3, #1\n");
    fprintf(g->out, "_its_rv:\n    cmp x4, x5\n    b.ge _its_alloc\n");
    fprintf(g->out, "    ldrb w6, [x1, x4]\n    ldrb w7, [x1, x5]\n    strb w7, [x1, x4]\n    strb w6, [x1, x5]\n");
    fprintf(g->out, "    add x4, x4, #1\n    sub x5, x5, #1\n    b _its_rv\n");
    // Alloc and copy
    fprintf(g->out, "_its_alloc:\n    strb wzr, [x1, x3]\n"); // null term
    fprintf(g->out, "    mov x19, x1\n    mov x20, x3\n");
    fprintf(g->out, "    add x0, x3, #1\n    bl _heap_alloc\n");
    fprintf(g->out, "    mov x4, #0\n");
    fprintf(g->out, "_its_cp:\n    ldrb w5, [x19, x4]\n    strb w5, [x0, x4]\n    add x4, x4, #1\n    cmp x4, x20\n    b.le _its_cp\n");
    // Epilogue: undo `sub sp, sp, #32`, restore x19/x20, then fp/lr.
    fprintf(g->out, "    add sp, sp, #32\n");
    fprintf(g->out, "    ldp x19, x20, [sp], #16\n");
    fprintf(g->out, "    ldp x29, x30, [sp], #16\n    ret\n\n");
}

static void emit_str_builtins(Gen *g) {
    // _str_len(x0=str) -> length in x0
    fprintf(g->out, "// built-in: _str_len(x0=str) -> int\n");
    fprintf(g->out, ".globl _str_len\n.p2align 2\n_str_len:\n");
    fprintf(g->out, "    mov x1, #0\n");
    fprintf(g->out, "_sl_loop:\n");
    fprintf(g->out, "    ldrb w2, [x0, x1]\n");
    fprintf(g->out, "    cbz w2, _sl_done\n");
    fprintf(g->out, "    add x1, x1, #1\n");
    fprintf(g->out, "    b _sl_loop\n");
    fprintf(g->out, "_sl_done:\n    mov x0, x1\n    ret\n\n");

    // _char_at(x0=str, x1=index) -> 1-char string on heap
    fprintf(g->out, "// built-in: _char_at(x0=str, x1=index) -> str\n");
    fprintf(g->out, ".globl _char_at\n.p2align 2\n_char_at:\n");
    fprintf(g->out, "    stp x29, x30, [sp, #-16]!\n    mov x29, sp\n");
    fprintf(g->out, "    ldrb w2, [x0, x1]\n");   // load char
    fprintf(g->out, "    str x2, [sp, #-16]!\n"); // save char
    fprintf(g->out, "    mov x0, #16\n");          // alloc 2 bytes (aligned to 16)
    fprintf(g->out, "    bl _heap_alloc\n");
    fprintf(g->out, "    ldr x2, [sp], #16\n");   // restore char
    fprintf(g->out, "    strb w2, [x0]\n");       // store char
    fprintf(g->out, "    strb wzr, [x0, #1]\n");  // null terminate
    fprintf(g->out, "    mov sp, x29\n    ldp x29, x30, [sp], #16\n    ret\n\n");
}

static void emit_map_builtins(Gen *g) {
    // Map layout (growable, indirection):
    //   Header (24 bytes, fixed): [capacity | count | data_ptr]
    //   Data (growable): [key0(8) | val0(8) | key1(8) | val1(8) | ...]
    //   Each entry = 16 bytes. On insert when full: alloc 2x, copy, free old.

    // _map_new() -> map header (initial capacity 8)
    fprintf(g->out, "// built-in: _map_new() -> map header ptr\n");
    fprintf(g->out, ".globl _map_new\n.p2align 2\n_map_new:\n");
    fprintf(g->out, "    stp x29, x30, [sp, #-16]!\n    mov x29, sp\n");
    // Alloc header
    fprintf(g->out, "    mov x0, #24\n    bl _heap_alloc\n");
    fprintf(g->out, "    mov x19, x0\n    str x19, [sp, #-16]!\n");
    // Alloc data (8 entries * 16 bytes = 128)
    fprintf(g->out, "    mov x0, #128\n    bl _heap_alloc\n");
    fprintf(g->out, "    ldr x19, [sp], #16\n");
    fprintf(g->out, "    mov x1, #8\n");
    fprintf(g->out, "    str x1, [x19]\n");        // capacity = 8
    fprintf(g->out, "    str xzr, [x19, #8]\n");   // count = 0
    fprintf(g->out, "    str x0, [x19, #16]\n");   // data_ptr
    fprintf(g->out, "    mov x0, x19\n");
    fprintf(g->out, "    mov sp, x29\n    ldp x29, x30, [sp], #16\n    ret\n\n");

    // _map_set(x0=map, x1=key, x2=value) — linear scan, insert or update, grows if needed
    fprintf(g->out, "// built-in: _map_set(x0=map, x1=key, x2=value)\n");
    fprintf(g->out, ".globl _map_set\n.p2align 2\n_map_set:\n");
    fprintf(g->out, "    stp x29, x30, [sp, #-16]!\n    mov x29, sp\n");
    fprintf(g->out, "    stp x19, x20, [sp, #-16]!\n");
    fprintf(g->out, "    stp x21, x22, [sp, #-16]!\n");
    fprintf(g->out, "    stp x23, x24, [sp, #-16]!\n");
    fprintf(g->out, "    mov x19, x0\n");   // header
    fprintf(g->out, "    mov x20, x1\n");   // key
    fprintf(g->out, "    mov x21, x2\n");   // value
    fprintf(g->out, "    ldr x22, [x19, #8]\n");   // count
    fprintf(g->out, "    ldr x23, [x19, #16]\n");  // data_ptr
    // Scan for existing key
    fprintf(g->out, "    mov x3, #0\n");
    fprintf(g->out, "_ms_scan:\n");
    fprintf(g->out, "    cmp x3, x22\n");
    fprintf(g->out, "    b.ge _ms_insert\n");
    fprintf(g->out, "    lsl x4, x3, #4\n");       // offset = i * 16
    fprintf(g->out, "    ldr x0, [x23, x4]\n");    // existing key
    fprintf(g->out, "    mov x1, x20\n");
    fprintf(g->out, "    stp x3, x4, [sp, #-16]!\n");
    fprintf(g->out, "    bl _str_eq\n");
    fprintf(g->out, "    ldp x3, x4, [sp], #16\n");
    fprintf(g->out, "    cbnz x0, _ms_update\n");
    fprintf(g->out, "    add x3, x3, #1\n");
    fprintf(g->out, "    b _ms_scan\n");
    // Update existing
    fprintf(g->out, "_ms_update:\n");
    fprintf(g->out, "    lsl x4, x3, #4\n");
    fprintf(g->out, "    add x4, x4, #8\n");       // value offset = key offset + 8
    fprintf(g->out, "    str x21, [x23, x4]\n");
    fprintf(g->out, "    b _ms_done\n");
    // Insert new — grow if needed
    fprintf(g->out, "_ms_insert:\n");
    fprintf(g->out, "    ldr x24, [x19]\n");       // capacity
    fprintf(g->out, "    cmp x22, x24\n");
    fprintf(g->out, "    b.lt _ms_do_insert\n");
    // Grow: alloc 2x capacity
    fprintf(g->out, "    lsl x24, x24, #1\n");     // new_cap = cap * 2
    fprintf(g->out, "    str x24, [x19]\n");        // update capacity
    fprintf(g->out, "    lsl x0, x24, #4\n");       // new_cap * 16 bytes
    fprintf(g->out, "    bl _heap_alloc\n");
    fprintf(g->out, "    mov x5, x0\n");            // new data
    // Copy old entries
    fprintf(g->out, "    mov x6, #0\n");
    fprintf(g->out, "    lsl x7, x22, #4\n");      // old_size = count * 16
    fprintf(g->out, "_ms_copy:\n");
    fprintf(g->out, "    cmp x6, x7\n");
    fprintf(g->out, "    b.ge _ms_copied\n");
    fprintf(g->out, "    ldr x8, [x23, x6]\n");
    fprintf(g->out, "    str x8, [x5, x6]\n");
    fprintf(g->out, "    add x6, x6, #8\n");
    fprintf(g->out, "    b _ms_copy\n");
    fprintf(g->out, "_ms_copied:\n");
    // Free old data
    fprintf(g->out, "    mov x0, x23\n");
    fprintf(g->out, "    lsl x1, x22, #4\n");      // old size = count * 16
    fprintf(g->out, "    bl _heap_free\n");
    // Update data_ptr
    fprintf(g->out, "    mov x23, x5\n");
    fprintf(g->out, "    str x23, [x19, #16]\n");
    // Do insert
    fprintf(g->out, "_ms_do_insert:\n");
    fprintf(g->out, "    lsl x4, x22, #4\n");
    fprintf(g->out, "    str x20, [x23, x4]\n");    // store key
    fprintf(g->out, "    add x4, x4, #8\n");
    fprintf(g->out, "    str x21, [x23, x4]\n");    // store value
    fprintf(g->out, "    add x22, x22, #1\n");
    fprintf(g->out, "    str x22, [x19, #8]\n");    // count++
    fprintf(g->out, "_ms_done:\n");
    fprintf(g->out, "    ldp x23, x24, [sp], #16\n");
    fprintf(g->out, "    ldp x21, x22, [sp], #16\n");
    fprintf(g->out, "    ldp x19, x20, [sp], #16\n");
    fprintf(g->out, "    mov sp, x29\n    ldp x29, x30, [sp], #16\n    ret\n\n");

    // _map_get(x0=map, x1=key) -> value (0 if not found)
    fprintf(g->out, "// built-in: _map_get(x0=map, x1=key) -> value in x0\n");
    fprintf(g->out, ".globl _map_get\n.p2align 2\n_map_get:\n");
    fprintf(g->out, "    stp x29, x30, [sp, #-16]!\n    mov x29, sp\n");
    fprintf(g->out, "    stp x19, x20, [sp, #-16]!\n");
    fprintf(g->out, "    mov x19, x0\n");
    fprintf(g->out, "    mov x20, x1\n");
    fprintf(g->out, "    ldr x5, [x19, #8]\n");    // count
    fprintf(g->out, "    ldr x6, [x19, #16]\n");   // data_ptr
    fprintf(g->out, "    mov x3, #0\n");
    fprintf(g->out, "_mg_scan:\n");
    fprintf(g->out, "    cmp x3, x5\n");
    fprintf(g->out, "    b.ge _mg_notfound\n");
    fprintf(g->out, "    lsl x4, x3, #4\n");
    fprintf(g->out, "    ldr x0, [x6, x4]\n");
    fprintf(g->out, "    mov x1, x20\n");
    fprintf(g->out, "    stp x3, x5, [sp, #-16]!\n");
    fprintf(g->out, "    str x6, [sp, #-16]!\n");
    fprintf(g->out, "    bl _str_eq\n");
    fprintf(g->out, "    ldr x6, [sp], #16\n");
    fprintf(g->out, "    ldp x3, x5, [sp], #16\n");
    fprintf(g->out, "    cbnz x0, _mg_found\n");
    fprintf(g->out, "    add x3, x3, #1\n");
    fprintf(g->out, "    b _mg_scan\n");
    fprintf(g->out, "_mg_found:\n");
    fprintf(g->out, "    lsl x4, x3, #4\n");
    fprintf(g->out, "    add x4, x4, #8\n");
    fprintf(g->out, "    ldr x0, [x6, x4]\n");
    fprintf(g->out, "    b _mg_done\n");
    fprintf(g->out, "_mg_notfound:\n");
    fprintf(g->out, "    mov x0, #0\n");
    fprintf(g->out, "_mg_done:\n");
    fprintf(g->out, "    ldp x19, x20, [sp], #16\n");
    fprintf(g->out, "    mov sp, x29\n    ldp x29, x30, [sp], #16\n    ret\n\n");

    // _map_len(x0=map) -> count
    fprintf(g->out, ".globl _map_len\n.p2align 2\n_map_len:\n");
    fprintf(g->out, "    ldr x0, [x0, #8]\n    ret\n\n");

    // _map_keys(x0=map) -> list of keys (header format)
    fprintf(g->out, "// built-in: _map_keys(x0=map) -> list ptr\n");
    fprintf(g->out, ".globl _map_keys\n.p2align 2\n_map_keys:\n");
    fprintf(g->out, "    stp x29, x30, [sp, #-16]!\n    mov x29, sp\n");
    fprintf(g->out, "    stp x19, x20, [sp, #-16]!\n");
    fprintf(g->out, "    stp x21, x22, [sp, #-16]!\n");
    fprintf(g->out, "    mov x19, x0\n");
    fprintf(g->out, "    ldr x20, [x19, #8]\n");    // count
    fprintf(g->out, "    ldr x24, [x19, #16]\n");   // map data_ptr
    // Alloc list header
    fprintf(g->out, "    mov x0, #24\n    bl _heap_alloc\n");
    fprintf(g->out, "    mov x21, x0\n");
    // Alloc list data
    fprintf(g->out, "    lsl x0, x20, #3\n");
    fprintf(g->out, "    cmp x0, #16\n    b.ge _mk_alloc\n");
    fprintf(g->out, "    mov x0, #16\n");
    fprintf(g->out, "_mk_alloc:\n");
    fprintf(g->out, "    bl _heap_alloc\n");
    fprintf(g->out, "    mov x22, x0\n");
    // Fill list header
    fprintf(g->out, "    str x20, [x21]\n");
    fprintf(g->out, "    str x20, [x21, #8]\n");
    fprintf(g->out, "    str x22, [x21, #16]\n");
    // Copy keys from map data
    fprintf(g->out, "    ldr x24, [x19, #16]\n");   // reload map data_ptr
    fprintf(g->out, "    mov x3, #0\n");
    fprintf(g->out, "_mk_loop:\n");
    fprintf(g->out, "    cmp x3, x20\n");
    fprintf(g->out, "    b.ge _mk_done\n");
    fprintf(g->out, "    lsl x4, x3, #4\n");        // map entry offset = i*16
    fprintf(g->out, "    ldr x5, [x24, x4]\n");     // key
    fprintf(g->out, "    str x5, [x22, x3, lsl #3]\n"); // list data[i]
    fprintf(g->out, "    add x3, x3, #1\n");
    fprintf(g->out, "    b _mk_loop\n");
    fprintf(g->out, "_mk_done:\n");
    fprintf(g->out, "    mov x0, x21\n");
    fprintf(g->out, "    ldp x21, x22, [sp], #16\n");
    fprintf(g->out, "    ldp x19, x20, [sp], #16\n");
    fprintf(g->out, "    mov sp, x29\n    ldp x29, x30, [sp], #16\n    ret\n\n");
}

static void emit_imap_builtins(Gen *g) {
    // imap: same layout as map [cap|count|data_ptr], data=[key0|val0|key1|val1|...]
    // but uses integer comparison (cmp) instead of _str_eq

    // _imap_new() -> imap header (initial capacity 8)
    fprintf(g->out, ".globl _imap_new\n.p2align 2\n_imap_new:\n");
    fprintf(g->out, "    stp x29, x30, [sp, #-16]!\n    mov x29, sp\n");
    fprintf(g->out, "    mov x0, #24\n    bl _heap_alloc\n");
    fprintf(g->out, "    mov x19, x0\n    str x19, [sp, #-16]!\n");
    fprintf(g->out, "    mov x0, #128\n    bl _heap_alloc\n");
    fprintf(g->out, "    ldr x19, [sp], #16\n");
    fprintf(g->out, "    mov x1, #8\n    str x1, [x19]\n");
    fprintf(g->out, "    str xzr, [x19, #8]\n    str x0, [x19, #16]\n");
    fprintf(g->out, "    mov x0, x19\n");
    fprintf(g->out, "    mov sp, x29\n    ldp x29, x30, [sp], #16\n    ret\n\n");

    // _imap_set(x0=imap, x1=key, x2=value)
    fprintf(g->out, ".globl _imap_set\n.p2align 2\n_imap_set:\n");
    fprintf(g->out, "    stp x29, x30, [sp, #-16]!\n    mov x29, sp\n");
    fprintf(g->out, "    stp x19, x20, [sp, #-16]!\n");
    fprintf(g->out, "    stp x21, x22, [sp, #-16]!\n");
    fprintf(g->out, "    stp x23, x24, [sp, #-16]!\n");
    fprintf(g->out, "    mov x19, x0\n    mov x20, x1\n    mov x21, x2\n");
    fprintf(g->out, "    ldr x22, [x19, #8]\n    ldr x23, [x19, #16]\n");
    // Scan for existing key (integer comparison)
    fprintf(g->out, "    mov x3, #0\n");
    fprintf(g->out, "_ims_scan:\n    cmp x3, x22\n    b.ge _ims_insert\n");
    fprintf(g->out, "    lsl x4, x3, #4\n    ldr x5, [x23, x4]\n");
    fprintf(g->out, "    cmp x5, x20\n    b.eq _ims_update\n");
    fprintf(g->out, "    add x3, x3, #1\n    b _ims_scan\n");
    // Update
    fprintf(g->out, "_ims_update:\n    lsl x4, x3, #4\n    add x4, x4, #8\n");
    fprintf(g->out, "    str x21, [x23, x4]\n    b _ims_done\n");
    // Insert (grow if needed)
    fprintf(g->out, "_ims_insert:\n    ldr x24, [x19]\n    cmp x22, x24\n    b.lt _ims_do_ins\n");
    fprintf(g->out, "    lsl x24, x24, #1\n    str x24, [x19]\n");
    fprintf(g->out, "    lsl x0, x24, #4\n    bl _heap_alloc\n    mov x5, x0\n");
    fprintf(g->out, "    mov x6, #0\n    lsl x7, x22, #4\n");
    fprintf(g->out, "_ims_cp:\n    cmp x6, x7\n    b.ge _ims_cpd\n");
    fprintf(g->out, "    ldr x8, [x23, x6]\n    str x8, [x5, x6]\n    add x6, x6, #8\n    b _ims_cp\n");
    fprintf(g->out, "_ims_cpd:\n    mov x0, x23\n    lsl x1, x22, #4\n    bl _heap_free\n");
    fprintf(g->out, "    mov x23, x5\n    str x23, [x19, #16]\n");
    fprintf(g->out, "_ims_do_ins:\n    lsl x4, x22, #4\n");
    fprintf(g->out, "    str x20, [x23, x4]\n    add x4, x4, #8\n    str x21, [x23, x4]\n");
    fprintf(g->out, "    add x22, x22, #1\n    str x22, [x19, #8]\n");
    fprintf(g->out, "_ims_done:\n    ldp x23, x24, [sp], #16\n    ldp x21, x22, [sp], #16\n");
    fprintf(g->out, "    ldp x19, x20, [sp], #16\n");
    fprintf(g->out, "    mov sp, x29\n    ldp x29, x30, [sp], #16\n    ret\n\n");

    // _imap_get(x0=imap, x1=key) -> value (0 if not found)
    fprintf(g->out, ".globl _imap_get\n.p2align 2\n_imap_get:\n");
    fprintf(g->out, "    ldr x2, [x0, #8]\n    ldr x3, [x0, #16]\n    mov x4, #0\n");
    fprintf(g->out, "_img_scan:\n    cmp x4, x2\n    b.ge _img_nf\n");
    fprintf(g->out, "    lsl x5, x4, #4\n    ldr x6, [x3, x5]\n");
    fprintf(g->out, "    cmp x6, x1\n    b.eq _img_found\n");
    fprintf(g->out, "    add x4, x4, #1\n    b _img_scan\n");
    fprintf(g->out, "_img_found:\n    lsl x5, x4, #4\n    add x5, x5, #8\n    ldr x0, [x3, x5]\n    ret\n");
    fprintf(g->out, "_img_nf:\n    mov x0, #0\n    ret\n\n");

    // _imap_len(x0=imap) -> count
    fprintf(g->out, ".globl _imap_len\n.p2align 2\n_imap_len:\n");
    fprintf(g->out, "    ldr x0, [x0, #8]\n    ret\n\n");
}

static void emit_list_builtins(Gen *g) {
    // List layout (indirection for growable):

    // List layout (indirection for growable):
    //   Header (fixed, 24 bytes): [capacity | count | data_ptr]
    //   Data (growable): [elem0 | elem1 | ...]
    // On push when full: alloc 2x, copy, free old, update data_ptr

    // _list_new() -> heap list header (initial capacity 8)
    fprintf(g->out, "// built-in: _list_new() -> list header ptr\n");
    fprintf(g->out, ".globl _list_new\n.p2align 2\n_list_new:\n");
    fprintf(g->out, "    stp x29, x30, [sp, #-16]!\n    mov x29, sp\n");
    // Alloc header (24 bytes)
    fprintf(g->out, "    mov x0, #24\n    bl _heap_alloc\n");
    fprintf(g->out, "    mov x19, x0\n"); // save header ptr
    fprintf(g->out, "    str x19, [sp, #-16]!\n");
    // Alloc initial data (8 slots * 8 bytes = 64)
    fprintf(g->out, "    mov x0, #64\n    bl _heap_alloc\n");
    fprintf(g->out, "    ldr x19, [sp], #16\n");
    fprintf(g->out, "    mov x1, #8\n");
    fprintf(g->out, "    str x1, [x19]\n");       // capacity = 8
    fprintf(g->out, "    str xzr, [x19, #8]\n");  // count = 0
    fprintf(g->out, "    str x0, [x19, #16]\n");  // data_ptr
    fprintf(g->out, "    mov x0, x19\n");
    fprintf(g->out, "    mov sp, x29\n    ldp x29, x30, [sp], #16\n    ret\n\n");

    // _list_push(x0=list, x1=value) -> grows if needed
    fprintf(g->out, "// built-in: _list_push(x0=list, x1=value)\n");
    fprintf(g->out, ".globl _list_push\n.p2align 2\n_list_push:\n");
    fprintf(g->out, "    stp x29, x30, [sp, #-16]!\n    mov x29, sp\n");
    fprintf(g->out, "    stp x19, x20, [sp, #-16]!\n");
    fprintf(g->out, "    mov x19, x0\n");          // header
    fprintf(g->out, "    mov x20, x1\n");          // value
    fprintf(g->out, "    ldr x2, [x19]\n");        // capacity
    fprintf(g->out, "    ldr x3, [x19, #8]\n");    // count
    fprintf(g->out, "    cmp x3, x2\n");
    fprintf(g->out, "    b.lt _lp_store\n");
    // Grow: alloc 2x capacity
    fprintf(g->out, "    lsl x4, x2, #1\n");      // new_cap = cap * 2
    fprintf(g->out, "    str x4, [x19]\n");        // update capacity
    fprintf(g->out, "    lsl x0, x4, #3\n");       // new_cap * 8 bytes
    fprintf(g->out, "    str x3, [sp, #-16]!\n");  // save count
    fprintf(g->out, "    bl _heap_alloc\n");        // new data
    fprintf(g->out, "    ldr x3, [sp], #16\n");    // restore count
    fprintf(g->out, "    mov x5, x0\n");           // x5 = new data
    fprintf(g->out, "    ldr x6, [x19, #16]\n");   // x6 = old data
    // Copy old elements
    fprintf(g->out, "    mov x7, #0\n");
    fprintf(g->out, "_lp_copy:\n");
    fprintf(g->out, "    cmp x7, x3\n");
    fprintf(g->out, "    b.ge _lp_copied\n");
    fprintf(g->out, "    ldr x8, [x6, x7, lsl #3]\n");
    fprintf(g->out, "    str x8, [x5, x7, lsl #3]\n");
    fprintf(g->out, "    add x7, x7, #1\n");
    fprintf(g->out, "    b _lp_copy\n");
    fprintf(g->out, "_lp_copied:\n");
    // Free old data
    fprintf(g->out, "    mov x0, x6\n");
    fprintf(g->out, "    lsl x1, x3, #3\n");      // old size = count * 8
    fprintf(g->out, "    bl _heap_free\n");
    // Update data_ptr
    fprintf(g->out, "    str x5, [x19, #16]\n");
    fprintf(g->out, "    ldr x3, [x19, #8]\n");   // reload count
    fprintf(g->out, "    mov x0, x5\n");           // data = new
    fprintf(g->out, "    b _lp_do_store\n");
    fprintf(g->out, "_lp_store:\n");
    fprintf(g->out, "    ldr x0, [x19, #16]\n");  // data_ptr
    fprintf(g->out, "_lp_do_store:\n");
    fprintf(g->out, "    str x20, [x0, x3, lsl #3]\n"); // data[count] = value
    fprintf(g->out, "    add x3, x3, #1\n");
    fprintf(g->out, "    str x3, [x19, #8]\n");    // count++
    fprintf(g->out, "    ldp x19, x20, [sp], #16\n");
    fprintf(g->out, "    mov sp, x29\n    ldp x29, x30, [sp], #16\n    ret\n\n");

    // _list_pop(x0=list) -> removes and returns last element
    fprintf(g->out, "// built-in: _list_pop(x0=list) -> value\n");
    fprintf(g->out, ".globl _list_pop\n.p2align 2\n_list_pop:\n");
    fprintf(g->out, "    ldr x1, [x0, #8]\n");     // count
    fprintf(g->out, "    cbz x1, _lpop_empty\n");
    fprintf(g->out, "    sub x1, x1, #1\n");
    fprintf(g->out, "    str x1, [x0, #8]\n");     // count--
    fprintf(g->out, "    ldr x2, [x0, #16]\n");    // data_ptr
    fprintf(g->out, "    ldr x0, [x2, x1, lsl #3]\n"); // data[count-1]
    fprintf(g->out, "    ret\n");
    fprintf(g->out, "_lpop_empty:\n    mov x0, #0\n    ret\n\n");

    // _list_len(x0=list) -> count
    fprintf(g->out, "// built-in: _list_len(x0=list) -> count\n");
    fprintf(g->out, ".globl _list_len\n.p2align 2\n_list_len:\n");
    fprintf(g->out, "    ldr x0, [x0, #8]\n    ret\n\n");

    // _list_set(x0=list, x1=index, x2=value) -> sets element at index (bounds checked)
    fprintf(g->out, "// built-in: _list_set(x0=list, x1=index, x2=value)\n");
    fprintf(g->out, ".globl _list_set\n.p2align 2\n_list_set:\n");
    fprintf(g->out, "    ldr x4, [x0, #8]\n");     // count
    fprintf(g->out, "    cmp x1, x4\n");
    fprintf(g->out, "    b.ge _panic_oob\n");
    fprintf(g->out, "    cmp x1, #0\n");
    fprintf(g->out, "    b.lt _panic_oob\n");
    fprintf(g->out, "    ldr x3, [x0, #16]\n");    // data_ptr
    fprintf(g->out, "    str x2, [x3, x1, lsl #3]\n");
    fprintf(g->out, "    ret\n\n");
}

void codegen(Node *program, const char *output_path) {
    Gen g = {0};
    g.out = fopen(output_path, "w");
    if (!g.out) { fprintf(stderr, "error: cannot open '%s'\n", output_path); exit(1); }

    // Register structs
    g.struct_count = program->program.structs.count;
    if (g.struct_count > 0) {
        g.struct_names = malloc(g.struct_count * sizeof(char *));
        g.struct_field_names = malloc(g.struct_count * sizeof(char **));
        g.struct_field_counts = malloc(g.struct_count * sizeof(int));
        for (int i = 0; i < g.struct_count; i++) {
            Node *s = program->program.structs.items[i];
            g.struct_names[i] = s->struct_def.name;
            g.struct_field_names[i] = s->struct_def.field_names;
            g.struct_field_counts[i] = s->struct_def.field_count;
        }
    }

    // Register enums
    g.enum_count = program->program.enums.count;
    if (g.enum_count > 0) {
        g.enum_names = malloc(g.enum_count * sizeof(char *));
        g.enum_variant_names = malloc(g.enum_count * sizeof(char **));
        g.enum_variant_counts = malloc(g.enum_count * sizeof(int));
        for (int i = 0; i < g.enum_count; i++) {
            Node *e = program->program.enums.items[i];
            g.enum_names[i] = e->enum_def.name;
            g.enum_variant_names[i] = e->enum_def.variant_names;
            g.enum_variant_counts[i] = e->enum_def.variant_count;
        }
    }

    // Register import aliases
    g.import_aliases = program->program.use_aliases;
    g.import_count = program->program.use_count;

    fprintf(g.out, ".section __TEXT,__text\n\n");

    emit_yell_int(&g);
    emit_yell_str(&g);
    emit_yell_dispatch(&g);
    emit_task_builtins(&g);
    emit_heap_alloc(&g);
    emit_str_eq(&g);
    emit_str_concat(&g);
    emit_int_to_str(&g);
    emit_str_builtins(&g);
    emit_map_builtins(&g);
    emit_imap_builtins(&g);
    emit_list_builtins(&g);

    // Panic handler for out-of-bounds
    fprintf(g.out, "// panic: index out of bounds\n");
    fprintf(g.out, ".globl _panic_oob\n.p2align 2\n_panic_oob:\n");
    fprintf(g.out, "    adrp x0, _oob_msg@PAGE\n");
    fprintf(g.out, "    add x0, x0, _oob_msg@PAGEOFF\n");
    fprintf(g.out, "    bl _yell_str\n");
    fprintf(g.out, "    mov x16, #1\n    mov x0, #1\n    svc #0x80\n\n");

    // Panic handler for capacity overflow
    fprintf(g.out, "// panic: capacity overflow\n");
    fprintf(g.out, ".globl _panic_capacity\n.p2align 2\n_panic_capacity:\n");
    fprintf(g.out, "    adrp x0, _cap_msg@PAGE\n");
    fprintf(g.out, "    add x0, x0, _cap_msg@PAGEOFF\n");
    fprintf(g.out, "    bl _yell_str\n");
    fprintf(g.out, "    mov x16, #1\n    mov x0, #1\n    svc #0x80\n\n");

    // Emit struct allocators (heap-based)
    for (int i = 0; i < g.struct_count; i++) {
        Node *s = program->program.structs.items[i];
        int size = s->struct_def.field_count * 8;
        if (size == 0) size = 8;
        fprintf(g.out, ".globl _alloc_%s\n.p2align 2\n_alloc_%s:\n", s->struct_def.name, s->struct_def.name);
        fprintf(g.out, "    stp x29, x30, [sp, #-16]!\n    mov x29, sp\n");
        fprintf(g.out, "    mov x0, #%d\n", size);
        fprintf(g.out, "    bl _heap_alloc\n");
        fprintf(g.out, "    mov sp, x29\n    ldp x29, x30, [sp], #16\n    ret\n\n");
    }

    for (int i = 0; i < program->program.funcs.count; i++)
        emit_func(&g, program->program.funcs.items[i]);

    // Emit test functions
    int test_count = program->program.tests.count;
    for (int i = 0; i < test_count; i++) {
        Node *t = program->program.tests.items[i];
        // Emit as _test_N function
        g.local_count = 0;
        g.stack_size = 0;
        g.loop_start_label = -1;
        g.loop_end_label = -1;
        g.loop_continue_label = -1;
        fprintf(g.out, ".globl _test_%d\n.p2align 2\n_test_%d:\n", i, i);
        fprintf(g.out, "    stp x29, x30, [sp, #-16]!\n    mov x29, sp\n    sub sp, sp, #1024\n");
        Node *body = t->test_def.body;
        for (int j = 0; j < body->block.stmts.count; j++)
            emit_stmt(&g, body->block.stmts.items[j]);
        emit_scope_cleanup(&g, 0);
        fprintf(g.out, "    mov x0, #0\n    mov sp, x29\n    ldp x29, x30, [sp], #16\n    ret\n\n");
    }

    // _assert_fail(line_number in x0): print failure and exit with code 1
    fprintf(g.out, ".globl _assert_fail\n.p2align 2\n_assert_fail:\n");
    fprintf(g.out, "    stp x29, x30, [sp, #-16]!\n    mov x29, sp\n");
    fprintf(g.out, "    bl _yell_int\n"); // print line number
    fprintf(g.out, "    adrp x0, _assert_msg@PAGE\n    add x0, x0, _assert_msg@PAGEOFF\n");
    fprintf(g.out, "    bl _yell_str\n");
    fprintf(g.out, "    mov x16, #1\n    mov x0, #1\n    svc #0x80\n\n");

    // Entry point
    if (test_count > 0) {
        // Test runner: call each test, print pass/fail
        fprintf(g.out, ".globl _start\n.p2align 2\n_start:\n");
        for (int i = 0; i < test_count; i++) {
            Node *t = program->program.tests.items[i];
            int name_idx = add_string(&g, t->test_def.name);
            // Print test name
            fprintf(g.out, "    adrp x0, _pass_prefix@PAGE\n    add x0, x0, _pass_prefix@PAGEOFF\n");
            fprintf(g.out, "    bl _yell_str\n");
            fprintf(g.out, "    adrp x0, _str%d@PAGE\n    add x0, x0, _str%d@PAGEOFF\n", name_idx, name_idx);
            fprintf(g.out, "    bl _yell_str\n");
            // Call test (if assert fails, it exits — never returns)
            fprintf(g.out, "    bl _test_%d\n", i);
        }
        fprintf(g.out, "    mov x16, #1\n    mov x0, #0\n    svc #0x80\n\n");
    } else {
        // Normal mode: call spark
        fprintf(g.out, ".globl _start\n.p2align 2\n_start:\n");
        fprintf(g.out, "    bl _spark\n");
        fprintf(g.out, "    mov x16, #1\n    mov x0, #0\n    svc #0x80\n\n");
    }

    if (g.string_count > 0) {
        fprintf(g.out, ".section __DATA,__data\n");
        for (int i = 0; i < g.string_count; i++)
            fprintf(g.out, "_str%d: .asciz \"%s\"\n", i, g.strings[i]);
        fprintf(g.out, "\n");
    }

    // Heap allocator state
    fprintf(g.out, ".section __DATA,__bss\n");
    fprintf(g.out, ".p2align 3\n");
    fprintf(g.out, "_heap_ptr: .quad 0\n");
    fprintf(g.out, "_heap_end: .quad 0\n");
    fprintf(g.out, "_heap_free_list: .quad 0\n");

    // Panic/test messages
    fprintf(g.out, ".section __DATA,__data\n");
    fprintf(g.out, "_oob_msg: .asciz \"panic: index out of bounds\"\n");
    fprintf(g.out, "_cap_msg: .asciz \"panic: collection capacity exceeded\"\n");
    fprintf(g.out, "_assert_msg: .asciz \" assertion failed\"\n");
    fprintf(g.out, "_pass_prefix: .asciz \"pass: \"\n");

    fclose(g.out);
}

void codegen_emit_builtins(FILE *out) {
    Gen g = {0};
    g.out = out;
    emit_yell_int(&g);
    emit_yell_str(&g);
    emit_yell_dispatch(&g);
    emit_task_builtins(&g);
    emit_heap_alloc(&g);
    emit_str_eq(&g);
    emit_str_concat(&g);
    emit_int_to_str(&g);
    emit_str_builtins(&g);
    emit_map_builtins(&g);
    emit_imap_builtins(&g);
    emit_list_builtins(&g);
    // Panic handlers
    fprintf(out, ".globl _panic_oob\n.p2align 2\n_panic_oob:\n");
    fprintf(out, "    adrp x0, _oob_msg@PAGE\n    add x0, x0, _oob_msg@PAGEOFF\n");
    fprintf(out, "    bl _yell_str\n    mov x16, #1\n    mov x0, #1\n    svc #0x80\n\n");
    fprintf(out, ".globl _panic_capacity\n.p2align 2\n_panic_capacity:\n");
    fprintf(out, "    adrp x0, _cap_msg@PAGE\n    add x0, x0, _cap_msg@PAGEOFF\n");
    fprintf(out, "    bl _yell_str\n    mov x16, #1\n    mov x0, #1\n    svc #0x80\n\n");
    // Data section for panic messages
    fprintf(out, ".section __DATA,__data\n");
    fprintf(out, "_oob_msg: .asciz \"panic: index out of bounds\"\n");
    fprintf(out, "_cap_msg: .asciz \"panic: capacity overflow\"\n");
    // Heap allocator state
    fprintf(out, ".section __DATA,__bss\n.p2align 3\n");
    fprintf(out, "_heap_ptr: .quad 0\n");
    fprintf(out, "_heap_end: .quad 0\n");
    fprintf(out, "_heap_free_list: .quad 0\n");
    fprintf(out, ".section __TEXT,__text\n\n");
}
