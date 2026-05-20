#ifndef TARGET_H
#define TARGET_H

#include <stdio.h>

// Backend interface. Each target (e.g. darwin-arm64, linux-arm64)
// implements these callbacks; the frontend in compiler/main.c picks
// the active target once and routes every program-level emission
// step through it.
//
// Differences between Darwin/Mach-O AArch64 and Linux/ELF AArch64
// that this interface abstracts:
//   - Section directives (`__TEXT,__text` vs `.text`).
//   - Address-load relocation syntax (`@PAGE`/`@PAGEOFF` vs `:lo12:`).
//   - Syscall ABI (Darwin: x16 holds the number, svc #0x80; Linux:
//     x8 holds the number, svc #0).
//   - Toolchain (xcrun + ld -lSystem -syslibroot vs gas + ld with
//     no libc dependency).
//
// What this interface does NOT abstract: the actual ARM64 instructions
// emitted by the IR backend (compiler/iremit.c) — both targets share
// the same instruction set, calling convention (AAPCS64), and frame
// layout. The handful of Darwin-flavored points in iremit.c are
// surfaced here as `emit_addr_load` / `emit_text_section` /
// `emit_data_section` so a single iremit.c serves both targets.
//
// Naming: kebab-case strings ("darwin-arm64", "linux-arm64") form the
// stable target identifier surface.

typedef struct Target {
    // Emit the assembly file's leading directives:
    //   .global _start
    //   .align 2
    //   .section <text-section>
    //
    // Symbol-name policy: the compiler emits raw assembly with literal
    // identifiers like `_yell_int`, `_alloc_String`, `_start`, etc.
    // Neither Mach-O `as` nor ELF `as` mangles assembler-source labels,
    // so those literal names ARE the on-disk symbols on both targets.
    // The `_` prefix is part of the symbol, not target-conditional.
    void (*emit_prologue)(FILE *out);

    // Emit a section switch to the active "text" section.
    // Mach-O (Darwin): __TEXT,__text
    // ELF   (Linux):   .text
    void (*emit_text_section)(FILE *out);

    // Emit a section switch to the active "data" section.
    // Mach-O (Darwin): __DATA,__data
    // ELF   (Linux):   .data
    void (*emit_data_section)(FILE *out);

    // Emit a section switch to the zero-initialised BSS section.
    // Mach-O (Darwin): __DATA,__bss
    // ELF   (Linux):   .bss
    void (*emit_bss_section)(FILE *out);

    // Emit a 2-instruction PC-relative address load that materializes
    // the address of `sym` into `xN`. The frontend assumes the second
    // instruction completes the materialization (no GOT indirection
    // needed for the symbols this compiler emits — they're all
    // file-local data emitted in the same translation unit). Both
    // backends produce a 2-instruction sequence:
    //
    //   Darwin: adrp xN, <sym>@PAGE
    //           add  xN, xN, <sym>@PAGEOFF
    //   Linux:  adrp xN, <sym>
    //           add  xN, xN, :lo12:<sym>
    //
    // Source for Linux form: AArch64 ELF ABI relocations
    // R_AARCH64_ADR_PREL_PG_HI21 / R_AARCH64_ADD_ABS_LO12_NC, surfaced
    // by GAS via `:lo12:` and bare-symbol `adrp`.
    void (*emit_addr_load)(FILE *out, int reg, const char *sym);

    // Emit a write(2) syscall to stdout. Convention: caller has
    // already placed buf in x1 and len in x2. The callback sets x0=1
    // (stdout) and emits the syscall.
    //
    //   Darwin AArch64: mov x16, #4 (SYS_write); svc #0x80.
    //   Linux  AArch64: mov x8,  #64 (__NR_write per
    //                   linux/include/uapi/asm-generic/unistd.h);
    //                   svc #0.
    void (*emit_sys_write_stdout)(FILE *out);

    // Emit an exit(2) syscall with the given exit code (constant int).
    //   Darwin AArch64: mov x0, #<code>; mov x16, #1 (SYS_exit);
    //                   svc #0x80.
    //   Linux  AArch64: mov x0, #<code>; mov x8, #93 (__NR_exit);
    //                   svc #0.
    // Source for __NR_exit: asm-generic/unistd.h.
    void (*emit_sys_exit)(FILE *out, int code);

    // Emit the full mmap-an-anonymous-64KB-page syscall sequence used
    // by `_heap_alloc`'s bump-region grow path. On entry: nothing
    // assumed in x0..x7. On exit: x0 = base of the new 64KB region.
    //
    // Both targets pass:
    //   addr = 0, len = 0x10000, prot = PROT_READ|PROT_WRITE (=3),
    //   fd = -1, offset = 0.
    // Differences:
    //   Darwin AArch64: SYS_mmap = 197;  flags = MAP_PRIVATE(0x0002)
    //                   | MAP_ANON(0x1000) = 0x1002.
    //                   x16 = 197, svc #0x80.
    //   Linux  AArch64: __NR_mmap = 222 (asm-generic/unistd.h);
    //                   flags = MAP_PRIVATE(0x02)
    //                   | MAP_ANONYMOUS(0x20) = 0x22.
    //                   x8 = 222, svc #0.
    // Source for Linux flag values: include/uapi/asm-generic/mman.h
    // (the AArch64 generic).
    void (*emit_sys_mmap_anon_64k)(FILE *out);

    // Emit the C-emitted runtime helpers + runtime data section.
    // Each target plugs in its own runtime_emit() variant; the variant
    // is responsible for using this target's sym_prefix, syscalls, and
    // section directives.
    void (*emit_runtime_builtins)(FILE *out);

    // Emit the program entry point.
    //   test_count == 0  -> default <prefix>start: bl <prefix>spark,
    //                       then exit(0).
    //   test_count >  0  -> test harness: walk every <prefix>test_<i>,
    //                       printing its name first, then exit(0).
    void (*emit_entry)(FILE *out, int test_count);

    // Emit per-test name data block. Called only when test_count > 0.
    // After this returns the assembler MUST be back in the text section
    // so `iremit_finalize_data` (which selects the data section on its
    // own) and any further emission don't trip over a stuck section.
    void (*emit_test_data)(FILE *out, int test_count,
                           const char *const *test_names);

    // Drive the assembler and linker: assemble `asm_path` into
    // `obj_path`, then link `obj_path` into `out_name`. Returns 0 on
    // success, non-zero on failure (with a diagnostic already printed
    // to stderr).
    int (*assemble_and_link)(const char *asm_path,
                             const char *obj_path,
                             const char *out_name);
} Target;

// Result of `target_by_name`. Two outcomes: the requested target
// resolved, or the name was not recognized.
typedef enum {
    TARGET_LOOKUP_OK,        // name resolved; *out is non-NULL
    TARGET_LOOKUP_UNKNOWN,   // name is not a recognized target
} TargetLookupResult;

// Returns the default target for the host this compiler was built on.
// Today: always darwin-arm64 (the only host supported).
const Target *target_default(void);

// Look up a target by its kebab-case name. Recognizes "darwin-arm64"
// and "linux-arm64"; anything else returns UNKNOWN.
//
// On OK, `*out_target` receives the backend pointer; on UNKNOWN it
// is left untouched.
TargetLookupResult target_by_name(const char *name, const Target **out_target);

#endif
