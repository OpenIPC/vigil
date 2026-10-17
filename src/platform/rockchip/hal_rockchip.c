/* SPDX-License-Identifier: GPL-3.0-or-later */
/* hal_rockchip.c — Rockchip (RK3588 / RK356x) with the vendor kernel:
 * MPP hardware decoder per channel, RGA to compose, DRM/KMS to show.
 *
 *   channel thread:  hal_dec_send -> MPP decode -> newest frame into the
 *                    channel's slot (older ones are released)
 *   compositor:      ~30 times a second, RGA scales each slot's frame into
 *                    its tile of the back NV12 mosaic (aspect kept, black
 *                    bars), then flips the mosaic onto the video plane
 *
 * Libraries: rockchip-linux/mpp and librga, built from source by
 * scripts/rockchip/build-deps.sh.
 *
 * Headless check: SIGUSR1 makes the next flip also capture the composed
 * output through the writeback connector into $VIGIL_SNAPSHOT (default
 * /tmp/vigil-snapshot.argb, raw ARGB8888).
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <rockchip/rk_mpi.h>
#include <rga/im2d.h>
#include <rga/rga.h>

#include "hal.h"
#include "hal_capture.h"
#include "hal_decoder.h"
#include "hal_display.h"
#include "rk_display.h"

#define MAX_CH        4                 /* quad view */
#define COMPOSE_HZ    30

static const hal_caps_t g_caps = {
    .name = "rockchip", .display = 1, .decoder = 1,
    .dec_max_w = 8192, .dec_max_h = 4320,
    .max_channels = MAX_CH,
};

typedef struct {
    /* decoder (channel thread) */
    MppCtx           ctx;
    MppApi          *mpi;
    MppBufferGroup   grp;
    vg_codec_t       codec;
    /* slot shared with the compositor */
    pthread_mutex_t  lock;
    MppFrame         frame;             /* newest decoded picture, or NULL */
    int              fresh;             /* not composed yet */
    vg_rect_t        tile;              /* screen rectangle */
    int              tile_set;
    int              clear;             /* tile background needs repainting */
    unsigned         last_w, last_h;
} chan_t;

static chan_t          g_ch[MAX_CH];
static pthread_t       g_comp;
static volatile int    g_comp_stop;
static int             g_comp_up;
static volatile sig_atomic_t g_shot;

const hal_caps_t *hal_caps(void) { return &g_caps; }

static void on_usr1(int s) { (void)s; g_shot = 1; }

/* ---- compositor ---------------------------------------------------------- */

/* Paint a tile black in one mosaic buffer (Y 16, UV 128). */
static void tile_clear(rk_buf_t *b, const vg_rect_t *r)
{
    for (int y = 0; y < r->h; ++y)
        memset(b->map + (size_t)(r->y + y) * b->pitch + r->x, 16, (size_t)r->w);
    uint8_t *uv = b->map + (size_t)b->pitch * b->height;
    for (int y = 0; y < r->h / 2; ++y)
        memset(uv + (size_t)(r->y / 2 + y) * b->pitch + r->x, 128, (size_t)r->w);
}

/* The largest rectangle with the picture's aspect inside the tile, centred,
 * even-aligned for NV12. */
static vg_rect_t fit(const vg_rect_t *t, unsigned w, unsigned h)
{
    vg_rect_t r = *t;
    if ((uint64_t)w * (unsigned)t->h > (uint64_t)h * (unsigned)t->w) {
        r.h = (int)((uint64_t)t->w * h / w) & ~1;
        r.y = t->y + ((t->h - r.h) / 2 & ~1);
    } else {
        r.w = (int)((uint64_t)t->h * w / h) & ~1;
        r.x = t->x + ((t->w - r.w) / 2 & ~1);
    }
    return r;
}

