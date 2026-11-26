/* SPDX-License-Identifier: GPL-3.0-or-later */
/* events.c — event ring (see events.h). */
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>

#include "events.h"

static event_t         g_ring[EVENTS_KEEP];
static uint64_t        g_last;          /* seq of the newest event, 0 = none */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

void events_push(int ch, const char *fmt, ...)
{
    char text[sizeof g_ring[0].text];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&g_lock);
    event_t *e = &g_ring[g_last % EVENTS_KEEP];
    e->seq = ++g_last;
    e->ch = ch;
    e->time = time(NULL);
    e->missed = 0;
    snprintf(e->text, sizeof e->text, "%s", text);
    pthread_mutex_unlock(&g_lock);
}

int events_next(uint64_t *cursor, event_t *ev)
{
    pthread_mutex_lock(&g_lock);
    int got = *cursor < g_last;
    if (got) {
        uint64_t oldest = g_last > EVENTS_KEEP ? g_last - EVENTS_KEEP + 1 : 1;
        uint64_t want = *cursor + 1 < oldest ? oldest : *cursor + 1;
        *ev = g_ring[(want - 1) % EVENTS_KEEP];
        ev->missed = (unsigned)(want - (*cursor + 1));
        *cursor = want;
    }
    pthread_mutex_unlock(&g_lock);
    return got;
}

uint64_t events_head(void)
{
    pthread_mutex_lock(&g_lock);
    uint64_t h = g_last;
    pthread_mutex_unlock(&g_lock);
    return h;
}
