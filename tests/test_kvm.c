/* SPDX-License-Identifier: GPL-3.0-or-later */
/* test_kvm.c — the zero channel: its JPEG encoder, changed-parts encoder
 * and input mapping as units, then the whole of src/app/kvm.c on an
 * in-memory LVGL display (PARTIAL rendering, as the fbdev UI) with the
 * real evdev reader: pictures come out, and input put in moves, clicks,
 * types into and right-clicks a small UI.
 *
 *   test_kvm                 the tests
 *   test_kvm --serve FILE    that UI with the web server (web.c) as
 *                            FILE configures it, until stdin closes, for
 *                            web_test.py; it prints what the UI got
 *                            ("clicks 1", "text ab", "menu 1", "wheel -1")
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <linux/input.h>

#include "lvgl.h"

#include "config.h"
#include "hal.h"
#include "hal_capture.h"
#include "hal_decoder.h"
#include "hal_display.h"
#include "hal_ui.h"
#include "input_evdev.h"
#include "jpeg_enc.h"
#include "kvm.h"
#include "kvm_app.h"
#include "kvm_enc.h"
#include "kvm_input.h"
#include "web.h"

#define W 1280
#define H 720

static int g_fail, g_checks;
#define CHECK(c, ...)                                                                \
    do {                                                                             \
        ++g_checks;                                                                  \
        if (!(c)) {                                                                  \
            ++g_fail;                                                                \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                     \
            fprintf(stderr, __VA_ARGS__);                                            \
            fputc('\n', stderr);                                                     \
        }                                                                            \
    } while (0)

/* ---- the platform, faked ----------------------------------------------------------------- */

static const hal_caps_t g_caps = { .name = "test", .display = 1, .decoder = 1, .max_channels = 4 };
const hal_caps_t *hal_caps(void) { return &g_caps; }
/* api.c (the login check is there) comes with the channels: no decoders. */
int  hal_dec_open(int ch, vg_codec_t c, const vg_stream_info_t *si) { (void)ch; (void)c; (void)si; return -1; }
int  hal_dec_send(int ch, const vg_au_t *au) { (void)ch; (void)au; return -1; }
void hal_dec_reset(int ch) { (void)ch; }
void hal_dec_close(int ch) { (void)ch; }
int  hal_display_set_tile(int ch, const vg_rect_t *r) { (void)ch; (void)r; return 0; }
void hal_display_clear_tile(int ch) { (void)ch; }
void hal_display_set_aspect(int ch, unsigned w, unsigned h) { (void)ch; (void)w; (void)h; }
/* No cursor plane: LVGL draws the pointer, so the pictures have it. */
int  hal_ui_cursor_create(const uint32_t *argb, int w, int h) { (void)argb; (void)w; (void)h; return -1; }
void hal_ui_cursor_move(int x, int y) { (void)x; (void)y; }
void hal_ui_cursor_show(int on) { (void)on; }
void hal_ui_cursor_delete(void) {}
int  hal_cap_start(int in, int ch, hal_cap_cb cb, void *user) { (void)in; (void)ch; (void)cb; (void)user; return -1; }
void hal_cap_stop(int in) { (void)in; }
int  hal_cap_signal(int in) { (void)in; return 0; }

void hal_ui_hole(lv_color_t *c, lv_opa_t *o)
{
    *c = lv_color_make(0xff, 0x00, 0xff);       /* SigmaStar's colour key */
    *o = LV_OPA_COVER;
}

/* One tile, top right, whose "camera" shows a bar moving with time. */
static int g_keep, g_video_frame;
void hal_display_grab_keep(int on) { g_keep = on; }
int hal_display_grab(const hal_pic_t *p)
{
    if (!g_keep) return -1;
    int bar = (g_video_frame++ * 16) % 640;
    for (unsigned y = 0; y < 360; ++y)
        for (unsigned x = 640; x < 1280; ++x)
            p->y[(size_t)y * p->ys + x] = (unsigned char)((int)(x - 640) >= bar && (int)(x - 640) < bar + 32 ? 235 : 80);
    for (unsigned y = 0; y < 180; ++y)
        for (unsigned x = 320; x < 640; ++x) {
            p->u[(size_t)y * p->cs + x] = 160;   /* bluish */
            p->v[(size_t)y * p->cs + x] = 110;
        }
    return 0;
}

/* ---- a small UI --------------------------------------------------------------------------- */

