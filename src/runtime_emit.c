// C-emitted runtime helpers shared by every compiled .ptt program.
//
// Extracted from the now-retired src/codegen.c (#34 P4.3g). The IR
// pipeline (src/main.c) calls runtime_emit_builtins() once at the
// top of every compiled binary; the helpers themselves are written
// in raw ARM64 assembly via fprintf.
//
// Why C-emitted instead of pure Potato? Most of these helpers are
// hot enough that the regalloc-based hand-written assembly still
// beats what the IR backend produces today. Once the P5 optimization
// passes (inlining, SRA, escape analysis, BCE, LICM) land, P6 will
// rewrite the collection helpers in std/map.ptt / std/list.ptt and
// delete the C-emitted versions; the I/O, heap allocator, and panic
// handlers will likely stay here permanently because they're tightly
// coupled to syscall numbers and ABI conventions.

#include <stdio.h>
#include "runtime_emit.h"

static void emit_yell_int(FILE *out) {
    fprintf(out, "// built-in: _yell_int (signed)\n.globl _yell_int\n.p2align 2\n_yell_int:\n");
    fprintf(out, "    stp x29, x30, [sp, #-16]!\n    mov x29, sp\n    sub sp, sp, #64\n");
    fprintf(out, "    mov x2, x0\n    add x1, sp, #32\n    mov x3, #0\n");
    fprintf(out, "    cmp x2, #0\n    b.ge _yi_pos\n");
    fprintf(out, "    mov w4, #45\n    strb w4, [x1]\n    mov x3, #1\n");
    fprintf(out, "    neg x2, x2\n");
    fprintf(out, "_yi_pos:\n");
    fprintf(out, "    cmp x2, #0\n    b.ne _yi_loop\n");
    fprintf(out, "    mov w4, #48\n    strb w4, [x1, x3]\n    add x3, x3, #1\n    b _yi_write\n");
    fprintf(out, "_yi_loop:\n    cbz x2, _yi_reverse\n");
    fprintf(out, "    mov x4, #10\n    udiv x5, x2, x4\n    msub x6, x5, x4, x2\n");
    fprintf(out, "    add w6, w6, #48\n    strb w6, [x1, x3]\n    add x3, x3, #1\n    mov x2, x5\n    b _yi_loop\n");
    fprintf(out, "_yi_reverse:\n");
    fprintf(out, "    ldrb w7, [x1]\n    cmp w7, #45\n");
    fprintf(out, "    mov x4, #0\n    b.ne _yi_revstart\n    mov x4, #1\n");
    fprintf(out, "_yi_revstart:\n    sub x5, x3, #1\n");
    fprintf(out, "_yi_rev:\n    cmp x4, x5\n    b.ge _yi_write\n");
    fprintf(out, "    ldrb w6, [x1, x4]\n    ldrb w7, [x1, x5]\n    strb w7, [x1, x4]\n    strb w6, [x1, x5]\n");
    fprintf(out, "    add x4, x4, #1\n    sub x5, x5, #1\n    b _yi_rev\n");
    fprintf(out, "_yi_write:\n    mov w4, #10\n    strb w4, [x1, x3]\n    add x3, x3, #1\n");
    fprintf(out, "    mov x16, #4\n    mov x0, #1\n    mov x2, x3\n    svc #0x80\n");
    fprintf(out, "    mov sp, x29\n    ldp x29, x30, [sp], #16\n    ret\n\n");
}

static void emit_yell_str(FILE *out) {
    // β4: x0 is a String header (4 quads: cap, count, data,
    // owned) where data is an `array of byte` ptr. We pull
    // count from offset 8 and the byte buffer via TWO loads:
    // first the array of byte header from String.data
    // ([x0,#16]), then the byte ptr from that array's data
    // field ([arr,#8]). Then write count bytes + newline.
    fprintf(out, "// built-in: _yell_str (x0 = String header ptr)\n");
    fprintf(out, ".globl _yell_str\n.p2align 2\n_yell_str:\n");
    fprintf(out, "    stp x29, x30, [sp, #-16]!\n    mov x29, sp\n");
    fprintf(out, "    ldr x2, [x0, #8]\n");      // count -> x2
    fprintf(out, "    ldr x1, [x0, #16]\n");     // array-of-byte hdr -> x1
    fprintf(out, "    ldr x1, [x1, #8]\n");      // byte ptr -> x1
    fprintf(out, "    mov x0, #1\n");            // fd=stdout
    fprintf(out, "    mov x16, #4\n    svc #0x80\n");
    // Trailing newline.
    fprintf(out, "    sub sp, sp, #16\n    mov w4, #10\n    strb w4, [sp]\n");
    fprintf(out, "    mov x16, #4\n    mov x0, #1\n    mov x1, sp\n    mov x2, #1\n    svc #0x80\n");
    fprintf(out, "    add sp, sp, #16\n");
    fprintf(out, "    mov sp, x29\n    ldp x29, x30, [sp], #16\n    ret\n\n");
}

// P3.3a primitive: _write_bytes(x0=ptr, x1=len) -> void
// Writes `len` bytes starting at `ptr` to stdout via the macOS
// write(2) syscall. No null terminator scanning, no trailing
// newline — pure raw byte output. Used as the building block for
// pure-Potato String.yell once String becomes a stdlib struct.
static void emit_write_bytes(FILE *out) {
    fprintf(out, "// built-in: _write_bytes(x0=ptr, x1=len)\n");
    fprintf(out, ".globl _write_bytes\n.p2align 2\n_write_bytes:\n");
    fprintf(out, "    stp x29, x30, [sp, #-16]!\n    mov x29, sp\n");
    // syscall write(fd=1, buf=x0_in, len=x1_in): x16=4, x0=fd, x1=buf, x2=len.
    fprintf(out, "    mov x2, x1\n");        // len -> x2
    fprintf(out, "    mov x1, x0\n");        // ptr -> x1
    fprintf(out, "    mov x0, #1\n");        // fd=1 (stdout)
    fprintf(out, "    mov x16, #4\n");       // SYS_write on Darwin
    fprintf(out, "    svc #0x80\n");
    fprintf(out, "    mov sp, x29\n    ldp x29, x30, [sp], #16\n    ret\n\n");
}

