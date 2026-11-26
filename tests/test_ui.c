/* SPDX-License-Identifier: GPL-3.0-or-later */
/* test_ui.c — the on-screen UI without a board: LVGL renders into memory,
 * a pipe stands in for the mouse (through the real evdev reader), the
 * settings go through the real configuration store (a temporary file),
 * and the channels are fakes. Each step checks widget states and writes
 * a screenshot (PPM) that tests/ui_compare.py compares with the reference
 * images in tests/ui/.
 *
 *   test_ui OUTDIR
 */
#define _DEFAULT_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <linux/input.h>

#include "lvgl.h"

#include "channel.h"
#include "config.h"
#include "hal.h"
#include "hal_capture.h"
#include "hal_decoder.h"
#include "hal_display.h"
#include "hal_ui.h"
#include "input_evdev.h"
#include "ui_app.h"
#include "ui_bridge.h"
#include "ui_layout.h"
#include "ui_osk.h"
#include "theme_tokens.h"
#include "icons.h"

#define W 1280
#define H 720

static int g_fail, g_checks;
#define CHECK(cond, ...) do { ++g_checks; if (!(cond)) { ++g_fail; \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* ---- a platform: four decoders, a UI layer with black (video) holes -------------------- */

static const hal_caps_t g_caps = { .name = "test", .display = 1, .decoder = 1, .dec_max_w = 1920,
                                   .dec_max_h = 1080, .max_channels = 4 };
const hal_caps_t *hal_caps(void) { return &g_caps; }
int  hal_dec_open(int ch, vg_codec_t c, const vg_stream_info_t *si) { (void)ch; (void)c; (void)si; return -1; }
int  hal_dec_send(int ch, const vg_au_t *au) { (void)ch; (void)au; return -1; }
void hal_dec_reset(int ch) { (void)ch; }
void hal_dec_close(int ch) { (void)ch; }
int  hal_display_set_tile(int ch, const vg_rect_t *r) { (void)ch; (void)r; return 0; }
void hal_display_clear_tile(int ch) { (void)ch; }
int  hal_cap_start(int in, int ch, hal_cap_cb cb, void *user) { (void)in; (void)ch; (void)cb; (void)user; return -1; }
void hal_cap_stop(int in) { (void)in; }
int  hal_cap_signal(int in) { (void)in; return 0; }
void hal_ui_hole(lv_color_t *c, lv_opa_t *o)
{
    *c = lv_color_hex(0x000000);
    *o = LV_OPA_COVER;
}

/* ---- the model: the real bridge, fake channels -------------------------------------- */

static ui_model_t g_model;
static int        g_channels = 4;       /* the platform's, as the model says */
static int        g_shots;              /* --shots: every screen, not the tests */
static void shots_status(int idx, ui_chan_t *st);
static ui_rect_t  g_tiles[UI_MAX_CHANNELS];
static int        g_tile_calls;

static void fake_status(int idx, ui_chan_t *st)
{
    switch (idx) {
    case 0:
        *st = (ui_chan_t){ .configured = 1, .status = UI_CH_LIVE, .live = 1, .recording = 1,
                           .events = UI_EV_MOTION | UI_EV_HUMAN, .rec_bytes = 734u << 20, .name = "Gate",
                           .codec = "H.265", .width = 1920, .height = 1080, .fps = 25, .view = "main",
                           .rec_state = "recording" };
        break;
    case 1:
        *st = (ui_chan_t){ .configured = 1, .status = UI_CH_CONNECTING, .name = "Yard", .state = "CONNECTING" };
        break;
    case 2:
        *st = (ui_chan_t){ .configured = 1, .status = UI_CH_ERROR, .state = "DESCRIBE: 404" };
        break;
    default:
        /* Past the first four (the runs with more channels): live, some
         * recording. */
        if (idx >= 3 && g_channels > 4)
            *st = (ui_chan_t){ .configured = 1, .status = UI_CH_LIVE, .live = 1, .recording = idx % 3 == 0,
                               .codec = "H.264", .width = 640, .height = 360, .fps = 15, .view = "sub" };
        break;
    }
}

static const ui_event_t k_events[] = {
    { 1, 0, "online" }, { 3, 0, "offline" }, { 0, 0, "config applied" },
};
static int g_ev;

static time_t fake_now(void) { return 1795703525; }     /* 2026-11-26 14:32:05 UTC */

static ui_event_t g_more[8];             /* pushed by the steps */
static int        g_n_more, g_more_read;

static int fake_event(ui_event_t *ev)
{
    if (g_ev < (int)(sizeof k_events / sizeof k_events[0])) {
        *ev = k_events[g_ev++];
        ev->time = fake_now() - 60 + 20 * g_ev;
        return 1;
    }
    if (g_more_read == g_n_more) return 0;
    *ev = g_more[g_more_read++];
    ev->time = fake_now();
    return 1;
}

static void push_event(int ch, const char *text)
{
    if (g_n_more == (int)(sizeof g_more / sizeof g_more[0])) abort();
    g_more[g_n_more] = (ui_event_t){ .ch = ch };
    snprintf(g_more[g_n_more++].text, sizeof g_more[0].text, "%s", text);
}

static void fake_tiles(const ui_rect_t *t, int n)
{
    memset(g_tiles, 0, sizeof g_tiles);
    memcpy(g_tiles, t, sizeof *t * (size_t)n);
    ++g_tile_calls;
}

static int fake_info(ui_info_t *info, int max)
{
    static const ui_info_t k[] = {
        { "Version", "vigild 0.1.0" }, { "Platform", "test" },
        { "Decoder", "H.264 / H.265, up to 1920x1080" }, { "Channels", "4" },
        { "Uptime", "0 d 01:02:03" }, { "Memory", "71 MiB available of 113 MiB; vigild uses 9 MiB" },
        { "Cameras", "3 configured, 1 live" }, { "Recording", "off" },
    };
    int n = (int)(sizeof k / sizeof k[0]);
    if (n > max) n = max;
    memcpy(info, k, sizeof *info * (size_t)n);
    return n;
}

/* ---- display, clock and mouse ------------------------------------------------------------ */

/* ---- camera discovery: fake devices and jobs ----------------------------------------- */

static ui_disc_job_t g_dj;
static int           g_dj_devices;      /* how many the "network" has once a scan ends */
static int           g_dj_hold;         /* the next job stays running until fake_job_end */
static char          g_dj_last_user[64], g_dj_last_pass[64];

static const ui_disc_dev_t k_ddev[] = {
    { "00:12:31:4f:2a:10", "192.168.1.10", "00:12:31:4f:2a:10", "255.255.255.0", "192.168.1.1", "Xiongmai", "IPC",
      "LocalHost", UI_DISC_NETIP | UI_DISC_ONVIF, 34567, 1 },
    { "3c:ef:8c:12:34:56", "192.168.1.108", "3c:ef:8c:12:34:56", "255.255.255.0", "192.168.1.1", "Dahua",
      "IPC-HFW1230S", "Gate", UI_DISC_DHIP, 80, 0 },
    { "b4:c9:b9:8d:0a:1f", "192.168.1.175", "b4:c9:b9:8d:0a:1f", "255.255.255.0", "192.168.1.1", "OpenIPC",
      "SigmaStar", "ssc325de", UI_DISC_ONVIF | UI_DISC_NETIP, 34567, 0 },
    { "00:12:31:00:00:22", "192.168.1.22", "00:12:31:00:00:22", "255.255.255.0", "192.168.1.1", "Xiongmai", "NVR",
      "NVR-8", UI_DISC_NETIP, 34567, 0 },
    { "e0:50:8b:00:00:01", "192.168.1.64", "e0:50:8b:00:00:01", "255.255.255.0", "192.168.1.1", "Acme",
      "DS-2CD", "Lobby", UI_DISC_ONVIF, 80, 0 },
};

static int fake_disc_devices(ui_disc_dev_t *out, int max)
{
    int n = g_dj_devices < max ? g_dj_devices : max;
    memcpy(out, k_ddev, (size_t)n * sizeof *out);
    return n;
}

static void fake_disc_job(ui_disc_job_t *j) { *j = g_dj; }

static int fake_job_new(ui_job_kind_t kind, int dry)
{
    unsigned id = g_dj.id + 1;
    memset(&g_dj, 0, sizeof g_dj);
    g_dj.id = id;
    g_dj.kind = kind;
    g_dj.dry_run = dry;
    g_dj.running = g_dj_hold;
    g_dj.progress = g_dj_hold ? 40 : 100;
    return (int)id;
}

static void fake_job_end(void)
{
    g_dj.running = 0;
    g_dj.progress = 100;
    if (g_dj.kind == UI_JOB_SCAN) {
        g_dj_devices = (int)(sizeof k_ddev / sizeof k_ddev[0]);
        snprintf(g_dj.msg, sizeof g_dj.msg, "%d devices", g_dj_devices);
    }
}

static int fake_disc_scan(unsigned protos, char *err, size_t cap)
{
    (void)protos;
    (void)err;
    (void)cap;
    int id = fake_job_new(UI_JOB_SCAN, 0);
    snprintf(g_dj.msg, sizeof g_dj.msg, "searching");
    if (!g_dj_hold) fake_job_end();
    return id;
}

static int fake_disc_ip(const ui_disc_ip_t *items, int n, const char *start, const char *mask, const char *gw,
                        const char *user, const char *pass, int dry_run, char *err, size_t cap)
{
    (void)mask;
    (void)gw;
    (void)err;
    (void)cap;
    snprintf(g_dj_last_user, sizeof g_dj_last_user, "%s", user);
    snprintf(g_dj_last_pass, sizeof g_dj_last_pass, "%s", pass);
    int id = fake_job_new(UI_JOB_IP, dry_run);
    unsigned next = start && *start ? (unsigned)atoi(strrchr(start, '.') + 1) : 0;
    for (int i = 0; i < n && i < UI_DISC_MAX; ++i) {
        ui_disc_res_t *r = &g_dj.r[i];
        const ui_disc_dev_t *d = NULL;
        for (size_t k = 0; k < sizeof k_ddev / sizeof k_ddev[0]; ++k)
            if (!strcmp(k_ddev[k].key, items[i].key)) d = &k_ddev[k];
        snprintf(r->key, sizeof r->key, "%s", items[i].key);
        snprintf(r->ip, sizeof r->ip, "%s", d ? d->ip : "?");
        if (items[i].ip[0]) snprintf(r->new_ip, sizeof r->new_ip, "%s", items[i].ip);
        else snprintf(r->new_ip, sizeof r->new_ip, "192.168.0.%u", next == 20 ? (next += 2, 21) : next++);
        r->proto = d && (d->protos & UI_DISC_NETIP) ? UI_DISC_NETIP : d && (d->protos & UI_DISC_DHIP) ? UI_DISC_DHIP
                 : UI_DISC_ONVIF;
        r->done = 1;
        r->ok = dry_run || r->proto != UI_DISC_ONVIF;
        r->reprobed = r->ok && !dry_run;
        char from[16], to[16];
        memcpy(from, r->ip, sizeof from);
        memcpy(to, r->new_ip, sizeof to);
        if (dry_run) snprintf(r->msg, sizeof r->msg, "%s -> %s", from, to);
        else snprintf(r->msg, sizeof r->msg, "%s", r->ok ? "changed" : "GetNetworkInterfaces: not authorized");
    }
    g_dj.n = n;
    snprintf(g_dj.msg, sizeof g_dj.msg, dry_run ? "plan for %d devices" : "%d changed", n);
    return id;
}