static int       g_clicks, g_wheel, g_menu, g_serve;
static lv_obj_t *g_ta, *g_label, *g_menu_obj;
static char      g_said[256];

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
    if (!g_serve) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_said, sizeof g_said, fmt, ap);
    va_end(ap);
    printf("%s\n", g_said);
    fflush(stdout);
}

static void clicked(lv_event_t *e)
{
    (void)e;
    lv_label_set_text_fmt(g_label, "Clicked %d", ++g_clicks);
    say("clicks %d", g_clicks);
}

static void ta_changed(lv_event_t *e)
{
    (void)e;
    say("text %s", lv_textarea_get_text(g_ta));
}

static lv_indev_t *pointer(void)
{
    lv_indev_t *i = NULL;
    while ((i = lv_indev_get_next(i)) && lv_indev_get_type(i) != LV_INDEV_TYPE_POINTER) {}
    return i;
}

static void right(void)
{
    lv_point_t p;
    lv_indev_get_point(pointer(), &p);
    lv_obj_set_pos(g_menu_obj, p.x, p.y);
    lv_obj_remove_flag(g_menu_obj, LV_OBJ_FLAG_HIDDEN);
    g_menu = 1;
    say("menu 1");
}

static void wheel(int steps)
{
    g_wheel += steps;
    say("wheel %d", g_wheel);
}

static void build_ui(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x202830), 0);
    lv_obj_t *hole = lv_obj_create(scr);
    lv_obj_remove_style_all(hole);
    lv_obj_set_pos(hole, 640, 0);
    lv_obj_set_size(hole, 640, 360);
    lv_obj_set_style_bg_color(hole, lv_color_make(0xff, 0, 0xff), 0);
    lv_obj_set_style_bg_opa(hole, LV_OPA_COVER, 0);
    lv_obj_t *b = lv_button_create(scr);
    lv_obj_set_pos(b, 100, 100);
    lv_obj_set_size(b, 200, 60);
    lv_obj_add_event_cb(b, clicked, LV_EVENT_CLICKED, NULL);
    g_label = lv_label_create(b);
    lv_label_set_text(g_label, "Click me");
    lv_obj_center(g_label);
    g_ta = lv_textarea_create(scr);
    lv_obj_set_pos(g_ta, 100, 200);
    lv_obj_set_size(g_ta, 400, 50);
    lv_textarea_set_one_line(g_ta, true);
    lv_obj_add_event_cb(g_ta, ta_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_group_t *grp = lv_group_create();
    lv_group_add_obj(grp, g_ta);
    lv_indev_set_group(input_evdev_keypad(), grp);
    lv_group_focus_obj(g_ta);
    g_menu_obj = lv_obj_create(scr);
    lv_obj_set_size(g_menu_obj, 220, 160);
    lv_obj_set_style_bg_color(g_menu_obj, lv_color_hex(0x16233a), 0);
    lv_obj_t *ml = lv_label_create(g_menu_obj);
    lv_label_set_text(ml, "Context menu\nLive view\nPlayback\nSettings");
    lv_obj_add_flag(g_menu_obj, LV_OBJ_FLAG_HIDDEN);
}

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

static uint32_t g_buf[W * 40];

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    (void)a;
    (void)px;
    lv_display_flush_ready(d);
}

static lv_display_t *ui_up(void)
{
    lv_init();
    lv_tick_set_cb(now_ms);
    lv_display_t *disp = lv_display_create(W, H);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_buffers(disp, g_buf, NULL, sizeof g_buf, LV_DISPLAY_RENDER_MODE_PARTIAL);
    static const input_hooks_t hooks = { .right = right, .wheel = wheel };
    input_evdev_create(disp, &hooks, 0);
    build_ui();
    kvm_attach(disp);
    return disp;
}

static void pump(int ms)
{
    uint32_t end = now_ms() + (uint32_t)ms;
    while ((int32_t)(end - now_ms()) > 0) {
        lv_timer_handler();
        usleep(2000);
    }
}

/* ---- units --------------------------------------------------------------------------------- */

