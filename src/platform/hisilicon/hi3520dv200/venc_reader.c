/* SPDX-License-Identifier: GPL-3.0-or-later */
/* venc_reader.c — see venc_reader.h.
 *
 * One thread per VENC channel: select() on HI_MPI_VENC_GetFd with a
 * 200 ms timeout (so a stop is seen quickly), then Query -> GetStream ->
 * copy the packs (each may wrap: pu8Addr[0] + u32Offset .. u32Len[0], then
 * pu8Addr[1] .. u32Len[1]) into one Annex-B access unit -> callback ->
 * ReleaseStream. Keyframes are recognised downstream from the NAL types
 * (au_norm), so no VENC-specific frame type is needed here.
 *
 * Known limitation on the lab DVR (hi3520dv200, MPP V2.0.5.1): VENC channel
 * creation fails with 0xa007800c (NOMEM) because the h264e module allocates
 * buffers libmpi then tries to mmap; see hi_video_pipe.c. Until that is
 * solved no thread here ever receives data.
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/select.h>

#include "hi_type.h"
#include "hi_comm_venc.h"
#include "mpi_venc.h"

#include "venc_reader.h"

#define SELECT_MS  200
#define AU_MAX     (2u << 20)

typedef struct {
    pthread_mutex_t lock;           /* start/stop of this reader */
    pthread_t       thr;
    int             running;
    int             discont;        /* an AU was dropped: flag the next one */
    volatile int    stop;
    hal_cap_cb      cb;
    void           *user;
    uint8_t        *buf;
    size_t          cap;
    volatile time_t last_data;
} reader_t;

static reader_t g_r[VENC_READER_CHANNELS] = {
    [0 ... VENC_READER_CHANNELS - 1] = { .lock = PTHREAD_MUTEX_INITIALIZER },
};

static void drain_one(int chn, reader_t *r)
{
    VENC_CHN_STAT_S stat;
    if (HI_MPI_VENC_Query(chn, &stat) != HI_SUCCESS || stat.u32CurPacks == 0) return;

    VENC_STREAM_S stream;
    memset(&stream, 0, sizeof stream);
    stream.pstPack = calloc(stat.u32CurPacks, sizeof(VENC_PACK_S));
    if (!stream.pstPack) return;
    stream.u32PackCount = stat.u32CurPacks;
    if (HI_MPI_VENC_GetStream(chn, &stream, HI_FALSE) != HI_SUCCESS) {
        free(stream.pstPack);
        return;
    }

    size_t len = 0;
    uint64_t pts = 0;
    int whole = 1;                  /* every pack fitted */
    for (HI_U32 i = 0; i < stream.u32PackCount && whole; ++i) {
        VENC_PACK_S *p = &stream.pstPack[i];
        const uint8_t *seg[2] = { p->pu8Addr[0] + p->u32Offset, p->pu8Addr[1] };
        size_t seg_len[2] = { p->u32Len[0] - p->u32Offset, p->pu8Addr[1] ? p->u32Len[1] : 0 };
        for (int k = 0; k < 2 && whole; ++k) {
            if (!seg[k] || !seg_len[k]) continue;
            if (len + seg_len[k] > AU_MAX) { whole = 0; break; }
            if (len + seg_len[k] > r->cap) {
                size_t c = r->cap ? r->cap : 256 * 1024;
                while (c < len + seg_len[k]) c *= 2;
                uint8_t *b = realloc(r->buf, c);
                if (!b) { whole = 0; break; }
                r->buf = b;
                r->cap = c;
            }
            memcpy(r->buf + len, seg[k], seg_len[k]);
            len += seg_len[k];
        }
        pts = p->u64PTS;
    }
    HI_MPI_VENC_ReleaseStream(chn, &stream);
    free(stream.pstPack);

    /* A frame missing a pack is not a frame: drop it, and flag the next one
     * so nothing that depends on it is used before a keyframe. */
    if (!whole) {
        r->discont = 1;
        return;
    }
    if (len) {
        vg_au_t au = { .codec = VG_CODEC_H264, .data = r->buf, .len = len, .pts_us = pts,
                       .discont = r->discont };
        r->discont = 0;
        r->last_data = time(NULL);
        r->cb(r->user, &au);
    }
}

static void *reader_main(void *arg)
{
    int chn = (int)(intptr_t)arg;
    reader_t *r = &g_r[chn];
    HI_S32 fd = HI_MPI_VENC_GetFd(chn);
    if (fd < 0) {
        fprintf(stderr, "hi3520dv200: VENC_GetFd(%d) 0x%x\n", chn, fd);
        return NULL;
    }
    while (!r->stop) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        struct timeval tv = { .tv_sec = 0, .tv_usec = SELECT_MS * 1000 };
        int rs = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (rs < 0 && errno != EINTR) break;
        if (rs > 0) drain_one(chn, r);
    }
    return NULL;
}

int venc_reader_start(int chn, hal_cap_cb cb, void *user)
{
    if (chn < 0 || chn >= VENC_READER_CHANNELS) return -1;
    reader_t *r = &g_r[chn];
    pthread_mutex_lock(&r->lock);
    int rc = -1;
    if (!r->running) {
        r->cb = cb;
        r->user = user;
        r->stop = 0;
        r->discont = 0;
        if (pthread_create(&r->thr, NULL, reader_main, (void *)(intptr_t)chn) == 0) {
            r->running = 1;
            rc = 0;
        }
    }
    pthread_mutex_unlock(&r->lock);
    return rc;
}

void venc_reader_stop(int chn)
{
    if (chn < 0 || chn >= VENC_READER_CHANNELS) return;
    reader_t *r = &g_r[chn];
    /* Under the lock: a stop that races a start waits for it, then joins
     * the thread it created, so the callback never outlives its source. */
    pthread_mutex_lock(&r->lock);
    if (r->running) {
        r->stop = 1;
        pthread_join(r->thr, NULL);
        r->running = 0;
        free(r->buf);
        r->buf = NULL;
        r->cap = 0;
    }
    pthread_mutex_unlock(&r->lock);
}

int venc_reader_active(int chn)
{
    return chn >= 0 && chn < VENC_READER_CHANNELS && time(NULL) - g_r[chn].last_data < 2;
}
