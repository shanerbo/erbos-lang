#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include "lexer.h"
#include "parser.h"
#include "monomorph.h"
#include "checker.h"
#include "optimizer.h"
#include "codegen.h"
#include "irgen.h"
#include "regalloc.h"
#include "iremit.h"

static char *read_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "error: cannot open '%s'\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    char *buf = malloc(len + 1);
    fread(buf, 1, len, f);
    buf[len] = '\0';
    fclose(f);
    return buf;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: erbos <file.ptt>\n       erbos run <file.ptt>\n       erbos test <file.ptt>\n       erbos ir <file.ptt>\n");
        return 1;
    }

    // Check for "erbos run file.ptt" or "erbos test file.ptt" or "erbos ir file.ptt"
    // (test mode currently behaves identically to run mode at the driver level —
    //  the test framework is invoked from inside the compiled program.)
    int run_mode = 0;
    int ir_mode = 0;
    const char *input;
    if (argc >= 3 && strcmp(argv[1], "run") == 0) {
        run_mode = 1;
        input = argv[2];
    } else if (argc >= 3 && strcmp(argv[1], "test") == 0) {
        run_mode = 1;
        input = argv[2];
    } else if (argc >= 3 && strcmp(argv[1], "ir") == 0) {
        ir_mode = 1;
        input = argv[2];
    } else {
        input = argv[1];
    }

    // Check file extension
    const char *ext = strrchr(input, '.');
    if (!ext || strcmp(ext, ".ptt") != 0) {
        fprintf(stderr, "error: expected .ptt file, got '%s'\n", input);
        return 1;
    }

    // Derive output name: replace .ptt with nothing
    char out_name[256];
    strncpy(out_name, input, sizeof(out_name) - 1);
    char *dot = strrchr(out_name, '.');
    if (dot) *dot = '\0';

    char asm_path[256];
    snprintf(asm_path, sizeof(asm_path), "%s.s", out_name);

    // Lex
    char *src = read_file(input);
    Lexer l;
    lexer_init(&l, src);
    lexer_tokenize(&l);

    // Parse
    Parser p;
    parser_init(&p, &l);
    p.filename = input;
    Node *program = parser_parse(&p);

    // Resolve imports
    for (int ui = 0; ui < program->program.use_count; ui++) {
        char import_path[512];
        // Try relative to input file directory
        char dir[256] = {0};
        strncpy(dir, input, sizeof(dir) - 1);
        char *last_slash = strrchr(dir, '/');
        if (last_slash) *(last_slash + 1) = '\0';
        else dir[0] = '\0';

        // Try: dir/path.ptt, then std/path.ptt
        snprintf(import_path, sizeof(import_path), "%s%s.ptt", dir, program->program.use_paths[ui]);
        FILE *test_f = fopen(import_path, "r");
        if (!test_f) {
            // Try std/ relative to compiler location
            snprintf(import_path, sizeof(import_path), "std/%s.ptt", program->program.use_paths[ui] + (strncmp(program->program.use_paths[ui], "std/", 4) == 0 ? 4 : 0));
            test_f = fopen(import_path, "r");
        }
        if (!test_f) {
            fprintf(stderr, "error: cannot find module '%s'\n", program->program.use_paths[ui]);
            return 1;
        }
        fclose(test_f);

        // Lex + parse the imported file
        char *imp_src = read_file(import_path);
        Lexer imp_l;
        lexer_init(&imp_l, imp_src);
        lexer_tokenize(&imp_l);
        Parser imp_p;
        parser_init(&imp_p, &imp_l);
        imp_p.filename = import_path;
        Node *imp_prog = parser_parse(&imp_p);

        // Merge: prefix function names with alias_
        const char *alias = program->program.use_aliases[ui];
        char prefixed[256];
        for (int fi = 0; fi < imp_prog->program.funcs.count; fi++) {
            Node *f = imp_prog->program.funcs.items[fi];
            snprintf(prefixed, sizeof(prefixed), "%s_%s", alias, f->func_def.name);
            f->func_def.name = strdup(prefixed);
            // Add to main program
            if (program->program.funcs.count >= program->program.funcs.cap) {
                program->program.funcs.cap = program->program.funcs.cap ? program->program.funcs.cap * 2 : 4;
                program->program.funcs.items = realloc(program->program.funcs.items, program->program.funcs.cap * sizeof(Node *));
            }
            program->program.funcs.items[program->program.funcs.count++] = f;
        }
        // Merge structs
        for (int si = 0; si < imp_prog->program.structs.count; si++) {
            Node *s = imp_prog->program.structs.items[si];
            if (program->program.structs.count >= program->program.structs.cap) {
                program->program.structs.cap = program->program.structs.cap ? program->program.structs.cap * 2 : 4;
                program->program.structs.items = realloc(program->program.structs.items, program->program.structs.cap * sizeof(Node *));
            }
            program->program.structs.items[program->program.structs.count++] = s;
        }
        // Merge enums
        for (int ei = 0; ei < imp_prog->program.enums.count; ei++) {
            Node *e = imp_prog->program.enums.items[ei];
            if (program->program.enums.count >= program->program.enums.cap) {
                program->program.enums.cap = program->program.enums.cap ? program->program.enums.cap * 2 : 4;
                program->program.enums.items = realloc(program->program.enums.items, program->program.enums.cap * sizeof(Node *));
            }
            program->program.enums.items[program->program.enums.count++] = e;
        }
        free(imp_src);
    }

    // Monomorphize generic structs and methods. After this pass the
    // AST is fully concrete; the checker, optimizer, and codegen need
    // no awareness of generics.
    monomorph_run(program);

    // Type check
    checker_run(program);

    // Optimize
    optimizer_run(program);

    // IR pipeline (experimental — erbos ir mode)
    if (ir_mode) {
        IRProgram *ir = irgen_generate(program);
        FILE *ir_out = fopen(asm_path, "w");
        fprintf(ir_out, ".global _start\n.align 2\n");
        fprintf(ir_out, ".section __TEXT,__text\n\n");
        // Emit builtins (yell, heap_alloc, str ops, etc.)
        codegen_emit_builtins(ir_out);
        // Emit struct allocators
        for (int si = 0; si < program->program.structs.count; si++) {
            Node *s = program->program.structs.items[si];
            int size = s->struct_def.field_count * 8;
            if (size == 0) size = 8;
            fprintf(ir_out, ".globl _alloc_%s\n.p2align 2\n_alloc_%s:\n", s->struct_def.name, s->struct_def.name);
            fprintf(ir_out, "    stp x29, x30, [sp, #-16]!\n    mov x29, sp\n");
            fprintf(ir_out, "    mov x0, #%d\n    bl _heap_alloc\n", size);
            fprintf(ir_out, "    mov sp, x29\n    ldp x29, x30, [sp], #16\n    ret\n\n");
        }
        // Emit user functions via IR pipeline
        for (int i = 0; i < ir->func_count; i++) {
            RegAllocResult alloc = regalloc_run(&ir->funcs[i]);
            iremit_func(ir_out, &ir->funcs[i], &alloc);
            fprintf(ir_out, "\n");
            free(alloc.vreg_to_phys);
            free(alloc.vreg_to_spill);
        }
        // _start calls spark
        fprintf(ir_out, "_start:\n    bl _spark\n    mov x16, #1\n    mov x0, #0\n    svc #0x80\n");
        fclose(ir_out);
        printf("IR pipeline: generated %s (%d functions)\n", asm_path, ir->func_count);
        free(src);
        return 0;
    }

    // Codegen
    codegen(program, asm_path);
    if (!run_mode) printf("generated %s\n", asm_path);

    // Assemble + link
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "as -o %s.o %s", out_name, asm_path);
    if (system(cmd) != 0) { fprintf(stderr, "error: assembly failed\n"); return 1; }

    snprintf(cmd, sizeof(cmd), "ld -o %s %s.o -lSystem -syslibroot $(xcrun --show-sdk-path) -e _start", out_name, out_name);
    if (system(cmd) != 0) { fprintf(stderr, "error: linking failed\n"); return 1; }

    // Cleanup
    snprintf(cmd, sizeof(cmd), "rm -f %s.o %s.s", out_name, out_name);
    system(cmd);

    if (run_mode) {
        // Run the binary
        int ret = system(out_name);
        // Delete binary after running
        snprintf(cmd, sizeof(cmd), "rm -f %s", out_name);
        system(cmd);
        free(src);
        return WEXITSTATUS(ret);
    }

    printf("built %s\n", out_name);
    free(src);
    return 0;
}
