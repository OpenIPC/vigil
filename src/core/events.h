/* SPDX-License-Identifier: GPL-3.0-or-later */
/* events.h — short user-facing events (camera online/offline/...) from any
 * thread to every reader: the UI's event strip, the headless loop and the
 * API's WebSocket clients.
 *
 * One ring, many readers: each reader keeps its own cursor, so reading
 * never takes an event away from another reader. A reader that falls more
 * than EVENTS_KEEP events behind skips to the oldest one still kept and is
 * told how many it missed. */
#ifndef EVENTS_H
#define EVENTS_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define EVENTS_KEEP 64

typedef struct {
    uint64_t seq;               /* 1, 2, ... */
    int      ch;                /* 1-based camera; 0 = system */
    time_t   time;
    char     text[40];
    unsigned missed;            /* events lost before this one, for this reader */
} event_t;

/* Queue an event for channel `ch` (1-based; 0 = system). Thread-safe. */
void events_push(int ch, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* The next event after *cursor (start at 0 for every kept event, or at
 * events_head() for new ones only): 1 with `ev` and the cursor advanced,
 * 0 when there is none yet. */
int events_next(uint64_t *cursor, event_t *ev);

/* Cursor of the newest event. */
uint64_t events_head(void);

#endif /* EVENTS_H */
