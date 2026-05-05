#ifndef RUNTIME_H
#define RUNTIME_H

#include <stdint.h>

#define TASK_STACK_SIZE (64 * 1024)
#define MAX_TASKS 4096
#define MAX_WORKERS 16

typedef enum {
    TASK_READY,
    TASK_RUNNING,
    TASK_DONE,
} TaskState;

typedef struct Task {
    uint64_t sp;
    uint8_t *stack;
    TaskState state;
    void (*func)(void *);
    void *arg;
    int id;
} Task;

// Public API
void runtime_init(void);
int  task_fire(void (*func)(void *), void *arg);
void task_collapse(int task_id);
void task_yield(void);
void runtime_run(void);          // single-threaded scheduler
void runtime_run_parallel(int num_workers);  // multi-core

// Assembly context switch
extern void context_switch(uint64_t *old_sp, uint64_t new_sp);

#endif
