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

static void emit_yell_int(FILE *out, const Target *target) {
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
    // x1 already points at the digit buffer; copy length into x2 and
    // hand off to the per-target write(2) syscall.
    fprintf(out, "    mov x2, x3\n");
    target->emit_sys_write_stdout(out);
    fprintf(out, "    mov sp, x29\n    ldp x29, x30, [sp], #16\n    ret\n\n");
}

static void emit_String_yell(FILE *out, const Target *target) {
    // γ2: this is the canonical `String.yell(self)` symbol —
    // every `yell(s String)` call in user code routes here at
    // compile time (γ1 rewrites `yell` to `String_yell` based
    // on the static type of the argument).
    //
    // x0 is a String header (4 quads: cap, count, data, owned)
    // where data is an `array of byte` ptr. Pull count from
    // offset 8; reach the byte buffer via two loads — first
    // the array-of-byte header from String.data ([x0,#16]),
    // then the byte ptr from that header's data field ([arr,#8]).
    // Write count bytes + newline.
    //
    // Same .globl symbol exposes _yell_str for now so the
    // runtime-internal panic / pass / assert paths still link.
    // ζ2 will sweep those internal callers and drop the alias.
    fprintf(out, "// built-in: _String_yell (x0 = String header ptr)\n");
    fprintf(out, ".globl _String_yell\n.globl _yell_str\n.p2align 2\n_String_yell:\n_yell_str:\n");
    fprintf(out, "    stp x29, x30, [sp, #-16]!\n    mov x29, sp\n");
    // Empty-String guard: a freshly-allocated `String()` has
    // count=0 and data=NULL. Skip the byte-buffer write but
    // still emit the trailing newline so `yell(String())`
    // produces a blank line rather than dereferencing NULL.
    // Without this, every Path / String stdlib helper that
    // returns a fresh empty String crashes when piped through
    // `yell`.
    fprintf(out, "    ldr x2, [x0, #8]\n");      // count -> x2
    fprintf(out, "    cbz x2, _Sy_newline\n");
    fprintf(out, "    ldr x1, [x0, #16]\n");     // array-of-byte hdr -> x1
    fprintf(out, "    cbz x1, _Sy_newline\n");
    fprintf(out, "    ldr x1, [x1, #8]\n");      // byte ptr -> x1
    target->emit_sys_write_stdout(out);
    fprintf(out, "_Sy_newline:\n");
    // Trailing newline.
    fprintf(out, "    sub sp, sp, #16\n    mov w4, #10\n    strb w4, [sp]\n");
    fprintf(out, "    mov x1, sp\n    mov x2, #1\n");
    target->emit_sys_write_stdout(out);
    fprintf(out, "    add sp, sp, #16\n");
    fprintf(out, "    mov sp, x29\n    ldp x29, x30, [sp], #16\n    ret\n\n");
}

// γ1 transitional: most `yell(x)` calls resolve at compile time
// on x's static type (see checker.c NODE_CALL "yell" branch) and
// emit `bl _yell_int` / `bl _String_yell` / `bl _<UserType>_yell`
// directly. This runtime `_yell` shim handles the residue —
// values whose static type the checker couldn't pin down (e.g.
// elements of a legacy untyped `list` reached via chained
// indexing). Once ε drops the legacy `list` / `map` keyword
// forms every value carries a concrete type and this shim can go.
static void emit_yell_dispatch(FILE *out, const Target *target) {
    (void)target; // no target-specific instructions in this dispatcher
    fprintf(out, "// transitional: _yell (magic-number int/str dispatch)\n");
    fprintf(out, ".globl _yell\n.p2align 2\n_yell:\n");
    fprintf(out, "    stp x29, x30, [sp, #-16]!\n    mov x29, sp\n");
    fprintf(out, "    mov x1, #0x100000\n");
    fprintf(out, "    cmp x0, x1\n");
    fprintf(out, "    b.ge _yell_is_str\n");
    fprintf(out, "    bl _yell_int\n");
    fprintf(out, "    b _yell_done\n");
    fprintf(out, "_yell_is_str:\n");
    fprintf(out, "    bl _String_yell\n");
    fprintf(out, "_yell_done:\n");
    fprintf(out, "    mov sp, x29\n    ldp x29, x30, [sp], #16\n    ret\n\n");
}

