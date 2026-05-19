// Shared toolchain-driver helpers. See compiler/target_spawn.h for
// the contract; this file is the Phase-5 dedupe extraction.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <spawn.h>
#include <errno.h>

#include "target_spawn.h"

extern char **environ;

int target_spawn_argv(const char *prog, char *const argv[]) {
    pid_t pid;
    int rc = posix_spawnp(&pid, prog, NULL, NULL, argv, environ);
    if (rc != 0) {
        fprintf(stderr, "error: failed to spawn '%s': %s\n", prog, strerror(rc));
        return 1;
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        fprintf(stderr, "error: waitpid failed for '%s'\n", prog);
        return 1;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

int target_capture_stdout(const char *prog, char *const argv[],
                          char *out, size_t out_size) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return 0;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addclose(&fa, pipefd[0]);
    posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&fa, pipefd[1]);
    pid_t pid;
    int rc = posix_spawnp(&pid, prog, &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(pipefd[1]);
    if (rc != 0) {
        close(pipefd[0]);
        return 0;
    }
    size_t n = 0;
    while (n + 1 < out_size) {
        ssize_t r = read(pipefd[0], out + n, out_size - 1 - n);
        if (r <= 0) break;
        n += (size_t)r;
    }
    out[n] = '\0';
    close(pipefd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return 0;
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' ||
                     out[n - 1] == ' ' || out[n - 1] == '\t')) {
        out[--n] = '\0';
    }
    return n > 0;
}
