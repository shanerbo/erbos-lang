#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include "lexer.h"
#include "parser.h"
#include "monomorph.h"
#include "checker.h"
#include "optimizer.h"
#include "irgen.h"
#include "iropt.h"
#include "regalloc.h"
#include "iremit.h"
#include "runtime_emit.h"

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
        fprintf(stderr,
            "usage: erbos [-O0|-O1|-O2] <file.ptt>      # build to binary\n"
            "       erbos [-O0|-O1|-O2] run <file.ptt>  # build and run, then clean up\n"
            "       erbos [-O0|-O1|-O2] test <file.ptt> # same as run; the test framework runs in the binary\n"
            "       erbos [-O0|-O1|-O2] ir <file.ptt>   # emit the .s only, don't assemble\n"
            "\n"
            "  -O0  skip iropt entirely (no IR-level transformations)\n"
            "  -O1  default — every iropt pass runs (currently scaffold-only; P5.x passes land here)\n"
            "  -O2  reserved for tuning; identical to -O1 today\n");
        return 1;
    }

    // First pass: extract any -O0/-O1/-O2 anywhere in argv. Default
    // is -O1 if no flag is given. The flag may appear before or
    // after the subcommand (`-O0 run file.ptt` and `run -O0 file.ptt`
    // are equivalent), since CLI ergonomics shouldn't depend on
    // memorising flag-vs-subcommand order. After this pass, argv is
    // compacted to remove the consumed flag(s).
    IROptLevel opt_level = IROPT_O1;
    int j = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-O0"))      opt_level = IROPT_O0;
        else if (!strcmp(argv[i], "-O1")) opt_level = IROPT_O1;
        else if (!strcmp(argv[i], "-O2")) opt_level = IROPT_O2;
        else                              argv[j++] = argv[i];
    }
    argc = j;

    // Subcommand parsing. The IR backend is the only backend now;
    // `run` / `test` add execute-and-cleanup, `ir` stops after .s
    // emission. The earlier `legacy` opt-out (for the retired direct
    // codegen, src/codegen.c) was removed in #34 P4.3g.
    int run_mode = 0;     // run binary after linking, then delete it
    int ir_only = 0;      // stop after generating .s (no assemble/link/run)
    const char *input = NULL;

    if (argc >= 3 && strcmp(argv[1], "run") == 0) {
        run_mode = 1;
        input = argv[2];
    } else if (argc >= 3 && strcmp(argv[1], "test") == 0) {
        run_mode = 1;
        input = argv[2];
    } else if (argc >= 3 && strcmp(argv[1], "ir") == 0) {
        ir_only = 1;
        input = argv[2];
    } else if (argc >= 2) {
        input = argv[1];
    }
    if (!input) {
        fprintf(stderr, "error: no input file\n");
        return 1;
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

        // Merge funcs.
        //   - Free functions get prefixed with `<alias>_<name>` so the
        //     caller writes `math.max(...)` and the call-site dispatch
        //     resolves to `_math_max`.
        //   - Methods (func_def.receiver_type != NULL) keep their
        //     declared name; method dispatch is by receiver type, not
        //     by alias. `use std/string as foo` still gives `String.len`
        //     globally — `foo.len` would be nonsensical because there's
        //     no `foo` *value* to call a method on.
        const char *alias = program->program.use_aliases[ui];
        char prefixed[256];
        for (int fi = 0; fi < imp_prog->program.funcs.count; fi++) {
            Node *f = imp_prog->program.funcs.items[fi];
            if (!f->func_def.receiver_type) {
                snprintf(prefixed, sizeof(prefixed), "%s_%s", alias, f->func_def.name);
                f->func_def.name = strdup(prefixed);
            }
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
        // Recursively absorb the imported file's own `use` directives
        // into the parent program. This is what makes
        // `use std/string_map` (which itself does `use std/string`)
        // pull `String` into the parent program's struct table — no
        // explicit re-import needed at the top of the user's file.
        // Deduplicate: skip a path that's already present.
        for (int rui = 0; rui < imp_prog->program.use_count; rui++) {
            const char *rpath = imp_prog->program.use_paths[rui];
            int already = 0;
            for (int k = 0; k < program->program.use_count; k++) {
                if (!strcmp(program->program.use_paths[k], rpath)) { already = 1; break; }
            }
            if (already) continue;
            int idx2 = program->program.use_count++;
            program->program.use_paths = realloc(program->program.use_paths,
                program->program.use_count * sizeof(char *));
            program->program.use_aliases = realloc(program->program.use_aliases,
                program->program.use_count * sizeof(char *));
            program->program.use_paths[idx2] = strdup(rpath);
            program->program.use_aliases[idx2] = strdup(imp_prog->program.use_aliases[rui]);
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

    // Helper: write the IR-pipeline assembly to `asm_path`. Splits its
    // work via the irgen + iremit + finalise sequence; runtime helpers
    // (yell, heap allocator, str/list/map/imap helpers, panic and
    // assert handlers, data section) come from src/runtime_emit.c.
    // Returns the number of IR functions emitted.
    #define EMIT_IR_TO_FILE(asm_path_arg) ({                                    \
        IRProgram *ir = irgen_generate(program);                                \
        iropt_run(ir, opt_level);                                               \
        FILE *ir_out = fopen((asm_path_arg), "w");                              \
        fprintf(ir_out, ".global _start\n.align 2\n");                          \
        fprintf(ir_out, ".section __TEXT,__text\n\n");                          \
        runtime_emit_builtins(ir_out);                                          \
        for (int si = 0; si < program->program.structs.count; si++) {           \
            Node *s = program->program.structs.items[si];                       \
            int size = s->struct_def.field_count * 8;                           \
            if (size == 0) size = 8;                                            \
            fprintf(ir_out, ".globl _alloc_%s\n.p2align 2\n_alloc_%s:\n",       \
                    s->struct_def.name, s->struct_def.name);                    \
            fprintf(ir_out, "    stp x29, x30, [sp, #-16]!\n    mov x29, sp\n");\
            fprintf(ir_out, "    mov x0, #%d\n    bl _heap_alloc\n", size);     \
            fprintf(ir_out, "    mov sp, x29\n    ldp x29, x30, [sp], #16\n    ret\n\n"); \
        }                                                                       \
        for (int i = 0; i < ir->func_count; i++) {                              \
            RegAllocResult alloc = regalloc_run(&ir->funcs[i]);                 \
            iremit_func(ir_out, &ir->funcs[i], &alloc);                         \
            fprintf(ir_out, "\n");                                              \
            free(alloc.vreg_to_phys);                                           \
            free(alloc.vreg_to_spill);                                          \
        }                                                                       \
        int test_count = program->program.tests.count;                          \
        if (test_count > 0) {                                                   \
            fprintf(ir_out, ".globl _start\n.p2align 2\n_start:\n");            \
            for (int i = 0; i < test_count; i++) {                              \
                fprintf(ir_out, "    adrp x0, _pass_prefix@PAGE\n    add x0, x0, _pass_prefix@PAGEOFF\n"); \
                fprintf(ir_out, "    bl _yell_str\n");                          \
                fprintf(ir_out, "    adrp x0, _test_name_%d@PAGE\n    add x0, x0, _test_name_%d@PAGEOFF\n", i, i); \
                fprintf(ir_out, "    bl _yell_str\n");                          \
                fprintf(ir_out, "    bl _test_%d\n", i);                        \
            }                                                                   \
            fprintf(ir_out, "    mov x16, #1\n    mov x0, #0\n    svc #0x80\n\n"); \
        } else {                                                                \
            fprintf(ir_out, "_start:\n    bl _spark\n    mov x16, #1\n    mov x0, #0\n    svc #0x80\n\n"); \
        }                                                                       \
        iremit_finalize_data(ir_out);                                           \
        if (test_count > 0) {                                                   \
            /* P3.4: each test name is a `String` header (4 quads:              \
             * cap, count, data, owned=0) so the runtime test runner            \
             * can pass it to `_yell_str` (which now reads count from           \
             * the header instead of scanning bytes). The bytes live            \
             * in `_test_name_<i>_bytes`; the header is at                      \
             * `_test_name_<i>`. */                                              \
            fprintf(ir_out, ".section __DATA,__data\n");                        \
            for (int i = 0; i < test_count; i++) {                              \
                Node *t = program->program.tests.items[i];                      \
                int tn = (int)strlen(t->test_def.name);                         \
                fprintf(ir_out, "_test_name_%d_bytes: .asciz \"%s\"\n", i,      \
                    t->test_def.name);                                          \
                fprintf(ir_out, ".p2align 3\n_test_name_%d:\n", i);             \
                fprintf(ir_out, "    .quad %d\n    .quad %d\n", tn, tn);        \
                fprintf(ir_out, "    .quad _test_name_%d_bytes\n    .quad 0\n", i); \
            }                                                                   \
            fprintf(ir_out, ".section __TEXT,__text\n");                        \
        }                                                                       \
        fclose(ir_out);                                                         \
        ir->func_count;                                                         \
    })

    // `erbos ir <file.ptt>` — emit .s only, don't assemble or run.
    if (ir_only) {
        int func_count = EMIT_IR_TO_FILE(asm_path);
        printf("IR pipeline: generated %s (%d functions)\n", asm_path, func_count);
        free(src);
        return 0;
    }

    // Build path. The IR backend is now the only backend; the original
    // direct codegen (src/codegen.c) was retired in #34 P4.3g after a
    // release of opt-out testing through the `legacy` subcommand
    // confirmed the IR pipeline produced byte-identical output on
    // every program in the corpus.
    EMIT_IR_TO_FILE(asm_path);
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