static void test_jpeg(void)
{
    static unsigned char y[64 * 48], u[32 * 24], v[32 * 24];
    for (int i = 0; i < 64 * 48; ++i) y[i] = (unsigned char)(i % 64 * 4);
    memset(u, 100, sizeof u);
    memset(v, 150, sizeof v);
    jpeg_planes_t p = { y, u, v, 64, 48, 64, 32 };
    jpeg_buf_t b = { 0 };
    long n = jpeg_encode(&p, 0, 0, 64, 48, 75, &b);
    CHECK(n > 600 && b.p[0] == 0xff && b.p[1] == 0xd8 && b.p[n - 2] == 0xff && b.p[n - 1] == 0xd9,
          "jpeg: a whole file (%ld bytes)", n);
    /* SOF0: height and width of the part */
    long sof = -1;
    for (long i = 0; i + 1 < n && sof < 0; ++i)
        if (b.p[i] == 0xff && b.p[i + 1] == 0xc0) sof = i;
    CHECK(sof > 0 && b.p[sof + 5] * 256 + b.p[sof + 6] == 48 && b.p[sof + 7] * 256 + b.p[sof + 8] == 64,
          "jpeg: SOF0 says 64x48");
    size_t before = b.len;
    n = jpeg_encode(&p, 18, 6, 10, 6, 50, &b);         /* odd edges: repeated, not read past */
    CHECK(n > 0 && b.len == before + (size_t)n, "jpeg: a small part, appended");
    CHECK(jpeg_encode(&p, 60, 0, 8, 8, 50, &b) == -1, "jpeg: a part past the edge is refused");
    free(b.p);
}

static unsigned rd16(const unsigned char *p) { return p[0] | p[1] << 8; }

static void test_enc(void)
{
    enum { PW = 320, PH = 200 };
    static unsigned char pic[PW * PH * 3 / 2];
    memset(pic, 60, sizeof pic);
    jpeg_planes_t p = { pic, pic + PW * PH, pic + PW * PH * 5 / 4, PW, PH, PW, PW / 2 };
    kvm_enc_t e;
    CHECK(kvm_enc_init(&e, PW, PH) == 0, "enc: init");
    unsigned char *m;
    size_t len;
    CHECK(kvm_enc_frame(&e, &p, 70, &m, &len) == 1 && m[0] == 'F' && m[1] == 1 && rd16(m + 2) == PW &&
          rd16(m + 4) == PH && rd16(m + 6) == 1, "enc: the first picture is whole");
    free(m);
    CHECK(kvm_enc_frame(&e, &p, 70, &m, &len) == 0 && !m, "enc: nothing changed, nothing sent");
    pic[105 * PW + 100] = 200;                          /* block (6, 6) */
    CHECK(kvm_enc_frame(&e, &p, 70, &m, &len) == 1 && m[1] == 0 && rd16(m + 6) == 1 && rd16(m + 12) == 96 &&
          rd16(m + 14) == 96 && rd16(m + 16) == 16 && rd16(m + 18) == 16, "enc: one pixel: its block (%u,%u %ux%u)",
          m ? rd16(m + 12) : 0, m ? rd16(m + 14) : 0, m ? rd16(m + 16) : 0, m ? rd16(m + 18) : 0);
    unsigned seq1 = m ? m[8] : 0;
    free(m);
    pic[5 * PW + 5] = 1;                                /* two far apart in one band */
    pic[40 * PW + 300] = 1;
    pic[PW * PH] = 7;                                   /* chroma counts too (block 0, 0) */
    CHECK(kvm_enc_frame(&e, &p, 70, &m, &len) == 1 && rd16(m + 6) == 2, "enc: two places, two parts (%u)",
          m ? rd16(m + 6) : 0);
    CHECK(m && m[8] == seq1 + 1, "enc: numbered");
    if (m) {
        size_t off = KVM_MSG_HEADER;
        unsigned x0 = rd16(m + off), y0 = rd16(m + off + 2), w0 = rd16(m + off + 4), h0 = rd16(m + off + 6);
        off += KVM_PART_HEADER + (m[off + 8] | m[off + 9] << 8 | m[off + 10] << 16);
        unsigned x1 = rd16(m + off), y1 = rd16(m + off + 2), h1 = rd16(m + off + 6);
        CHECK(x0 == 0 && y0 == 0 && w0 == 16 && h0 == 16 && x1 == 288 && y1 == 32 && h1 == 16,
              "enc: parts (%u,%u %ux%u) (%u,%u h %u)", x0, y0, w0, h0, x1, y1, h1);
        CHECK(off + KVM_PART_HEADER + (m[off + 8] | m[off + 9] << 8) == len, "enc: the lengths add up");
    }
    free(m);
    memset(pic, 90, PW * PH);                           /* most of it */
    CHECK(kvm_enc_frame(&e, &p, 70, &m, &len) == 1 && m[1] == 1 && rd16(m + 6) == 1, "enc: most changed: whole");
    free(m);
    kvm_enc_free(&e);
}

