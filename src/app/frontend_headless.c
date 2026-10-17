/* SPDX-License-Identifier: GPL-3.0-or-later */
/* frontend_headless.c — no display: log each channel's status every 10 s
 * and the events as they happen. */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "channel.h"
#include "events.h"
#include "frontend.h"
#include "log.h"

#define STATUS_SECS 10

int frontend_run(int run_secs, volatile sig_atomic_t *stop)
{
    time_t t0 = time(NULL), last = t0;
    while (!*stop) {
        usleep(200 * 1000);
        int ch;
        char text[40];
        while (events_pop(&ch, text, sizeof text)) (void)0;   /* already logged */
        time_t now = time(NULL);
        if (now - last >= STATUS_SECS) {
            last = now;
            for (int i = 0; i < channels_count(); ++i) {
                channel_status_t st;
                channels_status(i, &st);
                vg_log("CAM%d %s: %s %s %ux%u %u fps, REC %s %llu KiB", i + 1, st.host,
                       st.live ? "live" : st.state, vg_codec_name(st.codec), st.width, st.height,
                       st.fps, st.recording ? "on" : "off", (unsigned long long)(st.rec_bytes >> 10));
            }
        }
        if (run_secs > 0 && now - t0 >= run_secs) break;
    }
    return 0;
}
