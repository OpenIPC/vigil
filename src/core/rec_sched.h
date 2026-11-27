/* SPDX-License-Identifier: GPL-3.0-or-later */
/* rec_sched.h — when each camera records: a week of half hours.
 *
 * A camera's week is 7 days (Monday first) of 48 letters, one per half
 * hour from 00:00 local time (record.schedule in the configuration):
 *
 *   C  continuous: record
 *   M  motion only: record while the camera reports motion
 *      (channel_status_t.events; no source reports it yet)
 *   -  off
 *
 * A camera without a week records all the time. The table is replaced as
 * a whole when the configuration changes and read by the channel threads;
 * changing it restarts nothing.
 */
#ifndef REC_SCHED_H
#define REC_SCHED_H

#include <time.h>

#define REC_SCHED_SLOTS 48

enum { REC_SCHED_OFF = '-', REC_SCHED_CONT = 'C', REC_SCHED_MOTION = 'M' };

typedef struct {
    int  set;                               /* 0: always C */
    char day[7][REC_SCHED_SLOTS + 1];       /* mon .. sun */
} rec_week_t;

/* Replace the whole table: weeks[i] for channel i, `n` of them (the rest:
 * always on). Any thread. */
void rec_sched_set(const rec_week_t *weeks, int n);

/* The letter for channel `idx` (0-based) at `now` in local time. */
char rec_sched_mode(int idx, time_t now);

/* The letter of `w` for a broken-down local time (tm_wday 0 = Sunday). */
char rec_week_at(const rec_week_t *w, const struct tm *tm);

#endif /* REC_SCHED_H */
