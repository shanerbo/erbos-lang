// Linux ARM64 backend.
//
// Phase 3 of docs/linux-arm64-backend-plan.md. Phase 4 cleared the
// runtime gates: 182 program runs (examples + framework + leetcode
// + IR-regression matrix at -O0/-O1/-O2 + runtime-panic tests) all
// produce output byte-identical to the Darwin baseline when executed
// inside an arm64 Alpine Linux container.
//
// Every Linux-specific constant below is cited inline to its
// source-of-truth document; do not change a value without re-reading
// the cited document.
//
// Sources of truth:
//   - AArch64 GAS relocation syntax (`:lo12:`):
//       binutils source `gas/config/tc-aarch64.c` and the AArch64 ELF
//       ABI; corresponding ELF relocs are R_AARCH64_ADR_PREL_PG_HI21
//       and R_AARCH64_ADD_ABS_LO12_NC.
//   - Linux/aarch64 syscall numbers:
//       linux/include/uapi/asm-generic/unistd.h. AArch64 inherits the
//       generic numbering (`__NR_use_generic_syscalls`).
//       __NR_write = 64, __NR_exit = 93, __NR_mmap = 222.
//   - Linux mmap flag values (asm-generic):
//       include/uapi/asm-generic/mman.h:
//         MAP_PRIVATE   = 0x02
//         MAP_ANONYMOUS = 0x20
//       PROT_READ|PROT_WRITE = 3 (asm-generic/mman-common.h).
//   - Linux syscall ABI on AArch64:
//       arch/arm64/kernel/entry.S, "AArch64 syscall calling convention":
//         x8 = syscall number, x0..x7 = args, dispatch via `svc #0`.
//       Source: man syscall(2), section "Architecture calling
//       conventions", aarch64 row.

#include <stdio.h>
#include <string.h>

#include "target.h"
#include "target_spawn.h"
#include "runtime_emit.h"

static const Target k_target_linux_arm64;

static void linux_arm64_emit_text_section(FILE *out) {
    // GAS recognises `.text` as the canonical name for the text
    // section on ELF targets; it is also the default at start of
    // assembly, but explicit emission keeps the file readable.
    fprintf(out, ".text\n");
}

static void linux_arm64_emit_data_section(FILE *out) {
    fprintf(out, ".data\n");
}

static void linux_arm64_emit_bss_section(FILE *out) {
    fprintf(out, ".bss\n");
}

static void linux_arm64_emit_addr_load(FILE *out, int reg, const char *sym) {
    // ELF AArch64 PC-relative-page address load. `adrp` materialises
    // the page address (low 12 bits zero); the `:lo12:` modifier on
    // `add` adds the symbol's offset within the page. Matches the
    // pair of relocations emitted by clang/gcc for unprotected
    // local symbols.
    fprintf(out, "    adrp x%d, %s\n", reg, sym);
    fprintf(out, "    add x%d, x%d, :lo12:%s\n", reg, reg, sym);
}

// Linux/AArch64 syscall ABI: x8 = syscall number, args x0..x7,
// dispatch via `svc #0`. Source: arch/arm64/kernel/entry.S and
// man syscall(2) "aarch64" entry.

static void linux_arm64_emit_sys_write_stdout(FILE *out) {
    // write(fd=1, buf, len). __NR_write = 64.
    fprintf(out, "    mov x0, #1\n");
    fprintf(out, "    mov x8, #64\n");
    fprintf(out, "    svc #0\n");
}

static void linux_arm64_emit_sys_exit(FILE *out, int code) {
    // exit(code). __NR_exit = 93.
    // (asm-generic/unistd.h. The thread-group variant __NR_exit_group
    // = 94 also works for single-threaded processes; we use exit
    // here for symmetry with the Darwin emitter.)
    fprintf(out, "    mov x0, #%d\n", code);
    fprintf(out, "    mov x8, #93\n");
    fprintf(out, "    svc #0\n");
}

static void linux_arm64_emit_sys_mmap_anon_64k(FILE *out) {
    // mmap(NULL, 0x10000, PROT_READ|PROT_WRITE,
    //      MAP_PRIVATE|MAP_ANONYMOUS, fd=-1, offset=0).
    // __NR_mmap = 222 (asm-generic/unistd.h).
    // MAP_PRIVATE = 0x02, MAP_ANONYMOUS = 0x20 (asm-generic/mman.h).
    // PROT_READ|PROT_WRITE = 3 (asm-generic/mman-common.h).
    fprintf(out, "    mov x0, #0\n");
    fprintf(out, "    mov x1, #0x10000\n");
    fprintf(out, "    mov x2, #3\n");
    fprintf(out, "    mov x3, #0x22\n");
    fprintf(out, "    mov x4, #-1\n");
    fprintf(out, "    mov x5, #0\n");
    fprintf(out, "    mov x8, #222\n");
    fprintf(out, "    svc #0\n");
}

