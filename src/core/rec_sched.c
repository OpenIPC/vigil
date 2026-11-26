/* SPDX-License-Identifier: GPL-3.0-or-later */
/* rec_sched.c — the recording schedule (see rec_sched.h). */
#include <pthread.h>
#include <string.h>

#include "channel.h"
#include "rec_sched.h"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static rec_week_t      g_weeks[CHANNEL_MAX];

void rec_sched_set(const rec_week_t *weeks, int n)
{
    pthread_mutex_lock(&g_lock);
    memset(g_weeks, 0, sizeof g_weeks);
    for (int i = 0; i < n && i < CHANNEL_MAX; ++i) g_weeks[i] = weeks[i];
    pthread_mutex_unlock(&g_lock);
}

char rec_week_at(const rec_week_t *w, const struct tm *tm)
{
    if (!w->set) return REC_SCHED_CONT;
    int day = (tm->tm_wday + 6) % 7;                    /* Monday first */
    int slot = tm->tm_hour * 2 + (tm->tm_min >= 30);
    char c = w->day[day][slot];
    return c == REC_SCHED_OFF || c == REC_SCHED_MOTION ? c : REC_SCHED_CONT;
}

char rec_sched_mode(int idx, time_t now)
{
    if (idx < 0 || idx >= CHANNEL_MAX) return REC_SCHED_CONT;
    struct tm tm;
    localtime_r(&now, &tm);
    pthread_mutex_lock(&g_lock);
    char c = rec_week_at(&g_weeks[idx], &tm);
    pthread_mutex_unlock(&g_lock);
    return c;
}
