/*
 * hifb_demo — Hisilicon framebuffer userspace API example
 * (hi3520dv200, hi3536dv100).
 *
 * Brings up MPP + VO through hi_display.c (SYS/VB init, VO device
 * and HDMI), puts hifb into the SoC's
 * pixel format (RGB565 on hi3520dv200, ARGB8888 on hi3536dv100), then
 * exercises the standard Linux fb mmap path and the hifb-specific ioctls
 * (FBIOPUT_SHOW_HIFB, FBIOPUT_COLORKEY_HIFB).
 *
 * Draws SMPTE-style 7 color bars + a bouncing rectangle at ~30 fps so the
 * effect is visible on the wired display (HDMI / VGA / CVBS depending on
 * board jumpering).
 */
#define _DEFAULT_SOURCE        /* usleep() under glibc */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

#include "hi_type.h"
#include "hi_comm_vb.h"
#include "hi_comm_vo.h"
#include "hifb.h"
#include "hi_display.h"

static volatile sig_atomic_t g_stop;
static void on_sigint(int s) { (void)s; g_stop = 1; }

/* The mapped framebuffer, 16 bpp (RGB565) or 32 bpp (ARGB8888). */
typedef struct {
    uint8_t *mem;
    unsigned w, h, pitch, bpp;
} fb_t;

static uint32_t fb_color(const fb_t *fb, uint8_t r, uint8_t g, uint8_t b)
{
    if (fb->bpp == 32)
        return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    return (uint32_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static void fb_span(const fb_t *fb, unsigned y, unsigned x0, unsigned x1, uint32_t c)
{
    uint8_t *row = fb->mem + (size_t)y * fb->pitch;
    if (fb->bpp == 32) {
        uint32_t *p = (uint32_t *)row;
        for (unsigned x = x0; x < x1; ++x) p[x] = c;
    } else {
        uint16_t *p = (uint16_t *)row;
        for (unsigned x = x0; x < x1; ++x) p[x] = (uint16_t)c;
    }
}

static const uint8_t SMPTE_BARS[7][3] = {
    {255,255,255}, {255,255,0}, {0,255,255}, {0,255,0},
    {255,0,255},   {255,0,0},   {0,0,255}
};

/* Repaint the bars over columns [x0, x1) of rows [y0, y1). */
static void draw_color_bars(const fb_t *fb, int x0, int y0, int x1, int y1)
{
    const unsigned bar_w = fb->w / 7;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)fb->w) x1 = (int)fb->w;
    if (y1 > (int)fb->h) y1 = (int)fb->h;
    for (int i = 0; i < 7; ++i) {
        int bx0 = i * (int)bar_w, bx1 = (i == 6) ? (int)fb->w : bx0 + (int)bar_w;
        if (bx0 < x0) bx0 = x0;
        if (bx1 > x1) bx1 = x1;
        if (bx0 >= bx1) continue;
        uint32_t c = fb_color(fb, SMPTE_BARS[i][0], SMPTE_BARS[i][1], SMPTE_BARS[i][2]);
        for (int y = y0; y < y1; ++y) fb_span(fb, (unsigned)y, (unsigned)bx0, (unsigned)bx1, c);
    }
}

static void draw_rect(const fb_t *fb, int x0, int y0, int w, int h, uint32_t color)
{
    if (x0 < 0) { w += x0; x0 = 0; }
    if (y0 < 0) { h += y0; y0 = 0; }
    if (x0 + w > (int)fb->w) w = (int)fb->w - x0;
    if (y0 + h > (int)fb->h) h = (int)fb->h - y0;
    if (w <= 0 || h <= 0) return;
    for (int y = 0; y < h; ++y)
        fb_span(fb, (unsigned)(y0 + y), (unsigned)x0, (unsigned)(x0 + w), color);
}

static void sync_size(VO_INTF_SYNC_E sync, unsigned *w, unsigned *h)
{
    switch (sync) {
    case VO_OUTPUT_PAL:          *w = 720;  *h = 576; break;
    case VO_OUTPUT_1024x768_60:  *w = 1024; *h = 768; break;
    default:                     *w = 1280; *h = 720; break;
    }
}