static void test_input(void)
{
    CHECK(kvm_key_code("KeyA") == KEY_A && kvm_key_code("Enter") == KEY_ENTER && kvm_key_code("Escape") == KEY_ESC &&
          kvm_key_code("F1") == KEY_F1 && kvm_key_code("ArrowUp") == KEY_UP && kvm_key_code("Tab") == KEY_TAB &&
          kvm_key_code("ContextMenu") == KEY_COMPOSE && kvm_key_code("ShiftLeft") == KEY_LEFTSHIFT &&
          kvm_key_code("Numpad5") == KEY_KP5 && kvm_key_code("Digit0") == KEY_0, "input: key codes");
    CHECK(kvm_key_code("MetaLeft") == 0 && kvm_key_code("") == 0 && kvm_key_code(NULL) == 0, "input: no such key");
    kvm_ev_t ev[8];
    const char *m = "{\"t\":\"move\",\"x\":2000,\"y\":-5}";
    int n = kvm_input_events(m, strlen(m), W, H, ev, 8);
    CHECK(n == 3 && ev[0].dev == KVM_DEV_POINTER && ev[0].type == EV_ABS && ev[0].code == ABS_X &&
          ev[0].value == W - 1 && ev[1].code == ABS_Y && ev[1].value == 0 && ev[2].type == EV_SYN,
          "input: move, clamped to the screen");
    m = "{\"t\":\"btn\",\"b\":2,\"down\":true}";
    n = kvm_input_events(m, strlen(m), W, H, ev, 8);
    CHECK(n == 2 && ev[0].type == EV_KEY && ev[0].code == BTN_RIGHT && ev[0].value == 1, "input: right button");
    m = "{\"t\":\"btn\",\"b\":0,\"down\":false}";
    n = kvm_input_events(m, strlen(m), W, H, ev, 8);
    CHECK(n == 2 && ev[0].code == BTN_LEFT && ev[0].value == 0, "input: left button up");
    m = "{\"t\":\"wheel\",\"n\":-40}";
    n = kvm_input_events(m, strlen(m), W, H, ev, 8);
    CHECK(n == 2 && ev[0].type == EV_REL && ev[0].code == REL_WHEEL && ev[0].value == -10, "input: wheel, bounded");
    m = "{\"t\":\"key\",\"code\":\"KeyQ\",\"v\":2}";
    n = kvm_input_events(m, strlen(m), W, H, ev, 8);
    CHECK(n == 2 && ev[0].dev == KVM_DEV_KEYBOARD && ev[0].code == KEY_Q && ev[0].value == 2, "input: key repeat");
    m = "{\"t\":\"key\",\"code\":\"MetaLeft\",\"v\":1}";
    CHECK(kvm_input_events(m, strlen(m), W, H, ev, 8) == 0, "input: a key the console has no use for: nothing");
    m = "{\"t\":\"ack\",\"seq\":3}";
    CHECK(kvm_input_events(m, strlen(m), W, H, ev, 8) == -1, "input: not input");
    m = "{\"t\":\"btn\",\"b\":7,\"down\":true}";
    CHECK(kvm_input_events(m, strlen(m), W, H, ev, 8) == -1, "input: no such button");
    CHECK(kvm_input_events("nonsense", 8, W, H, ev, 8) == -1, "input: not JSON");
}

/* ---- the whole: kvm.c on a display ------------------------------------------------------------ */

static int next(kvm_client_t *c, unsigned char **m, size_t *len)
{
    /* kvm_next waits for the UI the LVGL thread draws: this is that thread */
    for (int i = 0; i < 100; ++i) {
        pump(10);
        int rc = kvm_next(c, 30, m, len, 5);
        if (rc) return rc;
    }
    return 0;
}

