#ifndef RUNTIME_EMIT_H
#define RUNTIME_EMIT_H

#include <stdio.h>

// Emit the full set of C-emitted runtime helpers + panic handlers +
// data section that every compiled .ptt program depends on.
//
// Includes:
//   - I/O:                  _yell, _yell_int, _yell_str
//   - Heap allocator:       _heap_alloc, _heap_free
//   - Strings:              _str_eq, _str_concat, _str_len, _char_at,
//                           _int_to_str
//   - Collections:          _list_new/_push/_pop/_len/_set,
//                           _map_new/_set/_get/_len/_keys,
//                           _imap_new/_set/_get/_len
//   - Tasks:                _task_fire, _task_collapse (no-ops in
//                           single-threaded compiled output)
//   - Panic handlers:       _panic_oob, _panic_capacity, _assert_fail
//   - Data:                 _oob_msg, _cap_msg, _assert_msg,
//                           _pass_prefix, _heap_ptr, _heap_end,
//                           _heap_free_list
//
// Called once at the top of compiled .s output by the IR pipeline
// (src/main.c). Self-contained: switches into __TEXT,__text on entry
// and back into __TEXT,__text on exit so the caller can keep emitting
// code immediately after.
void runtime_emit_builtins(FILE *out);

#endif