static int fake_disc_assign(const char *key, int channel, const char *user, const char *pass, int replace,
                            char *err, size_t cap)
{
    (void)replace;
    (void)err;
    (void)cap;
    snprintf(g_dj_last_user, sizeof g_dj_last_user, "%s", user);
    snprintf(g_dj_last_pass, sizeof g_dj_last_pass, "%s", pass);
    int id = fake_job_new(UI_JOB_ASSIGN, 0);
    ui_disc_res_t *r = &g_dj.r[0];
    snprintf(r->key, sizeof r->key, "%s", key);
    snprintf(r->ip, sizeof r->ip, "192.168.1.108");
    r->channel = channel ? channel : 4;
    r->proto = UI_DISC_DHIP;
    r->done = r->ok = 1;
    snprintf(r->msg, sizeof r->msg, "added to channel %d", r->channel);
    g_dj.n = 1;
    return id;
}

static int fake_disc_autoadd(const char *user, const char *pass, char *err, size_t cap)
{
    (void)err;
    (void)cap;
    snprintf(g_dj_last_user, sizeof g_dj_last_user, "%s", user);
    snprintf(g_dj_last_pass, sizeof g_dj_last_pass, "%s", pass);
    int id = fake_job_new(UI_JOB_AUTOADD, 0);
    static const struct { int dev, ch, ok; unsigned proto; const char *msg; } R[] = {
        { 3, 3, 1, UI_DISC_NETIP, "channel 3" }, { 1, 4, 1, UI_DISC_DHIP, "channel 4" },
        { 2, 0, 0, UI_DISC_NETIP, "no empty channel left" },
        { 4, 0, 0, UI_DISC_ONVIF, "GetCapabilities: not authorized" },
    };
    for (size_t i = 0; i < sizeof R / sizeof R[0]; ++i) {
        ui_disc_res_t *r = &g_dj.r[i];
        snprintf(r->key, sizeof r->key, "%s", k_ddev[R[i].dev].key);
        snprintf(r->ip, sizeof r->ip, "%s", k_ddev[R[i].dev].ip);
        r->channel = R[i].ch;
        r->proto = R[i].proto;
        r->done = 1;
        r->ok = R[i].ok;
        snprintf(r->msg, sizeof r->msg, "%s", R[i].msg);
    }
    g_dj.n = (int)(sizeof R / sizeof R[0]);
    snprintf(g_dj.msg, sizeof g_dj.msg, "2 added, 2 not");
    return id;
}

static void fake_discovery(ui_model_t *m)
{
    m->disc_scan = fake_disc_scan;
    m->disc_devices = fake_disc_devices;
    m->disc_job = fake_disc_job;
    m->disc_ip = fake_disc_ip;
    m->disc_assign = fake_disc_assign;
    m->disc_autoadd = fake_disc_autoadd;
}

static uint32_t g_fb[W * H];
static uint32_t g_ms;
static int      g_mouse[2];             /* pipe: test -> evdev reader */
static int      g_px = W / 2, g_py = H / 2;
static const char *g_out;

static uint32_t fake_ms(void) { return g_ms; }

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    (void)a;
    (void)px;
    lv_display_flush_ready(d);
}

static void run(int ms)
{
    for (int t = 0; t < ms; t += 5) {
        g_ms += 5;
        lv_timer_handler();
    }
}

static void ev(int type, int code, int value)
{
    struct input_event e = { .type = (unsigned short)type, .code = (unsigned short)code, .value = value };
    if (write(g_mouse[1], &e, sizeof e) != (ssize_t)sizeof e) abort();
}

static void move_to(int x, int y)
{
    ev(EV_REL, REL_X, x - g_px);
    ev(EV_REL, REL_Y, y - g_py);
    ev(EV_SYN, SYN_REPORT, 0);
    g_px = x < 0 ? 0 : x > W - 1 ? W - 1 : x;
    g_py = y < 0 ? 0 : y > H - 1 ? H - 1 : y;
    run(60);
}

static void button(int code)
{
    ev(EV_KEY, code, 1);
    ev(EV_SYN, SYN_REPORT, 0);
    run(40);
    ev(EV_KEY, code, 0);
    ev(EV_SYN, SYN_REPORT, 0);
    run(60);
}

static void click_at(int x, int y)
{
    move_to(x, y);
    button(BTN_LEFT);
}

static void shot(const char *name)
{
    run(400);                           /* animations end, timers settle */
    lv_refr_now(NULL);
    char path[512];
    snprintf(path, sizeof path, "%s/%s.ppm", g_out, name);
    FILE *f = fopen(path, "wb");
    if (!f) {
        CHECK(0, "cannot write %s", path);
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    static uint8_t row[W * 3];
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            uint32_t p = g_fb[y * W + x];
            row[x * 3] = (uint8_t)(p >> 16);
            row[x * 3 + 1] = (uint8_t)(p >> 8);
            row[x * 3 + 2] = (uint8_t)p;
        }
        fwrite(row, 1, sizeof row, f);
    }
    fclose(f);
}

/* ---- finding widgets ------------------------------------------------------------------------- */

static int visible(lv_obj_t *o)
{
    for (; o; o = lv_obj_get_parent(o))
        if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return 0;
    return 1;
}

static lv_obj_t *find_in(lv_obj_t *o, const char *text, int contains)
{
    if (lv_obj_check_type(o, &lv_label_class) && visible(o)) {
        const char *t = lv_label_get_text(o);
        if (contains ? strstr(t, text) != NULL : !strcmp(t, text)) return o;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); ++i) {
        lv_obj_t *r = find_in(lv_obj_get_child(o, (int32_t)i), text, contains);
        if (r) return r;
    }
    return NULL;
}

/* A visible label: on the top layer (popups, toasts) or the screen. */
static lv_obj_t *find(const char *text, int contains)
{
    lv_obj_t *o = find_in(lv_layer_top(), text, contains);
    return o ? o : find_in(lv_screen_active(), text, contains);
}

/* The nth (0-based) visible label with exactly this text, in tree order. */
static lv_obj_t *nth_in(lv_obj_t *o, const char *text, int *n)
{
    if (lv_obj_check_type(o, &lv_label_class) && visible(o) && !strcmp(lv_label_get_text(o), text) &&
        (*n)-- == 0)
        return o;
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); ++i) {
        lv_obj_t *r = nth_in(lv_obj_get_child(o, (int32_t)i), text, n);
        if (r) return r;
    }
    return NULL;
}

static lv_obj_t *find_nth(lv_obj_t *root, const char *text, int n) { return nth_in(root, text, &n); }

static void center_of(lv_obj_t *o, int *x, int *y)
{
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    *x = (a.x1 + a.x2) / 2;
    *y = (a.y1 + a.y2) / 2;
}

/* Click the widget showing `text` (a button's label clicks the button). */
static int click_text(const char *text)
{
    lv_obj_t *l = find(text, 0);
    if (!l) l = find(text, 1);
    CHECK(l, "no '%s' on screen", text);
    if (!l) return -1;
    int x, y;
    center_of(l, &x, &y);
    click_at(x, y);
    return 0;
}

static lv_obj_t *find_textarea(lv_obj_t *o, int nth, int *seen)
{
    if (lv_obj_check_type(o, &lv_textarea_class) && (*seen)++ == nth) return o;
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); ++i) {
        lv_obj_t *r = find_textarea(lv_obj_get_child(o, (int32_t)i), nth, seen);
        if (r) return r;
    }
    return NULL;
}

static lv_obj_t *textarea(int nth)
{
    int seen = 0;
    return find_textarea(lv_screen_active(), nth, &seen);
}

/* A visible A8 icon `img` among `o`'s descendants. */
static int icon_shown(lv_obj_t *o, const lv_image_dsc_t *img)
{
    if (!o) return 0;
    if (lv_obj_check_type(o, &lv_image_class) && lv_image_get_src(o) == img && visible(o)) return 1;
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); ++i)
        if (icon_shown(lv_obj_get_child(o, (int32_t)i), img)) return 1;
    return 0;
}

/* The live view's page arrow (0 previous, 1 next): a round button on the
 * screen holding a LV_SYMBOL_LEFT / RIGHT label. */
static lv_obj_t *page_arrow(int next)
{
    lv_obj_t *scr = lv_screen_active();
    for (uint32_t i = 0; i < lv_obj_get_child_count(scr); ++i) {
        lv_obj_t *b = lv_obj_get_child(scr, (int32_t)i);
        if (!lv_obj_check_type(b, &lv_button_class)) continue;
        for (uint32_t k = 0; k < lv_obj_get_child_count(b); ++k) {
            lv_obj_t *l = lv_obj_get_child(b, (int32_t)k);
            if (lv_obj_check_type(l, &lv_label_class) &&
                !strcmp(lv_label_get_text(l), next ? LV_SYMBOL_RIGHT : LV_SYMBOL_LEFT))
                return b;
        }
    }
    return NULL;
}

static int arrow_up(int next)
{
    lv_obj_t *b = page_arrow(next);
    return b && visible(b);
}

/* The tile showing `title` (its name chip's parent). */
static lv_obj_t *tile(const char *title)
{
    lv_obj_t *l = find(title, 0);
    return l ? lv_obj_get_parent(lv_obj_get_parent(l)) : NULL;
}

/* ---- the steps ---------------------------------------------------------------------------- */

#define SAVE LV_SYMBOL_SAVE "  Save"

static void test_layout_math(void)
{
    ui_cell_t c[UI_LAYOUT_MAX];
    CHECK(ui_layout_cells(W, H, 4, 0, 4, c) == 4, "4 cells");
    CHECK(c[3].x == 640 && c[3].y == 360 && c[3].w == 640 && c[3].h == 360, "cell 4 %d,%d %dx%d",
          c[3].x, c[3].y, c[3].w, c[3].h);
    CHECK(ui_layout_pages(1, 4) == 4 && ui_layout_pages(4, 4) == 1 && ui_layout_pages(9, 10) == 2, "pages");
    ui_layout_cells(W, H, 1, 2, 4, c);
    CHECK(c[0].ch == 2 && c[0].x == 0 && c[0].y == 0 && c[0].w == W && c[0].h == H, "page 3 of 1x1 shows CAM3");
    ui_layout_cells(W, H, 9, 0, 4, c);
    int area = 0;
    for (int i = 0; i < 9; ++i) area += c[i].w * c[i].h;
    CHECK(area == W * H && c[4].ch == -1, "3x3 covers the screen, no channel past 4");
    CHECK(c[1].x == 416 && c[2].x == 848 && c[2].w == W - 848, "3x3 columns on 16-pixel edges (%d, %d)",
          c[1].x, c[2].x);
    CHECK(!ui_layout_valid(2) && ui_layout_cells(W, H, 2, 0, 4, c) == 0, "2 is not a layout");
    static const int layouts[] = { 1, 4, 8, 9, 16, 32, 64 };
    for (size_t k = 0; k < sizeof layouts / sizeof layouts[0]; ++k) {
        int n = ui_layout_cells(W, H, layouts[k], 0, 64, c);
        long a = 0;
        int inside = 1;
        for (int i = 0; i < n; ++i) {
            a += (long)c[i].w * c[i].h;
            inside &= c[i].x >= 0 && c[i].y >= 0 && c[i].x + c[i].w <= W && c[i].y + c[i].h <= H &&
                      c[i].x % 16 == 0 && c[i].y % 2 == 0;
        }
        CHECK(n == layouts[k] && a == (long)W * H && inside, "%d windows cover the screen, aligned", layouts[k]);
    }
}

