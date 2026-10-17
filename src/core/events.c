/* SPDX-License-Identifier: GPL-3.0-or-later */
/* events.c — event queue (see events.h). */
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>

#include "events.h"

#define EVQ_LEN 16

static struct { int ch; char text[40]; } g_q[EVQ_LEN];
static int             g_head, g_n;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

void events_push(int ch, const char *fmt, ...)
{
    char text[40];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&g_lock);
    int slot = (g_head + g_n) % EVQ_LEN;
    if (g_n == EVQ_LEN) g_head = (g_head + 1) % EVQ_LEN;
    else ++g_n;
    g_q[slot].ch = ch;
    snprintf(g_q[slot].text, sizeof g_q[slot].text, "%s", text);
    pthread_mutex_unlock(&g_lock);
}

int events_pop(int *ch, char *text, size_t cap)
{
    pthread_mutex_lock(&g_lock);
    int got = g_n > 0;
    if (got) {
        *ch = g_q[g_head].ch;
        snprintf(text, cap, "%s", g_q[g_head].text);
        g_head = (g_head + 1) % EVQ_LEN;
        --g_n;
    }
    pthread_mutex_unlock(&g_lock);
    return got;
}