static void emit_yell_dispatch(FILE *out) {
    fprintf(out, "// built-in: _yell (auto-dispatch int/str)\n");
    fprintf(out, ".globl _yell\n.p2align 2\n_yell:\n");
    fprintf(out, "    stp x29, x30, [sp, #-16]!\n    mov x29, sp\n");
    fprintf(out, "    mov x1, #0x100000\n");
    fprintf(out, "    cmp x0, x1\n");
    fprintf(out, "    b.ge _yell_is_str\n");
    fprintf(out, "    bl _yell_int\n");
    fprintf(out, "    b _yell_done\n");
    fprintf(out, "_yell_is_str:\n");
    fprintf(out, "    bl _yell_str\n");
    fprintf(out, "_yell_done:\n");
    fprintf(out, "    mov sp, x29\n    ldp x29, x30, [sp], #16\n    ret\n\n");
}

static void emit_task_builtins(FILE *out) {
    // In single-threaded compiled mode, task.fire(fn()) already calls fn,
    // so _task_fire and _task_collapse are no-ops.
    fprintf(out, ".globl _task_fire\n.p2align 2\n_task_fire:\n    ret\n\n");
    fprintf(out, ".globl _task_collapse\n.p2align 2\n_task_collapse:\n    ret\n\n");
}

static void emit_heap_alloc(FILE *out) {
    // Free list: singly-linked list of freed blocks.
    // Each free block: [next_ptr(8) | size(8) | unused space ...]
    // _heap_free_list points to the first free block (or 0).
    fprintf(out, "// built-in: _heap_free(ptr=x0, size=x1)\n");
    fprintf(out, ".globl _heap_free\n.p2align 2\n_heap_free:\n");
    fprintf(out, "    cbz x0, _hf_ret\n");
    fprintf(out, "    adrp x2, _heap_free_list@PAGE\n");
    fprintf(out, "    add x2, x2, _heap_free_list@PAGEOFF\n");
    fprintf(out, "    ldr x3, [x2]\n");
    fprintf(out, "    str x3, [x0]\n");
    fprintf(out, "    str x1, [x0, #8]\n");
    fprintf(out, "    str x0, [x2]\n");
    fprintf(out, "_hf_ret:\n    ret\n\n");

    fprintf(out, "// built-in: _heap_alloc(size in x0) -> ptr in x0\n");
    fprintf(out, ".globl _heap_alloc\n.p2align 2\n_heap_alloc:\n");
    fprintf(out, "    stp x29, x30, [sp, #-16]!\n    mov x29, sp\n");
    fprintf(out, "    add x0, x0, #15\n");
    fprintf(out, "    and x0, x0, #-16\n");
    fprintf(out, "    mov x9, x0\n");
    fprintf(out, "    adrp x10, _heap_free_list@PAGE\n");
    fprintf(out, "    add x10, x10, _heap_free_list@PAGEOFF\n");
    fprintf(out, "    ldr x11, [x10]\n");
    fprintf(out, "    cbz x11, _ha_bump\n");
    fprintf(out, "    mov x12, x10\n");
    fprintf(out, "_ha_search:\n");
    fprintf(out, "    cbz x11, _ha_bump\n");
    fprintf(out, "    ldr x13, [x11, #8]\n");
    fprintf(out, "    cmp x13, x9\n");
    fprintf(out, "    b.ge _ha_found\n");
    fprintf(out, "    mov x12, x11\n");
    fprintf(out, "    ldr x11, [x11]\n");
    fprintf(out, "    b _ha_search\n");
    fprintf(out, "_ha_found:\n");
    fprintf(out, "    ldr x14, [x11]\n");
    fprintf(out, "    str x14, [x12]\n");
    fprintf(out, "    mov x0, x11\n");
    fprintf(out, "    mov sp, x29\n    ldp x29, x30, [sp], #16\n    ret\n");
    fprintf(out, "_ha_bump:\n");
    fprintf(out, "    adrp x10, _heap_ptr@PAGE\n");
    fprintf(out, "    add x10, x10, _heap_ptr@PAGEOFF\n");
    fprintf(out, "    ldr x11, [x10]\n");
    fprintf(out, "    adrp x12, _heap_end@PAGE\n");
    fprintf(out, "    add x12, x12, _heap_end@PAGEOFF\n");
    fprintf(out, "    ldr x13, [x12]\n");
    fprintf(out, "    add x14, x11, x9\n");
    fprintf(out, "    cmp x14, x13\n");
    fprintf(out, "    b.le _ha_ok\n");
    fprintf(out, "    mov x0, #0\n");
    fprintf(out, "    mov x1, #0x10000\n");
    fprintf(out, "    mov x2, #3\n");
    fprintf(out, "    mov x3, #0x1002\n");
    fprintf(out, "    mov x4, #-1\n");
    fprintf(out, "    mov x5, #0\n");
    fprintf(out, "    mov x16, #197\n");
    fprintf(out, "    svc #0x80\n");
    fprintf(out, "    mov x11, x0\n");
    fprintf(out, "    add x13, x0, #0x10000\n");
    fprintf(out, "    add x14, x11, x9\n");
    fprintf(out, "_ha_ok:\n");
    fprintf(out, "    mov x0, x11\n");
    fprintf(out, "    str x14, [x10]\n");
    fprintf(out, "    str x13, [x12]\n");
    fprintf(out, "    mov sp, x29\n    ldp x29, x30, [sp], #16\n    ret\n\n");
}

static void emit_str_eq(FILE *out) {
    // β4: x0, x1 are String header ptrs. data field is an
    // `array of byte` (16-byte header), so two-step load to
    // get the byte ptr. Then walk count bytes.
    fprintf(out, "// built-in: _str_eq(x0=String, x1=String) -> bool int\n");
    fprintf(out, ".globl _str_eq\n.p2align 2\n_str_eq:\n");
    fprintf(out, "    ldr x4, [x0, #8]\n");      // count_a
    fprintf(out, "    ldr x5, [x1, #8]\n");      // count_b
    fprintf(out, "    cmp x4, x5\n");
    fprintf(out, "    b.ne _se_no\n");
    fprintf(out, "    ldr x0, [x0, #16]\n");     // arr_a hdr
    fprintf(out, "    ldr x0, [x0, #8]\n");      // bytes_a
    fprintf(out, "    ldr x1, [x1, #16]\n");     // arr_b hdr
    fprintf(out, "    ldr x1, [x1, #8]\n");      // bytes_b
    fprintf(out, "    mov x6, #0\n");            // i = 0
    fprintf(out, "_se_loop:\n");
    fprintf(out, "    cmp x6, x4\n");
    fprintf(out, "    b.ge _se_yes\n");
    fprintf(out, "    ldrb w2, [x0, x6]\n");
    fprintf(out, "    ldrb w3, [x1, x6]\n");
    fprintf(out, "    cmp w2, w3\n");
    fprintf(out, "    b.ne _se_no\n");
    fprintf(out, "    add x6, x6, #1\n");
    fprintf(out, "    b _se_loop\n");
    fprintf(out, "_se_yes:\n    mov x0, #1\n    ret\n");
    fprintf(out, "_se_no:\n    mov x0, #0\n    ret\n\n");
}