static void test_live(void)
{
    CHECK(find("Gate", 0) && find("Yard", 0) && find("CAM 3", 0) && find("CAM 4", 0), "tile titles");
    CHECK(!find("1920x1080", 1), "no stream line on the tiles");
    CHECK(find("Connecting...", 0) && find("No video", 0) && find("DESCRIBE: 404", 0), "tile states");
    CHECK(icon_shown(tile("CAM 4"), &ui_icon_add) && !find("No camera", 0), "an empty slot: a \"+\"");
    lv_obj_t *gate = find("Gate", 0);
    if (gate) {
        int gx, gy;
        center_of(gate, &gx, &gy);
        CHECK(gx < 120 && gy > 300 && gy < 360, "the name at the bottom left (%d, %d)", gx, gy);
    }
    CHECK(find("LIVE 1/3", 1), "totals in the navigation bar");
    lv_obj_t *clk = find("2026-11-26  14:32:05", 0);
    int x, y;
    CHECK(clk, "clock");
    if (clk) {
        center_of(clk, &x, &y);
        CHECK(x > W - 200 && y < 40, "clock in the top right corner (%d, %d)", x, y);
    }
    CHECK(icon_shown(tile("Gate"), &ui_icon_rec) && icon_shown(tile("Gate"), &ui_icon_ev_motion) &&
          icon_shown(tile("Gate"), &ui_icon_ev_human) && !icon_shown(tile("Gate"), &ui_icon_ev_face) &&
          !icon_shown(tile("Yard"), &ui_icon_rec), "REC, motion and human icons on the recording tile only");
    CHECK(!find("offline", 1), "no toast for a camera whose tile shows it");
    CHECK(!find("Main menu", 0), "navigation bar hidden at first");
    shot("live");
}

static void test_navbar(void)
{
    move_to(W / 2, H + 50);                     /* clamped to the bottom edge */
    lv_obj_t *menu = find("Main menu", 1);
    int x, y;
    CHECK(menu, "navigation bar on the bottom edge");
    if (menu) {
        center_of(menu, &x, &y);
        CHECK(y > H - VG_SIZE_NAVBAR, "bar at the bottom (y %d)", y);
    }
    shot("navbar");
    move_to(W / 2, H / 2);
    run(VG_DURATION_NAVBAR_HIDE + 300);
    menu = find("Main menu", 1);
    if (menu) center_of(menu, &x, &y);
    CHECK(!menu || y >= H, "bar hidden %d ms after the pointer left", VG_DURATION_NAVBAR_HIDE);
}

static void test_context_and_full_screen(void)
{
    move_to(300, 200);                          /* over CAM1 */
    button(BTN_RIGHT);
    run(50);
    CHECK(ui_popup_is_open() && !find("Full screen", 1) && !find("Pin navigation bar", 1) &&
          !find("Next page", 1), "the context menu has nothing but its seven items");
    {
        static const char *const order[] = { "Main menu", "Channels", "Playback", "Record", "PTZ", "Display", "Split" };
        int prev = -1, ok = 1;
        for (size_t i = 0; i < sizeof order / sizeof order[0]; ++i) {
            lv_obj_t *l = find(order[i], 1);
            int x, y;
            if (l) center_of(l, &x, &y);
            ok &= l && y > prev;
            prev = l ? y : prev;
        }
        CHECK(ok, "context menu: Main menu, Channels, Playback, Record, PTZ, Display, Split");
    }
    shot("context");
    button(BTN_RIGHT);                          /* right again: closed */
    run(50);
    CHECK(!ui_popup_is_open(), "right button closes the context menu");

    int calls = g_tile_calls;
    click_at(900, 200);                         /* CAM2 */
    run(100);
    click_at(900, 200);
    run(50);
    CHECK(g_tile_calls == calls + 1 && g_tiles[1].w == W && g_tiles[0].w == 0,
          "double click: CAM2 full screen (%dx%d)", g_tiles[1].w, g_tiles[1].h);
    CHECK(g_tiles[1].h == H && g_tiles[1].y == 0, "full screen is the whole screen");
    CHECK(find("Yard", 0) && !find("Gate", 0), "one tile");
    shot("fullscreen");
    click_at(640, 300);
    run(100);
    click_at(640, 300);
    run(50);
    CHECK(g_tiles[0].w == 640 && g_tiles[1].x == 640 && g_tiles[1].w == 640, "double click again: 2x2");

    /* Two clicks too far apart are not a double click. */
    calls = g_tile_calls;
    click_at(300, 200);
    run(VG_DURATION_DOUBLE_CLICK + 100);
    click_at(300, 200);
    run(50);
    CHECK(g_tile_calls == calls, "slow clicks keep the layout");

    /* Paging in single view through the context menu. */
    move_to(300, 200);
    button(BTN_RIGHT);
    click_text("Split");
    CHECK(ui_popup_sub_is_open() && find("1 window", 1) && find("4 windows", 1) && !find("8 windows", 1),
          "Split: only the layouts a 4-channel platform can show");
    click_text("1 window");
    CHECK(g_tiles[0].w == W && g_tiles[1].w == 0, "single view shows CAM1");
    push_event(4, "offline");                   /* CAM4 is not on the screen */
    ui_app_refresh();
    CHECK(find("CAM 4: offline", 1), "toast for a camera off the screen");
    run(VG_DURATION_TOAST + 50);
    push_event(2, "REC waiting for disk");
    push_event(3, "REC waiting for disk");
    push_event(1, "REC on");
    ui_app_refresh();
    CHECK(find("2 cameras: REC waiting for disk", 1), "one toast for the same trouble on two cameras");
    shot("toast");
    run(VG_DURATION_TOAST);                     /* gone before the next screens */

    /* Different trouble at once: one after the other, none lost. The same
     * camera twice is still one camera. A camera off the screen failing
     * to connect ("down") is said too. */
    push_event(1, "REC failed");
    push_event(1, "REC failed");
    push_event(2, "no decoder");
    push_event(3, "down");
    ui_app_refresh();
    CHECK(find("Gate: REC failed", 1) && !find("no decoder", 1), "the first toast, one camera");
    run(VG_DURATION_TOAST + 50);
    CHECK(find("Yard: no decoder", 1), "then the second");
    run(VG_DURATION_TOAST + 50);
    CHECK(find("CAM 3: down", 1), "then the third");
    run(VG_DURATION_TOAST + 50);
    CHECK(!find("down", 1), "all shown");
    /* The page arrows: near the right edge, half way down. */
    CHECK(!arrow_up(1), "no arrow while the pointer is elsewhere");
    move_to(W - 20, H / 2);
    run(150);                                   /* the next poll */
    CHECK(arrow_up(1) && !arrow_up(0), "the next-page arrow at the right edge");
    shot("page_arrow");
    int ax, ay;
    center_of(page_arrow(1), &ax, &ay);
    click_at(ax, ay);
    CHECK(g_tiles[1].w == W && g_tiles[0].w == 0, "clicked: the next page shows CAM2");
    move_to(20, H / 2);
    run(150);
    center_of(page_arrow(0), &ax, &ay);
    CHECK(arrow_up(0), "the previous-page arrow at the left edge");
    click_at(ax, ay);
    CHECK(g_tiles[0].w == W, "clicked: back to CAM1");
    run(2200);
    CHECK(!arrow_up(0), "gone after 2 s without moving");
    move_to(20, 50);
    run(150);
    CHECK(!arrow_up(0), "not near the corners");
    click_at(ax, ay);
    button(BTN_RIGHT);
    click_text("Split");
    click_text("4 windows");
    CHECK(g_tiles[0].w == 640 && g_tiles[3].w == 640, "back to 2x2");

    /* The other items. */
    button(BTN_RIGHT);
    click_text("Playback");
    CHECK(find("Playback is not available yet", 1), "Playback: not there yet, said so");
    button(BTN_RIGHT);
    CHECK(!ui_popup_is_open(), "closed");
    button(BTN_RIGHT);
    click_text("Channels");
    CHECK(ui_page_top() && find("Add", 1), "Channels: the camera list");
    button(BTN_RIGHT);
    button(BTN_RIGHT);
    click_text("Record");
    CHECK(ui_page_top() && find("Recording directory", 0), "Record: the recording settings");
    button(BTN_RIGHT);
    button(BTN_RIGHT);
    click_text("Display");
    CHECK(ui_page_top() && find("top right", 0) && find("1, 4", 0), "Display: clock corner, layouts");
    shot("display");
    button(BTN_RIGHT);
    CHECK(!ui_page_top(), "back on the live view");
}

static void test_menu_and_settings(void)
{
    move_to(W / 2, H - 1);
    run(300);
    click_text("Main menu");
    CHECK(ui_page_top() && find("STORAGE", 0) && find("CAMERA", 0) && find("SYSTEM", 0) && find("PLAYBACK", 0) &&
          find("DISPLAY", 0) && find("MANAGEMENT", 0), "main menu: cards and the management row");
    shot("menu");

    lv_obj_t *rec = lv_obj_get_parent(find("STORAGE", 0));
    int rx, ry;
    center_of(rec, &rx, &ry);
    move_to(rx, ry);
    CHECK(lv_obj_has_state(rec, LV_STATE_HOVERED), "the tile under the pointer is hovered");
    click_text("STORAGE");
    CHECK(find("Recording directory", 0) && find("Segment length (s)", 0), "recording form from the schema");
    CHECK(find("Recording: 1 camera.", 1), "the recording's state");
    lv_obj_t *path = textarea(0), *seg = textarea(1);
    CHECK(path && seg && !strcmp(lv_textarea_get_text(seg), "60"), "values from the store");

    /* Save an invalid value: nothing changes, the store's error shows
     * under its field. */
    unsigned gen = config_generation();
    lv_textarea_set_text(path, "rec");
    click_text(SAVE);
    run(50);
    CHECK(config_generation() == gen, "invalid: not saved");
    CHECK(find("must be an absolute path", 1), "error under the field");
    CHECK(find("Not saved: 1 problem", 1), "toast");
    shot("error");

    /* A valid change: saved through the store, the generation moves. */
    lv_textarea_set_text(path, "/tmp");
    click_text(LV_SYMBOL_PLUS);
    click_text(LV_SYMBOL_PLUS);
    click_text(SAVE);
    run(50);
    vigil_config_t c;
    config_get(&c);
    CHECK(config_generation() == gen + 1 && c.record.segment == 62 && !strcmp(c.record.path, "/tmp"),
          "saved: segment %d, path %s", c.record.segment, c.record.path);
    CHECK(!find("must be an absolute path", 1) && find("Saved", 1), "errors gone, toast");
    shot("saved");
    button(BTN_RIGHT);                          /* back to the menu */
    CHECK(find("Main menu", 0), "back from the form");

    click_text("NETWORK");
    CHECK(find("applies after restart", 1), "non-live setting marked");
    click_text("Listen on");                    /* the label: no keyboard */
    lv_obj_t *listen = textarea(0);
    int x, y;
    center_of(listen, &x, &y);
    click_at(x, y);
    CHECK(find("Listen on", 0) && lv_obj_check_type(lv_obj_get_child(lv_screen_active(), -1),
                                                    &lv_keyboard_class) &&
          visible(lv_obj_get_child(lv_screen_active(), -1)), "keyboard on a text field");
    run(VG_DURATION_TOAST);                     /* the "Saved" toast is gone */
    shot("keyboard");
    button(BTN_RIGHT);                          /* closes the keyboard */
    button(BTN_RIGHT);                          /* leaves the page (unchanged) */
    CHECK(find("SYSTEM", 0) && !ui_popup_is_open(), "back in the menu");

    /* A function vigil does not have yet: said so. */
    click_text("BACKUP");
    CHECK(ui_popup_is_open() && find("Backup is not available yet", 1), "BACKUP: not there yet");
    button(BTN_RIGHT);
    click_text("DISPLAY");
    CHECK(find("top right", 0), "DISPLAY: the display page");
    button(BTN_RIGHT);
}

