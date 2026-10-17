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
    pthread_t       thr;
    int             running;
    volatile int    stop;
    hal_cap_cb      cb;
    void           *user;
    uint8_t        *buf;
    size_t          cap;
    volatile time_t last_data;
} reader_t;

static reader_t g_r[VENC_READER_CHANNELS];

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
    for (HI_U32 i = 0; i < stream.u32PackCount; ++i) {
        VENC_PACK_S *p = &stream.pstPack[i];
        const uint8_t *seg[2] = { p->pu8Addr[0] + p->u32Offset, p->pu8Addr[1] };
        size_t seg_len[2] = { p->u32Len[0] - p->u32Offset, p->pu8Addr[1] ? p->u32Len[1] : 0 };
        for (int k = 0; k < 2; ++k) {
            if (!seg[k] || !seg_len[k] || len + seg_len[k] > AU_MAX) continue;
            if (len + seg_len[k] > r->cap) {
                size_t c = r->cap ? r->cap : 256 * 1024;
                while (c < len + seg_len[k]) c *= 2;
                uint8_t *b = realloc(r->buf, c);
                if (!b) continue;
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

    if (len) {
        vg_au_t au = { .codec = VG_CODEC_H264, .data = r->buf, .len = len, .pts_us = pts };
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
    if (chn < 0 || chn >= VENC_READER_CHANNELS || g_r[chn].running) return -1;
    reader_t *r = &g_r[chn];
    r->cb = cb;
    r->user = user;
    r->stop = 0;
    if (pthread_create(&r->thr, NULL, reader_main, (void *)(intptr_t)chn) != 0) return -1;
    r->running = 1;
    return 0;
}

void venc_reader_stop(int chn)
{
    if (chn < 0 || chn >= VENC_READER_CHANNELS || !g_r[chn].running) return;
    g_r[chn].stop = 1;
    pthread_join(g_r[chn].thr, NULL);
    g_r[chn].running = 0;
    free(g_r[chn].buf);
    g_r[chn].buf = NULL;
    g_r[chn].cap = 0;
}

int venc_reader_active(int chn)
{
    return chn >= 0 && chn < VENC_READER_CHANNELS && time(NULL) - g_r[chn].last_data < 2;
}
