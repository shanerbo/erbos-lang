#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include "runtime.h"

// --- Global task pool ---

static Task tasks[MAX_TASKS];
static int task_count = 0;
static pthread_mutex_t task_lock = PTHREAD_MUTEX_INITIALIZER;

// --- Per-thread state ---

typedef struct {
    int current_task;
    uint64_t scheduler_sp;
} WorkerState;

static __thread WorkerState worker_state = { .current_task = -1 };

// --- Task lifecycle ---

static void task_entry(void) {
    Task *t = &tasks[worker_state.current_task];
    t->func(t->arg);
    t->state = TASK_DONE;
    context_switch(&t->sp, worker_state.scheduler_sp);
}

void runtime_init(void) {
    task_count = 0;
    memset(tasks, 0, sizeof(tasks));
}

int task_fire(void (*func)(void *), void *arg) {
    pthread_mutex_lock(&task_lock);
    if (task_count >= MAX_TASKS) {
        pthread_mutex_unlock(&task_lock);
        fprintf(stderr, "error: max tasks reached\n");
        return -1;
    }

    int id = task_count++;
    Task *t = &tasks[id];
    t->id = id;
    t->func = func;
    t->arg = arg;
    t->state = TASK_READY;
    t->stack = malloc(TASK_STACK_SIZE);

    // Set up stack: context_switch will "return" into task_entry
    uint64_t *sp = (uint64_t *)(t->stack + TASK_STACK_SIZE);
    sp = (uint64_t *)((uint64_t)sp & ~0xFULL);
    sp -= 12;
    memset(sp, 0, 12 * sizeof(uint64_t));
    sp[10] = 0;                        // x29
    sp[11] = (uint64_t)task_entry;     // x30 = lr
    t->sp = (uint64_t)sp;

    pthread_mutex_unlock(&task_lock);
    return id;
}

void task_yield(void) {
    if (worker_state.current_task < 0) return;
    Task *t = &tasks[worker_state.current_task];
    context_switch(&t->sp, worker_state.scheduler_sp);
}

void task_collapse(int task_id) {
    while (tasks[task_id].state != TASK_DONE) {
        task_yield();
    }
}

// --- Single-threaded scheduler ---

void runtime_run(void) {
    while (1) {
        int found = 0;
        for (int i = 0; i < task_count; i++) {
            if (tasks[i].state == TASK_READY) {
                found = 1;
                tasks[i].state = TASK_RUNNING;
                worker_state.current_task = i;
                context_switch(&worker_state.scheduler_sp, tasks[i].sp);
                if (tasks[i].state == TASK_RUNNING)
                    tasks[i].state = TASK_READY;
                worker_state.current_task = -1;
            }
        }
        if (!found) break;
    }
}

// --- Multi-core scheduler (work-stealing) ---

static int steal_task(void) {
    pthread_mutex_lock(&task_lock);
    for (int i = 0; i < task_count; i++) {
        if (tasks[i].state == TASK_READY) {
            tasks[i].state = TASK_RUNNING;
            pthread_mutex_unlock(&task_lock);
            return i;
        }
    }
    pthread_mutex_unlock(&task_lock);
    return -1;
}

static void mark_task_yielded(int id) {
    pthread_mutex_lock(&task_lock);
    if (tasks[id].state == TASK_RUNNING)
        tasks[id].state = TASK_READY;
    pthread_mutex_unlock(&task_lock);
}

static int all_done(void) {
    pthread_mutex_lock(&task_lock);
    for (int i = 0; i < task_count; i++) {
        if (tasks[i].state != TASK_DONE) {
            pthread_mutex_unlock(&task_lock);
            return 0;
        }
    }
    pthread_mutex_unlock(&task_lock);
    return 1;
}

static void *worker_loop(void *arg) {
    (void)arg;
    while (!all_done()) {
        int id = steal_task();
        if (id < 0) {
            // No work — spin briefly
            for (volatile int i = 0; i < 1000; i++);
            continue;
        }
        worker_state.current_task = id;
        context_switch(&worker_state.scheduler_sp, tasks[id].sp);
        mark_task_yielded(id);
        worker_state.current_task = -1;
    }
    return NULL;
}

void runtime_run_parallel(int num_workers) {
    if (num_workers > MAX_WORKERS) num_workers = MAX_WORKERS;

    pthread_t threads[MAX_WORKERS];
    for (int i = 0; i < num_workers; i++) {
        pthread_create(&threads[i], NULL, worker_loop, NULL);
    }
    for (int i = 0; i < num_workers; i++) {
        pthread_join(threads[i], NULL);
    }
}
