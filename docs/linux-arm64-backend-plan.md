# Linux ARM64 Backend Plan

This document is the concrete implementation plan for turning Potato's current native backend into an explicit `darwin-arm64` target and then adding `linux-arm64` as the next supported native backend.

## Verified current state

The compiler is not target-split yet. The current Darwin ARM64 backend behavior is spread across four surfaces:

1. `compiler/main.c`
   - hardwires Mach-O text/data section setup
   - emits `_start` and the test harness entry path
   - calls `runtime_emit_builtins()`
   - calls `iremit_func()` / `iremit_finalize_data()`
   - invokes the Apple toolchain (`xcrun`, `ld`, `-lSystem`)

2. `compiler/iremit.c`
   - emits ARM64 assembly
   - uses Mach-O section names and Darwin relocations such as `@PAGE` / `@PAGEOFF`

3. `compiler/runtime_emit.c`
   - emits ARM64 builtin/runtime helpers
   - uses Darwin syscall/ABI assumptions (`svc #0x80` path, Mach-O section switching)

4. `compiler/runtime/`
   - contains the separate green-thread runtime and runtime C tests
   - is not wired into compiled `.ptt` output today
   - must not become the accidental portability path for this backend project

## Goal

Add a real `linux-arm64` native backend without routing Potato through C and without regressing the existing Darwin ARM64 backend.

## Non-goals

- Do not add `linux-x86_64` in this batch.
- Do not redesign the language runtime model.
- Do not replace the current emitted builtin/runtime path with `compiler/runtime/` as part of this effort.
- Do not claim generic multi-platform support until real Linux ARM64 execution is verified.

## Phase 1: Extract the current backend with zero behavior change

Create an explicit backend layer around the current Darwin ARM64 path.

### New files

- `compiler/target.h`
- `compiler/target_darwin_arm64.c`

### Responsibilities to move out of `compiler/main.c`

- backend selection and dispatch
- text/data section prologue emission
- `_start` emission
- test harness entry emission
- runtime builtin emission call
- assembler/linker driver invocation

### Rules

- Keep `compiler/iremit.c` and `compiler/runtime_emit.c` behavior unchanged in this phase.
- The resulting backend must still produce the same Darwin ARM64 output.
- `make test` on the current macOS setup must stay green before Phase 2 starts.

## Phase 2: Add explicit target selection

Introduce explicit target selection in the compiler driver.

### Required behavior

- support `darwin-arm64`
- support `linux-arm64` as a recognized target value even before it is fully implemented
- fail explicitly for unsupported targets
- preserve current default host behavior until the target flag is fully wired

### Expected touch points

- `compiler/main.c`
- `compiler/target.h`

## Phase 3: Add the Linux ARM64 backend

Add a second backend by cloning the Darwin backend and changing only the Linux-specific surfaces.

### New files

- `compiler/target_linux_arm64.c`
- likely `compiler/iremit_linux_arm64.c`
- likely `compiler/runtime_emit_linux_arm64.c`

### Linux-specific work

1. object format and assembly directives
   - ELF text/data sections
   - ELF symbol/export directives
   - Linux-compatible relocation syntax

2. entry and program startup
   - Linux `_start` path
   - test harness entry path
   - process exit path

3. builtin/runtime helper emission
   - Linux syscall ABI
   - Linux syscall numbers
   - Linux-specific data symbol/address materialization

4. toolchain driver
   - assembler invocation
   - linker invocation
   - output binary production without the Apple-specific `xcrun` / `-lSystem` path

### Rules

- Do not guess Linux ELF syntax, relocation forms, or syscall ABI from the Darwin code.
- Verify each Linux-specific emission path against a real Linux ARM64 environment while implementing.
- Keep Darwin and Linux backend files separate first; dedupe only after both work.

## Phase 4: Validate on real Linux ARM64

A Linux backend is not complete until it executes on Linux ARM64.

### Minimum proof

1. `erbos ir` produces Linux-targeted assembly for small programs.
2. the Linux assembler/linker path produces runnable binaries.
3. tiny end-to-end programs run correctly on Linux ARM64.
4. repo tests pass on Linux ARM64, or any temporary exclusions are explicit and tracked.

### Acceptable validation environments

- native Linux ARM64 machine
- Linux ARM64 VM
- remote Linux ARM64 instance
- Linux ARM64 CI runner

## Phase 5: Extract ARM64-common pieces only after both backends work

Only after Darwin ARM64 and Linux ARM64 are both working should shared ARM64 code move into common helpers.

### Candidate future extractions

- ARM64 instruction-emission helpers
- string/data lowering helpers
- common register/stack materialization helpers

### Rule

Do not start with `arm64_common` abstractions. First get two correct backends.