static void emit_str_concat(FILE *out) {
    // β4: inputs are String headers; data field is `array of byte`
    // (16-byte header). Output is a freshly allocated String
    // (32 bytes) whose data is a freshly allocated array-of-byte
    // header (16 bytes) pointing at freshly allocated bytes
    // holding s1.bytes ++ s2.bytes.
    //
    // x19 = s1 header, x20 = s2 header,
    // x21 = count_a, x22 = count_b,
    // x23 = bytes_a,  x24 = bytes_b,
    // x25 = result bytes ptr, x26 = result String header,
    // x27 = result array-of-byte header.
    fprintf(out, "// built-in: _str_concat(x0=String, x1=String) -> new String\n");
    fprintf(out, ".globl _str_concat\n.p2align 2\n_str_concat:\n");
    fprintf(out, "    stp x29, x30, [sp, #-16]!\n    mov x29, sp\n");
    fprintf(out, "    stp x19, x20, [sp, #-16]!\n");
    fprintf(out, "    stp x21, x22, [sp, #-16]!\n");
    fprintf(out, "    stp x23, x24, [sp, #-16]!\n");
    fprintf(out, "    stp x25, x26, [sp, #-16]!\n");
    fprintf(out, "    str x27, [sp, #-16]!\n");
    fprintf(out, "    mov x19, x0\n    mov x20, x1\n");
    fprintf(out, "    ldr x21, [x19, #8]\n");      // count_a
    fprintf(out, "    ldr x22, [x20, #8]\n");      // count_b
    fprintf(out, "    ldr x23, [x19, #16]\n");     // arr_a hdr
    fprintf(out, "    ldr x23, [x23, #8]\n");      // bytes_a
    fprintf(out, "    ldr x24, [x20, #16]\n");     // arr_b hdr
    fprintf(out, "    ldr x24, [x24, #8]\n");      // bytes_b
    // Allocate count_a + count_b + 1 bytes for the joined data.
    fprintf(out, "    add x0, x21, x22\n    add x0, x0, #1\n    bl _heap_alloc\n");
    fprintf(out, "    mov x25, x0\n");
    // Copy s1 bytes.
    fprintf(out, "    mov x4, #0\n");
    fprintf(out, "_sc_c1:\n    cmp x4, x21\n    b.ge _sc_c2s\n    ldrb w5, [x23, x4]\n    strb w5, [x25, x4]\n    add x4, x4, #1\n    b _sc_c1\n");
    // Copy s2 bytes after s1.
    fprintf(out, "_sc_c2s:\n    mov x5, #0\n");
    fprintf(out, "_sc_c2:\n    cmp x5, x22\n    b.ge _sc_end\n    ldrb w6, [x24, x5]\n    add x7, x21, x5\n    strb w6, [x25, x7]\n    add x5, x5, #1\n    b _sc_c2\n");
    fprintf(out, "_sc_end:\n    add x7, x21, x22\n    strb wzr, [x25, x7]\n");
    // Allocate array-of-byte header (16 bytes) for the result data.
    fprintf(out, "    mov x0, #16\n    bl _heap_alloc\n");
    fprintf(out, "    mov x27, x0\n");
    fprintf(out, "    add x0, x21, x22\n");        // total
    fprintf(out, "    str x0, [x27, #0]\n");       // arr.cap = total
    fprintf(out, "    str x25, [x27, #8]\n");      // arr.data = bytes
    // Allocate String header (32 bytes).
    fprintf(out, "    mov x0, #32\n    bl _heap_alloc\n");
    fprintf(out, "    mov x26, x0\n");
    fprintf(out, "    add x0, x21, x22\n");
    fprintf(out, "    str x0, [x26, #0]\n");       // String.cap
    fprintf(out, "    str x0, [x26, #8]\n");       // String.count
    fprintf(out, "    str x27, [x26, #16]\n");     // String.data = arr hdr
    fprintf(out, "    mov x0, #1\n");
    fprintf(out, "    str x0, [x26, #24]\n");      // String.owned = 1
    fprintf(out, "    mov x0, x26\n");
    fprintf(out, "    ldr x27, [sp], #16\n");
    fprintf(out, "    ldp x25, x26, [sp], #16\n");
    fprintf(out, "    ldp x23, x24, [sp], #16\n");
    fprintf(out, "    ldp x21, x22, [sp], #16\n");
    fprintf(out, "    ldp x19, x20, [sp], #16\n");
    fprintf(out, "    mov sp, x29\n    ldp x29, x30, [sp], #16\n    ret\n\n");
}