int main(int argc, char **argv)
{
    int run_secs = 30;
    const char *fbdev = "/dev/fb0";
    VO_INTF_TYPE_E intf = VO_INTF_HDMI;
    VO_INTF_SYNC_E sync = VO_OUTPUT_720P60;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-t") && i + 1 < argc) run_secs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-f") && i + 1 < argc) fbdev = argv[++i];
        else if (!strcmp(argv[i], "-cvbs")) { intf = VO_INTF_CVBS; sync = VO_OUTPUT_PAL; }
        else if (!strcmp(argv[i], "-vga"))  { intf = VO_INTF_VGA;  sync = VO_OUTPUT_1024x768_60; }
        else if (!strcmp(argv[i], "-h")) {
            printf("usage: %s [-t secs] [-f /dev/fbN] [-cvbs|-vga]\n"
                   "  default: HDMI 720P60 on /dev/fb0 for 30 s\n", argv[0]);
            return 0;
        }
    }

    signal(SIGINT,  on_sigint);
    signal(SIGTERM, on_sigint);

    printf("hifb_demo: bringing up MPP (intf=0x%x sync=%d)\n",
           (unsigned)intf, (int)sync);
    unsigned scr_w, scr_h;
    sync_size(sync, &scr_w, &scr_h);
    VB_CONF_S vb_cfg = { .u32MaxPoolCnt = 0 };  /* fb-only demo, no video buffer pools */
    if (hi_display_start(&vb_cfg, intf, sync, scr_w, scr_h) < 0) return 1;

    printf("hifb_demo: open %s\n", fbdev);
    int fd = open(fbdev, O_RDWR);
    if (fd < 0) { perror("open"); hi_display_stop(); return 1; }
    if (hi_display_fb_set_mode(fd, scr_w, scr_h) < 0) { close(fd); hi_display_stop(); return 1; }

    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &v) < 0) { perror("FBIOGET_VSCREENINFO"); return 1; }
    if (ioctl(fd, FBIOGET_FSCREENINFO, &f) < 0) { perror("FBIOGET_FSCREENINFO"); return 1; }
    printf("hifb_demo: %ux%u bpp=%u line_length=%u smem_len=%u\n",
           v.xres, v.yres, v.bits_per_pixel, f.line_length, f.smem_len);

    if (v.bits_per_pixel != 16 && v.bits_per_pixel != 32) {
        fprintf(stderr, "unsupported %u bpp\n", v.bits_per_pixel);
        close(fd); hi_display_stop(); return 1;
    }

    void *mem = mmap(NULL, f.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED) { perror("mmap"); return 1; }

    /* Colorkey disabled, no alpha ioctl, just SHOW.
     * Per-layer alpha on hi3520dv200 hifb appears to leave the GFX layer
     * dark when enabled even with u8GlobalAlpha=0xFF; the layer becomes
     * visible without the alpha ioctl. SCREEN_ORIGIN defaults to (0,0). */
    HIFB_COLORKEY_S ck = { .bKeyEnable = HI_FALSE, .u32Key = 0 };
    if (ioctl(fd, FBIOPUT_COLORKEY_HIFB, &ck) < 0) perror("FBIOPUT_COLORKEY_HIFB (non-fatal)");

    HI_BOOL show = HI_TRUE;
    if (ioctl(fd, FBIOPUT_SHOW_HIFB, &show) < 0) perror("FBIOPUT_SHOW_HIFB");

    fb_t fb = { mem, v.xres, v.yres, f.line_length, v.bits_per_pixel };
    const int w = (int)fb.w, h = (int)fb.h;

    printf("hifb_demo: drawing color bars + moving square for ~%ds (Ctrl-C to stop)\n",
           run_secs);

    draw_color_bars(&fb, 0, 0, w, h);

    int rx = 0, ry = h / 2 - 32, rw = 96, rh = 64;
    int vx = 6, vy = 3;
    uint32_t rect_color = fb_color(&fb, 0, 0, 0);

    struct timespec t0, now;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    while (!g_stop) {
        /* Erase old rectangle by repainting the bar slice it covered. */
        draw_color_bars(&fb, rx, ry, rx + rw, ry + rh);
        rx += vx; ry += vy;
        if (rx < 0 || rx + rw > w) vx = -vx;
        if (ry < 0 || ry + rh > h) vy = -vy;
        if (rx < 0) rx = 0;
        if (ry < 0) ry = 0;
        if (rx + rw > w) rx = w - rw;
        if (ry + rh > h) ry = h - rh;
        draw_rect(&fb, rx, ry, rw, rh, rect_color);

        usleep(33 * 1000);

        clock_gettime(CLOCK_MONOTONIC, &now);
        if (run_secs > 0 && now.tv_sec - t0.tv_sec >= run_secs) break;
    }

    printf("hifb_demo: cleanup\n");
    show = HI_FALSE;
    ioctl(fd, FBIOPUT_SHOW_HIFB, &show);
    munmap(mem, f.smem_len);
    close(fd);
    hi_display_stop();
    return 0;
}
