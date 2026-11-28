/* SPDX-License-Identifier: GPL-3.0-or-later */
/* alarm_feed.c — the alarm log from what the channels and the events ring
 * say (logs.h, alarm_feed_poll). */
#include <stdio.h>
#include <string.h>

#include "channel.h"
#include "events.h"
#include "logs.h"

int alarm_types(const char **out, int max)
{
    static const char *const own[] = { ALARM_OFFLINE, ALARM_REC_FAILED, ALARM_REC_OFF, ALARM_DISK_FULL,
                                       ALARM_DISK_ERROR, ALARM_DISK_SMART, ALARM_DISK_BAD, ALARM_NO_DECODER };
    int n = 0;
    for (int b = 0; n < max && ch_event_name(b); ++b) out[n++] = ch_event_name(b);
    for (size_t i = 0; n < max && i < sizeof own / sizeof own[0]; ++i) out[n++] = own[i];
    return n;
}

static int starts(const char *s, const char *p) { return !strncmp(s, p, strlen(p)); }

int alarm_feed_classify(int ch, const char *text, char *type, size_t cap, int *active)
{
    const char *t = NULL;
    int a = -1;
    if (ch > 0) {
        if (!strcmp(text, "down") || !strcmp(text, "offline")) t = ALARM_OFFLINE, a = 1;
        else if (!strcmp(text, "online") || !strcmp(text, "removed")) t = ALARM_OFFLINE, a = 0;
        else if (!strcmp(text, "REC failed")) t = ALARM_REC_FAILED;
        else if (!strcmp(text, "REC disk full")) t = ALARM_DISK_FULL, a = 1;
        else if (!strcmp(text, "REC on")) t = ALARM_REC_OFF, a = 0;     /* and the disk is not full */
        else if (!strcmp(text, "REC waiting for disk") || !strcmp(text, "REC not a disk") ||
                 !strcmp(text, "REC read-only") || !strcmp(text, "REC error"))
            t = ALARM_REC_OFF, a = 1;
        else if (!strcmp(text, "no decoder")) t = ALARM_NO_DECODER;
    } else {
        if (starts(text, "disk ") && strstr(text, "format failed")) t = ALARM_DISK_ERROR;
    }
    if (!t) return 0;
    snprintf(type, cap, "%s", t);
    *active = a;
    return 1;
}

static struct {
    uint64_t cursor;
    unsigned bits[CHANNEL_MAX];     /* the events held at the last look */
    char     off[CHANNEL_MAX];      /* offline said */
    char     full[CHANNEL_MAX];     /* disk full said */
    char     rec_off[CHANNEL_MAX][24];  /* recording off said, why */
} F;

static void feed_push(int ch, const char *type, int active, const char *detail, time_t when)
{
    logrec_t r;
    memset(&r, 0, sizeof r);
    r.time = when;
    r.ch = ch;
    r.active = active;
    snprintf(r.source, sizeof r.source, "vigild");
    snprintf(r.type, sizeof r.type, "%s", type);
    snprintf(r.detail, sizeof r.detail, "%s", detail ? detail : "");
    logbook_push(logs_alarms(), &r);
}

void alarm_feed_poll(time_t now)
{
    (void)now;
    /* The cameras' events: their edges, as the channels hold them. */
    for (int i = 0; i < CHANNEL_MAX; ++i) {
        channel_status_t st;
        channels_status(i, &st);
        unsigned bits = st.configured ? st.events : 0, was = F.bits[i];
        for (int b = 0; b < CH_EV_TYPES; ++b) {
            unsigned m = 1u << b;
            if ((bits ^ was) & m) feed_push(i + 1, ch_event_name(b), (bits & m) != 0, NULL, 0);
        }
        F.bits[i] = bits;
    }
    /* What the events ring says: offline and back, recording, disks. */
    event_t ev;
    while (events_next(&F.cursor, &ev)) {
        char type[LOG_TYPE];
        int active, ch = ev.ch;
        if (!alarm_feed_classify(ch, ev.text, type, sizeof type, &active)) continue;
        int k = ch - 1;
        char detail[LOG_DETAIL] = "";
        if (ch > 0 && k < CHANNEL_MAX) {
            if (!strcmp(type, ALARM_OFFLINE)) {
                if (active == F.off[k]) continue;       /* said already (or never offline) */
                F.off[k] = (char)active;
                channel_status_t st;
                channels_status(k, &st);
                snprintf(detail, sizeof detail, "%s%s%s", active ? st.host : ev.text, active && st.main_state[0] ? ": " : "",
                         active ? st.main_state : "");
            } else if (!strcmp(type, ALARM_DISK_FULL)) {
                if (F.full[k]) continue;
                F.full[k] = 1;
            } else if (!strcmp(type, ALARM_REC_OFF)) {
                if (!active) {
                    if (F.full[k]) feed_push(ch, ALARM_DISK_FULL, 0, "room again", ev.time);
                    F.full[k] = 0;
                    if (!F.rec_off[k][0]) continue;
                    F.rec_off[k][0] = 0;
                    snprintf(detail, sizeof detail, "recording");
                } else {
                    const char *why = ev.text + 4;      /* after "REC " */
                    if (!strcmp(F.rec_off[k], why)) continue;
                    snprintf(F.rec_off[k], sizeof F.rec_off[k], "%s", why);
                    snprintf(detail, sizeof detail, "%s", why);
                }
            }
        } else if (ch == 0) {
            snprintf(detail, sizeof detail, "%s", ev.text);
        }
        feed_push(ch, type, active, detail, ev.time);
    }
}