static void emit_int_to_str(FILE *out) {
    // P3.4: returns a `String` header (32 bytes) whose data points at
    // freshly heap-allocated bytes (NUL-terminated; owned=1).
    //
    // x19 = scratch buffer ptr (sp-based, 32 bytes), x20 = digit count,
    // x25 = data ptr (heap-allocated), x26 = String header ptr (heap).
    // Saving x19/x20/x25/x26 in the prologue because we make multiple
    // `bl _heap_alloc` calls and the IR backend's regalloc relies on
    // callee-save semantics being honoured.
    //
    // Negative numbers: prefix '-' then format the positive magnitude.
    fprintf(out, "// built-in: _int_to_str(x0) -> String header\n");
    fprintf(out, ".globl _int_to_str\n.p2align 2\n_int_to_str:\n");
    fprintf(out, "    stp x29, x30, [sp, #-16]!\n    mov x29, sp\n");
    fprintf(out, "    stp x19, x20, [sp, #-16]!\n");
    fprintf(out, "    stp x25, x26, [sp, #-16]!\n");
    fprintf(out, "    sub sp, sp, #32\n");
    // x2 = work value, x1 = scratch buf (sp-relative), x3 = digit count,
    // x9 = sign-prefix flag (0 = positive, 1 = leading '-').
    fprintf(out, "    mov x2, x0\n    add x1, sp, #0\n    mov x3, #0\n    mov x9, #0\n");
    fprintf(out, "    cmp x2, #0\n    b.ge _its_pos\n");
    fprintf(out, "    mov w4, #45\n    strb w4, [x1]\n");
    fprintf(out, "    mov x3, #1\n    mov x9, #1\n");
    fprintf(out, "    neg x2, x2\n");
    fprintf(out, "_its_pos:\n");
    fprintf(out, "    cmp x2, #0\n    b.ne _its_loop\n");
    fprintf(out, "    mov w4, #48\n    strb w4, [x1, x3]\n    add x3, x3, #1\n    b _its_alloc\n");
    fprintf(out, "_its_loop:\n    cbz x2, _its_rev\n");
    fprintf(out, "    mov x4, #10\n    udiv x5, x2, x4\n    msub x6, x5, x4, x2\n");
    fprintf(out, "    add w6, w6, #48\n    strb w6, [x1, x3]\n    add x3, x3, #1\n    mov x2, x5\n    b _its_loop\n");
    // Reverse the digits we just wrote (positions [x9, x3)). x9 is
    // the start (1 if leading '-', 0 otherwise); x3 is the past-end.
    fprintf(out, "_its_rev:\n    mov x4, x9\n    sub x5, x3, #1\n");
    fprintf(out, "_its_rv:\n    cmp x4, x5\n    b.ge _its_alloc\n");
    fprintf(out, "    ldrb w6, [x1, x4]\n    ldrb w7, [x1, x5]\n    strb w7, [x1, x4]\n    strb w6, [x1, x5]\n");
    fprintf(out, "    add x4, x4, #1\n    sub x5, x5, #1\n    b _its_rv\n");
    fprintf(out, "_its_alloc:\n    strb wzr, [x1, x3]\n");
    // x19 = scratch ptr, x20 = digit count.
    fprintf(out, "    mov x19, x1\n    mov x20, x3\n");
    // Allocate heap bytes (count + 1 for NUL).
    fprintf(out, "    add x0, x3, #1\n    bl _heap_alloc\n");
    fprintf(out, "    mov x25, x0\n");
    // Copy scratch -> heap.
    fprintf(out, "    mov x4, #0\n");
    fprintf(out, "_its_cp:\n    ldrb w5, [x19, x4]\n    strb w5, [x25, x4]\n    add x4, x4, #1\n    cmp x4, x20\n    b.le _its_cp\n");
    // β4: wrap heap bytes in an array-of-byte header (16 bytes),
    // then build the String header (32 bytes) referencing it.
    // x21 = result array-of-byte hdr.
    fprintf(out, "    str x21, [sp, #-16]!\n");
    fprintf(out, "    mov x0, #16\n    bl _heap_alloc\n");
    fprintf(out, "    mov x21, x0\n");
    fprintf(out, "    str x20, [x21, #0]\n");      // arr.cap = digit_count
    fprintf(out, "    str x25, [x21, #8]\n");      // arr.data = bytes
    fprintf(out, "    mov x0, #32\n    bl _heap_alloc\n");
    fprintf(out, "    mov x26, x0\n");
    fprintf(out, "    str x20, [x26, #0]\n");      // String.cap
    fprintf(out, "    str x20, [x26, #8]\n");      // String.count
    fprintf(out, "    str x21, [x26, #16]\n");     // String.data = arr hdr
    fprintf(out, "    mov x0, #1\n");
    fprintf(out, "    str x0, [x26, #24]\n");      // String.owned = 1
    fprintf(out, "    mov x0, x26\n");
    // Unwind in reverse-push order: pop x21 first (pushed last), then
    // free the 32-byte scratch buffer, then pop x25/x26 / x19/x20 /
    // x29/x30. The previous order ran `add sp, sp, #32` BEFORE popping
    // x21, which read garbage from the scratch buffer into x21 and
    // returned with x21 trashed in the caller — corrupting any loop
    // variable held in x21 across `bl _int_to_str`.
    fprintf(out, "    ldr x21, [sp], #16\n");
    fprintf(out, "    add sp, sp, #32\n");
    fprintf(out, "    ldp x25, x26, [sp], #16\n");
    fprintf(out, "    ldp x19, x20, [sp], #16\n");
    fprintf(out, "    ldp x29, x30, [sp], #16\n    ret\n\n");
}

static void emit_str_builtins(FILE *out) {
    // β4: _str_len reads count from the String header at offset 8.
    fprintf(out, "// built-in: _str_len(x0=String) -> int\n");
    fprintf(out, ".globl _str_len\n.p2align 2\n_str_len:\n");
    fprintf(out, "    ldr x0, [x0, #8]\n");
    fprintf(out, "    ret\n\n");

    // _char_at(x0=String header, x1=index) -> new 1-char String.
    // β4: source data field is array of byte (16-byte hdr); load
    // bytes ptr via two-step. Result: allocate bytes (2), array
    // of byte hdr (16), and String hdr (32).
    fprintf(out, "// built-in: _char_at(x0=String, x1=int) -> String\n");
    fprintf(out, ".globl _char_at\n.p2align 2\n_char_at:\n");
    fprintf(out, "    stp x29, x30, [sp, #-16]!\n    mov x29, sp\n");
    fprintf(out, "    stp x19, x20, [sp, #-16]!\n");
    fprintf(out, "    stp x25, x26, [sp, #-16]!\n");
    fprintf(out, "    str x27, [sp, #-16]!\n");
    fprintf(out, "    ldr x19, [x0, #16]\n");      // arr hdr
    fprintf(out, "    ldr x19, [x19, #8]\n");      // src bytes ptr
    fprintf(out, "    mov x20, x1\n");             // index
    fprintf(out, "    mov x0, #2\n    bl _heap_alloc\n");
    fprintf(out, "    mov x25, x0\n");
    fprintf(out, "    ldrb w2, [x19, x20]\n");
    fprintf(out, "    strb w2, [x25, #0]\n");
    fprintf(out, "    strb wzr, [x25, #1]\n");
    // array-of-byte header for result.
    fprintf(out, "    mov x0, #16\n    bl _heap_alloc\n");
    fprintf(out, "    mov x27, x0\n");
    fprintf(out, "    mov x4, #1\n");
    fprintf(out, "    str x4, [x27, #0]\n");       // arr.cap
    fprintf(out, "    str x25, [x27, #8]\n");      // arr.data
    // String header.
    fprintf(out, "    mov x0, #32\n    bl _heap_alloc\n");
    fprintf(out, "    mov x26, x0\n");
    fprintf(out, "    mov x4, #1\n");
    fprintf(out, "    str x4, [x26, #0]\n");
    fprintf(out, "    str x4, [x26, #8]\n");
    fprintf(out, "    str x27, [x26, #16]\n");     // data = arr hdr
    fprintf(out, "    str x4, [x26, #24]\n");      // owned = 1
    fprintf(out, "    mov x0, x26\n");
    fprintf(out, "    ldr x27, [sp], #16\n");
    fprintf(out, "    ldp x25, x26, [sp], #16\n");
    fprintf(out, "    ldp x19, x20, [sp], #16\n");
    fprintf(out, "    mov sp, x29\n    ldp x29, x30, [sp], #16\n    ret\n\n");
}

