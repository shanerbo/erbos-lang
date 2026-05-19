#ifndef RUNTIME_EMIT_H
#define RUNTIME_EMIT_H

#include <stdio.h>
#include "target.h"

// Emit the full set of C-emitted runtime helpers + panic handlers +
// data section that every compiled .ptt program depends on.
//
// Includes:
//   - I/O:                  _yell, _yell_int, _yell_str
//   - Heap allocator:       _heap_alloc, _heap_free
//   - Strings:              _str_eq, _str_concat, _int_to_str
//   - Tasks:                _task_fire, _task_collapse (no-ops in
//                           single-threaded compiled output)
//   - Panic handlers:       _panic_oob, _panic_capacity, _assert_fail
//   - Data:                 _oob_msg, _cap_msg, _assert_msg,
//                           _pass_prefix, _heap_ptr, _heap_end,
//                           _heap_free_list
//
// Called once at the top of compiled .s output by the IR pipeline.
// `target` selects the AArch64 dialect: per-target syscall numbers,
// section directives, and PC-relative-address-load form. Self-
// contained: switches back into the text section on exit so the
// caller can keep emitting code immediately after.
void runtime_emit_builtins(FILE *out, const Target *target);

#endif
