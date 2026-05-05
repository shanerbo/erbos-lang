#include <stdlib.h>
#include <stdio.h>
#include "channel.h"
#include "runtime.h"

Channel *chan_new(void) {
    Channel *ch = calloc(1, sizeof(Channel));
    ch->sender_waiting = -1;
    ch->receiver_waiting = -1;
    return ch;
}

void chan_send(Channel *ch, int64_t val) {
    // Block until there's space
    while (ch->count >= CHAN_BUF_SIZE) {
        task_yield();
    }
    ch->buf[ch->tail] = val;
    ch->tail = (ch->tail + 1) % CHAN_BUF_SIZE;
    ch->count++;
}

int64_t chan_recv(Channel *ch) {
    // Block until there's data
    while (ch->count == 0) {
        if (ch->closed) return 0;
        task_yield();
    }
    int64_t val = ch->buf[ch->head];
    ch->head = (ch->head + 1) % CHAN_BUF_SIZE;
    ch->count--;
    return val;
}

void chan_close(Channel *ch) {
    ch->closed = 1;
}