static void test_cameras(void)
{
    click_text("CAMERA");
    CHECK(find("rtsp://admin:***@192.168.1.10/stream1", 0), "URL redacted as in the API");
    CHECK(find("Search settings", 1) && find("No devices yet", 1) && find("Empty", 0),
          "CAMERA: the Channels page, its two panes, channel 4 empty");
    shot("cameras");
    lv_obj_t *edit = find(LV_SYMBOL_EDIT "  Edit", 0);
    int x, y;
    center_of(edit, &x, &y);
    click_at(x, y);
    lv_obj_t *url = textarea(0), *name = textarea(1);
    CHECK(url && !strcmp(lv_textarea_get_text(url), "rtsp://admin:***@192.168.1.10/stream1"), "form shows ***");
    lv_textarea_set_text(name, "Front gate");
    click_text(SAVE);
    run(50);
    vigil_config_t c;
    config_get(&c);
    CHECK(!strcmp(c.cameras[0].name, "Front gate") &&
          !strcmp(c.cameras[0].url, "rtsp://admin:secret@192.168.1.10/stream1"),
          "name saved, password kept: %s", c.cameras[0].url);

    /* A change, then back: asked first. */
    lv_textarea_set_text(name, "Changed");
    button(BTN_RIGHT);
    CHECK(ui_popup_is_open() && find("Discard the changes", 1), "discard question");
    click_text("Discard");
    CHECK(find("Search settings", 1) && find("Front gate", 0), "back on the Channels page, updated");

    /* Clear the third channel, confirmed. */
    lv_obj_t *rm = find_nth(lv_screen_active(), LV_SYMBOL_TRASH "  Clear", 2);
    CHECK(rm, "third Clear button");
    if (rm) {
        center_of(rm, &x, &y);
        click_at(x, y);
        CHECK(ui_popup_is_open() && find("Clear channel 3?", 1), "clear question");
        lv_obj_t *yes = find_in(lv_layer_top(), "Clear", 0);
        CHECK(yes, "Clear button in the question");
        if (yes) {
            center_of(yes, &x, &y);
            click_at(x, y);
        }
    }
    config_get(&c);
    CHECK(c.n_cameras == 2, "camera removed (%d left)", c.n_cameras);
    button(BTN_RIGHT);
}

/* The Channels page's search pane with fake devices and jobs: a search
 * that runs and ends, checking devices, Change IP, a mass change's preview
 * and its apply, auto add, adding one device to a channel. */
static void test_channels_discovery(void)
{
    click_text("CAMERA");
    g_dj_hold = 1;
    click_text(LV_SYMBOL_REFRESH "  Search");
    run(300);
    CHECK(find("Searching... 40%", 1), "Search: the job's progress");
    shot("channels_searching");
    fake_job_end();
    g_dj_hold = 0;
    run(300);
    CHECK(find("5 devices", 0) && find("192.168.1.108", 0) && find("On channel 1", 0) && find("New", 0) &&
          find("NETIP, ONVIF", 0), "the devices found, with their protocols and status");
    lv_obj_t *dh = find("192.168.1.108", 0);
    int x, y;
    if (!dh) return;
    center_of(dh, &x, &y);
    click_at(x, y);
    run(100);
    CHECK(find("1 checked", 0) && find("Replace", 1) && find("Add checked", 1), "one device checked: Add/Replace");
    shot("channels_results");

    click_text("Change IP...");
    CHECK(find("Change IP: 192.168.1.108", 0) && textarea(0) &&
          !strcmp(lv_textarea_get_text(textarea(0)), "192.168.1.108"), "Change IP: its form");
    lv_textarea_set_text(textarea(0), "192.168.0.31");
    lv_textarea_set_text(textarea(4), "pw1");
    shot("channels_change_ip");
    click_text(LV_SYMBOL_OK "  Change");
    run(400);
    CHECK(find("1 changed", 0) && find("changed", 0) && !strcmp(g_dj_last_pass, "pw1"), "Change IP: done");
    button(BTN_RIGHT);
    run(100);

    click_text("Select all");
    run(100);
    CHECK(find("5 checked", 0), "Select all");
    click_text("Mass change...");
    lv_textarea_set_text(textarea(0), "192.168.0.20");
    click_text(LV_SYMBOL_EYE_OPEN "  Preview");
    run(400);
    CHECK(find("plan for 5 devices", 0) && find("192.168.0.21", 0) && find("192.168.0.24", 0), "the preview");
    shot("channels_mass_preview");
    click_text(LV_SYMBOL_OK "  Apply");
    run(400);
    CHECK(find("5 changed", 0) && find("not authorized", 1), "applied: one refused");
    button(BTN_RIGHT);
    run(100);

    click_text("Auto add all");
    CHECK(find("go to the 2 empty channels", 1), "Auto add: what it will do");
    click_text(LV_SYMBOL_OK "  Add all");
    run(400);
    CHECK(find("2 added, 2 not", 0) && find("no empty channel left", 0), "Auto add: the results");
    shot("channels_autoadd");
    button(BTN_RIGHT);
    run(100);

    /* One device to a channel, from its row's menu. */
    click_text("Select all");                   /* none checked again */
    run(100);
    lv_obj_t *add_to = find_nth(lv_screen_active(), LV_SYMBOL_PLUS "  Add to...", 1);
    if (add_to) {
        center_of(add_to, &x, &y);
        click_at(x, y);
    }
    CHECK(ui_popup_is_open() && find_in(lv_layer_top(), "Add to channel 3", 1), "Add to...: the empty channels");
    lv_obj_t *to3 = find_in(lv_layer_top(), "Add to channel 3", 1);
    if (to3) {
        center_of(to3, &x, &y);
        click_at(x, y);
    }
    run(400);
    CHECK(find("added to channel 3", 1), "added: a toast");
    button(BTN_RIGHT);
    g_dj_devices = 0;
}

static void test_system(void)
{
    run(VG_DURATION_TOAST);
    click_text("SYSTEM");
    CHECK(find("vigild 0.1.0", 0) && find("0 d 01:02:03", 0), "system page");
    shot("system");
    button(BTN_RIGHT);
    button(BTN_RIGHT);
    CHECK(!ui_page_top(), "back on the live view");
}

/* Two cameras whose URLs differ only in the password redact the same way;
 * saving one must not give it (or the other) the other's password. */
static void test_same_redacted_urls(void)
{
    static const char cams[] = "[{\"url\":\"rtsp://admin:one@192.168.1.20/s\",\"name\":\"A\"},"
                               "{\"url\":\"rtsp://admin:two@192.168.1.20/s\",\"name\":\"B\"}]";
    schema_errors_t se = { 0 };
    CHECK(config_put_json("cameras", cams, strlen(cams), &se) == CONFIG_OK, "two cameras");
    run(300);
    move_to(W / 2, H - 1);
    run(300);
    click_text("Main menu");
    click_text("CAMERA");
    lv_obj_t *edit = find_nth(lv_screen_active(), LV_SYMBOL_EDIT "  Edit", 1);
    int x, y;
    center_of(edit, &x, &y);
    click_at(x, y);
    lv_textarea_set_text(textarea(1), "B2");
    click_text(SAVE);
    vigil_config_t c;
    config_get(&c);
    CHECK(!strcmp(c.cameras[0].url, "rtsp://admin:one@192.168.1.20/s") &&
          !strcmp(c.cameras[1].url, "rtsp://admin:two@192.168.1.20/s") && !strcmp(c.cameras[1].name, "B2"),
          "each camera keeps its own password: %s, %s", c.cameras[0].url, c.cameras[1].url);

    /* Changed through the API while the form is open: refused, nothing
     * of the API's change lost, back to the (current) list. */
    lv_textarea_set_text(textarea(1), "B3");
    static const char cams2[] = "[{\"url\":\"rtsp://admin:two@192.168.1.20/s\",\"name\":\"B\"}]";
    CHECK(config_put_json("cameras", cams2, strlen(cams2), &se) == CONFIG_OK, "API change");
    click_text(SAVE);
    config_get(&c);
    CHECK(c.n_cameras == 1 && !strcmp(c.cameras[0].name, "B"), "stale camera form not saved");
    CHECK(find("changed elsewhere", 1) && find("Add", 1), "toast, back in the list");
    button(BTN_RIGHT);                          /* to the menu */
}

static void test_stale_and_cancel(void)
{
    click_text("STORAGE");
    lv_obj_t *seg = textarea(1);
    lv_textarea_set_text(seg, "300");
    schema_errors_t se = { 0 };
    CHECK(config_put_json("record", "{\"keep\":7}", 10, &se) == CONFIG_OK, "API change");
    click_text(SAVE);
    vigil_config_t c;
    config_get(&c);
    CHECK(c.record.keep == 7 && c.record.segment == 60, "stale form not saved (keep %d, segment %d)",
          c.record.keep, c.record.segment);
    CHECK(!strcmp(lv_textarea_get_text(textarea(2)), "7") && !strcmp(lv_textarea_get_text(seg), "60"),
          "the form shows the settings now");
    lv_textarea_set_text(seg, "120");
    click_text("Cancel");
    CHECK(ui_popup_is_open() && find("Discard the changes", 1), "Cancel with changes asks");
    click_text("Discard");
    CHECK(find("Main menu", 0) && !find("Recording directory", 0), "discarded");
    button(BTN_RIGHT);
    CHECK(!ui_page_top(), "back on the live view");
}

/* Two pointing devices: one's release must not end the other's press;
 * unplugging a device that is pressed releases. */
static void test_two_mice(lv_indev_t *ptr)
{
    int b[2];
    if (pipe(b) < 0) return;
    fcntl(b[0], F_SETFL, O_NONBLOCK);
    input_evdev_attach(b[0], "second mouse", 0, 0, 0, 0, 0);
    struct input_event dn = { .type = EV_KEY, .code = BTN_LEFT, .value = 1 }, up = dn,
                       syn = { .type = EV_SYN, .code = SYN_REPORT };
    up.value = 0;
    ui_system_open();                           /* a page with room where nothing is to click */
    run(50);
    move_to(640, 650);
    ev(EV_KEY, BTN_LEFT, 1);
    ev(EV_SYN, SYN_REPORT, 0);
    run(40);
    if (write(b[1], &dn, sizeof dn) < 0 || write(b[1], &syn, sizeof syn) < 0) abort();
    run(40);
    ev(EV_KEY, BTN_LEFT, 0);
    ev(EV_SYN, SYN_REPORT, 0);
    run(40);
    CHECK(lv_indev_get_state(ptr) == LV_INDEV_STATE_PRESSED, "still pressed by the second mouse");
    if (write(b[1], &up, sizeof up) < 0 || write(b[1], &syn, sizeof syn) < 0) abort();
    run(40);
    CHECK(lv_indev_get_state(ptr) == LV_INDEV_STATE_RELEASED, "released by both");
    if (write(b[1], &dn, sizeof dn) < 0 || write(b[1], &syn, sizeof syn) < 0) abort();
    run(40);
    close(b[1]);                                /* unplugged while pressed */
    run(80);
    CHECK(lv_indev_get_state(ptr) == LV_INDEV_STATE_RELEASED, "unplugging releases");
    button(BTN_RIGHT);
    CHECK(!ui_page_top(), "back on the live view");
}

