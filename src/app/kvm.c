/* SPDX-License-Identifier: GPL-3.0-or-later */
/* kvm.c — the zero channel on the UI's display (kvm.h): the screen as the
 * monitor shows it, and a mouse and keyboard plugged into the UI.
 *
 * The picture is put together here, in software, because the SoCs have
 * nothing that reads the output back (SigmaStar's i2m display has no
 * writeback: hal_sigmastar.c). Two layers, as the hardware mixes them:
 *
 *   - the UI: LVGL's own pixels, copied as it flushes each area it drew
 *     (LV_EVENT_FLUSH_START, on the LVGL thread) into a copy of the
 *     screen, so nothing is read from the framebuffer (uncached on these
 *     boards) and an unchanged UI costs nothing;
 *   - the video below it, where the UI has its holes (the platform's
 *     colour key, or transparency: hal_ui_hole), drawn by the platform
 *     from its decoders' pictures (hal_display_grab).
 *
 * The UI is kept as YUV with each pixel's coverage, converted only where
 * it changed; each picture is the video (BT.601 video range, expanded to
 * the full range JPEG has) under it. A viewer then gets only the blocks
 * that changed since its last picture (kvm_enc.c).
 *
 * Nothing runs while no viewer is connected: the flush hook returns at
 * once, the buffers (about 9 MiB at 1280x720) are freed, and the platform
 * stops keeping pictures aside. A viewer's mouse and keyboard are pipes
 * the evdev reader takes as one more pointer (absolute) and keyboard, so
 * its input goes through the same code as a USB mouse's and keyboard's,
 * and the monitor shows its pointer.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <linux/input.h>

#include "hal_display.h"
#include "hal_ui.h"
#include "input_evdev.h"
#include "jpeg_enc.h"
#include "kvm.h"
#include "kvm_app.h"
#include "kvm_enc.h"
#include "kvm_input.h"
#include "log.h"

#define TICK_MS        30
#define PRIME_WAIT_MS  1000         /* a first picture waits this long for the whole UI */

struct kvm_client {
    int            used;
    char           who[64];
    int            ptr_fd, kbd_fd;  /* the write ends */
    int            scale, quality;
    int            reinit;          /* options changed: a new encoder, a whole picture */
    kvm_enc_t      enc;
    unsigned char *half;            /* the picture at half size (scale 2) */
    unsigned       ui_seen;         /* the UI generation of its last picture */
    struct timespec next;           /* not before */
    unsigned       last_ms;
    size_t         last_bytes;
    int            wake;
};

static struct {
    lv_display_t   *disp;
    unsigned        w, h;
    int             key_mode;       /* holes: the colour key (else alpha) */
    uint32_t        key;            /* 0xRRGGBB */
    lv_timer_t     *timer;
    pthread_mutex_t lock;           /* below, up to comp */
    pthread_cond_t  cond;           /* the UI changed, a client woken */
    kvm_client_t    c[KVM_CLIENTS];
    int             n;
    volatile int    active;         /* buffers allocated: the flush hook copies */
    int             pend[2 * KVM_CLIENTS], pend_kbd[2 * KVM_CLIENTS], n_pend;
    int             want_full, full_pending, primed;
    uint32_t       *ui;             /* the UI, 0xAARRGGBB */
    int             dx0, dy0, dx1, dy1;     /* changed since converted; dx1 == 0: nothing */
    unsigned        ui_gen;
    int             video;          /* the last picture had video (so each is new) */

    pthread_mutex_t comp;           /* the converted UI and the picture */
    unsigned char  *uy, *ua, *uu, *uv;      /* UI: luma x coverage, coverage, chroma x coverage */
    uint16_t       *uw;             /* coverage of each 2x2 (0..1020) */
    unsigned char  *cy, *cu, *cv;   /* the picture */
    unsigned char   lut_y[256], lut_c[256];
} g = { .lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER,
        .comp = PTHREAD_MUTEX_INITIALIZER };

