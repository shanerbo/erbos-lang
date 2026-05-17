# Potato Runtime — what `_runtime_emit_builtins` emits

The Potato compiler emits a small set of hand-rolled ARM64
helpers at the top of every compiled `.ptt` binary. They live
in `src/runtime_emit.c::runtime_emit_builtins` and are written
directly via `fprintf` rather than going through irgen, because
each one is tightly coupled to a syscall ABI, a calling
convention, or a literal layout that the rest of the pipeline
treats as a fixed contract.

This document is the source of truth for which symbols survive
and why. It tracks the post-overhaul shape (after phases α
through γ + ε partial + ζ partial — see
[`../OVERHAUL.md`](../OVERHAUL.md)).

## Irreducible — these stay forever

These helpers wrap kernel boundaries (syscalls, mmap-backed
allocator, panic handlers) or document layout contracts the
language defines. They have no pure-Potato replacement.

| Symbol | Purpose |
|---|---|
| `_heap_alloc(size)` | mmap-backed bump allocator + free-list reuse. Returns 16-byte-aligned pointer. |
| `_heap_free(ptr, size)` | Insert block into free-list (LIFO). |
| `_yell_int(x)` | Sign-aware decimal stringify + write(2) + newline. The `yell(int)` and `yell(bool)` direct dispatch target. |
| `_String_yell(s)` | Read count + array-of-byte data ptr from a String header, write(2) + newline. The `yell(String)` and `s.yell()` direct dispatch target. |
| `_write_bytes(ptr, len)` | Raw write(2) syscall wrapper. The kernel-layer building block; not user-callable (banned in β5). |
| `_panic_oob` / `_panic_capacity` | Bounds-check / capacity-overflow panic. Print message via `_yell_str` then `exit(1)`. |
| `_assert_fail(line)` | Print line number + " assertion failed" then `exit(1)`. The `assert(cond)` lowering target. |
| `_alloc_<Struct>` | Per-user-struct constructor — emits `_heap_alloc(field_count*8)` then zero-fills. The latter is required for stdlib container lazy-init sentinels (`self.data eq 0`). |
| `_task_fire` / `_task_collapse` | No-op placeholders for the green-thread runtime not yet wired into compiled output. |

## Transitional — minor residue

A few helpers stay because binary `+`, `eq`, and string
interpolation still call them inline (rather than dispatching
through `String.concat` / `String.equals` / `int.to_string`):

| Symbol | Why kept |
|---|---|
| `_yell_str` | Alias for `_String_yell` referenced by the runtime-internal panic / pass / assert message paths. The aliasing is a single `.globl` line. |
| `_yell` | Magic-number int-vs-String dispatch (compares against `0x100000`). γ1 routes typed `yell(x)` directly; this shim survives only for TYPE_UNKNOWN values. |
| `_str_eq` | Called from binary `eq` / `ne` on String operands. |
| `_str_concat` | Called from `+` on String operands and from string interpolation. |
| `_int_to_str` | Called from string interpolation when an int variable is embedded. |

## Already retired

| Symbol | Retired in |
|---|---|
| `_str_len` | ζ2 (γ4 routed user code through `s.len()`) |
| `_char_at` | ζ2 (γ4 routed user code through `s.char_at(i)`) |
| `_yell_dispatch` (renamed `_yell`) | γ1 (compile-time resolution; shim only kept for TYPE_UNKNOWN) |
| `_list_new` / `_list_push` / `_list_pop` / `_list_set` / `_list_len` | ζ1 (ε1 retired the legacy `list of T` keyword form; user code uses `List of T` from std/list) |
| `_map_new` / `_map_set` / `_map_get` / `_map_keys` / `_map_len` | ζ1 (legacy `map of K to V` retired; user code uses `Map of K to V` and `StringMap of V`) |
| `_imap_new` / `_imap_set` / `_imap_get` / `_imap_len` | ζ1 (legacy `imap of int to V` retired; user code uses `Map of int to V`) |

## Layout contracts the runtime reads

The runtime helpers depend on these layouts, which iremit /
main.c emit and the stdlib structs declare. Any change to a
layout requires updating both sides in lockstep.

### `String` (32 bytes, in `std/string.ptt`)
| Offset | Field | Purpose |
|---|---|---|
| 0  | `cap`   | bytes allocated at the data pointer |
| 8  | `count` | bytes used (the visible length) |
| 16 | `data`  | pointer to a 16-byte `array of byte` header |
| 24 | `owned` | 1 if heap-allocated, 0 if borrowed (rodata literal) |

### `array of byte` / `array of T` (16 bytes, language primitive)
| Offset | Field | Purpose |
|---|---|---|
| 0 | `cap`  | element count (NOT bytes) |
| 8 | `data` | pointer to raw element storage |

For `array of byte` the element size is 1 (ldrb/strb); for
every other element type it's 8 (ldr/str). The compiler
synthesises the offset multiplication; the runtime never sees
the elements directly.

### `_pass_prefix` / `_oob_*` / `_assert_*` data sections
Each runtime-emitted message is laid out as a String value to
match β4's literal contract:
```
_<name>_msg:    .asciz "..."        ; the bytes
_<name>_arr:    .quad N, _<name>_msg ; array-of-byte hdr
_<name>:        .quad N, N, _<name>_arr, 0  ; String hdr
```

`_yell_str` (alias for `_String_yell`) reads count from offset
8 and the data ptr through the two-tier String → array-of-byte
indirection, identical to user-Potato code.