/* ---- the keyboard ------------------------------------------------------------------------------- */

static int g_kbd[2] = { -1, -1 };       /* pipe: test -> evdev reader, as a keyboard */

static void kev(int fd, int code, int value)
{
    struct input_event e[2] = { { .type = EV_KEY, .code = (unsigned short)code, .value = value },
                                { .type = EV_SYN, .code = SYN_REPORT } };
    if (write(fd, e, sizeof e) != (ssize_t)sizeof e) abort();
}

/* A key pressed and released, with a modifier held around it (0: none). */
static void key_with(int mod, int code)
{
    if (mod) kev(g_kbd[1], mod, 1);
    kev(g_kbd[1], code, 1);
    kev(g_kbd[1], code, 0);
    if (mod) kev(g_kbd[1], mod, 0);
    run(100);
}

static void key(int code) { key_with(0, code); }

/* Type ASCII text the way a US keyboard does. */
static void type(const char *text)
{
    static const char row1[] = "qwertyuiop", row2[] = "asdfghjkl", row3[] = "zxcvbnm";
    for (; *text; ++text) {
        char c = *text;
        int shift = c >= 'A' && c <= 'Z', code = 0;
        if (shift) c = (char)(c - 'A' + 'a');
        const char *p;
        if ((p = strchr(row1, c)) && c) code = KEY_Q + (int)(p - row1);
        else if ((p = strchr(row2, c)) && c) code = KEY_A + (int)(p - row2);
        else if ((p = strchr(row3, c)) && c) code = KEY_Z + (int)(p - row3);
        else if (c >= '1' && c <= '9') code = KEY_1 + c - '1';
        else if (c == '0') code = KEY_0;
        else if (c == '/') code = KEY_SLASH;
        else if (c == '.') code = KEY_DOT;
        else if (c == '-') code = KEY_MINUS;
        else if (c == ' ') code = KEY_SPACE;
        else abort();
        key_with(shift ? KEY_LEFTSHIFT : 0, code);
    }
}

static lv_obj_t *focused(void)
{
    lv_group_t *g = lv_indev_get_group(input_evdev_keypad());
    return g ? lv_group_get_focused(g) : NULL;
}

static int has_focus(lv_obj_t *o) { return o && lv_obj_has_state(o, LV_STATE_FOCUSED); }

static void test_keyboard_live(void)
{
    if (pipe(g_kbd) < 0) abort();
    fcntl(g_kbd[0], F_SETFL, O_NONBLOCK);
    input_evdev_attach_keyboard(g_kbd[0], "test keyboard");
    run(VG_DURATION_TOAST);                     /* the last toast is gone */
    click_at(300, 200);                         /* CAM1, by the mouse: selected */
    run(VG_DURATION_DOUBLE_CLICK + 100);
    CHECK(!ui_keyboard_in_use(), "no key yet");

    key(KEY_RIGHT);
    CHECK(ui_keyboard_in_use(), "a key: the keyboard is in use");
    CHECK(has_focus(tile("Yard")) && !has_focus(tile("Gate")), "Right: CAM2 selected");
    move_to(1000, 600);                         /* the mouse over CAM4 changes nothing */
    CHECK(has_focus(tile("Yard")), "hover does not take the selection");
    shot("focus");

    key(KEY_ENTER);
    CHECK(g_tiles[1].w == W && g_tiles[0].w == 0, "Enter: CAM2 full screen");
    CHECK(has_focus(tile("Yard")), "still selected full screen");
    key(KEY_ENTER);
    CHECK(g_tiles[0].w == 640 && g_tiles[1].w == 640, "Enter again: back to 2x2");
    key(KEY_DOWN);
    CHECK(has_focus(tile("CAM 4")), "Down: CAM4");
    key(KEY_1);
    CHECK(g_tiles[3].w == W && g_tiles[0].w == 0, "1: single view of the selected CAM4");
    key(KEY_PAGEDOWN);
    CHECK(g_tiles[0].w == W, "PageDown: next page (wraps to CAM1)");
    key(KEY_RIGHTBRACE);
    CHECK(g_tiles[1].w == W, "]: next page, CAM2");
    key(KEY_PAGEUP);
    key(KEY_LEFTBRACE);
    CHECK(g_tiles[3].w == W, "PageUp, [: back two pages, CAM4");
    key(KEY_4);
    CHECK(g_tiles[0].w == 640 && g_tiles[3].w == 640 && has_focus(tile("CAM 4")), "4: 2x2, CAM4 still selected");

    key(KEY_TAB);
    run(300);                                   /* it slides up */
    lv_obj_t *menu = find("Main menu", 1);
    int x, y;
    if (menu) center_of(menu, &x, &y);
    CHECK(menu && y < H && has_focus(lv_obj_get_parent(menu)), "Tab: the navigation bar comes up, focused");
    key(KEY_ESC);
    run(VG_DURATION_NAVBAR_HIDE + 300);
    menu = find("Main menu", 1);
    if (menu) center_of(menu, &x, &y);
    CHECK(has_focus(tile("CAM 4")) && (!menu || y >= H), "Esc: back to the tiles, the bar hides");

    key_with(KEY_LEFTSHIFT, KEY_F10);
    CHECK(ui_popup_is_open() && find("Split", 1), "Shift+F10: the context menu");
    key(KEY_ESC);

    /* Split: Right opens it, Esc or Left closes it, then the menu. */
    key(KEY_COMPOSE);
    for (int i = 0; i < 6; ++i) key(KEY_DOWN);  /* down to Split */
    key(KEY_RIGHT);
    CHECK(ui_popup_sub_is_open() && has_focus(lv_obj_get_parent(find("4 windows", 1))),
          "Menu key, Down x6, Right: Split open on the current layout");
    shot("split");
    key(KEY_ESC);
    CHECK(!ui_popup_sub_is_open() && ui_popup_is_open(), "Esc closes the submenu only");
    key(KEY_RIGHT);
    key(KEY_LEFT);
    CHECK(!ui_popup_sub_is_open() && ui_popup_is_open(), "Left closes it too");
    key(KEY_ENTER);
    key(KEY_UP);
    key(KEY_ENTER);
    CHECK(!ui_popup_is_open() && g_tiles[3].w == W && g_tiles[0].w == 0, "Enter, Up, Enter: 1 window, CAM4");
    key(KEY_8);
    key(KEY_0);
    CHECK(g_tiles[3].w == W, "8 and 0 do nothing on a 4-channel platform");
    key(KEY_4);

    key(KEY_F1);
    CHECK(ui_popup_is_open() && find("Keyboard shortcuts", 1), "F1: help");
    shot("help");
    key(KEY_ESC);
    CHECK(!ui_popup_is_open(), "Esc closes the help");
    key_with(KEY_LEFTSHIFT, KEY_SLASH);
    CHECK(ui_popup_is_open() && find("Keyboard shortcuts", 1), "?: help");
    key(KEY_F1);
    CHECK(!ui_popup_is_open(), "F1 again closes it");
    key(KEY_ESC);
    CHECK(!ui_popup_is_open() && !ui_page_top(), "Esc on the live view opens nothing");
}

static void test_keyboard_settings(void)
{
    key(KEY_M);
    CHECK(ui_page_top() && find("STORAGE", 0), "M: main menu");
    lv_obj_t *card1 = lv_obj_get_parent(lv_obj_get_parent(find("PLAYBACK", 0)));
    CHECK(has_focus(card1), "the first card focused");
    key(KEY_RIGHT);
    key(KEY_DOWN);
    lv_obj_t *backup = lv_obj_get_parent(lv_obj_get_parent(find("BACKUP", 0)));
    CHECK(has_focus(backup), "Right, Down: BACKUP");
    shot("menu_focus");
    key(KEY_ENTER);
    CHECK(ui_popup_is_open() && find("Backup is not available yet", 1), "Enter: not there yet");
    key(KEY_ENTER);
    CHECK(!ui_popup_is_open(), "Enter: closed");

    /* The mouse on another card: the ring moves there, one ring only. */
    click_text("DISPLAY");
    CHECK(find("top right", 0), "a click on DISPLAY opens it");
    key(KEY_ESC);
    lv_obj_t *display = lv_obj_get_parent(lv_obj_get_parent(find("DISPLAY", 0)));
    CHECK(has_focus(display) && !has_focus(backup) && !has_focus(card1), "the clicked card has the ring, alone");
    key(KEY_LEFT);
    CHECK(has_focus(backup), "and the arrows go on from it");
    key(KEY_TAB);
    CHECK(has_focus(lv_obj_get_parent(find("CAMERA", 0))), "Tab: the management row");
    key(KEY_RIGHT);
    key(KEY_RIGHT);
    CHECK(has_focus(lv_obj_get_parent(find("STORAGE", 0))), "Right, Right: STORAGE");
    key(KEY_ENTER);
    lv_obj_t *path = textarea(0), *seg = textarea(1);
    CHECK(find("Recording directory", 0) && focused() == path, "Enter: the form, its first field focused");
    lv_obj_t *kb = lv_obj_get_child(lv_screen_active(), -1);

    /* Backspace, held: the kernel's repeats delete too. */
    kev(g_kbd[1], KEY_BACKSPACE, 1);
    for (int i = 0; i < 9; ++i) kev(g_kbd[1], KEY_BACKSPACE, 2);
    kev(g_kbd[1], KEY_BACKSPACE, 0);
    run(400);
    CHECK(!strcmp(lv_textarea_get_text(path), ""), "Backspace and its repeats: '%s' left", lv_textarea_get_text(path));
    type("/tmp/Kbd");
    CHECK(!strcmp(lv_textarea_get_text(path), "/tmp/Kbd"), "typed: %s", lv_textarea_get_text(path));
    CHECK(lv_obj_has_flag(kb, LV_OBJ_FLAG_HIDDEN), "no on-screen keyboard with a keyboard in use");
    key(KEY_ENTER);
    CHECK(focused() == seg, "Enter in a text field: the next field");
    int before = atoi(lv_textarea_get_text(seg));
    key(KEY_UP);
    key(KEY_UP);
    key(KEY_DOWN);
    CHECK(atoi(lv_textarea_get_text(seg)) == before + 1, "Up, Up, Down: %d -> %s", before, lv_textarea_get_text(seg));
    CHECK(focused() == seg, "arrows on a number change it, not the focus");
    key(KEY_TAB);
    lv_obj_t *after = focused();
    key_with(KEY_LEFTSHIFT, KEY_TAB);
    CHECK(after != seg && focused() == seg, "Tab, Shift+Tab");
    shot("form_focus");

    unsigned gen = config_generation();
    key_with(KEY_LEFTCTRL, KEY_S);
    vigil_config_t c;
    config_get(&c);
    CHECK(config_generation() == gen + 1 && !strcmp(c.record.path, "/tmp/Kbd") && c.record.segment == before + 1,
          "Ctrl+S saved: %s, %d", c.record.path, c.record.segment);
    CHECK(find("Saved", 1), "toast");
    key(KEY_ESC);
    CHECK(find("Main menu", 0) && ui_page_top() && !find("Recording directory", 0), "Esc: back to the menu");

    /* Unsaved changes: Esc asks; Enter keeps the page (Cancel is focused);
     * Right, Enter discards. */
    key(KEY_ENTER);
    CHECK(find("Recording directory", 0), "Enter on the menu tile again");
    type("x");
    key(KEY_ESC);
    CHECK(ui_popup_is_open() && find("Discard the changes", 1), "Esc with changes asks");
    key(KEY_ENTER);
    CHECK(!ui_popup_is_open() && find("Recording directory", 0), "Enter: Cancel, still on the form");
    key(KEY_ESC);
    key(KEY_RIGHT);
    key(KEY_ENTER);
    CHECK(!ui_popup_is_open() && find("Main menu", 0) && !find("Recording directory", 0), "Right, Enter: discarded");
    config_get(&c);
    CHECK(!strcmp(c.record.path, "/tmp/Kbd"), "nothing saved");

    /* The Channels page: Tab to the channel table, Enter edits, Space
     * toggles, Delete asks to clear. */
    key(KEY_LEFT);
    key(KEY_LEFT);
    key(KEY_ENTER);
    CHECK(find("Search settings", 1), "Left, Left: CAMERA, the Channels page");
    CHECK(c.n_cameras == 1, "one camera left by the steps before");
    lv_obj_t *chans = NULL;
    for (int i = 0; i < 20 && !chans; ++i) {
        key(KEY_TAB);
        lv_obj_t *f = focused();
        if (f && lv_obj_has_flag(f, UI_FLAG_ARROWS) && lv_obj_get_child_count(f) &&
            find_in(lv_obj_get_child(f, 0), "Edit", 1))
            chans = f;                          /* the rows of channels: Edit on the first */
    }
    CHECK(chans, "Tab reaches the channel table");
    key(KEY_ENTER);
    CHECK(find("Cameras: 1", 0), "Enter: the camera's form");
    lv_obj_t *sw = NULL;
    key(KEY_TAB);
    key(KEY_TAB);
    sw = focused();
    CHECK(sw && lv_obj_check_type(sw, &lv_switch_class), "Tab, Tab: the switch");
    int on = sw && lv_obj_has_state(sw, LV_STATE_CHECKED);
    key(KEY_SPACE);
    CHECK(sw && lv_obj_has_state(sw, LV_STATE_CHECKED) != on, "Space toggles it");
    key(KEY_ENTER);
    CHECK(sw && lv_obj_has_state(sw, LV_STATE_CHECKED) == on, "Enter toggles it back");
    key(KEY_UP);
    CHECK(focused() == textarea(1), "Up: the field above");
    key(KEY_ESC);
    CHECK(find("Search settings", 1) && !ui_popup_is_open(), "Esc: unchanged, back on the Channels page");
    key(KEY_DELETE);
    CHECK(ui_popup_is_open() && find("Clear channel 1?", 1), "Delete on a row asks");
    key(KEY_ESC);
    CHECK(!ui_popup_is_open(), "Esc: not cleared");
    key(KEY_ESC);
    key(KEY_ESC);
    CHECK(!ui_page_top(), "Esc, Esc: back on the live view");
}