static long ms_until(const struct timespec *t)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (t->tv_sec - now.tv_sec) * 1000L + (t->tv_nsec - now.tv_nsec) / 1000000L;
}

static struct timespec ms_from_now(long ms)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    t.tv_sec += ms / 1000;
    t.tv_nsec += (ms % 1000) * 1000000L;
    if (t.tv_nsec >= 1000000000L) {
        ++t.tv_sec;
        t.tv_nsec -= 1000000000L;
    }
    return t;
}

/* The condition variable runs on CLOCK_MONOTONIC (kvm_attach). */
static int wait_until(const struct timespec *t)
{
    return pthread_cond_timedwait(&g.cond, &g.lock, t);
}

/* ---- the UI, as LVGL draws it (LVGL thread) ---------------------------------------------- */

static void mark(int x0, int y0, int x1, int y1)
{
    if (!g.dx1) {
        g.dx0 = x0, g.dy0 = y0, g.dx1 = x1, g.dy1 = y1;
        return;
    }
    if (x0 < g.dx0) g.dx0 = x0;
    if (y0 < g.dy0) g.dy0 = y0;
    if (x1 > g.dx1) g.dx1 = x1;
    if (y1 > g.dy1) g.dy1 = y1;
}

static void flush_ev(lv_event_t *e)
{
    if (!g.active) return;
    const lv_area_t *a = lv_event_get_param(e);
    lv_draw_buf_t *db = lv_display_get_buf_active(g.disp);
    if (!a || !db || !db->data) return;
    lv_color_format_t cf = db->header.cf;
    int bpp = cf == LV_COLOR_FORMAT_RGB565 ? 2
              : cf == LV_COLOR_FORMAT_ARGB8888 || cf == LV_COLOR_FORMAT_XRGB8888 ? 4 : 0;
    if (!bpp) return;
    /* PARTIAL: the buffer is the area; DIRECT and FULL: the screen. */
    int whole = db->header.w >= g.w && db->header.h >= g.h;
    int x0 = a->x1 < 0 ? 0 : a->x1, y0 = a->y1 < 0 ? 0 : a->y1;
    int x1 = a->x2 + 1 > (int)g.w ? (int)g.w : a->x2 + 1, y1 = a->y2 + 1 > (int)g.h ? (int)g.h : a->y2 + 1;
    if (x0 >= x1 || y0 >= y1) return;
    int ox = whole ? 0 : a->x1, oy = whole ? 0 : a->y1;
    pthread_mutex_lock(&g.lock);
    if (g.ui) {
        for (int y = y0; y < y1; ++y) {
            const uint8_t *src = db->data + (size_t)(y - oy) * db->header.stride + (size_t)(x0 - ox) * bpp;
            uint32_t *dst = g.ui + (size_t)y * g.w + x0;
            if (bpp == 4 && cf == LV_COLOR_FORMAT_ARGB8888) {
                memcpy(dst, src, (size_t)(x1 - x0) * 4);
            } else if (bpp == 4) {
                const uint32_t *s = (const uint32_t *)src;
                for (int x = 0; x < x1 - x0; ++x) dst[x] = s[x] | 0xff000000u;
            } else {
                const uint16_t *s = (const uint16_t *)src;
                for (int x = 0; x < x1 - x0; ++x) {
                    unsigned p = s[x], r = p >> 11, gg = (p >> 5) & 63, b = p & 31;
                    dst[x] = 0xff000000u | ((r << 3 | r >> 2) << 16) | ((gg << 2 | gg >> 4) << 8) | (b << 3 | b >> 2);
                }
            }
        }
        mark(x0, y0, x1, y1);
        ++g.ui_gen;
        pthread_cond_broadcast(&g.cond);
    }
    pthread_mutex_unlock(&g.lock);
}

