#ifndef CHANNEL_H
#define CHANNEL_H

#include <stdint.h>

#define CHAN_BUF_SIZE 64

typedef struct {
    int64_t buf[CHAN_BUF_SIZE];
    int head;
    int tail;
    int count;
    int closed;
    // Waiting task IDs (-1 = none)
    int sender_waiting;
    int receiver_waiting;
} Channel;

Channel *chan_new(void);
void chan_send(Channel *ch, int64_t val);
int64_t chan_recv(Channel *ch);
void chan_close(Channel *ch);

#endif
