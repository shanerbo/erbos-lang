#include <stdio.h>
#include "runtime.h"
#include "channel.h"

static Channel *ch;

static void producer(void *arg) {
    int id = *(int *)arg;
    for (int i = 0; i < 5; i++) {
        int64_t val = id * 100 + i;
        printf("producer %d: sending %lld\n", id, val);
        chan_send(ch, val);
        task_yield();
    }
}

static void consumer(void *arg) {
    (void)arg;
    for (int i = 0; i < 10; i++) {
        int64_t val = chan_recv(ch);
        printf("consumer: got %lld\n", val);
        task_yield();
    }
}

int main(void) {
    runtime_init();
    ch = chan_new();

    int a = 1, b = 2;
    task_fire(producer, &a);
    task_fire(producer, &b);
    task_fire(consumer, NULL);

    printf("--- single-threaded ---\n");
    runtime_run();

    printf("\nall done\n");
    return 0;
}
