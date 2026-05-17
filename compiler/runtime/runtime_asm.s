// ARM64 context switch for Erbos green threads
// void context_switch(uint64_t *old_sp, uint64_t new_sp)
// x0 = pointer to save current sp
// x1 = new sp to restore

.globl _context_switch
.p2align 2
_context_switch:
    // Save callee-saved registers onto current stack
    sub sp, sp, #96          // 12 * 8 bytes
    stp x19, x20, [sp, #0]
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]

    // Save current sp
    mov x2, sp
    str x2, [x0]

    // Restore new sp
    mov sp, x1

    // Restore callee-saved registers from new stack
    ldp x19, x20, [sp, #0]
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp x29, x30, [sp, #80]
    add sp, sp, #96

    ret