static void send(kvm_client_t *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void send(kvm_client_t *c, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    CHECK(kvm_input(c, buf, (size_t)n) == 0, "input accepted: %s", buf);
    pump(60);
}

static void test_kvm(void)
{
    ui_up();
    pump(50);
    char why[128] = "";
    CHECK(kvm_available(), "kvm: a screen");
    unsigned w, h;
    kvm_screen(&w, &h);
    CHECK(w == W && h == H, "kvm: the screen's size");
    kvm_client_t *a = kvm_open("test a", why, sizeof why), *b = kvm_open("test b", why, sizeof why);
    CHECK(a && b && g_keep, "kvm: two viewers, the platform keeps pictures (%s)", why);
    CHECK(!kvm_open("test c", why, sizeof why) && strstr(why, "viewers"), "kvm: a third is refused (%s)", why);
    unsigned char *m = NULL;
    size_t len = 0;
    CHECK(next(a, &m, &len) == 1 && m[1] == 1 && rd16(m + 2) == W && rd16(m + 4) == H,
          "kvm: a whole first picture at the screen's size");
    free(m);
    /* The video moves: the next picture is the tile (its part of the screen). */
    CHECK(next(a, &m, &len) == 1 && m[1] == 0, "kvm: then only what changed");
    if (m) {
        unsigned x = rd16(m + 12), y = rd16(m + 14);
        CHECK(x >= 640 && y < 360, "kvm: ... in the video's tile (%u,%u)", x, y);
    }
    free(m);
    kvm_options(b, 2, 50);
    CHECK(next(b, &m, &len) == 1 && m[1] == 1 && rd16(m + 2) == W / 2 && rd16(m + 4) == H / 2,
          "kvm: half size, whole after the change");
    free(m);
    kvm_close(b);

    /* Input: the viewer's pointer and keyboard reach the UI through the
     * evdev reader. */
    send(a, "{\"t\":\"move\",\"x\":200,\"y\":130}");
    send(a, "{\"t\":\"btn\",\"b\":0,\"down\":true}");
    send(a, "{\"t\":\"btn\",\"b\":0,\"down\":false}");
    CHECK(g_clicks == 1, "kvm: a click on the button (%d)", g_clicks);
    lv_point_t pt;
    lv_indev_get_point(pointer(), &pt);
    CHECK(pt.x == 200 && pt.y == 130, "kvm: the pointer is where the browser put it (%d,%d)", (int)pt.x, (int)pt.y);
    send(a, "{\"t\":\"key\",\"code\":\"ShiftLeft\",\"v\":1}");
    send(a, "{\"t\":\"key\",\"code\":\"KeyH\",\"v\":1}");
    send(a, "{\"t\":\"key\",\"code\":\"KeyH\",\"v\":0}");
    send(a, "{\"t\":\"key\",\"code\":\"ShiftLeft\",\"v\":0}");
    send(a, "{\"t\":\"key\",\"code\":\"KeyI\",\"v\":1}");
    send(a, "{\"t\":\"key\",\"code\":\"KeyI\",\"v\":0}");
    CHECK(!strcmp(lv_textarea_get_text(g_ta), "Hi"), "kvm: typed (%s)", lv_textarea_get_text(g_ta));
    send(a, "{\"t\":\"move\",\"x\":700,\"y\":500}");
    send(a, "{\"t\":\"btn\",\"b\":2,\"down\":true}");
    send(a, "{\"t\":\"btn\",\"b\":2,\"down\":false}");
    CHECK(g_menu == 1, "kvm: the right button opens the menu");
    send(a, "{\"t\":\"wheel\",\"n\":-2}");
    CHECK(g_wheel == -2, "kvm: the wheel (%d)", g_wheel);
    /* What the UI drew since: the button's new label, the text, the menu. */
    CHECK(next(a, &m, &len) == 1 && rd16(m + 6) >= 2, "kvm: the UI's changes come as parts (%u)",
          m ? rd16(m + 6) : 0);
    free(m);
    /* A button held when the viewer goes is let go (the pipe ends). */
    send(a, "{\"t\":\"move\",\"x\":200,\"y\":130}");
    send(a, "{\"t\":\"btn\",\"b\":0,\"down\":true}");
    kvm_close(a);
    pump(100);
    CHECK(g_clicks == 2 && !g_keep, "kvm: closed: the press ends as a click, the platform lets go (%d)", g_clicks);
    kvm_detach();
    CHECK(!kvm_available() && !kvm_open("late", why, sizeof why), "kvm: detached: no screen");
}

/* ---- --serve ---------------------------------------------------------------------------------- */

static volatile sig_atomic_t g_stop;
static void on_signal(int s) { (void)s; g_stop = 1; }

static int serve(const char *cfg)
{
    g_serve = 1;
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGPIPE, SIG_IGN);
    if (config_init(cfg) < 0) {
        fprintf(stderr, "config %s\n", cfg);
        return 1;
    }
    ui_up();
    if (web_start() < 0) return 1;
    printf("ready\n");
    fflush(stdout);
    struct pollfd pf = { .fd = 0, .events = POLLIN };
    while (!g_stop) {
        lv_timer_handler();
        if (poll(&pf, 1, 5) > 0) {
            char b[64];
            if (read(0, b, sizeof b) <= 0) break;
        }
    }
    web_stop();
    kvm_detach();
    input_evdev_delete();
    lv_deinit();
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--serve")) return serve(argv[2]);
    test_jpeg();
    test_enc();
    test_input();
    test_kvm();
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
