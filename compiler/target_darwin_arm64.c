// Darwin ARM64 backend.
//
// Phase 1 of the linux-arm64 backend plan: the program-level
// scaffolding that used to live inline in compiler/main.c — section
// prologue, runtime emission, _start / test harness, per-test name
// data, and the assembler/linker driver — now lives behind the
// `Target` interface declared in compiler/target.h.
//
// What stays elsewhere (per the plan's "Phase 1: zero behavior change"
// rule):
//   - compiler/iremit.c             — IR -> ARM64 instruction emitter
//                                     (still emits Mach-O sections and
//                                     Darwin @PAGE/@PAGEOFF references)
//   - compiler/runtime_emit.c       — _yell / _heap_alloc / panic /
//                                     etc. (still uses svc #0x80 and
//                                     Mach-O __DATA/__TEXT)
//   - compiler/main.c               — _alloc_<X> / _clone_<X> /
//                                     _drop_<X> per-struct helpers
//                                     (still emit ARM64 directly)
//
// Phase 3 will introduce the Linux variants of those files; the
// Darwin layer below stays as-is because Phase 1 is a pure refactor.

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "target.h"
#include "target_spawn.h"
#include "runtime_emit.h"

static const Target k_target_darwin_arm64;

static void darwin_arm64_emit_text_section(FILE *out) {
    fprintf(out, ".section __TEXT,__text\n");
}

static void darwin_arm64_emit_data_section(FILE *out) {
    fprintf(out, ".section __DATA,__data\n");
}

static void darwin_arm64_emit_bss_section(FILE *out) {
    fprintf(out, ".section __DATA,__bss\n");
}

static void darwin_arm64_emit_addr_load(FILE *out, int reg, const char *sym) {
    fprintf(out, "    adrp x%d, %s@PAGE\n", reg, sym);
    fprintf(out, "    add x%d, x%d, %s@PAGEOFF\n", reg, reg, sym);
}

// Darwin AArch64 syscall ABI: x16 = syscall number, args x0..x7,
// dispatch via `svc #0x80`. Source: bsd/kern/syscalls.master in xnu
// for SYS_write = 4 / SYS_exit = 1 / SYS_mmap = 197.
static void darwin_arm64_emit_sys_write_stdout(FILE *out) {
    fprintf(out, "    mov x0, #1\n");
    fprintf(out, "    mov x16, #4\n");
    fprintf(out, "    svc #0x80\n");
}

static void darwin_arm64_emit_sys_exit(FILE *out, int code) {
    fprintf(out, "    mov x16, #1\n    mov x0, #%d\n    svc #0x80\n", code);
}

static void darwin_arm64_emit_sys_mmap_anon_64k(FILE *out) {
    // mmap(NULL, 0x10000, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANON, -1, 0)
    // Darwin flag values: MAP_PRIVATE = 0x0002, MAP_ANON = 0x1000
    // (sys/mman.h). PROT_READ|PROT_WRITE = 3.
    fprintf(out, "    mov x0, #0\n");
    fprintf(out, "    mov x1, #0x10000\n");
    fprintf(out, "    mov x2, #3\n");
    fprintf(out, "    mov x3, #0x1002\n");
    fprintf(out, "    mov x4, #-1\n");
    fprintf(out, "    mov x5, #0\n");
    fprintf(out, "    mov x16, #197\n");
    fprintf(out, "    svc #0x80\n");
}

static void darwin_arm64_emit_prologue(FILE *out) {
    fprintf(out, ".global _start\n.align 2\n");
    darwin_arm64_emit_text_section(out);
    fprintf(out, "\n");
}

static void darwin_arm64_emit_runtime_builtins(FILE *out) {
    runtime_emit_builtins(out, &k_target_darwin_arm64);
}

static void darwin_arm64_emit_entry(FILE *out, int test_count) {
    if (test_count > 0) {
        fprintf(out, ".globl _start\n.p2align 2\n_start:\n");
        for (int i = 0; i < test_count; i++) {
            darwin_arm64_emit_addr_load(out, 0, "_pass_prefix");
            fprintf(out, "    bl _yell_str\n");
            char sym[32];
            snprintf(sym, sizeof(sym), "_test_name_%d", i);
            darwin_arm64_emit_addr_load(out, 0, sym);
            fprintf(out, "    bl _yell_str\n");
            fprintf(out, "    bl _test_%d\n", i);
        }
        darwin_arm64_emit_sys_exit(out, 0);
        fprintf(out, "\n");
    } else {
        fprintf(out, "_start:\n    bl _spark\n");
        darwin_arm64_emit_sys_exit(out, 0);
        fprintf(out, "\n");
    }
}

