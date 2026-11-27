/* SPDX-License-Identifier: GPL-3.0-or-later */
/* hal_ui_fbdev.c — the UI layer on SigmaStar: LVGL's fbdev driver on the
 * GOP framebuffer /dev/fb0 (fbdev.ko), above the display's video layer.
 * Tiles are holes keyed out by the GOP's colour key. The GOP stretches
 * the buffer to the output mode, or into the margins' inner rectangle
 * (sstar_fb_screen). The mouse pointer is fbdev.ko's hardware cursor, a
 * window on GOP 0 (hal_ui_cursor_*). */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

#include "lvgl.h"
#include "src/drivers/display/fb/lv_linux_fbdev.h"
#include "mi_common_datatype.h"
#include "sstarFb.h"

#include "hal_ui.h"
#include "sstar_fb.h"

#define FB_DEV        "/dev/fb0"
/* The GOP compares its colour key with the pixel's RGB. */
#define COLORKEY_R    0xFF
#define COLORKEY_G    0x00
#define COLORKEY_B    0xFF

static int g_fd = -1;

static void cursor_output(unsigned w, unsigned h, const vg_rect_t *inner);
static void cursor_ui(unsigned w, unsigned h);
static void soft_set(int on, const vg_rect_t *dst);

/* The fd stays open for the life of the process, as on hifb. */
static int fb_open(void)
{
    if (g_fd < 0) {
        g_fd = open(FB_DEV, O_RDWR);
        if (g_fd < 0) fprintf(stderr, "open %s: %s\n", FB_DEV, strerror(errno));
    }
    return g_fd;
}

/*
 * The buffer stays at the size fbdev.ko gave it at load (fbdev.ini's
 * FB_WIDTH/FB_HEIGHT): changing that with FBIOSET_DISPLAYLAYER_ATTRIBUTES
 * left the scan-out reading the old geometry (seen in qemu). The screen
 * (the timing's size, fbdev.ini's FB_TIMMING_*) and the stretch window's
 * destination are what follow the output: the GOP scales the buffer up
 * to it, in hardware, and fbdev.ko refuses a destination that does not
 * fit the screen, so both go in one call.
 *
 * The margins cannot be the GOP's: a destination smaller than the screen,
 * placed in it (u32Xpos/u32YPos, u32dstWidth/Height) is taken, but
 * fbdev.ko's vsync interrupt (_sstar_FB_VsyncISR, from the first call that
 * waits for a vsync) compares the destination with the display's timing
 * and, as they differ, queues GOPTimingSync_WorkHandler, which sets it
 * back to the whole timing, keeping the position (seen in qemu; the sync
 * is switched on at open, and nothing switches it off). GOP 1 does not
 * shrink either ("GOP1 Not Support H/V scaling down", HalGopSetStretch-
 * WindowSize), which any margin at 720p would need. So the window is
 * always the whole output, and with margins the UI is shrunk into the
 * buffer as it is drawn (soft_flush): nearest pixel, which keeps the
 * colour key exact, the margins black.
 */
static unsigned g_bw, g_bh;             /* the buffer: the UI's coordinates */

static int fb_geometry(void)
{
    if (g_bw) return 0;
    struct fb_var_screeninfo var;
    if (ioctl(g_fd, FBIOGET_VSCREENINFO, &var) < 0) {
        fprintf(stderr, "FBIOGET_VSCREENINFO: %s\n", strerror(errno));
        return -1;
    }
    g_bw = var.xres;
    g_bh = var.yres;
    return 0;
}