static void emit_task_builtins(FILE *out, const Target *target) {
    (void)target;
    // In single-threaded compiled mode, task.fire(fn()) already calls fn,
    // so _task_fire and _task_collapse are no-ops.
    fprintf(out, ".globl _task_fire\n.p2align 2\n_task_fire:\n    ret\n\n");
    fprintf(out, ".globl _task_collapse\n.p2align 2\n_task_collapse:\n    ret\n\n");
}

static void emit_heap_alloc(FILE *out, const Target *target) {
    // Free list: singly-linked list of freed blocks.
    // Each free block: [next_ptr(8) | size(8) | unused space ...]
    // _heap_free_list points to the first free block (or 0).
    //
    // Important invariant: every freed block must be at least
    // 16 bytes so the (next_ptr, size) metadata fits without
    // corrupting adjacent memory. _heap_alloc rounds requests
    // up to a 16-byte multiple (see below), so the *physical*
    // block always satisfies that. _heap_free's `size` argument
    // is whatever the caller passed — typically computed as
    // `cap * elem_size` in the array-drop path, which can be
    // < 16 (e.g. `array of byte with cap 8` → 8 bytes). Round
    // up here to match the allocator's rounding so the metadata
    // never overflows the block. Without this, a small byte-
    // array drop wrote 16 bytes into an 8-byte slot and clobbered
    // free-list links downstream.
    fprintf(out, "// built-in: _heap_free(ptr=x0, size=x1)\n");
    fprintf(out, ".globl _heap_free\n.p2align 2\n_heap_free:\n");
    fprintf(out, "    cbz x0, _hf_ret\n");
    fprintf(out, "    add x1, x1, #15\n");
    fprintf(out, "    and x1, x1, #-16\n");
    fprintf(out, "    cmp x1, #16\n");
    fprintf(out, "    b.ge _hf_meta\n");
    fprintf(out, "    mov x1, #16\n");
    fprintf(out, "_hf_meta:\n");
    target->emit_addr_load(out, 2, "_heap_free_list");
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
    target->emit_addr_load(out, 10, "_heap_free_list");
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
    // Zero-fill the returned block. Free-list reuse hands back
    // memory that previously held heap metadata (next/size) and
    // user data, so without this every fresh-shaped allocation
    // (struct, array) has to remember to zero its own slots.
    // The bump path also routes through here so a single
    // zero-fill loop covers both. Cost: one stp per 16 bytes.
    // x9 holds the rounded-up size (multiple of 16).
    fprintf(out, "    mov x15, x0\n");
    fprintf(out, "    mov x16, x9\n");
    fprintf(out, "_ha_zero:\n");
    fprintf(out, "    cbz x16, _ha_zero_done\n");
    fprintf(out, "    stp xzr, xzr, [x15], #16\n");
    fprintf(out, "    sub x16, x16, #16\n");
    fprintf(out, "    b _ha_zero\n");
    fprintf(out, "_ha_zero_done:\n");
    fprintf(out, "    mov sp, x29\n    ldp x29, x30, [sp], #16\n    ret\n");
    fprintf(out, "_ha_bump:\n");
    target->emit_addr_load(out, 10, "_heap_ptr");
    fprintf(out, "    ldr x11, [x10]\n");
    target->emit_addr_load(out, 12, "_heap_end");
    fprintf(out, "    ldr x13, [x12]\n");
    fprintf(out, "    add x14, x11, x9\n");
    fprintf(out, "    cmp x14, x13\n");
    fprintf(out, "    b.le _ha_ok\n");
    target->emit_sys_mmap_anon_64k(out);
    fprintf(out, "    mov x11, x0\n");
    fprintf(out, "    add x13, x0, #0x10000\n");
    fprintf(out, "    add x14, x11, x9\n");
    fprintf(out, "_ha_ok:\n");
    fprintf(out, "    mov x0, x11\n");
    fprintf(out, "    str x14, [x10]\n");
    fprintf(out, "    str x13, [x12]\n");
    // Bump path: mmap returns zeroed memory (MAP_ANON), but
    // the bump pointer slides over the same region we already
    // returned from before, so zeroing here is also correct.
    // Reuse the same loop as the free-list path.
    fprintf(out, "    mov x15, x0\n");
    fprintf(out, "    mov x16, x9\n");
    fprintf(out, "_hb_zero:\n");
    fprintf(out, "    cbz x16, _hb_zero_done\n");
    fprintf(out, "    stp xzr, xzr, [x15], #16\n");
    fprintf(out, "    sub x16, x16, #16\n");
    fprintf(out, "    b _hb_zero\n");
    fprintf(out, "_hb_zero_done:\n");
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

// ζ2: emit_str_builtins (which used to emit `_str_len` and
// `_char_at`) is gone. Both symbols had no remaining callers
// after γ4 routed user code through the `s.len()` / `s.char_at(i)`
// method form. The `String.len` / `String.char_at` user methods
// in `std/string.ptt` provide the same semantics through the
// canonical method-dispatch path.


// ζ1: emit_map_builtins / emit_imap_builtins / emit_list_builtins
// removed. The legacy `list of T` / `map of K, V` /
// `imap of int, V` keyword forms are gone (ε1); user code uses
// the pure-Potato stdlib types `List of T` (std/list),
// `Map of K, V` (std/map), `StringMap of V` (std/string_map),
// all backed by `array of T` and emitted as monomorphized
// `_<Type>__<args>_<method>` symbols by the normal user-method
// codegen path.


void runtime_emit_builtins(FILE *out, const Target *target) {
    emit_yell_int(out, target);
    emit_String_yell(out, target);
    emit_yell_dispatch(out, target);
    emit_task_builtins(out, target);
    emit_heap_alloc(out, target);
    emit_str_eq(out);
    emit_str_concat(out);
    emit_int_to_str(out);
    // ζ2: emit_str_builtins removed — `_str_len` and `_char_at`
    // had no remaining callers after γ4.
    // ζ1: emit_map_builtins / emit_imap_builtins / emit_list_builtins
    // removed. ε1 retired the legacy `list` / `map` / `imap` keyword
    // forms; user code uses pure-Potato `List of T` / `Map of K, V` /
    // `StringMap of V`, which monomorphize through the normal user-
    // method codegen path.

    // Panic handlers — used by the IR backend's bounds-check emission
    // and capacity-overflow paths in the helpers above. After P3.4
    // _yell_str takes a String header; the runtime-internal messages
    // below get hand-rolled `_*_str` headers in the data section.
    fprintf(out, ".globl _panic_oob\n.p2align 2\n_panic_oob:\n");
    target->emit_addr_load(out, 0, "_oob_str");
    fprintf(out, "    bl _yell_str\n");
    target->emit_sys_exit(out, 1);
    fprintf(out, "\n");

    // Assert handler — used by `assert(...)` calls in user test bodies.
    // Prints the line number, then " assertion failed", then exits 1.
    fprintf(out, ".globl _assert_fail\n.p2align 2\n_assert_fail:\n");
    fprintf(out, "    stp x29, x30, [sp, #-16]!\n    mov x29, sp\n");
    fprintf(out, "    bl _yell_int\n");
    target->emit_addr_load(out, 0, "_assert_str");
    fprintf(out, "    bl _yell_str\n");
    target->emit_sys_exit(out, 1);
    fprintf(out, "\n");

    // β4: panic / pass / assert messages need both an array-of-byte
    // header (cap, data) and a String header (cap, count,
    // data=arr_hdr, owned=0). Each lives in the data section.
    target->emit_data_section(out);
    fprintf(out, "_oob_msg: .asciz \"panic: index out of bounds\"\n");
    fprintf(out, ".p2align 3\n_oob_arr:\n");
    fprintf(out, "    .quad 26\n    .quad _oob_msg\n");
    fprintf(out, "_oob_str:\n");
    fprintf(out, "    .quad 26\n    .quad 26\n    .quad _oob_arr\n    .quad 0\n");
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
    target->emit_bss_section(out);
    fprintf(out, ".p2align 3\n");
    fprintf(out, "_heap_ptr: .quad 0\n");
    fprintf(out, "_heap_end: .quad 0\n");
    fprintf(out, "_heap_free_list: .quad 0\n");
    target->emit_text_section(out);
    fprintf(out, "\n");
}
