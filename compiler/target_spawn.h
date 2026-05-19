#ifndef TARGET_SPAWN_H
#define TARGET_SPAWN_H

// Shared toolchain-driver helpers used by every Target's
// assemble_and_link callback. Extracted in Phase 5 of the
// linux-arm64-backend-plan.md after both backends were verified
// working — the plan's rule was "first get two correct backends,
// then dedupe."

#include <stddef.h>

// Spawn `prog` with the given NULL-terminated argv, without going
// through the shell. `prog` is looked up via posix_spawnp (PATH
// search). Returns the wait-status convention used elsewhere:
//   - WIFEXITED:    returns WEXITSTATUS
//   - WIFSIGNALED:  returns 128 + WTERMSIG (shell convention)
//   - everything else: 1
int target_spawn_argv(const char *prog, char *const argv[]);

// Capture stdout from `prog argv...` into `out` (size out_size,
// trimmed of trailing newline). Returns 1 on success, 0 on failure.
// Used today by darwin-arm64's xcrun --show-sdk-path query; kept
// shared so future targets that need a one-shot CLI capture have
// it ready.
int target_capture_stdout(const char *prog, char *const argv[],
                          char *out, size_t out_size);

#endif