static void emit_map_builtins(FILE *out) {
    // Map layout: header [capacity | count | data_ptr], data is
    // [k0|v0|k1|v1|...] (16 bytes per entry). On insert when full:
    // alloc 2x, copy, free old.
    fprintf(out, "// built-in: _map_new() -> map header ptr\n");
    fprintf(out, ".globl _map_new\n.p2align 2\n_map_new:\n");
    fprintf(out, "    stp x29, x30, [sp, #-16]!\n    mov x29, sp\n");
    fprintf(out, "    mov x0, #24\n    bl _heap_alloc\n");
    fprintf(out, "    mov x19, x0\n    str x19, [sp, #-16]!\n");
    fprintf(out, "    mov x0, #128\n    bl _heap_alloc\n");
    fprintf(out, "    ldr x19, [sp], #16\n");
    fprintf(out, "    mov x1, #8\n");
    fprintf(out, "    str x1, [x19]\n");
    fprintf(out, "    str xzr, [x19, #8]\n");
    fprintf(out, "    str x0, [x19, #16]\n");
    fprintf(out, "    mov x0, x19\n");
    fprintf(out, "    mov sp, x29\n    ldp x29, x30, [sp], #16\n    ret\n\n");

    fprintf(out, "// built-in: _map_set(x0=map, x1=key, x2=value)\n");
    fprintf(out, ".globl _map_set\n.p2align 2\n_map_set:\n");
    fprintf(out, "    stp x29, x30, [sp, #-16]!\n    mov x29, sp\n");
    fprintf(out, "    stp x19, x20, [sp, #-16]!\n");
    fprintf(out, "    stp x21, x22, [sp, #-16]!\n");
    fprintf(out, "    stp x23, x24, [sp, #-16]!\n");
    fprintf(out, "    mov x19, x0\n");
    fprintf(out, "    mov x20, x1\n");
    fprintf(out, "    mov x21, x2\n");
    fprintf(out, "    ldr x22, [x19, #8]\n");
    fprintf(out, "    ldr x23, [x19, #16]\n");
    fprintf(out, "    mov x3, #0\n");
    fprintf(out, "_ms_scan:\n");
    fprintf(out, "    cmp x3, x22\n");
    fprintf(out, "    b.ge _ms_insert\n");
    fprintf(out, "    lsl x4, x3, #4\n");
    fprintf(out, "    ldr x0, [x23, x4]\n");
    fprintf(out, "    mov x1, x20\n");
    fprintf(out, "    stp x3, x4, [sp, #-16]!\n");
    fprintf(out, "    bl _str_eq\n");
    fprintf(out, "    ldp x3, x4, [sp], #16\n");
    fprintf(out, "    cbnz x0, _ms_update\n");
    fprintf(out, "    add x3, x3, #1\n");
    fprintf(out, "    b _ms_scan\n");
    fprintf(out, "_ms_update:\n");
    fprintf(out, "    lsl x4, x3, #4\n");
    fprintf(out, "    add x4, x4, #8\n");
    fprintf(out, "    str x21, [x23, x4]\n");
    fprintf(out, "    b _ms_done\n");
    fprintf(out, "_ms_insert:\n");
    fprintf(out, "    ldr x24, [x19]\n");
    fprintf(out, "    cmp x22, x24\n");
    fprintf(out, "    b.lt _ms_do_insert\n");
    fprintf(out, "    lsl x24, x24, #1\n");
    fprintf(out, "    str x24, [x19]\n");
    fprintf(out, "    lsl x0, x24, #4\n");
    fprintf(out, "    bl _heap_alloc\n");
    fprintf(out, "    mov x5, x0\n");
    fprintf(out, "    mov x6, #0\n");
    fprintf(out, "    lsl x7, x22, #4\n");
    fprintf(out, "_ms_copy:\n");
    fprintf(out, "    cmp x6, x7\n");
    fprintf(out, "    b.ge _ms_copied\n");
    fprintf(out, "    ldr x8, [x23, x6]\n");
    fprintf(out, "    str x8, [x5, x6]\n");
    fprintf(out, "    add x6, x6, #8\n");
    fprintf(out, "    b _ms_copy\n");
    fprintf(out, "_ms_copied:\n");
    fprintf(out, "    mov x0, x23\n");
    fprintf(out, "    lsl x1, x22, #4\n");
    fprintf(out, "    bl _heap_free\n");
    fprintf(out, "    mov x23, x5\n");
    fprintf(out, "    str x23, [x19, #16]\n");
    fprintf(out, "_ms_do_insert:\n");
    fprintf(out, "    lsl x4, x22, #4\n");
    fprintf(out, "    str x20, [x23, x4]\n");
    fprintf(out, "    add x4, x4, #8\n");
    fprintf(out, "    str x21, [x23, x4]\n");
    fprintf(out, "    add x22, x22, #1\n");
    fprintf(out, "    str x22, [x19, #8]\n");
    fprintf(out, "_ms_done:\n");
    fprintf(out, "    ldp x23, x24, [sp], #16\n");
    fprintf(out, "    ldp x21, x22, [sp], #16\n");
    fprintf(out, "    ldp x19, x20, [sp], #16\n");
    fprintf(out, "    mov sp, x29\n    ldp x29, x30, [sp], #16\n    ret\n\n");

    fprintf(out, "// built-in: _map_get(x0=map, x1=key) -> value in x0\n");
    fprintf(out, ".globl _map_get\n.p2align 2\n_map_get:\n");
    fprintf(out, "    stp x29, x30, [sp, #-16]!\n    mov x29, sp\n");
    fprintf(out, "    stp x19, x20, [sp, #-16]!\n");
    fprintf(out, "    mov x19, x0\n");
    fprintf(out, "    mov x20, x1\n");
    fprintf(out, "    ldr x5, [x19, #8]\n");
    fprintf(out, "    ldr x6, [x19, #16]\n");
    fprintf(out, "    mov x3, #0\n");
    fprintf(out, "_mg_scan:\n");
    fprintf(out, "    cmp x3, x5\n");
    fprintf(out, "    b.ge _mg_notfound\n");
    fprintf(out, "    lsl x4, x3, #4\n");
    fprintf(out, "    ldr x0, [x6, x4]\n");
    fprintf(out, "    mov x1, x20\n");
    fprintf(out, "    stp x3, x5, [sp, #-16]!\n");
    fprintf(out, "    str x6, [sp, #-16]!\n");
    fprintf(out, "    bl _str_eq\n");
    fprintf(out, "    ldr x6, [sp], #16\n");
    fprintf(out, "    ldp x3, x5, [sp], #16\n");
    fprintf(out, "    cbnz x0, _mg_found\n");
    fprintf(out, "    add x3, x3, #1\n");
    fprintf(out, "    b _mg_scan\n");
    fprintf(out, "_mg_found:\n");
    fprintf(out, "    lsl x4, x3, #4\n");
    fprintf(out, "    add x4, x4, #8\n");
    fprintf(out, "    ldr x0, [x6, x4]\n");
    fprintf(out, "    b _mg_done\n");
    fprintf(out, "_mg_notfound:\n");
    fprintf(out, "    mov x0, #0\n");
    fprintf(out, "_mg_done:\n");
    fprintf(out, "    ldp x19, x20, [sp], #16\n");
    fprintf(out, "    mov sp, x29\n    ldp x29, x30, [sp], #16\n    ret\n\n");

    fprintf(out, ".globl _map_len\n.p2align 2\n_map_len:\n");
    fprintf(out, "    ldr x0, [x0, #8]\n    ret\n\n");

    fprintf(out, "// built-in: _map_keys(x0=map) -> list ptr\n");
    fprintf(out, ".globl _map_keys\n.p2align 2\n_map_keys:\n");
    fprintf(out, "    stp x29, x30, [sp, #-16]!\n    mov x29, sp\n");
    fprintf(out, "    stp x19, x20, [sp, #-16]!\n");
    fprintf(out, "    stp x21, x22, [sp, #-16]!\n");
    // ABI fix: this helper uses x23/x24 internally too. Earlier
    // versions clobbered them without saving — invisible to the direct
    // codegen (which never put values in x23/x24 across calls) but
    // catastrophic for the IR backend's call-aware regalloc, which
    // does. Save and restore them like x19..x22.
    fprintf(out, "    stp x23, x24, [sp, #-16]!\n");
    fprintf(out, "    mov x19, x0\n");
    fprintf(out, "    ldr x20, [x19, #8]\n");
    fprintf(out, "    ldr x24, [x19, #16]\n");
    fprintf(out, "    mov x0, #24\n    bl _heap_alloc\n");
    fprintf(out, "    mov x21, x0\n");
    fprintf(out, "    lsl x0, x20, #3\n");
    fprintf(out, "    cmp x0, #16\n    b.ge _mk_alloc\n");
    fprintf(out, "    mov x0, #16\n");
    fprintf(out, "_mk_alloc:\n");
    fprintf(out, "    bl _heap_alloc\n");
    fprintf(out, "    mov x22, x0\n");
    fprintf(out, "    str x20, [x21]\n");
    fprintf(out, "    str x20, [x21, #8]\n");
    fprintf(out, "    str x22, [x21, #16]\n");
    fprintf(out, "    ldr x24, [x19, #16]\n");
    fprintf(out, "    mov x3, #0\n");
    fprintf(out, "_mk_loop:\n");
    fprintf(out, "    cmp x3, x20\n");
    fprintf(out, "    b.ge _mk_done\n");
    fprintf(out, "    lsl x4, x3, #4\n");
    fprintf(out, "    ldr x5, [x24, x4]\n");
    fprintf(out, "    str x5, [x22, x3, lsl #3]\n");
    fprintf(out, "    add x3, x3, #1\n");
    fprintf(out, "    b _mk_loop\n");
    fprintf(out, "_mk_done:\n");
    fprintf(out, "    mov x0, x21\n");
    fprintf(out, "    ldp x23, x24, [sp], #16\n");
    fprintf(out, "    ldp x21, x22, [sp], #16\n");
    fprintf(out, "    ldp x19, x20, [sp], #16\n");
    fprintf(out, "    mov sp, x29\n    ldp x29, x30, [sp], #16\n    ret\n\n");
}