/* A drop-down (no setting has one yet): the arrows change it in place,
 * Enter opens the list. */
static void test_keyboard_dropdown(void)
{
    ui_page_t *p = ui_page_push("Test");
    lv_obj_t *dd = lv_dropdown_create(p->body);
    lv_dropdown_set_options(dd, "one\ntwo\nthree");
    lv_group_add_obj(p->group, dd);
    ui_page_ready(p);
    run(50);
    CHECK(focused() == dd, "the drop-down is focused");
    key(KEY_DOWN);
    key(KEY_RIGHT);
    CHECK(lv_dropdown_get_selected(dd) == 2, "Down, Right: third (%u)", (unsigned)lv_dropdown_get_selected(dd));
    key(KEY_UP);
    CHECK(lv_dropdown_get_selected(dd) == 1 && !lv_dropdown_is_open(dd), "Up: second, closed");
    key(KEY_ENTER);
    CHECK(lv_dropdown_is_open(dd), "Enter opens the list");
    key(KEY_UP);
    key(KEY_ENTER);
    CHECK(!lv_dropdown_is_open(dd) && lv_dropdown_get_selected(dd) == 0, "Up, Enter: first, closed");
    key(KEY_ENTER);
    key(KEY_ESC);
    CHECK(!lv_dropdown_is_open(dd) && ui_page_top() == p, "Esc closes the list, not the page");
    key(KEY_ESC);
    CHECK(!ui_page_top(), "Esc: the page");
}

static void osk_cancel_cb(lv_event_t *e) { ui_osk_hide(lv_event_get_target(e)); }

/* Press the on-screen keyboard's key `text` (by its text: the buttons
 * have no labels of their own to click). */
static int osk_key(lv_obj_t *kb, const char *text)
{
    for (uint32_t i = 0;; ++i) {
        const char *t = lv_buttonmatrix_get_button_text(kb, i);
        if (!t) return -1;
        if (strcmp(t, text)) continue;
        lv_buttonmatrix_set_selected_button(kb, i);
        lv_obj_send_event(kb, LV_EVENT_VALUE_CHANGED, &i);
        run(30);
        return 0;
    }
}

static lv_obj_t *osk(void)
{
    lv_obj_t *kb = lv_obj_get_child(lv_screen_active(), -1);
    return kb && lv_obj_check_type(kb, &lv_keyboard_class) ? kb : NULL;
}

/* An empty slot's "+": the camera form for that channel; the on-screen
 * keyboard (mouse only) next to the field; a camera added at channel 4
 * with 2 and 3 left empty. Then Channel info. */
static void test_empty_slot(void)
{
    vigil_config_t c;
    config_get(&c);
    CHECK(!ui_keyboard_in_use() && c.n_cameras == 1, "mouse only; one camera (%d)", c.n_cameras);
    lv_obj_t *plus = NULL;
    lv_obj_t *t4 = tile("CAM 4");
    for (uint32_t i = 0; t4 && i < lv_obj_get_child_count(t4); ++i) {
        lv_obj_t *o = lv_obj_get_child(t4, (int32_t)i);
        if (lv_obj_check_type(o, &lv_button_class) && visible(o)) plus = o;
    }
    CHECK(plus, "CAM 4: a \"+\"");
    if (!plus) return;
    int x, y;
    center_of(plus, &x, &y);
    click_at(x, y);
    run(100);
    CHECK(find("Cameras 4: new", 0), "the form for a camera on channel 4");
    lv_obj_t *url = textarea(0);
    center_of(url, &x, &y);
    click_at(x, y);
    lv_obj_t *kb = osk();
    lv_area_t ka, ua;
    if (kb) lv_obj_get_coords(kb, &ka);
    lv_obj_get_coords(url, &ua);
    CHECK(kb && visible(kb) && ka.y1 > ua.y2 && ka.x1 == ua.x1 && lv_obj_get_width(kb) <= 600 &&
          lv_obj_get_height(kb) <= 200, "the keyboard: small, right below the field (%d,%d %dx%d)",
          kb ? ka.x1 : -1, kb ? ka.y1 : -1, kb ? lv_obj_get_width(kb) : 0, kb ? lv_obj_get_height(kb) : 0);
    shot("osk");
    static const char *const typed[] = { "r", "t", "s", "p", ":", "/", "/", "1", "0", ".", "1", ".",
                                         "1", ".", "4", "/", LV_SYMBOL_UP, "C", "a", "m" };
    for (size_t i = 0; kb && i < sizeof typed / sizeof typed[0]; ++i) osk_key(kb, typed[i]);
    osk_key(kb, "#+=");
    shot("osk_symbols");
    osk_key(kb, "?");
    osk_key(kb, "a");                           /* not on this page */
    osk_key(kb, "abc");
    osk_key(kb, "x");
    CHECK(!strcmp(lv_textarea_get_text(url), "rtsp://10.1.1.4/Cam?x"), "typed with the mouse: %s",
          lv_textarea_get_text(url));
    osk_key(kb, LV_SYMBOL_OK);
    CHECK(kb && !visible(kb), "done: closed");
    click_text(SAVE);
    config_get(&c);
    CHECK(c.n_cameras == 4 && !c.cameras[1].url[0] && !c.cameras[2].url[0] &&
          !strcmp(c.cameras[3].url, "rtsp://10.1.1.4/Cam?x"), "a camera at channel 4, 2 and 3 empty (%d)",
          c.n_cameras);
    button(BTN_RIGHT);
    run(VG_DURATION_TOAST);

    /* A field low on the screen: the keyboard opens above it; a click
     * outside closes it (the page hides it on LV_EVENT_CANCEL). */
    ui_page_t *p = ui_page_push("Test");
    lv_obj_t *low = lv_textarea_create(p->scr);
    lv_textarea_set_one_line(low, 1);
    lv_obj_set_width(low, 400);
    lv_obj_set_pos(low, 800, H - 60);
    kb = ui_osk_create(p->scr);
    lv_obj_add_event_cb(kb, osk_cancel_cb, LV_EVENT_CANCEL, NULL);
    ui_osk_show(kb, low, 0);
    run(50);
    lv_obj_get_coords(kb, &ka);
    lv_obj_get_coords(low, &ua);
    CHECK(visible(kb) && ka.y2 < ua.y1 && ka.x2 <= W - VG_SPACE_SM, "a low field: the keyboard above it, on the "
          "screen (%d,%d-%d,%d)", ka.x1, ka.y1, ka.x2, ka.y2);
    shot("osk_above");
    click_at(200, 200);
    CHECK(!visible(kb), "a click outside closes it");
    button(BTN_RIGHT);
    move_to(W / 2, H - 1);
    run(300);
    click_text("Main menu");
    click_text("SYSTEM");
    click_text("Channel info");
    CHECK(find("Channel info", 0) && find("1920x1080", 0) && find("motion, human", 0) &&
          find("Connecting", 0), "Channel info: state, size, events");
    shot("channel_info");
    button(BTN_RIGHT);
    button(BTN_RIGHT);
    button(BTN_RIGHT);
    CHECK(!ui_page_top(), "back on the live view");
}

/* Caps Lock, Num Lock, a second keyboard, and unplugging with Shift held. */
static void test_keyboard_plug(void)
{
    ui_page_t *p = ui_page_push("Test");
    lv_obj_t *ta = lv_textarea_create(p->body);
    lv_textarea_set_one_line(ta, 1);
    lv_group_add_obj(p->group, ta);
    ui_page_ready(p);
    run(50);
    key(KEY_CAPSLOCK);
    type("aB");
    key(KEY_CAPSLOCK);
    key(KEY_KP5);
    key(KEY_NUMLOCK);
    key(KEY_KP4);                               /* Num Lock off: Left */
    key(KEY_KP1);                               /* Num Lock off: End */
    key(KEY_NUMLOCK);
    key(KEY_KPPLUS);
    CHECK(!strcmp(lv_textarea_get_text(ta), "Ab5+"), "Caps Lock, keypad: '%s'", lv_textarea_get_text(ta));

    int k2[2];
    if (pipe(k2) < 0) abort();
    fcntl(k2[0], F_SETFL, O_NONBLOCK);
    input_evdev_attach_keyboard(k2[0], "second keyboard");
    kev(g_kbd[1], KEY_LEFTSHIFT, 1);            /* held on the first */
    kev(k2[1], KEY_X, 1);
    kev(k2[1], KEY_X, 0);
    run(100);
    close(g_kbd[1]);                            /* the first unplugged, Shift down */
    run(100);
    kev(k2[1], KEY_X, 1);
    kev(k2[1], KEY_X, 0);
    run(100);
    CHECK(!strcmp(lv_textarea_get_text(ta), "Ab5+Xx"), "Shift from either keyboard, gone with it: '%s'",
          lv_textarea_get_text(ta));
    CHECK(ui_keyboard_in_use(), "a keyboard is left");
    /* A burst of more keys than the reader queues: none lost. */
    size_t before = strlen(lv_textarea_get_text(ta));
    for (int i = 0; i < 40; ++i) {
        kev(k2[1], KEY_A, 1);
        kev(k2[1], KEY_A, 0);
    }
    run(3000);
    CHECK(strlen(lv_textarea_get_text(ta)) == before + 40, "40 keys at once: %zu typed",
          strlen(lv_textarea_get_text(ta)) - before);
    kev(k2[1], KEY_ESC, 1);
    kev(k2[1], KEY_ESC, 0);
    run(100);
    CHECK(!ui_page_top(), "Esc from the second keyboard");
    close(k2[1]);
    run(100);
    CHECK(!ui_keyboard_in_use(), "no keyboard: the UI is for the mouse again");
    g_kbd[1] = -1;

    /* A keyboard unplugged with a key still unread: the key goes with it. */
    int k3[2];
    if (pipe(k3) < 0) abort();
    fcntl(k3[0], F_SETFL, O_NONBLOCK);
    input_evdev_attach_keyboard(k3[0], "third keyboard");
    kev(k3[1], KEY_DOWN, 1);
    kev(k3[1], KEY_DOWN, 0);
    close(k3[1]);
    run(200);
    CHECK(!ui_keyboard_in_use(), "a key read with the unplug does not bring the keyboard UI back");
}