static void refr_ev(lv_event_t *e)
{
    (void)e;
    pthread_mutex_lock(&g.lock);
    if (g.full_pending) {
        g.full_pending = 0;
        g.primed = 1;
        pthread_cond_broadcast(&g.cond);
    }
    pthread_mutex_unlock(&g.lock);
}

/* New viewers' mouse and keyboard into the evdev reader, and a whole new
 * UI for a first viewer. */
static void tick(lv_timer_t *t)
{
    (void)t;
    int fds[2 * KVM_CLIENTS], kbd[2 * KVM_CLIENTS], n, full;
    pthread_mutex_lock(&g.lock);
    n = g.n_pend;
    memcpy(fds, g.pend, sizeof fds);
    memcpy(kbd, g.pend_kbd, sizeof kbd);
    g.n_pend = 0;
    full = g.want_full;
    g.want_full = 0;
    if (full) g.full_pending = 1;
    pthread_mutex_unlock(&g.lock);
    for (int i = 0; i < n; ++i) {
        int rc = kbd[i] ? input_evdev_attach_keyboard(fds[i], "zero channel keyboard")
                        : input_evdev_attach(fds[i], "zero channel pointer", 1, 0, (int)g.w - 1, 0, (int)g.h - 1);
        if (rc < 0) {
            vg_log("kvm: no room for the zero channel's %s among the input devices", kbd[i] ? "keyboard" : "pointer");
            close(fds[i]);
        }
    }
    if (full) lv_obj_invalidate(lv_screen_active());
}

void kvm_attach(lv_display_t *disp)
{
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_destroy(&g.cond);
    pthread_cond_init(&g.cond, &ca);
    pthread_condattr_destroy(&ca);
    lv_color_t c;
    lv_opa_t o;
    hal_ui_hole(&c, &o);
    pthread_mutex_lock(&g.lock);
    g.disp = disp;
    g.w = (unsigned)lv_display_get_horizontal_resolution(disp) & ~1u;
    g.h = (unsigned)lv_display_get_vertical_resolution(disp) & ~1u;
    g.key_mode = o == LV_OPA_COVER;
    g.key = (uint32_t)c.red << 16 | (uint32_t)c.green << 8 | c.blue;
    pthread_mutex_unlock(&g.lock);
    for (int i = 0; i < 256; ++i) {
        int y = (i - 16) * 255 / 219, u = (i - 128) * 255 / 224 + 128;
        g.lut_y[i] = (unsigned char)(y < 0 ? 0 : y > 255 ? 255 : y);
        g.lut_c[i] = (unsigned char)(u < 0 ? 0 : u > 255 ? 255 : u);
    }
    lv_display_add_event_cb(disp, flush_ev, LV_EVENT_FLUSH_START, NULL);
    lv_display_add_event_cb(disp, refr_ev, LV_EVENT_REFR_READY, NULL);
    g.timer = lv_timer_create(tick, TICK_MS, NULL);
}

void kvm_detach(void)
{
    if (!g.disp) return;
    lv_display_remove_event_cb_with_user_data(g.disp, flush_ev, NULL);
    lv_display_remove_event_cb_with_user_data(g.disp, refr_ev, NULL);
    lv_timer_delete(g.timer);
    pthread_mutex_lock(&g.lock);
    g.disp = NULL;
    g.active = 0;
    for (int i = 0; i < g.n_pend; ++i) close(g.pend[i]);
    g.n_pend = 0;
    pthread_cond_broadcast(&g.cond);
    pthread_mutex_unlock(&g.lock);
}

/* ---- the picture ------------------------------------------------------------------------- */

