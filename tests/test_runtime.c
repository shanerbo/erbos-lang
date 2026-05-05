#include <stdio.h>
#include "runtime.h"

static void worker(void *arg) {
    int id = *(int *)arg;
    for (int i = 0; i < 3; i++) {
        printf("task %d: step %d\n", id, i);
        task_yield();
    }
    printf("task %d: done\n", id);
}

int main(void) {
    runtime_init();

    int a = 1, b = 2, c = 3;
    task_fire(worker, &a);
    task_fire(worker, &b);
    task_fire(worker, &c);

    runtime_run();
    printf("all tasks finished\n");
    return 0;
}