static void compose_one(chan_t *c, rk_buf_t *dst)
{
    MppFrame f = c->frame;
    unsigned w = mpp_frame_get_width(f), h = mpp_frame_get_height(f);
    unsigned hs = mpp_frame_get_hor_stride(f), vs = mpp_frame_get_ver_stride(f);
    MppBuffer buf = mpp_frame_get_buffer(f);
    if (!buf || !w || !h) return;
    int fd = mpp_buffer_get_fd(buf);
    if (w != c->last_w || h != c->last_h) {         /* new aspect: repaint bars */
        c->last_w = w;
        c->last_h = h;
        c->clear = 2;                               /* both mosaic buffers */
    }
    if (c->clear) {
        tile_clear(dst, &c->tile);
        --c->clear;
    }
    vg_rect_t d = fit(&c->tile, w, h);
    rga_buffer_t src = wrapbuffer_fd_t(fd, (int)w, (int)h, (int)hs, (int)vs, RK_FORMAT_YCbCr_420_SP);
    rga_buffer_t out = wrapbuffer_fd_t(dst->fd, (int)dst->width, (int)dst->height,
                                       (int)dst->pitch, (int)dst->height, RK_FORMAT_YCbCr_420_SP);
    rga_buffer_t pat;
    memset(&pat, 0, sizeof pat);
    im_rect srect = { 0, 0, (int)w, (int)h }, drect = { d.x, d.y, d.w, d.h }, prect = { 0, 0, 0, 0 };
    IM_STATUS st = improcess(src, out, pat, srect, drect, prect, IM_SYNC);
    if (st != IM_STATUS_SUCCESS && st != IM_STATUS_NOERROR)
        fprintf(stderr, "rockchip: RGA compose failed: %s\n", imStrError_t(st));
}

static void *compositor(void *arg)
{
    (void)arg;
    const char *shot = getenv("VIGIL_SNAPSHOT");
    int flip_errs = 0;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    while (!g_comp_stop) {
        rk_buf_t *back = rk_display_back();
        for (int i = 0; i < MAX_CH; ++i) {
            chan_t *c = &g_ch[i];
            pthread_mutex_lock(&c->lock);
            /* Both mosaic buffers must get each new picture, so a frame is
             * composed twice (once per buffer) before it is stale. */
            if (c->tile_set && c->frame && c->fresh) {
                compose_one(c, back);
                --c->fresh;
            } else if (c->tile_set && c->clear) {
                tile_clear(back, &c->tile);
                --c->clear;
            }
            pthread_mutex_unlock(&c->lock);
        }
        if (g_shot) {
            g_shot = 0;
            rk_display_request_shot(shot ? shot : "/tmp/vigil-snapshot.argb");
        }
        if (rk_display_flip() < 0) {
            /* Keep composing; say so at most every 10 s. */
            if (++flip_errs == 1 || flip_errs % (COMPOSE_HZ * 10) == 0)
                fprintf(stderr, "rockchip: display flip failed (%d so far)\n", flip_errs);
        }
        next.tv_nsec += 1000000000L / COMPOSE_HZ;
        if (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; ++next.tv_sec; }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }
    return NULL;
}

/* ---- platform ---------------------------------------------------------------- */

int hal_init(unsigned w, unsigned h, unsigned fps)
{
    (void)fps;
    for (int i = 0; i < MAX_CH; ++i) pthread_mutex_init(&g_ch[i].lock, NULL);
    if (rk_display_open(w, h) < 0) return -1;
    signal(SIGUSR1, on_usr1);
    g_comp_stop = 0;
    if (pthread_create(&g_comp, NULL, compositor, NULL) != 0) {
        rk_display_close();
        return -1;
    }
    g_comp_up = 1;
    return 0;
}

void hal_shutdown(void)
{
    for (int i = 0; i < MAX_CH; ++i) hal_dec_close(i);
    if (g_comp_up) {
        g_comp_stop = 1;
        pthread_join(g_comp, NULL);
        g_comp_up = 0;
    }
    rk_display_close();
}

int hal_display_set_tile(int ch, const vg_rect_t *r)
{
    if (ch < 0 || ch >= MAX_CH) return -1;
    pthread_mutex_lock(&g_ch[ch].lock);
    g_ch[ch].tile = *r;
    g_ch[ch].tile.x &= ~1;
    g_ch[ch].tile.y &= ~1;
    g_ch[ch].tile.w &= ~1;
    g_ch[ch].tile.h &= ~1;
    g_ch[ch].tile_set = 1;
    g_ch[ch].clear = 2;
    pthread_mutex_unlock(&g_ch[ch].lock);
    return 0;
}

void hal_display_clear_tile(int ch)
{
    if (ch < 0 || ch >= MAX_CH) return;
    pthread_mutex_lock(&g_ch[ch].lock);
    g_ch[ch].clear = 2;
    pthread_mutex_unlock(&g_ch[ch].lock);
}

/* ---- decoder ------------------------------------------------------------------ */

static void slot_put(chan_t *c, MppFrame f)
{
    pthread_mutex_lock(&c->lock);
    if (c->frame) mpp_frame_deinit(&c->frame);
    c->frame = f;
    c->fresh = 2;
    pthread_mutex_unlock(&c->lock);
}