## Suggested file ownership after Phase 1

- `compiler/main.c`
  - frontend pipeline orchestration
  - option parsing
  - target selection
  - no target-specific assembly details

- `compiler/target.h`
  - backend interface
  - target selection API

- `compiler/target_darwin_arm64.c`
  - current native backend behavior extracted from `main.c`
  - Darwin assembler/linker driver
  - Darwin startup/test harness emission

- `compiler/iremit.c`
  - current Darwin ARM64 IR emitter until Linux gets its own emitter

- `compiler/runtime_emit.c`
  - current Darwin ARM64 builtin/runtime helper emitter until Linux gets its own emitter

## Acceptance gates

### Gate A

Current macOS behavior remains green after backend extraction:

- `make test` passes
- default compile/run path still works
- `erbos ir` output still works for the current Darwin ARM64 flow

**Status: PASSED.** Phases 1, 2, and 3 all preserve `make test` green
on Darwin/aarch64. Default `erbos ir` and `erbos run` flows still
work; `--target=darwin-arm64` is byte-equivalent at the semantic
level (instruction reorderings within syscall sequences do exist
because the syscall emission moved into a per-target callback, but
no behavioral change).

### Gate B

Linux ARM64 backend can build and run minimal samples end to end.

**Status: PASSED.** Validation environment used: Apple's `container`
CLI (https://apple.github.io/container/documentation/) running an
arm64 Alpine Linux container natively on Apple Silicon via
Virtualization.framework, kernel 6.18.15, kata-static-3.28.0-arm64.

Toolchain on the macOS side: clang `--target=aarch64-linux-gnu` for
cross-assembly (LLVM's integrated assembler emits ELF aarch64
objects); LLD `ld.lld -static -e _start` for static linking. The
emitted ELF objects use exactly the relocation types cited in
`target_linux_arm64.c`: `R_AARCH64_ADR_PREL_PG_HI21`,
`R_AARCH64_ADD_ABS_LO12_NC`, `R_AARCH64_CALL26`, `R_AARCH64_ABS64`.

Smoke results: `examples/hello.ptt` (`yell(10 + 20)`) emits `30`
and exits 0 inside the Linux container, byte-identical to its
Darwin output.

### Gate C

Linux ARM64 passes the repo test suite, or any remaining exclusions are explicit, justified, and tracked as findings.

**Status: PASSED.** Validation matrix run inside the Apple container
(arm64 Alpine, kernel 6.18.15):

- 22/22 runnable `examples/*.ptt` programs — byte-identical output
  Linux ↔ Darwin.
- 65/65 framework + leetcode tests
  (`tests/test_*.ptt`, `tests/leetcode/test_*.ptt`) — byte-identical
  output Linux ↔ Darwin (test harness prints `pass: <name>` for
  each test on both targets).
- 63/63 IR-regression tests across all three optimization levels
  (`tests/ir/*.ptt` × {-O0, -O1, -O2}) — byte-identical output
  Linux ↔ Darwin.
- 32/32 runtime-panic tests (the same set Darwin's `make test-fail`
  iterates over) — exit non-zero on Linux, matching Darwin's
  behavior.

Total: 182 program runs on Linux/aarch64, all behaviorally
identical to the Darwin baseline. No exclusions, no findings.

### Gate D

Only after Gates A, B, and C may the repo claim that Potato now has native multi-platform support across Darwin ARM64 and Linux ARM64.

**Status: PASSED.** Gates A + B + C all green. The repo can now
claim Potato has native multi-platform support across Darwin
ARM64 and Linux ARM64.

Caveats to keep honest:
- Validation was run inside an arm64 Alpine container on Apple
  Silicon, not on bare-metal Linux/aarch64 hardware. The kernel ABI
  and instruction execution are real (Apple's `container` does not
  emulate; it virtualizes), so the Linux syscall and AArch64
  instruction paths exercised are exactly what a bare-metal
  Linux/aarch64 system would see. The container's libc was not
  involved (statically-linked ELF, no libc dependencies).
- Cross-assembly was done with clang's integrated assembler, not
  GNU `as`. The two should produce equivalent output for the
  AArch64 ELF directives this compiler emits (`.text`, `.data`,
  `.bss`, `adrp`, `:lo12:`, `.quad`, `.asciz`, `.p2align`,
  `.global`, `.globl`); confirming that against GNU `as` is a
  follow-up worth doing on a bare-metal Linux box but is not
  required to clear this gate.
- The Makefile `test` target still runs only on Darwin today; a
  future `make test-linux-arm64` that drives the same matrix
  through `container exec` would let CI catch Linux backend
  regressions automatically.

## Recommended implementation order

1. Extract `darwin-arm64` backend with zero behavior change.
2. Land target selection.
3. Clone the backend into `linux-arm64`.
4. Get tiny Linux ARM64 samples working.
5. Expand to the repo test suite.
6. Dedupe shared ARM64 pieces only after both backends are correct.

### Implementation deviation in Phase 3

The plan's step 3 says "clone the backend into linux-arm64." The
implementation took a slightly different route: instead of cloning
`compiler/iremit.c` and `compiler/runtime_emit.c` into Linux variants,
those files were parameterised through narrow per-target callbacks
on `Target` (`emit_text_section` / `emit_data_section` / `emit_bss_section`
/ `emit_addr_load` / `emit_sys_write_stdout` / `emit_sys_exit` /
`emit_sys_mmap_anon_64k`). The handful of Mach-O / Darwin-syscall
sites in those files now route through callbacks; everything else is
pure AArch64 instructions shared by both targets (AAPCS64 frame
layout, instruction selection, register classes).

Why deviate: cloning the files would have doubled the *unverified*
bytes (Linux variants cannot be compiled-tested on a Darwin host),
without changing the end state. The callback approach keeps Linux
output empirically diff-able against Darwin output for every shared
helper, while concentrating Linux-specific guesses into one file
(`target_linux_arm64.c`) where every constant is cited to its
source-of-truth document on the same line. Phase 5's "extract
ARM64-common pieces" rule is mostly already satisfied as a
side-effect.

This deviation does not relax Gate B / Gate C: the Linux backend is
still unverified until it executes on a real Linux/aarch64 host.

## Claude execution notes

- Keep each claim narrow.
- Do not mix backend extraction, Linux backend implementation, and broad refactors into one opaque batch.
- Prefer one accepted claim per phase milestone.
- Use exact snapshots for claims and Linux validation evidence.

## Phase 4 validation playbook

The following recipe was used to clear Gates B/C from a macOS host
running Apple Silicon. It can be reproduced from any macOS 15+
arm64 machine; the same shape applies on a bare-metal Linux/aarch64
host with `gcc` and `binutils` instead of clang/lld.

### One-time host setup (macOS arm64)

```sh
# LLVM's lld linker — the static linker for the Linux ELF output.
brew install lld

# Apple's native Linux container CLI; uses Virtualization.framework
# under the hood, no Docker needed.
brew install container
container system kernel set --recommended
container system start
```

Spin up a long-running Alpine Linux container with the repo and a
scratch dir mounted in:

```sh
mkdir -p /tmp/linuxvalid
container run -d --rm --arch arm64 --name potatovalid \
  -v "$(pwd):/repo:ro" \
  -v "/tmp/linuxvalid:/host:ro" \
  alpine:latest sleep 3600
```

### Per-program validation

For one `.ptt` source:

```sh
ERBOS=$(pwd)/erbos
"$ERBOS" --target=linux-arm64 ir path/to/prog.ptt   # emits prog.s
clang --target=aarch64-linux-gnu -c prog.s -o /tmp/linuxvalid/prog.o
ld.lld -static -e _start -o /tmp/linuxvalid/prog.elf /tmp/linuxvalid/prog.o
container exec potatovalid /host/prog.elf            # runs on Linux/arm64
```

The two helper scripts used during the Phase 4 validation pass —
`run.sh` (compares Linux output against Darwin output for normal
exits) and `run_panic.sh` (asserts non-zero exit for runtime-panic
tests) — were ad-hoc and live under `/tmp/linuxvalid/` only during
the validation run; they do not need to be checked in.

### Bare-metal Linux/aarch64 alternative

If you want to validate the generated ELF on a real Linux/aarch64 host
instead of inside Apple's container, keep the distinction straight:

1. The compiler frontend itself is still macOS-host-only in this
   batch. `compiler/main.c` still depends on `_NSGetExecutablePath`
   for bundled-stdlib resolution, so a native Linux-host `make`
   path is not shipped yet.

2. Build `erbos` on the supported macOS host, then use
   `./erbos --target=linux-arm64 ir path/to/prog.ptt` to emit the
   Linux-targeted assembly exactly as in the container workflow above.

3. Assemble and link that `.s` into a static ELF, then copy the ELF
   onto the Linux/aarch64 machine and run it there. The Linux host
   needs GNU `as` and `ld` (or LLVM equivalents) on `PATH`; no libc,
   no glibc startup, no SDK.

4. Once a Linux host port lands, the expected workflow becomes
   `make` followed by `./erbos --target=linux-arm64 run ...`. That
   workflow is not part of the current acceptance claim.

If the Linux backend produces an `as` error, the most likely cause
is a constant value in `compiler/target_linux_arm64.c` that was
miscited from its source-of-truth document — re-read the document
named on the same line as the constant and fix the constant, not
the test.
