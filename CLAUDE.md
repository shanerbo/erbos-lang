# Potato — agent entry point 🥔

You're working on the Potato language compiler. Read this first;
load deeper docs only when the task needs them.

## What Potato is, in one paragraph

A systems language that reads like English (`is`, `be`, `give`,
`through`), compiles to native ARM64 macOS, has no garbage
collector, no runtime, no libc dependency. Written in C11. Source
files use the `.ptt` extension. Mature enough for real programs
(stdlib + leetcode tests + IR optimizer) but pre-1.0 — language
shape is still moving. Major 2025–2026 overhaul (typed arrays,
pure-Potato stdlib, keyword retirement, framework-only testing)
all shipped.

## Hard rules — non-negotiable

1. **No magic.** No C-isms, no preprocessor tricks, no implicit
   conversions, no auto-coercion. Everything user-visible is
   spelled out in `.ptt` source.
2. **No `<T>` template syntax anywhere.** Generics are word-style:
   `Box of T`, `Map of K to V`. Period.
3. **No `str` ↔ `String` sugar.** `str` is gone. `String` is the
   stdlib struct, defined in `std/string.ptt`.
4. **PascalCase for user-defined types** (struct, enum). lowercase
   for primitives (`int`, `bool`, `byte`, `void`). The case
   difference is a deliberate signal — see
   `docs/design-decisions.md` before proposing to flatten it.
5. **`make test` must end with `All tests passed.`** before any
   commit. No commits that knowingly break the suite "to fix in
   the next commit." If a hook or test fails, fix the root cause.
6. **Never modify a test to match buggy output.** If a test starts
   failing, the *implementation* is wrong unless proven otherwise.
7. **No assumptions.** Every claim about the codebase must be
   backed by tool output (grep, read, build, run). When uncertain,
   look it up.

## Repo layout

```
compiler/          C11 compiler frontend (was src/, renamed in the overhaul)
  *.c, *.h         lexer / parser / monomorph / checker / iropt / regalloc / iremit
  runtime/         green-thread runtime + channels (NOT integrated into compiled .ptt yet)
std/               Potato stdlib (.ptt source — pure Potato, no C)
  basics.ptt       bundle: re-exports string + list + map
  list.ptt         List of T
  map.ptt          Map of K to V
  string.ptt       String struct
  math.ptt, queue.ptt, stack.ptt
examples/          standalone demo programs (have spark blocks; readable as tutorials)
tests/
  compiler/        C runtime tests
  lib/leetcode/    library .ptt files (no spark blocks); imported by leetcode tests
  leetcode/        framework tests that import tests/lib/leetcode/<name>
  ir/              IR backend regression matrix (-O0/-O1/-O2)
  errors/          expected-compile-failure tests
  bench/           perf benchmarks
docs/              everything below
editor/vscode/     syntax highlighting extension
```

## Doc index — read on demand

| File | Read when |
|------|-----------|
| `docs/language-guide.md` | designing user-facing syntax / semantics; need to see what types/constructs exist |
| `docs/keywords.md` | quick keyword reference |
| `docs/builtins.md` | what `yell`/`assert` and the runtime intrinsics resolve to |
| `docs/generics-syntax.md` | adding/changing generic monomorphization |
| `docs/runtime.md` | touching `compiler/runtime_emit.c` or the `_alloc_*` / `_panic_*` symbols |
| `docs/ir-pipeline.md` | working on irgen / iropt / regalloc / iremit |
| `docs/examples.md` | adding example programs |
| `docs/design-decisions.md` | **READ BEFORE proposing language changes.** Append-only log of decided/parked discussions. The whole point is to not relitigate the same trade-offs. |

## Build + test

```bash
make             # build the `erbos` binary
make test        # full suite: passing examples, leetcode, errors, IR matrix at -O0/-O1/-O2,
                 # framework tests, C-runtime tests. Must end with "All tests passed."

./erbos run path/to/file.ptt   # compile + execute + clean up
./erbos test path/to/test.ptt  # run framework tests in a file
./erbos ir  path/to/file.ptt   # emit .s without assembling/linking
```

Optimization levels (`-O0` / `-O1` / `-O2`) accepted in any
position relative to the subcommand. Default is `-O1`.

## Commit conventions

- **5-W message style** — see recent `git log --oneline -10` for
  examples. `feat:` / `fix:` / `refactor:` / `docs:` / `test:` /
  `chore:` prefixes.
- **One logical change per commit.** Doc changes that motivate code
  changes go with the code; large doc reorganizations get their
  own commit.
- **Use `WorkspaceGitDetails` and `git status`** before committing;
  never `git add -A` without inspection.
- **Don't push hooks (`--no-verify`)** unless explicitly asked.
- **Don't amend** unless explicitly asked — make a new commit.

## Working style

1. **Plan first.** For multi-step work use `TaskCreate` /
   `TaskUpdate`. Mark tasks `in_progress` before starting and
   `completed` only when the implementation is fully done and
   `make test` is green.
2. **Push back when warranted.** "First principles, not flattery."
   If a proposal has a real downside, say so before implementing.
3. **Smallest possible change.** Edit existing files; don't create
   new ones unless required. Match surrounding style.
4. **Verify before claiming.** Build the change, run the suite at
   all three -O levels, run the relevant ASan/UBSan check for
   compiler-side changes.

## Out of scope without explicit ask

- Self-hosting the compiler in Potato (planned, not started).
- Integrating the green-thread runtime into compiled `.ptt`
  output (the runtime exists in `compiler/runtime/` but is not
  wired in).
- Operator overloading, traits, deep clone, `Result`/`Option` as
  built-in types — all on the roadmap, none in flight.
