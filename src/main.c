#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include "lexer.h"
#include "parser.h"
#include "checker.h"
#include "codegen.h"

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
        fprintf(stderr, "usage: erbos <file.ptt>\n       erbos run <file.ptt>\n       erbos test <file.ptt>\n");
        return 1;
    }

    // Check for "erbos run file.ptt" or "erbos test file.ptt"
    int run_mode = 0;
    int test_mode __attribute__((unused)) = 0;
    const char *input;
    if (argc >= 3 && strcmp(argv[1], "run") == 0) {
        run_mode = 1;
        input = argv[2];
    } else if (argc >= 3 && strcmp(argv[1], "test") == 0) {
        test_mode = 1;
        run_mode = 1; // test mode also runs
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

    // Type check
    checker_run(program);

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