/* Buffers for the first viewer (lock held). */
static int buffers_alloc(void)
{
    size_t px = (size_t)g.w * g.h, cpx = px / 4;
    g.ui = calloc(px, 4);
    g.uy = malloc(px);
    g.ua = malloc(px);
    g.uu = malloc(cpx);
    g.uv = malloc(cpx);
    g.uw = malloc(cpx * 2);
    g.cy = malloc(px);
    g.cu = malloc(cpx);
    g.cv = malloc(cpx);
    if (!g.ui || !g.uy || !g.ua || !g.uu || !g.uv || !g.uw || !g.cy || !g.cu || !g.cv) return -1;
    /* Until the UI is in: black, as nothing has been drawn. */
    memset(g.uy, 0, px);
    memset(g.ua, 255, px);
    memset(g.uu, 128, cpx);
    memset(g.uv, 128, cpx);
    for (size_t i = 0; i < cpx; ++i) g.uw[i] = 1020;
    g.dx1 = 0;
    return 0;
}

static void buffers_free(void)
{
    free(g.ui);
    free(g.uy);
    free(g.ua);
    free(g.uu);
    free(g.uv);
    free(g.uw);
    free(g.cy);
    free(g.cu);
    free(g.cv);
    g.ui = NULL;
    g.uy = g.ua = g.uu = g.uv = g.cy = g.cu = g.cv = NULL;
    g.uw = NULL;
}

/* The UI where it changed, as YUV and coverage (comp and lock held). */
static void ui_convert(int x0, int y0, int x1, int y1)
{
    x0 &= ~1;
    y0 &= ~1;
    x1 = (x1 + 1) & ~1;
    y1 = (y1 + 1) & ~1;
    unsigned w = g.w, cw = g.w / 2;
    for (int y = y0; y < y1; y += 2) {
        for (int x = x0; x < x1; x += 2) {
            int su = 0, sv = 0, sa = 0;
            for (int k = 0; k < 4; ++k) {
                size_t i = (size_t)(y + (k >> 1)) * w + (unsigned)(x + (k & 1));
                uint32_t p = g.ui[i];
                int r = (int)(p >> 16 & 255), gg = (int)(p >> 8 & 255), b = (int)(p & 255);
                int a = g.key_mode ? ((p & 0xffffffu) == g.key ? 0 : 255) : (int)(p >> 24);
                int Y = (77 * r + 150 * gg + 29 * b + 128) >> 8;
                int U = ((-43 * r - 85 * gg + 128 * b + 128) >> 8) + 128;
                int V = ((128 * r - 107 * gg - 21 * b + 128) >> 8) + 128;
                U = U < 0 ? 0 : U > 255 ? 255 : U;
                V = V < 0 ? 0 : V > 255 ? 255 : V;
                g.uy[i] = (unsigned char)((Y * a + 127) / 255);
                g.ua[i] = (unsigned char)a;
                su += U * a;
                sv += V * a;
                sa += a;
            }
            size_t j = (size_t)(y / 2) * cw + (unsigned)(x / 2);
            g.uu[j] = (unsigned char)((su + 510) / 1020);
            g.uv[j] = (unsigned char)((sv + 510) / 1020);
            g.uw[j] = (uint16_t)sa;
        }
    }
}