static int rect_is(int ch, int x, int y, int w, int h)
{
    const ui_rect_t *r = &g_tiles[ch];
    if (r->x == x && r->y == y && r->w == w && r->h == h) return 1;
    fprintf(stderr, "CAM%d at %d,%d %dx%d, not %d,%d %dx%d\n", ch + 1, r->x, r->y, r->w, r->h, x, y, w, h);
    return 0;
}

/* A platform with 16 channels: Split offers 1, 4, 8, 9, 16. */
static void test_split16(void)
{
    move_to(300, 200);
    button(BTN_RIGHT);
    click_text("Split");
    CHECK(find("8 windows", 1) && find("9 windows", 1) && find("16 windows", 1) && !find("32 windows", 1),
          "16 channels: Split up to 16");
    shot("split_menu16");
    click_text("8 windows");
    CHECK(rect_is(0, 0, 0, 960, 540) && rect_is(1, 960, 0, 320, 180) && rect_is(3, 960, 360, 320, 180) &&
          rect_is(4, 0, 540, 320, 180) && rect_is(7, 960, 540, 320, 180) && g_tiles[8].w == 0,
          "8: one 3x3 window, three down the right, four along the bottom");
    shot("split8");
    key(KEY_9);
    CHECK(rect_is(0, 0, 0, 416, 240) && rect_is(8, 848, 480, 432, 240), "9: 3x3 on 16-pixel columns");
    key(KEY_0);
    CHECK(rect_is(0, 0, 0, 320, 180) && rect_is(15, 960, 540, 320, 180), "0: 16");
    shot("split16");
    key(KEY_8);
    CHECK(rect_is(0, 0, 0, 960, 540), "8: the 8-split again");
    key(KEY_PAGEDOWN);
    CHECK(rect_is(8, 0, 0, 960, 540) && g_tiles[0].w == 0, "PageDown: CAM9 in the large window");
}

/* 64 channels: 32 (8x4) and 64 (8x8) too. */
static void test_split64(void)
{
    move_to(300, 200);
    button(BTN_RIGHT);
    click_text("Split");
    CHECK(find("32 windows", 1) && find("64 windows", 1), "64 channels: every layout");
    shot("split_menu64");
    click_text("32 windows");
    CHECK(rect_is(0, 0, 0, 160, 180) && rect_is(31, 1120, 540, 160, 180), "32: 8x4");
    shot("split32");
    button(BTN_RIGHT);
    click_text("Split");
    click_text("64 windows");
    CHECK(rect_is(0, 0, 0, 160, 90) && rect_is(63, 1120, 630, 160, 90), "64: 8x8");
    shot("split64");
}

/* The UI with `channels` channels, a configuration `json`, a mouse and a
 * keyboard (in a fresh process). Returns 0 when set up. */