/* The GOP's screen w x h, its stretch window over all of it. */
static int gop_screen(unsigned w, unsigned h)
{
    MI_FB_DisplayLayerAttr_t a;
    memset(&a, 0, sizeof a);
    if (ioctl(g_fd, FBIOGET_DISPLAYLAYER_ATTRIBUTES, &a) < 0) {
        fprintf(stderr, "FBIOGET_DISPLAYLAYER_ATTRIBUTES: %s\n", strerror(errno));
        return -1;
    }
    if (a.u32ScreenWidth == w && a.u32ScreenHeight == h && a.u32dstWidth == w && a.u32dstHeight == h &&
        !a.u32Xpos && !a.u32YPos)
        return 0;
    a.u32Xpos = 0;
    a.u32YPos = 0;
    a.u32dstWidth = w;
    a.u32dstHeight = h;
    a.u32ScreenWidth = w;
    a.u32ScreenHeight = h;
    a.u32SetAttrMask = E_MI_FB_DISPLAYLAYER_ATTR_MASK_SCREEN_SIZE | E_MI_FB_DISPLAYLAYER_ATTR_MASK_DISP_POS |
                       E_MI_FB_DISPLAYLAYER_ATTR_MASK_DISP_SIZE;
    if (ioctl(g_fd, FBIOSET_DISPLAYLAYER_ATTRIBUTES, &a) < 0) {
        fprintf(stderr, "FBIOSET_DISPLAYLAYER_ATTRIBUTES %ux%u: %s\n", w, h, strerror(errno));
        return -1;
    }
    return 0;
}

int sstar_fb_screen(unsigned w, unsigned h, const vg_rect_t *inner)
{
    if (fb_open() < 0 || fb_geometry() < 0 || gop_screen(w, h) < 0) return -1;
    vg_rect_t in = inner && inner->w > 0 && inner->h > 0 ? *inner : (vg_rect_t){ 0, 0, (int)w, (int)h };
    cursor_output(w, h, &in);
    /* the inner rectangle in buffer pixels (the GOP stretches the buffer
     * over the whole output); all of it without margins */
    int x0 = (int)((long long)in.x * g_bw / w), x1 = (int)((long long)(in.x + in.w) * g_bw / w);
    int y0 = (int)((long long)in.y * g_bh / h), y1 = (int)((long long)(in.y + in.h) * g_bh / h);
    vg_rect_t dst = { x0, y0, x1 - x0, y1 - y0 };
    soft_set(dst.x || dst.y || (unsigned)dst.w != g_bw || (unsigned)dst.h != g_bh, &dst);
    return 0;
}

/* ---- the UI drawn into the buffer: as it is, or shrunk into the margins --------------- */

static struct {
    lv_display_t *disp;
    uint8_t      *fbp;                  /* the buffer, mapped */
    size_t        len;
    unsigned      line, xoff, yoff;
    int           soft;                 /* shrunk into dst (buffer pixels) */
    vg_rect_t     dst;
    unsigned     *xmap;                 /* dst column -> UI column */
} F;

/* The whole buffer black (opaque: not the colour key). */
static void fb_black(void)
{
    if (!F.fbp) return;
    for (unsigned y = 0; y < g_bh; ++y) {
        uint32_t *row = (uint32_t *)(F.fbp + (size_t)(y + F.yoff) * F.line) + F.xoff;
        for (unsigned x = 0; x < g_bw; ++x) row[x] = 0xFF000000u;
    }
}

static void soft_set(int on, const vg_rect_t *dst)
{
    if (on && (dst->w <= 0 || dst->h <= 0)) on = 0;
    if (on == F.soft && (!on || (dst->x == F.dst.x && dst->y == F.dst.y && dst->w == F.dst.w &&
                                 dst->h == F.dst.h)))
        return;
    unsigned *xmap = NULL;
    if (on) {
        xmap = malloc(sizeof *xmap * (size_t)dst->w);
        if (!xmap) on = 0;
        for (int x = 0; xmap && x < dst->w; ++x) xmap[x] = (unsigned)((unsigned long long)x * g_bw / (unsigned)dst->w);
    }
    free(F.xmap);
    F.xmap = xmap;
    F.soft = on;
    F.dst = on ? *dst : (vg_rect_t){ 0, 0, 0, 0 };
    if (on)
        fprintf(stderr, "sigmastar: the UI shrunk into %d,%d %dx%d of its %ux%u buffer (margins)\n", F.dst.x,
                F.dst.y, F.dst.w, F.dst.h, g_bw, g_bh);
    /* everything again, at the new place (before the display exists, its
     * first refresh draws everything) */
    if (F.disp) {
        fb_black();
        lv_obj_invalidate(lv_display_get_screen_active(F.disp));
    }
}