static void emit_imap_builtins(FILE *out) {
    // imap: same layout as map, but uses integer key comparison
    // instead of _str_eq.
    fprintf(out, ".globl _imap_new\n.p2align 2\n_imap_new:\n");
    fprintf(out, "    stp x29, x30, [sp, #-16]!\n    mov x29, sp\n");
    fprintf(out, "    mov x0, #24\n    bl _heap_alloc\n");
    fprintf(out, "    mov x19, x0\n    str x19, [sp, #-16]!\n");
    fprintf(out, "    mov x0, #128\n    bl _heap_alloc\n");
    fprintf(out, "    ldr x19, [sp], #16\n");
    fprintf(out, "    mov x1, #8\n    str x1, [x19]\n");
    fprintf(out, "    str xzr, [x19, #8]\n    str x0, [x19, #16]\n");
    fprintf(out, "    mov x0, x19\n");
    fprintf(out, "    mov sp, x29\n    ldp x29, x30, [sp], #16\n    ret\n\n");

    fprintf(out, ".globl _imap_set\n.p2align 2\n_imap_set:\n");
    fprintf(out, "    stp x29, x30, [sp, #-16]!\n    mov x29, sp\n");
    fprintf(out, "    stp x19, x20, [sp, #-16]!\n");
    fprintf(out, "    stp x21, x22, [sp, #-16]!\n");
    fprintf(out, "    stp x23, x24, [sp, #-16]!\n");
    fprintf(out, "    mov x19, x0\n    mov x20, x1\n    mov x21, x2\n");
    fprintf(out, "    ldr x22, [x19, #8]\n    ldr x23, [x19, #16]\n");
    fprintf(out, "    mov x3, #0\n");
    fprintf(out, "_ims_scan:\n    cmp x3, x22\n    b.ge _ims_insert\n");
    fprintf(out, "    lsl x4, x3, #4\n    ldr x5, [x23, x4]\n");
    fprintf(out, "    cmp x5, x20\n    b.eq _ims_update\n");
    fprintf(out, "    add x3, x3, #1\n    b _ims_scan\n");
    fprintf(out, "_ims_update:\n    lsl x4, x3, #4\n    add x4, x4, #8\n");
    fprintf(out, "    str x21, [x23, x4]\n    b _ims_done\n");
    fprintf(out, "_ims_insert:\n    ldr x24, [x19]\n    cmp x22, x24\n    b.lt _ims_do_ins\n");
    fprintf(out, "    lsl x24, x24, #1\n    str x24, [x19]\n");
    fprintf(out, "    lsl x0, x24, #4\n    bl _heap_alloc\n    mov x5, x0\n");
    fprintf(out, "    mov x6, #0\n    lsl x7, x22, #4\n");
    fprintf(out, "_ims_cp:\n    cmp x6, x7\n    b.ge _ims_cpd\n");
    fprintf(out, "    ldr x8, [x23, x6]\n    str x8, [x5, x6]\n    add x6, x6, #8\n    b _ims_cp\n");
    fprintf(out, "_ims_cpd:\n    mov x0, x23\n    lsl x1, x22, #4\n    bl _heap_free\n");
    fprintf(out, "    mov x23, x5\n    str x23, [x19, #16]\n");
    fprintf(out, "_ims_do_ins:\n    lsl x4, x22, #4\n");
    fprintf(out, "    str x20, [x23, x4]\n    add x4, x4, #8\n    str x21, [x23, x4]\n");
    fprintf(out, "    add x22, x22, #1\n    str x22, [x19, #8]\n");
    fprintf(out, "_ims_done:\n    ldp x23, x24, [sp], #16\n    ldp x21, x22, [sp], #16\n");
    fprintf(out, "    ldp x19, x20, [sp], #16\n");
    fprintf(out, "    mov sp, x29\n    ldp x29, x30, [sp], #16\n    ret\n\n");

    fprintf(out, ".globl _imap_get\n.p2align 2\n_imap_get:\n");
    fprintf(out, "    ldr x2, [x0, #8]\n    ldr x3, [x0, #16]\n    mov x4, #0\n");
    fprintf(out, "_img_scan:\n    cmp x4, x2\n    b.ge _img_nf\n");
    fprintf(out, "    lsl x5, x4, #4\n    ldr x6, [x3, x5]\n");
    fprintf(out, "    cmp x6, x1\n    b.eq _img_found\n");
    fprintf(out, "    add x4, x4, #1\n    b _img_scan\n");
    fprintf(out, "_img_found:\n    lsl x5, x4, #4\n    add x5, x5, #8\n    ldr x0, [x3, x5]\n    ret\n");
    fprintf(out, "_img_nf:\n    mov x0, #0\n    ret\n\n");

    fprintf(out, ".globl _imap_len\n.p2align 2\n_imap_len:\n");
    fprintf(out, "    ldr x0, [x0, #8]\n    ret\n\n");
}

