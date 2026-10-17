/* SPDX-License-Identifier: GPL-3.0-or-later */
/* events.h — short user-facing events (camera online/offline/...) from any
 * thread to the UI thread. */
#ifndef EVENTS_H
#define EVENTS_H

#include <stddef.h>

/* Queue an event for channel `ch` (1-based; 0 = system). Thread-safe;
 * the oldest event is dropped when the queue is full. */
void events_push(int ch, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* Next event: 1 with `ch` and `text`, 0 when the queue is empty. */
int events_pop(int *ch, char *text, size_t cap);

#endif /* EVENTS_H */