/* The picture now (comp held). Returns the UI generation it shows. */
static unsigned compose(void)
{
    pthread_mutex_lock(&g.lock);
    unsigned gen = g.ui_gen;
    if (g.dx1) {
        ui_convert(g.dx0, g.dy0, g.dx1, g.dy1);
        g.dx1 = 0;
    }
    pthread_mutex_unlock(&g.lock);

    size_t px = (size_t)g.w * g.h, cpx = px / 4;
    memset(g.cy, 16, px);
    memset(g.cu, 128, cpx);
    memset(g.cv, 128, cpx);
    hal_pic_t pic = { g.cy, g.cu, g.cv, g.w, g.h, g.w, g.w / 2 };
    int video = hal_display_grab(&pic) == 0;
    for (size_t i = 0; i < px; ++i) {
        unsigned a = g.ua[i];
        if (a == 255) g.cy[i] = g.uy[i];
        else if (!a) g.cy[i] = g.lut_y[g.cy[i]];
        else g.cy[i] = (unsigned char)(g.uy[i] + (g.lut_y[g.cy[i]] * (255 - a) + 127) / 255);
    }
    for (size_t i = 0; i < cpx; ++i) {
        unsigned wt = g.uw[i];
        if (wt == 1020) {
            g.cu[i] = g.uu[i];
            g.cv[i] = g.uv[i];
        } else if (!wt) {
            g.cu[i] = g.lut_c[g.cu[i]];
            g.cv[i] = g.lut_c[g.cv[i]];
        } else {
            unsigned u = g.uu[i] + (g.lut_c[g.cu[i]] * (1020 - wt) + 510) / 1020;
            unsigned v = g.uv[i] + (g.lut_c[g.cv[i]] * (1020 - wt) + 510) / 1020;
            g.cu[i] = (unsigned char)(u > 255 ? 255 : u);
            g.cv[i] = (unsigned char)(v > 255 ? 255 : v);
        }
    }
    /* With a tile on screen the picture changes with every camera picture;
     * with none it changes only with the UI. */
    int any_hole = 0;
    for (size_t i = 0; i < cpx && !any_hole && video; ++i) any_hole = g.uw[i] != 1020;
    pthread_mutex_lock(&g.lock);
    g.video = video && any_hole;
    pthread_mutex_unlock(&g.lock);
    return gen;
}

/* The picture halved (box filter) into c->half. */
static void halve(kvm_client_t *c)
{
    unsigned w = g.w / 2, h = g.h / 2, cw = w / 2, ch = h / 2;
    unsigned char *y = c->half, *u = y + (size_t)w * h, *v = u + (size_t)cw * ch;
    for (unsigned r = 0; r < h; ++r) {
        const unsigned char *a = g.cy + (size_t)(2 * r) * g.w, *b = a + g.w;
        for (unsigned x = 0; x < w; ++x)
            y[(size_t)r * w + x] = (unsigned char)((a[2 * x] + a[2 * x + 1] + b[2 * x] + b[2 * x + 1] + 2) >> 2);
    }
    for (unsigned r = 0; r < ch; ++r) {
        size_t s0 = (size_t)(2 * r) * (g.w / 2), s1 = s0 + g.w / 2;
        for (unsigned x = 0; x < cw; ++x) {
            u[(size_t)r * cw + x] =
                (unsigned char)((g.cu[s0 + 2 * x] + g.cu[s0 + 2 * x + 1] + g.cu[s1 + 2 * x] + g.cu[s1 + 2 * x + 1] + 2) >> 2);
            v[(size_t)r * cw + x] =
                (unsigned char)((g.cv[s0 + 2 * x] + g.cv[s0 + 2 * x + 1] + g.cv[s1 + 2 * x] + g.cv[s1 + 2 * x + 1] + 2) >> 2);
        }
    }
}

/* ---- kvm.h -------------------------------------------------------------------------------- */

int kvm_available(void)
{
    pthread_mutex_lock(&g.lock);
    int ok = g.disp != NULL;
    pthread_mutex_unlock(&g.lock);
    return ok;
}

void kvm_screen(unsigned *w, unsigned *h)
{
    pthread_mutex_lock(&g.lock);
    *w = g.w;
    *h = g.h;
    pthread_mutex_unlock(&g.lock);
}

static int pipe_pair(int fd[2])
{
    if (pipe2(fd, O_CLOEXEC | O_NONBLOCK) < 0) return -1;
    return 0;
}