static void emit_list_builtins(FILE *out) {
    // List layout: header [capacity | count | data_ptr], data is a
    // flat 8-byte-element array. On push when full: alloc 2x, copy,
    // free old, update data_ptr.
    fprintf(out, "// built-in: _list_new() -> list header ptr\n");
    fprintf(out, ".globl _list_new\n.p2align 2\n_list_new:\n");
    fprintf(out, "    stp x29, x30, [sp, #-16]!\n    mov x29, sp\n");
    fprintf(out, "    mov x0, #24\n    bl _heap_alloc\n");
    fprintf(out, "    mov x19, x0\n");
    fprintf(out, "    str x19, [sp, #-16]!\n");
    fprintf(out, "    mov x0, #64\n    bl _heap_alloc\n");
    fprintf(out, "    ldr x19, [sp], #16\n");
    fprintf(out, "    mov x1, #8\n");
    fprintf(out, "    str x1, [x19]\n");
    fprintf(out, "    str xzr, [x19, #8]\n");
    fprintf(out, "    str x0, [x19, #16]\n");
    fprintf(out, "    mov x0, x19\n");
    fprintf(out, "    mov sp, x29\n    ldp x29, x30, [sp], #16\n    ret\n\n");

    fprintf(out, "// built-in: _list_push(x0=list, x1=value)\n");
    fprintf(out, ".globl _list_push\n.p2align 2\n_list_push:\n");
    fprintf(out, "    stp x29, x30, [sp, #-16]!\n    mov x29, sp\n");
    fprintf(out, "    stp x19, x20, [sp, #-16]!\n");
    fprintf(out, "    mov x19, x0\n");
    fprintf(out, "    mov x20, x1\n");
    fprintf(out, "    ldr x2, [x19]\n");
    fprintf(out, "    ldr x3, [x19, #8]\n");
    fprintf(out, "    cmp x3, x2\n");
    fprintf(out, "    b.lt _lp_store\n");
    fprintf(out, "    lsl x4, x2, #1\n");
    fprintf(out, "    str x4, [x19]\n");
    fprintf(out, "    lsl x0, x4, #3\n");
    fprintf(out, "    str x3, [sp, #-16]!\n");
    fprintf(out, "    bl _heap_alloc\n");
    fprintf(out, "    ldr x3, [sp], #16\n");
    fprintf(out, "    mov x5, x0\n");
    fprintf(out, "    ldr x6, [x19, #16]\n");
    fprintf(out, "    mov x7, #0\n");
    fprintf(out, "_lp_copy:\n");
    fprintf(out, "    cmp x7, x3\n");
    fprintf(out, "    b.ge _lp_copied\n");
    fprintf(out, "    ldr x8, [x6, x7, lsl #3]\n");
    fprintf(out, "    str x8, [x5, x7, lsl #3]\n");
    fprintf(out, "    add x7, x7, #1\n");
    fprintf(out, "    b _lp_copy\n");
    fprintf(out, "_lp_copied:\n");
    fprintf(out, "    mov x0, x6\n");
    fprintf(out, "    lsl x1, x3, #3\n");
    fprintf(out, "    bl _heap_free\n");
    fprintf(out, "    str x5, [x19, #16]\n");
    fprintf(out, "    ldr x3, [x19, #8]\n");
    fprintf(out, "    mov x0, x5\n");
    fprintf(out, "    b _lp_do_store\n");
    fprintf(out, "_lp_store:\n");
    fprintf(out, "    ldr x0, [x19, #16]\n");
    fprintf(out, "_lp_do_store:\n");
    fprintf(out, "    str x20, [x0, x3, lsl #3]\n");
    fprintf(out, "    add x3, x3, #1\n");
    fprintf(out, "    str x3, [x19, #8]\n");
    fprintf(out, "    ldp x19, x20, [sp], #16\n");
    fprintf(out, "    mov sp, x29\n    ldp x29, x30, [sp], #16\n    ret\n\n");

    fprintf(out, "// built-in: _list_pop(x0=list) -> value\n");
    fprintf(out, ".globl _list_pop\n.p2align 2\n_list_pop:\n");
    fprintf(out, "    ldr x1, [x0, #8]\n");
    fprintf(out, "    cbz x1, _lpop_empty\n");
    fprintf(out, "    sub x1, x1, #1\n");
    fprintf(out, "    str x1, [x0, #8]\n");
    fprintf(out, "    ldr x2, [x0, #16]\n");
    fprintf(out, "    ldr x0, [x2, x1, lsl #3]\n");
    fprintf(out, "    ret\n");
    fprintf(out, "_lpop_empty:\n    mov x0, #0\n    ret\n\n");

    fprintf(out, "// built-in: _list_len(x0=list) -> count\n");
    fprintf(out, ".globl _list_len\n.p2align 2\n_list_len:\n");
    fprintf(out, "    ldr x0, [x0, #8]\n    ret\n\n");

    fprintf(out, "// built-in: _list_set(x0=list, x1=index, x2=value)\n");
    fprintf(out, ".globl _list_set\n.p2align 2\n_list_set:\n");
    fprintf(out, "    ldr x4, [x0, #8]\n");
    fprintf(out, "    cmp x1, x4\n");
    fprintf(out, "    b.ge _panic_oob\n");
    fprintf(out, "    cmp x1, #0\n");
    fprintf(out, "    b.lt _panic_oob\n");
    fprintf(out, "    ldr x3, [x0, #16]\n");
    fprintf(out, "    str x2, [x3, x1, lsl #3]\n");
    fprintf(out, "    ret\n\n");
}

