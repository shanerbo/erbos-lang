#include <stdio.h>
#include "runtime.h"

static void heavy_work(void *arg) {
    int id = *(int *)arg;
    long sum = 0;
    for (long i = 0; i < 10000000; i++) {
        sum += i;
        if (i % 2000000 == 0) {
            printf("task %d: progress %ld\n", id, i);
            task_yield();
        }
    }
    printf("task %d: result = %ld\n", id, sum);
}

int main(void) {
    runtime_init();

    int ids[] = {1, 2, 3, 4};
    for (int i = 0; i < 4; i++) {
        task_fire(heavy_work, &ids[i]);
    }

    printf("--- running on 4 cores ---\n");
    runtime_run_parallel(4);
    printf("all done\n");
    return 0;
}