static int setup(int channels, const char *json, char *path, size_t cap)
{
    g_channels = channels;
    snprintf(path, cap, "%s/vigil-%d.json", g_out, channels);
    unlink(path);
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fputs(json, f);
    fclose(f);
    CHECK(config_init(path) == 0, "config");
    lv_init();
    lv_tick_set_cb(fake_ms);
    lv_display_t *disp = lv_display_create(W, H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(disp, g_fb, NULL, sizeof g_fb, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, flush_cb);
    if (ui_bridge_init(&g_model) < 0) return -1;
    g_model.channels = channels;
    g_model.chan_status = g_shots ? shots_status : fake_status;
    g_model.show_tiles = fake_tiles;
    g_model.now = fake_now;
    g_model.system_info = fake_info;
    fake_discovery(&g_model);
    ui_app_create(&g_model);
    if (pipe(g_mouse) < 0 || pipe(g_kbd) < 0) return -1;
    fcntl(g_mouse[0], F_SETFL, O_NONBLOCK);
    fcntl(g_kbd[0], F_SETFL, O_NONBLOCK);
    static const input_hooks_t hooks = { ui_app_back, ui_app_key, ui_app_keyboards };
    ui_app_set_pointer(input_evdev_create(disp, &hooks, 0));
    ui_app_set_keypad(input_evdev_keypad());
    input_evdev_attach(g_mouse[0], "test mouse", 0, 0, 0, 0, 0);
    input_evdev_attach_keyboard(g_kbd[0], "test keyboard");
    run(300);
    return 0;
}

/* The UI on a platform with `channels` channels (a fresh process). */
static int run_split(int channels)
{
    char path[512];
    if (setup(channels, "{\"version\":1,\"cameras\":[{\"url\":\"rtsp://192.168.1.10/s\",\"name\":\"Gate\"}]}",
              path, sizeof path) < 0)
        return 1;
    if (channels == 16) test_split16();
    else test_split64();
    input_evdev_delete();
    unlink(path);
    printf("%d channels: %d checks, %d failed\n", channels, g_checks, g_fail);
    fflush(NULL);
    return g_fail ? 1 : 0;
}

/* ---- every screen, for people (test_ui --shots DIR, scripts/screenshots.sh) ----------- */

static const char k_shots_json[] =
    "{\"version\":1,\"cameras\":["
    "{\"url\":\"rtsp://admin:secret@192.168.1.10/stream1\",\"name\":\"Front gate\"},"
    "{\"url\":\"rtsp://192.168.1.11/stream1\",\"name\":\"Yard\"},"
    "{\"url\":\"netip://admin:@192.168.1.12/?ch=0&stream=main\",\"name\":\"Garage\"},"
    "{\"url\":\"rtsp://192.168.1.13/live\",\"name\":\"Back door\"}],"
    "\"record\":{\"path\":\"/mnt/disk/rec\",\"segment\":300,\"keep\":48}}";

static void shots_status(int idx, ui_chan_t *st)
{
    static const char *const names[] = { "Front gate", "Yard", "Garage", "Back door", "Parking", "Lobby",
                                         "Office", "Warehouse", "Loading bay", "Roof", "Side street",
                                         "Reception", "Stairs", "Corridor", "Server room", "Fence" };
    *st = (ui_chan_t){ .configured = 1, .status = UI_CH_LIVE, .live = 1, .codec = "H.264", .width = 1920,
                       .height = 1080, .fps = 25, .view = "main" };
    snprintf(st->name, sizeof st->name, "%s", names[idx % 16]);
    st->recording = idx % 3 != 2;
    snprintf(st->rec_state, sizeof st->rec_state, "%s", st->recording ? "recording" : "waiting for disk");
    if (g_channels > 4) snprintf(st->view, sizeof st->view, "sub"), st->width = 704, st->height = 576;
    static const ui_ch_status_t k_state[16] = { [2] = UI_CH_CONNECTING, [3] = UI_CH_AUTH, [4] = UI_CH_OFFLINE,
                                                [5] = UI_CH_CODEC, [7] = UI_CH_DISABLED, [8] = UI_CH_NO_VIDEO,
                                                [10] = UI_CH_ERROR, [14] = UI_CH_EMPTY, [15] = UI_CH_EMPTY };
    ui_ch_status_t k = idx < 16 ? k_state[idx] : UI_CH_LIVE;
    if (g_channels == 4 && idx == 3) k = UI_CH_EMPTY;               /* a "+" among four */
    if (k != UI_CH_LIVE && (k || idx >= 14 || (g_channels == 4 && idx == 3))) {
        st->status = k;
        st->live = 0;
        st->codec[0] = st->view[0] = 0;
        st->recording = 0;
        st->rec_state[0] = 0;
        if (k == UI_CH_EMPTY) st->configured = 0, st->name[0] = 0;
        if (k == UI_CH_ERROR) snprintf(st->state, sizeof st->state, "DESCRIBE: 404");
    }
    if (idx == 9) st->no_decoder = 1;
    if (idx == 0) st->events = UI_EV_MOTION, snprintf(st->codec, sizeof st->codec, "H.265");
    if (idx == 1) st->events = UI_EV_HUMAN | UI_EV_VEHICLE;
    if (idx == 6) st->events = UI_EV_FACE | UI_EV_MOTION;
}

static void quiet(void) { run(VG_DURATION_TOAST + 100); }

static int shots_4(void)
{
    char path[512];
    if (setup(4, k_shots_json, path, sizeof path) < 0) return 1;
    shot("live-2x2");
    move_to(300, 200);
    button(BTN_RIGHT);
    shot("context-menu");
    click_text("Split");
    shot("context-split");
    click_text("1 window");
    shot("live-1x1");
    move_to(W - 20, H / 2);                     /* one camera a page: the page arrows */
    run(150);
    shot("live-page-arrow-next");
    move_to(20, H / 2);
    run(150);
    shot("live-page-arrow-prev");
    move_to(W / 2, H - 1);
    run(300);
    shot("live-navbar");
    move_to(300, 200);
    button(BTN_RIGHT);
    click_text("Split");
    click_text("4 windows");
    move_to(W / 2, H / 2);
    run(VG_DURATION_NAVBAR_HIDE + 300);
    click_at(300, 200);
    run(100);
    click_at(300, 200);                         /* double click: the camera full screen */
    shot("live-overlays");
    click_at(640, 300);
    run(100);
    click_at(640, 300);
    move_to(W / 2, H - 1);
    run(300);
    click_text("Main menu");
    shot("menu-p1");
    click_text("BACKUP");
    shot("not-available");
    button(BTN_RIGHT);
    click_text("CAMERA");
    shot("cameras");
    int x, y;
    /* The search pane: a search running, its results, the dialogs. */
    g_dj_hold = 1;
    click_text(LV_SYMBOL_REFRESH "  Search");
    run(300);
    shot("channels-searching");
    fake_job_end();
    g_dj_hold = 0;
    run(300);
    shot("channels-search-results");
    lv_obj_t *o = find("192.168.1.108", 0);
    if (o) {
        center_of(o, &x, &y);
        click_at(x, y);
        run(100);
    }
    shot("channels-device-checked");
    o = find_nth(lv_screen_active(), LV_SYMBOL_PLUS "  Add to...", 2);
    if (o) {
        center_of(o, &x, &y);
        click_at(x, y);
        run(100);
    }
    shot("channels-add-to-menu");
    button(BTN_RIGHT);
    click_text("Change IP...");
    if (textarea(0)) lv_textarea_set_text(textarea(0), "192.168.0.31");
    if (textarea(4)) lv_textarea_set_text(textarea(4), "secret");
    shot("channels-change-ip");
    click_text(LV_SYMBOL_OK "  Change");
    run(400);
    shot("channels-change-ip-done");
    button(BTN_RIGHT);
    run(100);
    click_text("Select all");
    run(100);
    click_text("Mass change...");
    if (textarea(0)) lv_textarea_set_text(textarea(0), "192.168.0.20");
    shot("channels-mass-change");
    click_text(LV_SYMBOL_EYE_OPEN "  Preview");
    run(400);
    shot("channels-mass-preview");
    click_text(LV_SYMBOL_OK "  Apply");
    run(400);
    shot("channels-mass-applied");
    button(BTN_RIGHT);
    run(100);
    click_text("Select all");
    run(100);
    click_text("Auto add all");
    shot("channels-autoadd");
    click_text(LV_SYMBOL_OK "  Add all");
    run(400);
    shot("channels-autoadd-result");
    button(BTN_RIGHT);
    run(100);
    click_text("Search settings");
    shot("channels-search-settings");
    button(BTN_RIGHT);
    run(VG_DURATION_TOAST + 100);
    lv_obj_t *edit = find("Edit", 1);
    if (edit) {
        center_of(edit, &x, &y);
        click_at(x, y);
    }
    shot("camera-form");
    lv_obj_t *url = textarea(0);
    if (url) {
        center_of(url, &x, &y);
        click_at(x, y);
    }
    shot("osk-letters");
    lv_obj_t *kb = osk();
    if (kb) {
        osk_key(kb, LV_SYMBOL_UP);
        shot("osk-shifted");
        osk_key(kb, "#+=");
        shot("osk-symbols");
        osk_key(kb, "abc");
    }
    button(BTN_RIGHT);                          /* the on-screen keyboard */
    lv_obj_t *name = textarea(1);
    if (name) {
        center_of(name, &x, &y);
        click_at(x, y);
    }
    shot("camera-form-keyboard");
    button(BTN_RIGHT);
    if (name) lv_textarea_set_text(name, "Front gate 2");
    button(BTN_RIGHT);
    shot("confirm-discard");
    click_text("Discard");
    button(BTN_RIGHT);
    click_text("STORAGE");
    shot("record");
    lv_obj_t *p = textarea(0);
    if (p) lv_textarea_set_text(p, "rec");
    click_text(SAVE);
    shot("validation-error");
    button(BTN_RIGHT);
    click_text("Discard");
    quiet();
    click_text("NETWORK");
    shot("network");
    button(BTN_RIGHT);
    click_text("DISPLAY");
    shot("display");
    button(BTN_RIGHT);
    click_text("SYSTEM");
    shot("system");
    click_text("Channel info");
    shot("channel-info");
    button(BTN_RIGHT);
    button(BTN_RIGHT);
    button(BTN_RIGHT);

    /* A text field low on the screen: the keyboard above it. */
    ui_page_t *tp = ui_page_push("A field low on the screen");
    lv_obj_t *low = lv_textarea_create(tp->scr);
    lv_textarea_set_one_line(low, 1);
    lv_textarea_set_text(low, "rtsp://admin:***@192.168.1.10/stream1");
    lv_obj_set_width(low, 520);
    lv_obj_set_pos(low, 700, H - 70);
    lv_obj_t *kb2 = ui_osk_create(tp->scr);
    ui_osk_show(kb2, low, 0);
    shot("osk-above");
    button(BTN_RIGHT);

    /* An empty channel's "+": the form for that channel. */
    lv_obj_t *t4 = tile("CAM 4");
    for (uint32_t i = 0; t4 && i < lv_obj_get_child_count(t4); ++i) {
        lv_obj_t *o = lv_obj_get_child(t4, (int32_t)i);
        if (lv_obj_check_type(o, &lv_button_class) && visible(o)) {
            center_of(o, &x, &y);
            move_to(x, y);
            shot("live-empty-plus-hover");
            click_at(x, y);
            run(100);
            shot("camera-add-channel-4");
            button(BTN_RIGHT);
            break;
        }
    }

    /* With the keyboard: the focus shows. */
    key(KEY_RIGHT);
    shot("live-focus");
    key(KEY_F1);
    shot("help-f1");
    key(KEY_ESC);
    key(KEY_M);
    key(KEY_DOWN);
    shot("menu-focus");
    key(KEY_TAB);
    key(KEY_RIGHT);
    key(KEY_RIGHT);
    key(KEY_ENTER);
    key(KEY_DOWN);
    shot("record-focus");
    input_evdev_delete();
    unlink(path);
    return g_fail ? 1 : 0;
}

/* Sixteen channels: cameras on 1-14 (8 disabled), 15 and 16 empty. */
static const char k_shots16_json[] =
    "{\"version\":1,\"cameras\":["
    "{\"url\":\"rtsp://admin:secret@192.168.1.10/stream1\",\"name\":\"Front gate\"},"
    "{\"url\":\"rtsp://192.168.1.11/stream1\",\"name\":\"Yard\"},"
    "{\"url\":\"netip://admin:@192.168.1.12/?ch=0&stream=main\",\"name\":\"Garage\"},"
    "{\"url\":\"rtsp://admin:x@192.168.1.13/live\",\"name\":\"Back door\"},"
    "{\"url\":\"netip://admin:@192.168.1.14/?ch=0&stream=main\",\"name\":\"Parking\"},"
    "{\"url\":\"rtsp://192.168.1.15/cam/realmonitor?channel=1&subtype=0\",\"name\":\"Lobby\"},"
    "{\"url\":\"rtsp://192.168.1.16/main\",\"name\":\"Office\"},"
    "{\"url\":\"netip://192.168.1.17/\",\"name\":\"Warehouse\",\"enabled\":false},"
    "{\"url\":\"netip://192.168.1.18/\",\"name\":\"Loading bay\"},"
    "{\"url\":\"rtsp://192.168.1.19/live\",\"name\":\"Roof\"},"
    "{\"url\":\"rtsp://192.168.1.20/live\",\"name\":\"Side street\"},"
    "{\"url\":\"netip://192.168.1.21/\",\"name\":\"Reception\"},"
    "{\"url\":\"netip://192.168.1.22/\",\"name\":\"Stairs\"},"
    "{\"url\":\"rtsp://192.168.1.23/live\",\"name\":\"Corridor\"}],"
    "\"record\":{\"path\":\"/mnt/disk/rec\",\"segment\":300,\"keep\":48}}";

static int shots_16(void)
{
    char path[512];
    if (setup(16, k_shots16_json, path, sizeof path) < 0) return 1;
    move_to(300, 200);
    button(BTN_RIGHT);
    click_text("Channels");
    run(300);
    shot("channels-16-states");
    lv_obj_t *t = NULL;
    for (lv_obj_t *l = find("Corridor", 0); l && !t; l = lv_obj_get_parent(l))
        if (lv_obj_has_flag(l, UI_FLAG_ARROWS)) t = l;
    if (t) lv_obj_scroll_to_y(t, 2000, LV_ANIM_OFF);
    run(100);
    shot("channels-16-states-end");
    button(BTN_RIGHT);
    move_to(300, 200);
    button(BTN_RIGHT);
    click_text("Split");
    shot("context-split-16");
    click_text("8 windows");
    move_to(W / 2, H / 2);
    shot("live-8");
    key(KEY_9);
    shot("live-9");
    key(KEY_0);
    shot("live-16");                            /* every state, icons, "+" on 15 and 16 */
    move_to(W / 2, H - 1);
    run(300);
    click_text("Main menu");
    click_text("SYSTEM");
    click_text("Channel info");
    shot("channel-info-16");
    input_evdev_delete();
    unlink(path);
    return g_fail ? 1 : 0;
}

static int shots(void)
{
    int (*const run_one[])(void) = { shots_4, shots_16 };
    int rc = 0;
    for (size_t i = 0; i < sizeof run_one / sizeof run_one[0]; ++i) {
        fflush(NULL);
        pid_t pid = fork();
        if (pid == 0) _exit(run_one[i]());
        int st = 0;
        if (pid < 0 || waitpid(pid, &st, 0) != pid || !WIFEXITED(st) || WEXITSTATUS(st)) rc = 1;
    }
    return rc;
}

int main(int argc, char **argv)
{
    if (argc > 2 && !strcmp(argv[1], "--shots")) {
        g_out = argv[2];
        g_shots = 1;
        mkdir(g_out, 0755);                     /* may be there */
        setenv("TZ", "UTC", 1);
        tzset();
        return shots();
    }
    g_out = argc > 1 ? argv[1] : ".";
    setenv("TZ", "UTC", 1);
    tzset();
    test_layout_math();

    /* More channels than the test platform has: each in a process of its
     * own, since LVGL and the UI are set up once per process. */
    static const int k_more[] = { 16, 64 };
    for (size_t i = 0; i < sizeof k_more / sizeof k_more[0]; ++i) {
        fflush(NULL);
        pid_t pid = fork();
        if (pid == 0) _exit(run_split(k_more[i]));
        int st = 0;
        CHECK(pid > 0 && waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0,
              "the %d-channel run", k_more[i]);
    }

    char path[512];
    snprintf(path, sizeof path, "%s/vigil.yaml", g_out);
    unlink(path);
    FILE *f = fopen(path, "w");
    fputs("version: 1\n"
          "cameras:\n"
          "  - url: \"rtsp://admin:secret@192.168.1.10/stream1\"\n"
          "    name: Gate\n"
          "  - url: \"netip://192.168.1.11/\"\n"
          "    name: Yard\n"
          "  - url: \"rtsp://192.168.1.12/live\"\n"
          "    enabled: false\n", f);
    fclose(f);
    CHECK(config_init(path) == 0, "config");

    lv_init();
    lv_tick_set_cb(fake_ms);
    lv_display_t *disp = lv_display_create(W, H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(disp, g_fb, NULL, sizeof g_fb, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, flush_cb);

    if (ui_bridge_init(&g_model) < 0) return 1;
    g_model.chan_status = fake_status;
    g_model.next_event = fake_event;
    g_model.show_tiles = fake_tiles;
    g_model.now = fake_now;
    g_model.system_info = fake_info;
    fake_discovery(&g_model);
    ui_app_create(&g_model);

    if (pipe(g_mouse) < 0) return 1;
    fcntl(g_mouse[0], F_SETFL, O_NONBLOCK);
    static const input_hooks_t hooks = { ui_app_back, ui_app_key, ui_app_keyboards };
    lv_indev_t *ptr = input_evdev_create(disp, &hooks, 0);
    ui_app_set_pointer(ptr);
    ui_app_set_keypad(input_evdev_keypad());
    input_evdev_attach(g_mouse[0], "test mouse", 0, 0, 0, 0, 0);
    run(300);

    test_live();
    test_navbar();
    test_context_and_full_screen();
    test_menu_and_settings();
    test_cameras();
    test_channels_discovery();
    test_system();
    test_same_redacted_urls();
    test_stale_and_cancel();
    test_two_mice(ptr);
    test_keyboard_live();
    test_keyboard_settings();
    test_keyboard_dropdown();
    test_keyboard_plug();
    test_empty_slot();

    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    printf("LVGL heap: %u KiB used at most of %u KiB\n", (unsigned)(mon.max_used >> 10),
           (unsigned)(mon.total_size >> 10));
    input_evdev_delete();
    close(g_mouse[1]);
    unlink(path);
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