void runtime_emit_builtins(FILE *out) {
    emit_yell_int(out);
    emit_yell_str(out);
    emit_write_bytes(out);
    emit_yell_dispatch(out);
    emit_task_builtins(out);
    emit_heap_alloc(out);
    emit_str_eq(out);
    emit_str_concat(out);
    emit_int_to_str(out);
    emit_str_builtins(out);
    emit_map_builtins(out);
    emit_imap_builtins(out);
    emit_list_builtins(out);

    // Panic handlers — used by the IR backend's bounds-check emission
    // and capacity-overflow paths in the helpers above. After P3.4
    // _yell_str takes a String header; the runtime-internal messages
    // below get hand-rolled `_*_str` headers in the __DATA section.
    fprintf(out, ".globl _panic_oob\n.p2align 2\n_panic_oob:\n");
    fprintf(out, "    adrp x0, _oob_str@PAGE\n    add x0, x0, _oob_str@PAGEOFF\n");
    fprintf(out, "    bl _yell_str\n    mov x16, #1\n    mov x0, #1\n    svc #0x80\n\n");
    fprintf(out, ".globl _panic_capacity\n.p2align 2\n_panic_capacity:\n");
    fprintf(out, "    adrp x0, _cap_str@PAGE\n    add x0, x0, _cap_str@PAGEOFF\n");
    fprintf(out, "    bl _yell_str\n    mov x16, #1\n    mov x0, #1\n    svc #0x80\n\n");

    // Assert handler — used by `assert(...)` calls in user test bodies.
    // Prints the line number, then " assertion failed", then exits 1.
    fprintf(out, ".globl _assert_fail\n.p2align 2\n_assert_fail:\n");
    fprintf(out, "    stp x29, x30, [sp, #-16]!\n    mov x29, sp\n");
    fprintf(out, "    bl _yell_int\n");
    fprintf(out, "    adrp x0, _assert_str@PAGE\n    add x0, x0, _assert_str@PAGEOFF\n");
    fprintf(out, "    bl _yell_str\n");
    fprintf(out, "    mov x16, #1\n    mov x0, #1\n    svc #0x80\n\n");

    // β4: panic / pass / assert messages need both an
    // array-of-byte header (cap, data) and a String header
    // (cap, count, data=arr_hdr, owned=0). Each lives in __DATA.
    fprintf(out, ".section __DATA,__data\n");
    fprintf(out, "_oob_msg: .asciz \"panic: index out of bounds\"\n");
    fprintf(out, ".p2align 3\n_oob_arr:\n");
    fprintf(out, "    .quad 26\n    .quad _oob_msg\n");
    fprintf(out, "_oob_str:\n");
    fprintf(out, "    .quad 26\n    .quad 26\n    .quad _oob_arr\n    .quad 0\n");
    fprintf(out, "_cap_msg: .asciz \"panic: capacity overflow\"\n");
    fprintf(out, ".p2align 3\n_cap_arr:\n");
    fprintf(out, "    .quad 24\n    .quad _cap_msg\n");
    fprintf(out, "_cap_str:\n");
    fprintf(out, "    .quad 24\n    .quad 24\n    .quad _cap_arr\n    .quad 0\n");
    fprintf(out, "_assert_msg: .asciz \" assertion failed\"\n");
    fprintf(out, ".p2align 3\n_assert_arr:\n");
    fprintf(out, "    .quad 17\n    .quad _assert_msg\n");
    fprintf(out, "_assert_str:\n");
    fprintf(out, "    .quad 17\n    .quad 17\n    .quad _assert_arr\n    .quad 0\n");
    fprintf(out, "_pass_prefix_msg: .asciz \"pass: \"\n");
    fprintf(out, ".p2align 3\n_pass_prefix_arr:\n");
    fprintf(out, "    .quad 6\n    .quad _pass_prefix_msg\n");
    fprintf(out, "_pass_prefix:\n");
    fprintf(out, "    .quad 6\n    .quad 6\n    .quad _pass_prefix_arr\n    .quad 0\n");
    fprintf(out, ".section __DATA,__bss\n.p2align 3\n");
    fprintf(out, "_heap_ptr: .quad 0\n");
    fprintf(out, "_heap_end: .quad 0\n");
    fprintf(out, "_heap_free_list: .quad 0\n");
    fprintf(out, ".section __TEXT,__text\n\n");
}