/* Area `a` of the UI into dst: each buffer pixel there the UI pixel it
 * shrinks from (column x of dst: UI column x * bw / dst.w), so the pixels
 * an area covers are the ones whose UI pixel is in it, and areas drawn
 * apart meet without a seam. */
static void soft_flush(const lv_area_t *a, const uint8_t *px, uint32_t stride)
{
    unsigned dw = (unsigned)F.dst.w, dh = (unsigned)F.dst.h;
    unsigned cx0 = ((unsigned)a->x1 * dw + g_bw - 1) / g_bw, cx1 = ((unsigned)(a->x2 + 1) * dw + g_bw - 1) / g_bw;
    unsigned cy0 = ((unsigned)a->y1 * dh + g_bh - 1) / g_bh, cy1 = ((unsigned)(a->y2 + 1) * dh + g_bh - 1) / g_bh;
    for (unsigned cy = cy0; cy < cy1 && cy < dh; ++cy) {
        unsigned sy = (unsigned)((unsigned long long)cy * g_bh / dh);
        const uint32_t *src = (const uint32_t *)(px + (size_t)(sy - (unsigned)a->y1) * stride);
        uint32_t *d = (uint32_t *)(F.fbp + (size_t)(F.dst.y + (int)cy + (int)F.yoff) * F.line) + F.dst.x + F.xoff;
        for (unsigned cx = cx0; cx < cx1 && cx < dw; ++cx) d[cx] = src[F.xmap[cx] - (unsigned)a->x1];
    }
}

/* LVGL's fbdev flush, which copies the area as it is, unless shrinking. */
static void flush_cb(lv_display_t *disp, const lv_area_t *a, uint8_t *px)
{
    if (F.fbp && a->x2 >= a->x1 && a->y2 >= a->y1 && a->x1 >= 0 && a->y1 >= 0 && (unsigned)a->x2 < g_bw &&
        (unsigned)a->y2 < g_bh) {
        int32_t w = lv_area_get_width(a);
        uint32_t stride = lv_draw_buf_width_to_stride((uint32_t)w, lv_display_get_color_format(disp));
        if (F.soft) {
            soft_flush(a, px, stride);
        } else {
            for (int32_t y = a->y1; y <= a->y2; ++y, px += stride)
                memcpy(F.fbp + (size_t)(y + (int32_t)F.yoff) * F.line + (size_t)(a->x1 + (int32_t)F.xoff) * 4, px,
                       (size_t)w * 4);
        }
    }
    lv_display_flush_ready(disp);
}

static int fb_map(void)
{
    struct fb_fix_screeninfo fix;
    struct fb_var_screeninfo var;
    if (ioctl(g_fd, FBIOGET_FSCREENINFO, &fix) < 0 || ioctl(g_fd, FBIOGET_VSCREENINFO, &var) < 0) {
        fprintf(stderr, "FBIOGET_FSCREENINFO: %s\n", strerror(errno));
        return -1;
    }
    void *p = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, g_fd, 0);
    if (p == MAP_FAILED) {
        fprintf(stderr, "mmap %s: %s\n", FB_DEV, strerror(errno));
        return -1;
    }
    F.fbp = p;
    F.len = fix.smem_len;
    F.line = fix.line_length;
    F.xoff = var.xoffset;
    F.yoff = var.yoffset;
    return 0;
}