kvm_client_t *kvm_open(const char *who, char *why, size_t cap)
{
    int p[2], k[2];
    if (pipe_pair(p) < 0) {
        snprintf(why, cap, "pipe: %s", strerror(errno));
        return NULL;
    }
    if (pipe_pair(k) < 0) {
        snprintf(why, cap, "pipe: %s", strerror(errno));
        close(p[0]);
        close(p[1]);
        return NULL;
    }
    kvm_client_t *c = NULL;
    int first = 0;
    pthread_mutex_lock(&g.lock);
    if (!g.disp) {
        snprintf(why, cap, "no screen");
    } else {
        for (int i = 0; i < KVM_CLIENTS && !c; ++i)
            if (!g.c[i].used) c = &g.c[i];
        if (!c) snprintf(why, cap, "%d viewers are connected already", KVM_CLIENTS);
    }
    if (c && !g.n) {
        if (buffers_alloc() < 0) {
            buffers_free();
            snprintf(why, cap, "out of memory");
            c = NULL;
        } else {
            first = 1;
            g.active = 1;
            g.want_full = 1;
            g.primed = 0;
            g.video = 1;
        }
    }
    if (c) {
        memset(c, 0, sizeof *c);
        c->used = 1;
        snprintf(c->who, sizeof c->who, "%s", who);
        c->ptr_fd = p[1];
        c->kbd_fd = k[1];
        c->scale = 1;
        c->quality = 75;
        c->reinit = 1;
        g.pend[g.n_pend] = p[0];
        g.pend_kbd[g.n_pend++] = 0;
        g.pend[g.n_pend] = k[0];
        g.pend_kbd[g.n_pend++] = 1;
        ++g.n;
    }
    pthread_mutex_unlock(&g.lock);
    if (!c) {
        close(p[0]);
        close(p[1]);
        close(k[0]);
        close(k[1]);
        return NULL;
    }
    if (first) hal_display_grab_keep(1);
    vg_log("kvm: %s connected", c->who);
    return c;
}

void kvm_close(kvm_client_t *c)
{
    if (!c) return;
    vg_log("kvm: %s disconnected", c->who);
    pthread_mutex_lock(&g.lock);
    /* The evdev reader sees the end of the pipes and lets go of the
     * buttons and keys they held. */
    close(c->ptr_fd);
    close(c->kbd_fd);
    kvm_enc_free(&c->enc);
    free(c->half);
    memset(c, 0, sizeof *c);
    int last = --g.n == 0;
    if (last) g.active = 0;
    pthread_mutex_unlock(&g.lock);
    if (!last) return;
    pthread_mutex_lock(&g.comp);
    pthread_mutex_lock(&g.lock);
    if (!g.n) buffers_free();
    pthread_mutex_unlock(&g.lock);
    pthread_mutex_unlock(&g.comp);
    hal_display_grab_keep(0);
}

void kvm_options(kvm_client_t *c, int scale, int quality)
{
    scale = scale == 2 ? 2 : 1;
    quality = quality < 10 ? 10 : quality > 95 ? 95 : quality;
    pthread_mutex_lock(&g.lock);
    if (scale != c->scale || quality != c->quality) {
        c->scale = scale;
        c->quality = quality;
        c->reinit = 1;
        pthread_cond_broadcast(&g.cond);
    }
    pthread_mutex_unlock(&g.lock);
}

void kvm_wake(kvm_client_t *c)
{
    pthread_mutex_lock(&g.lock);
    c->wake = 1;
    pthread_cond_broadcast(&g.cond);
    pthread_mutex_unlock(&g.lock);
}

void kvm_last(kvm_client_t *c, unsigned *ms, size_t *bytes)
{
    pthread_mutex_lock(&g.lock);
    *ms = c->last_ms;
    *bytes = c->last_bytes;
    pthread_mutex_unlock(&g.lock);
}