static void linux_arm64_emit_prologue(FILE *out) {
    // ELF AArch64: `.global` exports the symbol; `.align 2` sets
    // 4-byte instruction alignment. `_start` is the conventional
    // entry symbol that the static linker (`ld -e _start`) treats
    // as the program entry point on Linux.
    fprintf(out, ".global _start\n.align 2\n");
    linux_arm64_emit_text_section(out);
    fprintf(out, "\n");
}

static void linux_arm64_emit_runtime_builtins(FILE *out) {
    runtime_emit_builtins(out, &k_target_linux_arm64);
}

static void linux_arm64_emit_entry(FILE *out, int test_count) {
    if (test_count > 0) {
        fprintf(out, ".globl _start\n.p2align 2\n_start:\n");
        for (int i = 0; i < test_count; i++) {
            linux_arm64_emit_addr_load(out, 0, "_pass_prefix");
            fprintf(out, "    bl _yell_str\n");
            char sym[32];
            snprintf(sym, sizeof(sym), "_test_name_%d", i);
            linux_arm64_emit_addr_load(out, 0, sym);
            fprintf(out, "    bl _yell_str\n");
            fprintf(out, "    bl _test_%d\n", i);
        }
        linux_arm64_emit_sys_exit(out, 0);
        fprintf(out, "\n");
    } else {
        fprintf(out, "_start:\n    bl _spark\n");
        linux_arm64_emit_sys_exit(out, 0);
        fprintf(out, "\n");
    }
}

static void linux_arm64_emit_test_data(FILE *out, int test_count,
                                       const char *const *test_names) {
    if (test_count <= 0) return;
    linux_arm64_emit_data_section(out);
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
    linux_arm64_emit_text_section(out);
}

static int linux_arm64_assemble_and_link(const char *asm_path,
                                         const char *obj_path,
                                         const char *out_name) {
    // Linux toolchain: GNU `as` and `ld` (or LLVM equivalents under
    // the same names). No SDK lookup, no libc dependency: the
    // emitted assembly only uses raw syscalls (no calls to libc),
    // and `_start` is the program entry point (not `main`).
    {
        char *as_argv[] = {
            "as", "-o", (char *)obj_path, (char *)asm_path, NULL
        };
        if (target_spawn_argv("as", as_argv) != 0) {
            fprintf(stderr, "error: assembly failed\n");
            return 1;
        }
    }
    {
        // -nostdlib: don't pull in libc startup files (crt1, crti,
        //            crtn) — we provide our own `_start`.
        // -static:   freestanding binary; no dynamic loader required.
        // -e _start: explicit entry symbol.
        char *ld_argv[] = {
            "ld", "-o", (char *)out_name, (char *)obj_path,
            "-nostdlib", "-static", "-e", "_start", NULL
        };
        if (target_spawn_argv("ld", ld_argv) != 0) {
            fprintf(stderr, "error: linking failed\n");
            return 1;
        }
    }
    return 0;
}

static const Target k_target_linux_arm64 = {
    .name                    = "linux-arm64",
    .emit_prologue           = linux_arm64_emit_prologue,
    .emit_text_section       = linux_arm64_emit_text_section,
    .emit_data_section       = linux_arm64_emit_data_section,
    .emit_bss_section        = linux_arm64_emit_bss_section,
    .emit_addr_load          = linux_arm64_emit_addr_load,
    .emit_sys_write_stdout   = linux_arm64_emit_sys_write_stdout,
    .emit_sys_exit           = linux_arm64_emit_sys_exit,
    .emit_sys_mmap_anon_64k  = linux_arm64_emit_sys_mmap_anon_64k,
    .emit_runtime_builtins   = linux_arm64_emit_runtime_builtins,
    .emit_entry              = linux_arm64_emit_entry,
    .emit_test_data          = linux_arm64_emit_test_data,
    .assemble_and_link       = linux_arm64_assemble_and_link,
};

const Target *target_linux_arm64(void) {
    return &k_target_linux_arm64;
}