lv_display_t *hal_ui_create(unsigned w, unsigned h)
{
    /* fbdev.ko sets the layer up once, at load, from fbdev.ini (FB_WIDTH,
     * FB_HEIGHT, FB_HWWIN_FORMAT 5 = ARGB8888): checked here. */
    if (fb_open() < 0) return NULL;
    struct fb_var_screeninfo var;
    if (ioctl(g_fd, FBIOGET_VSCREENINFO, &var) < 0) {
        fprintf(stderr, "FBIOGET_VSCREENINFO: %s\n", strerror(errno));
        return NULL;
    }
    if (var.xres != w || var.yres != h || var.bits_per_pixel != LV_COLOR_DEPTH) {
        fprintf(stderr, "%s is %ux%u at %u bpp, the UI needs %ux%u at %d: "
                "set FB_WIDTH/FB_HEIGHT and FB_HWWIN_FORMAT in fbdev.ini\n",
                FB_DEV, var.xres, var.yres, var.bits_per_pixel, w, h, LV_COLOR_DEPTH);
        return NULL;
    }

    cursor_ui(var.xres, var.yres);
    g_bw = var.xres;
    g_bh = var.yres;
    if (fb_map() < 0) return NULL;
    lv_display_t *disp = lv_linux_fbdev_create();
    if (!disp) return NULL;
    lv_linux_fbdev_set_file(disp, FB_DEV);
    lv_display_set_flush_cb(disp, flush_cb);
    F.disp = disp;
    if (F.soft) fb_black();

    /* Without the key the UI's tiles would cover the video, and a hidden
     * layer shows no UI: either way there is no usable display. */
    MI_FB_ColorKey_t ck = { .bKeyEnable = 1, .u8Red = COLORKEY_R,
                            .u8Green = COLORKEY_G, .u8Blue = COLORKEY_B };
    if (ioctl(g_fd, FBIOSET_COLORKEY, &ck) < 0) {
        fprintf(stderr, "FBIOSET_COLORKEY: %s\n", strerror(errno));
        lv_display_delete(disp);
        return NULL;
    }
    MI_BOOL show = 1;
    if (ioctl(g_fd, FBIOSET_SHOW, &show) < 0) {
        fprintf(stderr, "FBIOSET_SHOW: %s\n", strerror(errno));
        lv_display_delete(disp);
        return NULL;
    }
    return disp;
}

void hal_ui_hole(lv_color_t *color, lv_opa_t *opa)
{
    *color = lv_color_make(COLORKEY_R, COLORKEY_G, COLORKEY_B);
    *opa = LV_OPA_COVER;
}

/*
 * The cursor plane: fbdev.ko's hardware cursor (FBIOSET_CURSOR_ATTRIBUTE),
 * set up by fbdev.ini's [FB_CURSOR] section on GOP 0, which the display
 * mixes above the UI's GOP 1. Read off fbdev.ko and the region code it
 * calls (opensigmastar kernel/rgn):
 *
 * - GOP 0 does not scale (gSupportScaleUp = {0, 1}; HalGopSetStretchWindow
 *   refuses it): fbdev.ko sets its stretch window to the output timing,
 *   1:1, and takes the position in output pixels. The SDK's demo says as
 *   much ("fb size should match disp size, otherwise cursor xpos,ypos will
 *   be wrong"). So the UI's coordinates are scaled here to the output
 *   (sstar_fb_screen), and so is the arrow, to keep its size on screen.
 * - GOP 0 has no ARGB8888 (_HalGopSrcFmtTransRealVal: I8/I4/I2, ARGB1555,
 *   ARGB4444): the arrow goes as ARGB1555, its alpha bit choosing alpha 0
 *   (0, transparent) or alpha 1 (255), as in the SDK's demo. The format
 *   comes with the icon; fbdev.ini's FB_HWWIN_FORMAT for the cursor is
 *   never used.
 * - At most 128 x 128 pixels (the ioctl refuses more), rows of 512 bytes.
 * - Every call waits for the next vsync (sstar_FB_EndTransaction) before it
 *   returns. A thread of its own makes the calls, always with the newest
 *   position: a move never waits, and the pointer is moved once a frame
 *   however fast the mouse reports.
 *
 * Without [FB_CURSOR] fbdev.ko has no cursor state and the ioctl
 * dereferences a NULL pointer in the kernel; so the section is looked for
 * in the file fbdev.ko loaded first, and without it LVGL draws the pointer.
 */
#define CUR_MAX 128

static struct {
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    pthread_t       thread;
    int             running, stop;
    uint32_t        arrow[CUR_MAX * CUR_MAX];   /* ARGB8888, in UI pixels */
    int             aw, ah;
    unsigned        ui_w, ui_h;                 /* the buffer: UI coordinates */
    unsigned        out_w, out_h;               /* the output timing */
    vg_rect_t       in;                         /* the UI's place on it (the margins') */
    int             x, y, show;
    int             new_icon, new_pos, new_show;
    uint16_t        icon[CUR_MAX * CUR_MAX];    /* the thread's: as sent */
} C = { .lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER };

static void cursor_ui(unsigned w, unsigned h)
{
    pthread_mutex_lock(&C.lock);
    C.ui_w = w;
    C.ui_h = h;
    pthread_mutex_unlock(&C.lock);
}

/* A new output, or new margins: the arrow scaled again, the position
 * too. `inner`: where the UI is on the output. */
static void cursor_output(unsigned w, unsigned h, const vg_rect_t *inner)
{
    pthread_mutex_lock(&C.lock);
    if (w != C.out_w || h != C.out_h || inner->x != C.in.x || inner->y != C.in.y || inner->w != C.in.w ||
        inner->h != C.in.h) {
        C.out_w = w;
        C.out_h = h;
        C.in = *inner;
        C.new_icon = C.new_pos = 1;
        pthread_cond_signal(&C.cond);
    }
    pthread_mutex_unlock(&C.lock);
}

/* Locked: the arrow at the output's scale, nearest pixel, ARGB1555. An even
 * width: fbdev.ko rounds the window's width down to one. */
static void cursor_scale(unsigned ow, unsigned oh, unsigned uw, unsigned uh, unsigned *iw, unsigned *ih)
{
    unsigned w = ((unsigned)C.aw * ow + uw - 1) / uw, h = ((unsigned)C.ah * oh + uh - 1) / uh;
    w = (w + 1) & ~1u;
    if (w > CUR_MAX) w = CUR_MAX;
    if (h > CUR_MAX) h = CUR_MAX;
    for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < w; ++x) {
            unsigned sx = x * uw / ow, sy = y * uh / oh;
            uint32_t p = sx < (unsigned)C.aw && sy < (unsigned)C.ah ? C.arrow[sy * (unsigned)C.aw + sx] : 0;
            C.icon[y * w + x] = p >> 31 ? (uint16_t)(0x8000 | (p >> 9 & 0x7C00) | (p >> 6 & 0x03E0) |
                                                     (p >> 3 & 0x001F))
                                        : 0;
        }
    *iw = w;
    *ih = h;
}

static void *cursor_thread(void *arg)
{
    (void)arg;
    int failed = 0;
    pthread_mutex_lock(&C.lock);
    for (;;) {
        while (!C.stop && !C.new_icon && !C.new_pos && !C.new_show) pthread_cond_wait(&C.cond, &C.lock);
        if (C.stop && !C.new_show) break;       /* the last hide still goes out */
        unsigned uw = C.ui_w ? C.ui_w : 1, uh = C.ui_h ? C.ui_h : 1;
        /* the UI's place on the output: the margins' inner rectangle */
        unsigned ox = C.in.w > 0 ? (unsigned)C.in.x : 0, oy = C.in.h > 0 ? (unsigned)C.in.y : 0;
        unsigned ow = C.in.w > 0 ? (unsigned)C.in.w : C.out_w ? C.out_w : uw;
        unsigned oh = C.in.h > 0 ? (unsigned)C.in.h : C.out_h ? C.out_h : uh;
        MI_FB_CursorAttr_t a;
        memset(&a, 0, sizeof a);
        a.u32XPos = (MI_U32)(ox + (unsigned long long)C.x * ow / uw);
        a.u32YPos = (MI_U32)(oy + (unsigned long long)C.y * oh / uh);
        a.u16CursorAttrMask = E_MI_FB_CURSOR_ATTR_MASK_POS;
        if (C.new_icon) {
            unsigned iw, ih;
            cursor_scale(ow, oh, uw, uh, &iw, &ih);
            a.stCursorImageInfo.u32Width = iw;
            a.stCursorImageInfo.u32Height = ih;
            a.stCursorImageInfo.u32Pitch = iw;
            a.stCursorImageInfo.eColorFmt = E_MI_FB_COLOR_FMT_ARGB1555;
            a.stCursorImageInfo.data = (const char *)C.icon;
            a.stAlpha.bAlphaEnable = 1;
            a.stAlpha.u8Alpha0 = 0;
            a.stAlpha.u8Alpha1 = 0xFF;
            a.stAlpha.u8GlobalAlpha = 0xFF;
            a.u16CursorAttrMask |= E_MI_FB_CURSOR_ATTR_MASK_ICON | E_MI_FB_CURSOR_ATTR_MASK_ALPHA;
        }
        if (C.new_show)
            a.u16CursorAttrMask |= C.show ? E_MI_FB_CURSOR_ATTR_MASK_SHOW : E_MI_FB_CURSOR_ATTR_MASK_HIDE;
        C.new_icon = C.new_pos = C.new_show = 0;
        /* Unlocked: the call waits for the next vsync, the icon stays (only
         * this thread writes it). */
        pthread_mutex_unlock(&C.lock);
        if (ioctl(g_fd, FBIOSET_CURSOR_ATTRIBUTE, &a) < 0 && !failed++)
            fprintf(stderr, "FBIOSET_CURSOR_ATTRIBUTE: %s\n", strerror(errno));
        pthread_mutex_lock(&C.lock);
    }
    pthread_mutex_unlock(&C.lock);
    return NULL;
}