void hal_dec_close(int ch)
{
    if (ch < 0 || ch >= MAX_CH) return;
    chan_t *c = &g_ch[ch];
    pthread_mutex_lock(&c->lock);
    if (c->frame) mpp_frame_deinit(&c->frame);
    c->frame = NULL;
    c->clear = 2;
    pthread_mutex_unlock(&c->lock);
    if (c->ctx) {
        c->mpi->reset(c->ctx);
        mpp_destroy(c->ctx);
        c->ctx = NULL;
    }
    if (c->grp) {
        mpp_buffer_group_put(c->grp);
        c->grp = NULL;
    }
    c->codec = VG_CODEC_NONE;
}

int hal_dec_open(int ch, vg_codec_t codec, const vg_stream_info_t *si)
{
    if (ch < 0 || ch >= MAX_CH || codec == VG_CODEC_NONE) return -1;
    if (si->width > g_caps.dec_max_w || si->height > g_caps.dec_max_h) return -1;
    hal_dec_close(ch);
    chan_t *c = &g_ch[ch];
    if (mpp_create(&c->ctx, &c->mpi) != MPP_OK) { c->ctx = NULL; return -1; }
    /* Whole access units in, frames out without blocking. */
    RK_U32 split = 0;
    c->mpi->control(c->ctx, MPP_DEC_SET_PARSER_SPLIT_MODE, &split);
    MppPollType nb = MPP_POLL_NON_BLOCK;
    c->mpi->control(c->ctx, MPP_SET_OUTPUT_TIMEOUT, &nb);
    MppCodingType coding = codec == VG_CODEC_H264 ? MPP_VIDEO_CodingAVC : MPP_VIDEO_CodingHEVC;
    if (mpp_init(c->ctx, MPP_CTX_DEC, coding) != MPP_OK) {
        mpp_destroy(c->ctx);
        c->ctx = NULL;
        return -1;
    }
    c->codec = codec;
    fprintf(stderr, "rockchip: decoder %d: %s %ux%u\n", ch,
            codec == VG_CODEC_H264 ? "H.264" : "H.265", si->width, si->height);
    return 0;
}

static void drain(chan_t *c)
{
    for (;;) {
        MppFrame f = NULL;
        if (c->mpi->decode_get_frame(c->ctx, &f) != MPP_OK || !f) return;
        if (mpp_frame_get_info_change(f)) {
            /* Picture buffers: DRM dmabufs (RGA reads them by fd) from an
             * internal group, allocated by MPP for the new size. */
            if (!c->grp && mpp_buffer_group_get_internal(&c->grp, MPP_BUFFER_TYPE_DRM) != MPP_OK) {
                mpp_frame_deinit(&f);
                return;
            }
            c->mpi->control(c->ctx, MPP_DEC_SET_EXT_BUF_GROUP, c->grp);
            c->mpi->control(c->ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);
            mpp_frame_deinit(&f);
            continue;
        }
        if (mpp_frame_get_errinfo(f) || mpp_frame_get_discard(f) || !mpp_frame_get_buffer(f)) {
            mpp_frame_deinit(&f);
            continue;
        }
        slot_put(c, f);
    }
}

int hal_dec_send(int ch, const vg_au_t *au)
{
    if (ch < 0 || ch >= MAX_CH || !g_ch[ch].ctx) return -1;
    chan_t *c = &g_ch[ch];
    MppPacket pkt = NULL;
    if (mpp_packet_init(&pkt, (void *)au->data, au->len) != MPP_OK) return -1;
    mpp_packet_set_pts(pkt, (RK_S64)au->pts_us);
    int rc = -1;
    for (int tries = 0; tries < 50; ++tries) {          /* <= ~100 ms */
        MPP_RET r = c->mpi->decode_put_packet(c->ctx, pkt);
        drain(c);
        if (r == MPP_OK) { rc = 0; break; }
        if (r != MPP_ERR_BUFFER_FULL) break;
        usleep(2000);
    }
    mpp_packet_deinit(&pkt);
    return rc;
}

void hal_dec_reset(int ch)
{
    if (ch < 0 || ch >= MAX_CH || !g_ch[ch].ctx) return;
    chan_t *c = &g_ch[ch];
    c->mpi->reset(c->ctx);
    /* Do not keep showing the picture from before the reconnect. */
    pthread_mutex_lock(&c->lock);
    if (c->frame) mpp_frame_deinit(&c->frame);
    c->frame = NULL;
    c->fresh = 0;
    c->clear = 2;                                   /* both mosaic buffers */
    pthread_mutex_unlock(&c->lock);
}

/* ---- capture: none --------------------------------------------------------------- */

int  hal_cap_start(int in, int ch, hal_cap_cb cb, void *user) { (void)in; (void)ch; (void)cb; (void)user; return -1; }
void hal_cap_stop(int in) { (void)in; }
int  hal_cap_signal(int in) { (void)in; return 0; }