static void darwin_arm64_emit_test_data(FILE *out, int test_count,
                                        const char *const *test_names) {
    if (test_count <= 0) return;
    // Each test name is a `String` header (4 quads: cap, count, data,
    // owned=0) so the runtime test runner can pass it to `_yell_str`
    // (which reads count from the header instead of scanning bytes).
    // The bytes live in `_test_name_<i>_bytes`; the array-of-byte
    // header is `_test_name_<i>_arr`; the String header is
    // `_test_name_<i>` itself.
    darwin_arm64_emit_data_section(out);
    for (int i = 0; i < test_count; i++) {
        const char *name = test_names[i];
        int n = (int)strlen(name);
        fprintf(out, "_test_name_%d_bytes: .asciz \"%s\"\n", i, name);
        fprintf(out, ".p2align 3\n_test_name_%d_arr:\n", i);
        fprintf(out, "    .quad %d\n    .quad _test_name_%d_bytes\n", n, i);
        fprintf(out, "_test_name_%d:\n", i);
        fprintf(out, "    .quad %d\n    .quad %d\n", n, n);
        fprintf(out, "    .quad _test_name_%d_arr\n    .quad 0\n", i);
    }
    // Restore the text section so any further emission keeps working.
    darwin_arm64_emit_text_section(out);
}

static int darwin_arm64_assemble_and_link(const char *asm_path,
                                          const char *obj_path,
                                          const char *out_name) {
    {
        char *as_argv[] = {
            "as", "-o", (char *)obj_path, (char *)asm_path, NULL
        };
        if (target_spawn_argv("as", as_argv) != 0) {
            fprintf(stderr, "error: assembly failed\n");
            return 1;
        }
    }

    // Capture the SDK path once. `xcrun --show-sdk-path` was
    // previously embedded as `$(xcrun ...)` in a shell command;
    // now we run xcrun ourselves and pass the output as an
    // explicit `-syslibroot <path>` argv pair.
    char sdk_path[1024] = {0};
    {
        char *xcrun_argv[] = { "xcrun", "--show-sdk-path", NULL };
        if (!target_capture_stdout("xcrun", xcrun_argv,
                            sdk_path, sizeof(sdk_path))) {
            fprintf(stderr, "error: failed to query SDK path "
                            "(`xcrun --show-sdk-path`)\n");
            return 1;
        }
    }

    {
        char *ld_argv[] = {
            "ld", "-o", (char *)out_name, (char *)obj_path,
            "-lSystem", "-syslibroot", sdk_path,
            "-e", "_start", NULL
        };
        if (target_spawn_argv("ld", ld_argv) != 0) {
            fprintf(stderr, "error: linking failed\n");
            return 1;
        }
    }
    return 0;
}

static const Target k_target_darwin_arm64 = {
    .emit_prologue           = darwin_arm64_emit_prologue,
    .emit_text_section       = darwin_arm64_emit_text_section,
    .emit_data_section       = darwin_arm64_emit_data_section,
    .emit_bss_section        = darwin_arm64_emit_bss_section,
    .emit_addr_load          = darwin_arm64_emit_addr_load,
    .emit_sys_write_stdout   = darwin_arm64_emit_sys_write_stdout,
    .emit_sys_exit           = darwin_arm64_emit_sys_exit,
    .emit_sys_mmap_anon_64k  = darwin_arm64_emit_sys_mmap_anon_64k,
    .emit_runtime_builtins   = darwin_arm64_emit_runtime_builtins,
    .emit_entry              = darwin_arm64_emit_entry,
    .emit_test_data          = darwin_arm64_emit_test_data,
    .assemble_and_link       = darwin_arm64_assemble_and_link,
};

const Target *target_darwin_arm64(void) {
    return &k_target_darwin_arm64;
}

// Default-target / by-name resolution lives here for now because
// darwin-arm64 is the host of record. The Linux backend is reachable
// via target_by_name("linux-arm64").
const Target *target_default(void) {
    return &k_target_darwin_arm64;
}

extern const Target *target_linux_arm64(void); // compiler/target_linux_arm64.c

TargetLookupResult target_by_name(const char *name, const Target **out_target) {
    if (!name) return TARGET_LOOKUP_UNKNOWN;
    if (!strcmp(name, "darwin-arm64")) {
        if (out_target) *out_target = &k_target_darwin_arm64;
        return TARGET_LOOKUP_OK;
    }
    if (!strcmp(name, "linux-arm64")) {
        if (out_target) *out_target = target_linux_arm64();
        return TARGET_LOOKUP_OK;
    }
    return TARGET_LOOKUP_UNKNOWN;
}