/* fbdev.ini as fbdev.ko read it has a [FB_CURSOR] section. */
static int cursor_configured(void)
{
    static const char *const params[] = {
        "/sys/module/open_fbdev/parameters/default_config_path_file",
        "/sys/module/fbdev/parameters/default_config_path_file",
    };
    char path[256] = "/config/fbdev.ini";       /* fbdev.ko's default */
    for (size_t i = 0; i < sizeof params / sizeof params[0]; ++i) {
        FILE *f = fopen(params[i], "r");
        if (!f) continue;
        char v[256];
        if (fgets(v, sizeof v, f)) {
            v[strcspn(v, "\n")] = 0;
            if (v[0] == '/') snprintf(path, sizeof path, "%s", v);
        }
        fclose(f);
        break;
    }
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[256];
    int found = 0;
    while (!found && fgets(line, sizeof line, f)) {
        const char *p = line + strspn(line, " \t");
        found = !strncmp(p, "[FB_CURSOR]", 11);
    }
    fclose(f);
    return found;
}

int hal_ui_cursor_create(const uint32_t *argb, int w, int h)
{
    if (g_fd < 0 || w <= 0 || h <= 0 || w > CUR_MAX || h > CUR_MAX || C.running) return -1;
    if (!cursor_configured()) {
        fprintf(stderr, "sigmastar: no [FB_CURSOR] in fbdev.ini: the pointer is drawn with the UI\n");
        return -1;
    }
    pthread_mutex_lock(&C.lock);
    memcpy(C.arrow, argb, (size_t)w * (size_t)h * sizeof *argb);
    C.aw = w;
    C.ah = h;
    C.x = (int)C.ui_w / 2;
    C.y = (int)C.ui_h / 2;
    C.show = 0;
    C.stop = 0;
    C.new_icon = C.new_pos = C.new_show = 1;
    C.running = pthread_create(&C.thread, NULL, cursor_thread, NULL) == 0;
    pthread_mutex_unlock(&C.lock);
    return C.running ? 0 : -1;
}

void hal_ui_cursor_move(int x, int y)
{
    pthread_mutex_lock(&C.lock);
    if (x != C.x || y != C.y) {
        C.x = x;
        C.y = y;
        C.new_pos = 1;
        pthread_cond_signal(&C.cond);
    }
    pthread_mutex_unlock(&C.lock);
}

void hal_ui_cursor_show(int on)
{
    pthread_mutex_lock(&C.lock);
    if (!on != !C.show) {
        C.show = on != 0;
        C.new_show = 1;
        pthread_cond_signal(&C.cond);
    }
    pthread_mutex_unlock(&C.lock);
}

/* Hidden: vigild gone, no pointer is left standing over the picture. */
void hal_ui_cursor_delete(void)
{
    if (!C.running) return;
    hal_ui_cursor_show(0);
    pthread_mutex_lock(&C.lock);
    C.stop = 1;
    pthread_cond_signal(&C.cond);
    pthread_mutex_unlock(&C.lock);
    pthread_join(C.thread, NULL);
    C.running = 0;
}