int kvm_next(kvm_client_t *c, int max_fps, unsigned char **msg, size_t *len, int timeout_ms)
{
    *msg = NULL;
    *len = 0;
    struct timespec deadline = ms_from_now(timeout_ms), prime_by = ms_from_now(PRIME_WAIT_MS);
    if (max_fps < 1) max_fps = 1;
    for (;;) {
        pthread_mutex_lock(&g.lock);
        for (;;) {
            if (!g.disp) {
                pthread_mutex_unlock(&g.lock);
                return -1;
            }
            if (c->wake) {
                c->wake = 0;
                pthread_mutex_unlock(&g.lock);
                return 0;
            }
            if (ms_until(&deadline) <= 0) {
                pthread_mutex_unlock(&g.lock);
                return 0;
            }
            if (!g.primed && ms_until(&prime_by) > 0) {
                wait_until(ms_until(&prime_by) < ms_until(&deadline) ? &prime_by : &deadline);
                continue;
            }
            if (ms_until(&c->next) > 0) {
                wait_until(ms_until(&c->next) < ms_until(&deadline) ? &c->next : &deadline);
                continue;
            }
            if (!c->reinit && !g.video && g.ui_gen == c->ui_seen) {
                wait_until(&deadline);
                continue;
            }
            break;
        }
        int scale = c->scale, quality = c->quality, reinit = c->reinit;
        c->reinit = 0;
        pthread_mutex_unlock(&g.lock);

        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        pthread_mutex_lock(&g.comp);
        int rc = -1;
        unsigned sw = g.w / (unsigned)scale, sh = g.h / (unsigned)scale;
        if (reinit || !c->enc.prev) {
            kvm_enc_free(&c->enc);
            free(c->half);
            c->half = scale == 2 ? malloc((size_t)sw * sh * 3 / 2) : NULL;
            if ((scale == 2 && !c->half) || kvm_enc_init(&c->enc, sw, sh) < 0) goto out;
        }
        if (!g.cy) goto out;            /* the last viewer went meanwhile */
        unsigned gen = compose();
        jpeg_planes_t p = { g.cy, g.cu, g.cv, g.w, g.h, g.w, g.w / 2 };
        if (scale == 2) {
            halve(c);
            p = (jpeg_planes_t){ c->half, c->half + (size_t)sw * sh, c->half + (size_t)sw * sh * 5 / 4,
                                 sw, sh, sw, sw / 2 };
        }
        rc = kvm_enc_frame(&c->enc, &p, quality, msg, len);
        c->ui_seen = gen;
out:
        pthread_mutex_unlock(&g.comp);
        long cost = -ms_until(&t0);
        /* At most max_fps, and encoding at most half the time: on a
         * two-core NVR the zero channel never takes more than half a core
         * per viewer from the decoders' feeders and the recordings. */
        long gap = 1000 / max_fps;
        if (gap < 2 * cost) gap = 2 * cost;
        pthread_mutex_lock(&g.lock);
        c->next = ms_from_now(gap - cost > 0 ? gap - cost : 0);
        if (rc == 1) {
            c->last_ms = (unsigned)cost;
            c->last_bytes = *len;
        }
        pthread_mutex_unlock(&g.lock);
        if (rc != 0) return rc < 0 ? (g.disp ? 0 : -1) : 1;
        /* nothing changed after all (a still camera): wait for more */
    }
}

int kvm_input(kvm_client_t *c, const char *msg, size_t len)
{
    unsigned w, h;
    kvm_screen(&w, &h);
    kvm_ev_t ev[8];
    int n = kvm_input_events(msg, len, w, h, ev, 8);
    if (n < 0) return -1;
    int np = 0, nk = 0;
    struct input_event pe[8], ke[8];
    for (int i = 0; i < n; ++i) {
        struct input_event e;
        memset(&e, 0, sizeof e);
        e.type = ev[i].type;
        e.code = ev[i].code;
        e.value = ev[i].value;
        if (ev[i].dev == KVM_DEV_KEYBOARD) ke[nk++] = e;
        else pe[np++] = e;
    }
    pthread_mutex_lock(&g.lock);
    /* At most a few events at once, under PIPE_BUF: written whole or not
     * at all. A full pipe (the UI stuck) drops them. */
    if (np && write(c->ptr_fd, pe, sizeof pe[0] * (size_t)np) < 0) { /* dropped */ }
    if (nk && write(c->kbd_fd, ke, sizeof ke[0] * (size_t)nk) < 0) { /* dropped */ }
    pthread_mutex_unlock(&g.lock);
    return 0;
}
