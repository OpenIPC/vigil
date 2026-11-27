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
#include "discovery.h"
#include "hal.h"
#include "hal_capture.h"
#include "hal_decoder.h"
#include "hal_display.h"
#include "hal_audio.h"
#include "hal_ui.h"
#include "input_evdev.h"
#include "ui_app.h"
#include "ui_bridge.h"
#include "ui_cursor.h"
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
                                   .dec_max_h = 1080, .max_channels = 4,
                                   .margins = 1, .picture = 1, .audio_out = 1 };
const hal_caps_t *hal_caps(void) { return &g_caps; }
int  hal_dec_open(int ch, vg_codec_t c, const vg_stream_info_t *si) { (void)ch; (void)c; (void)si; return -1; }
int  hal_dec_send(int ch, const vg_au_t *au) { (void)ch; (void)au; return -1; }
void hal_dec_reset(int ch) { (void)ch; }
void hal_dec_close(int ch) { (void)ch; }
int  hal_display_set_tile(int ch, const vg_rect_t *r) { (void)ch; (void)r; return 0; }
void hal_display_clear_tile(int ch) { (void)ch; }
void hal_display_set_aspect(int ch, unsigned w, unsigned h) { (void)ch; (void)w; (void)h; }
int  hal_cap_start(int in, int ch, hal_cap_cb cb, void *user) { (void)in; (void)ch; (void)cb; (void)user; return -1; }
void hal_cap_stop(int in) { (void)in; }
int  hal_cap_signal(int in) { (void)in; return 0; }
/* An output with three modes; switching only notes the new one. */
static const vg_mode_t k_modes[] = { { 1280, 720, 60 }, { 1920, 1080, 60 }, { 1024, 768, 60 } };
static vg_mode_t g_mode = { 1280, 720, 60 };
static int       g_mode_sets;
int hal_display_modes(vg_mode_t *out, int max)
{
    int n = 0;
    for (; n < 3 && n < max; ++n) out[n] = k_modes[n];
    return n;
}
void hal_display_mode(vg_mode_t *cur) { *cur = g_mode; }
int  hal_display_set_mode(const vg_mode_t *m)
{
    ++g_mode_sets;
    g_mode = *m;
    return 0;
}
/* The output's adjustments: what the Display page put in force last. */
static vg_margins_t g_margins;
static vg_picture_t g_picture = { 50, 50, 50, 50, 50, 100 };
static unsigned     g_volume = 80;
static int          g_mute, g_margin_sets;
int hal_display_set_margins(const vg_margins_t *m)
{
    ++g_margin_sets;
    g_margins = *m;
    return 0;
}
int hal_display_set_picture(const vg_picture_t *p)
{
    g_picture = *p;
    return 0;
}
int hal_audio_set_volume(unsigned volume, int mute)
{
    g_volume = volume;
    g_mute = mute;
    return 0;
}

/* Black holes for the tests' references; for the screenshots, the boards'
 * magenta key, replaced by a picture when the screen is written (shot),
 * as the video layer would: what is drawn over the video can be judged,
 * and a pixel blended with the key would show as a fringe. */
static int g_shots;                     /* --shots: every screen, not the tests */
static int g_live;                      /* --live-search: the real discovery, not the fakes */
static int g_video_all;                 /* a picture behind every tile, whatever its state */
#define KEY 0xFF00FFu

void hal_ui_hole(lv_color_t *c, lv_opa_t *o)
{
    *c = lv_color_hex(g_shots ? KEY : 0x000000);
    *o = LV_OPA_COVER;
}

/* A cursor plane, when a test asks for one (g_plane): where the pointer was
 * put and whether it is shown. Moves come from the reader thread too. */
static int g_plane, g_plane_made, g_plane_arrow;
static int g_plane_x = -1, g_plane_y = -1, g_plane_shown = -1;

int hal_ui_cursor_create(const uint32_t *argb, int w, int h)
{
    if (!g_plane) return -1;
    /* The hot spot is the top-left pixel, opaque; the top-right one is not. */
    g_plane_arrow = w == UI_CURSOR_WIDTH && h == UI_CURSOR_HEIGHT && argb[0] >> 24 == 0xFF && !(argb[w - 1] >> 24);
    g_plane_made = 1;
    return 0;
}

void hal_ui_cursor_move(int x, int y)
{
    __atomic_store_n(&g_plane_x, x, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_plane_y, y, __ATOMIC_SEQ_CST);
}

void hal_ui_cursor_show(int on) { __atomic_store_n(&g_plane_shown, on, __ATOMIC_SEQ_CST); }

void hal_ui_cursor_delete(void)
{
    g_plane_made = 0;
    g_plane_shown = 0;
}

/* ---- the model: the real bridge, fake channels -------------------------------------- */

static ui_model_t g_model;
static int        g_channels = 4;       /* the platform's, as the model says */
static void shots_status(int idx, ui_chan_t *st);
static void quiet(void);
static lv_obj_t *nth_of(const lv_obj_class_t *cls, int nth);
static ui_rect_t  g_tiles[UI_MAX_CHANNELS];
static int        g_tile_calls;

/* Channel 4 as an XM camera whose main stream is refused while a small
 * tile shows its sub-stream (Channel info only). */
static int g_xm_chan4;

static void fake_status(int idx, ui_chan_t *st)
{
    switch (idx) {
    case 0:
        *st = (ui_chan_t){ .configured = 1, .status = UI_CH_LIVE, .live = 1, .recording = 1,
                           .events = UI_EV_MOTION | UI_EV_HUMAN, .rec_bytes = 734u << 20, .name = "Gate",
                           .codec = "H.265", .width = 1920, .height = 1080, .fps = 25, .view = "main",
                           .rec_state = "recording", .main_status = UI_CH_LIVE };
        break;
    case 1:
        *st = (ui_chan_t){ .configured = 1, .status = UI_CH_CONNECTING, .name = "Yard", .state = "CONNECTING",
                           .main_status = UI_CH_CONNECTING, .main_state = "CONNECTING" };
        break;
    case 2:
        *st = (ui_chan_t){ .configured = 1, .status = UI_CH_ERROR, .state = "DESCRIBE: 404",
                           .main_status = UI_CH_ERROR, .main_state = "DESCRIBE: 404" };
        break;
    case 3:
        if (g_xm_chan4 && g_channels <= 4) {
            *st = (ui_chan_t){ .configured = 1, .status = UI_CH_LIVE, .live = 1, .name = "Front door",
                               .title = "Front door", .codec = "H.264", .width = 640, .height = 360, .fps = 15,
                               .view = "sub", .rec_state = "ready", .main_status = UI_CH_ERROR,
                               .main_state = "monitor refused (Ret 103)", .sub_status = UI_CH_LIVE };
            break;
        }
        /* fall through */
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

static int g_dj_many;                   /* > 0: that many made-up devices instead */

static int fake_disc_devices(ui_disc_dev_t *out, int max)
{
    if (g_dj_many) {
        static const char *const vendors[][2] = { { "Xiongmai", "IPC" }, { "Dahua", "IPC-HFW1230S" },
                                                  { "OpenIPC", "SigmaStar" }, { "Acme", "DS-2CD" } };
        int n = g_dj_many < max ? g_dj_many : max;
        for (int i = 0; i < n; ++i) {
            ui_disc_dev_t *d = &out[i];
            memset(d, 0, sizeof *d);
            snprintf(d->mac, sizeof d->mac, "02:00:00:00:01:%02x", (unsigned)i & 0xffu);
            snprintf(d->key, sizeof d->key, "%s", d->mac);
            snprintf(d->ip, sizeof d->ip, "10.0.0.%u", 10u + ((unsigned)i & 0x3fu));
            snprintf(d->mask, sizeof d->mask, "255.255.0.0");
            snprintf(d->vendor, sizeof d->vendor, "%s", vendors[i % 4][0]);
            snprintf(d->model, sizeof d->model, "%s", vendors[i % 4][1]);
            d->protos = i % 3 == 0 ? UI_DISC_NETIP | UI_DISC_ONVIF : i % 3 == 1 ? UI_DISC_ONVIF : UI_DISC_DHIP;
            d->port = d->protos & UI_DISC_NETIP ? 34567 : 80;
        }
        return n;
    }
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

/* Stop: the search ends with what it has. */
static int fake_disc_stop(void)
{
    if (!g_dj.running || g_dj.kind != UI_JOB_SCAN) return -1;
    g_dj.running = 0;
    g_dj.progress = 100;
    snprintf(g_dj.msg, sizeof g_dj.msg, "%d devices, stopped", g_dj_devices);
    return (int)g_dj.id;
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

static ui_manual_t g_dj_manual;           /* the last device tested or saved by hand */
static int         g_dj_manual_ch, g_dj_manual_replace;

static int fake_disc_test(const ui_manual_t *mm, char *err, size_t cap)
{
    (void)err;
    (void)cap;
    g_dj_manual = *mm;
    int id = fake_job_new(UI_JOB_TEST, 0);
    ui_disc_res_t *r = &g_dj.r[0];
    snprintf(r->key, sizeof r->key, "%.23s", mm->host);
    snprintf(r->ip, sizeof r->ip, "%.15s", mm->host);
    r->done = 1;
    if (!strcmp(mm->pass, "wrong")) {
        snprintf(r->error, sizeof r->error, "%s", mm->kind == UI_MANUAL_ONVIF ? "auth" : "auth");
        snprintf(r->msg, sizeof r->msg, "%s", mm->kind == UI_MANUAL_NETIP ? "login refused (Ret 203): user/password"
                                              : "GetCapabilities: not authorized");
    } else if (!strcmp(mm->host, "10.9.9.9")) {
        snprintf(r->error, sizeof r->error, "unreachable");
        snprintf(r->msg, sizeof r->msg, "connect: No route to host");
    } else {
        r->ok = 1;
        snprintf(r->codec, sizeof r->codec, "%s", mm->kind == UI_MANUAL_NETIP ? "H.265" : "H.264");
        r->width = 1920;
        r->height = 1080;
        snprintf(r->msg, sizeof r->msg, "%s 1920x1080", r->codec);
    }
    g_dj.n = 1;
    snprintf(g_dj.msg, sizeof g_dj.msg, "%s", r->ok ? "OK" : r->msg);
    return id;
}

static int fake_disc_assign_manual(const ui_manual_t *mm, int channel, const char *name, int replace, char *err,
                                   size_t cap)
{
    (void)name;
    (void)err;
    (void)cap;
    g_dj_manual = *mm;
    g_dj_manual_ch = channel;
    g_dj_manual_replace = replace;
    int id = fake_job_new(UI_JOB_ASSIGN, 0);
    ui_disc_res_t *r = &g_dj.r[0];
    r->done = r->ok = 1;
    r->channel = channel;
    snprintf(r->msg, sizeof r->msg, "added to channel %d", channel);
    g_dj.n = 1;
    return id;
}

static void fake_discovery(ui_model_t *m)
{
    m->disc_test = fake_disc_test;
    m->disc_assign_manual = fake_disc_assign_manual;
    m->disc_scan = fake_disc_scan;
    m->disc_stop = fake_disc_stop;
    m->disc_gen = NULL;                 /* the fakes change under it: read on every poll of a search */
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

/* A camera's picture, made up: sky, a sun, a hill, a road, houses with
 * lit windows, and some grain; `seed` varies it from tile to tile. u, v
 * in 0..1. 0xRRGGBB. */
static uint32_t scene(double u, double v, int seed)
{
    unsigned n = (unsigned)(u * 997 + v * 7919 + seed * 131) * 2654435761u;
    int grain = (int)(n >> 28) - 8;
    double r, g, b;
    double hill = 0.55 + 0.08 * __builtin_sin(u * 6.0 + seed) + 0.03 * __builtin_sin(u * 17.0);
    if (v < hill) {                                 /* sky, lighter to the horizon */
        r = 70 + 120 * v; g = 120 + 100 * v; b = 200 + 40 * v;
        double du = u - (0.2 + 0.1 * (seed % 5)), dv = v - 0.18;
        if (du * du + dv * dv < 0.004) r = 255, g = 245, b = 210;          /* the sun */
        if (v > hill - 0.18 && ((int)(u * 9 + seed) % 3) == 0) {         /* a house */
            r = 150; g = 120; b = 100;
            if (((int)(u * 60) % 4 == 1) && ((int)(v * 60) % 4 == 1)) r = 255, g = 230, b = 120;
        }
    } else if (__builtin_fabs(u - 0.5 - 0.3 * (v - hill)) < 0.06 + 0.25 * (v - hill)) {
        r = g = b = 80 + 40 * v;                    /* the road */
        if (__builtin_fabs(u - 0.5 - 0.3 * (v - hill)) < 0.006 && (int)(v * 40) % 2) r = g = b = 240;
    } else {
        r = 60 + 40 * v; g = 110 + 30 * v; b = 50;  /* grass */
    }
    r += grain; g += grain; b += grain;
    r = r < 0 ? 0 : r > 255 ? 255 : r; g = g < 0 ? 0 : g > 255 ? 255 : g; b = b < 0 ? 0 : b > 255 ? 255 : b;
    return (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
}

/* The picture behind a hole pixel: the tile it is in (the rectangles
 * handed to show_tiles), else the whole screen's. */
static uint32_t video_at(int x, int y)
{
    for (int i = 0; i < UI_MAX_CHANNELS; ++i) {
        const ui_rect_t *t = &g_tiles[i];
        if (t->w && x >= t->x && x < t->x + t->w && y >= t->y && y < t->y + t->h) {
            ui_chan_t st;
            shots_status(i, &st);
            /* A picture only where the camera's video is decoded (or
             * everywhere, to judge the text over a picture). */
            if (!g_video_all && (st.status != UI_CH_LIVE || st.no_decoder)) return 0x000000;
            return scene((double)(x - t->x) / t->w, (double)(y - t->y) / t->h, i);
        }
    }
    return scene((double)x / W, (double)y / H, 0);
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
            if (g_shots && (p & 0xFFFFFF) == KEY) p = video_at(x, y);
            row[x * 3] = (uint8_t)(p >> 16);
            row[x * 3 + 1] = (uint8_t)(p >> 8);
            row[x * 3 + 2] = (uint8_t)p;
        }
        fwrite(row, 1, sizeof row, f);
    }
    fclose(f);
}

/* ---- the management pages: the system faked, the rest real ------------------------------ */

/* What the pages show and do, without a system to change: an interface,
 * disks, a clock. Time zone parsing is the real one. */
static ui_mgmt_t    g_mg;
static int          g_mg_pending, g_mg_wifi, g_mg_jobs, g_mg_formatted;
static ui_net_cfg_t g_mg_eth = { .dhcp = 0, .address = "192.168.1.20", .netmask = "255.255.255.0",
                                 .gateway = "192.168.1.1", .dns1 = "192.168.1.1", .dns2 = "8.8.8.8" };
static char         g_mg_host[64] = "nvr-hall";
static char         g_mg_zone[64] = "Europe/Berlin", g_mg_tz[96] = "CET-1CEST,M3.5.0,M10.5.0/3";

static int  fm_managed(void) { return 1; }
static void fm_status(ui_net_status_t *st, int wifi)
{
    memset(st, 0, sizeof *st);
    st->present = st->up = st->link = 1;
    st->speed = wifi ? 0 : 100;
    snprintf(st->duplex, sizeof st->duplex, "%s", wifi ? "" : "full");
    snprintf(st->mac, sizeof st->mac, "%s", wifi ? "e8:4e:06:1a:22:31" : "00:12:31:a5:7c:02");
    snprintf(st->address, sizeof st->address, "%s", wifi ? "192.168.1.57" : "192.168.1.20");
    snprintf(st->netmask, sizeof st->netmask, "255.255.255.0");
    snprintf(st->gateway, sizeof st->gateway, "192.168.1.1");
    snprintf(st->dns1, sizeof st->dns1, "192.168.1.1");
    st->mtu = 1500;
}
static void fm_eth_get(ui_net_cfg_t *c, ui_net_status_t *st) { *c = g_mg_eth; fm_status(st, 0); }
static int  fm_eth_apply(const ui_net_cfg_t *c, char *field, size_t fcap, char *err, size_t cap)
{
    if (!c->dhcp && strcmp(c->netmask, "255.255.255.0") && strcmp(c->netmask, "255.255.0.0")) {
        snprintf(field, fcap, "netmask");
        snprintf(err, cap, "not a netmask (255.255.255.0, ...)");
        return UI_MGMT_EINVAL;
    }
    g_mg_eth = *c;
    g_mg_pending = 1;
    return ++g_mg_jobs;
}
static int  fm_pending(int *secs) { if (secs) *secs = 57; return g_mg_pending; }
static int  fm_confirm(void) { int p = g_mg_pending; g_mg_pending = 0; return p ? 0 : -1; }
static int  fm_revert(void) { return fm_confirm(); }
/* g_mg_wifi: 0 none, 1 wlan0 up (wlandev set), 2 a USB adapter plugged in
 * whose driver is not loaded yet (no wlandev). */
static char g_mg_wifi_profile[48];
static void fm_wifi_get(ui_wifi_t *w, ui_net_status_t *st)
{
    memset(w, 0, sizeof *w);
    fm_status(st, 1);
    if (!g_mg_wifi) return;
    w->n_adapters = 1;
    ui_radio_t *a = &w->adapters[0];
    snprintf(a->id, sizeof a->id, "148f:7601");
    snprintf(a->name, sizeof a->name, "MediaTek 802.11 n WLAN");
    snprintf(a->driver, sizeof a->driver, "mt7601u");
    snprintf(a->profile, sizeof a->profile, "mt7601u-generic");
    if (g_mg_wifi == 2) return;
    a->bound = 1;
    snprintf(w->profile, sizeof w->profile, "mt7601u-generic");
    snprintf(w->ifname, sizeof w->ifname, "wlan0");
    snprintf(w->ssid, sizeof w->ssid, "HomeNet");
    w->password_set = 1;
}
static int  fm_wifi_scan(ui_wifi_net_t *o, int max, char *err, size_t cap)
{
    (void)o; (void)max;
    snprintf(err, cap, "this firmware has no iw to scan with");
    return -1;
}
static int  fm_wifi_set(const char *ssid, const char *p, const char *profile, char *err, size_t cap)
{
    (void)ssid; (void)p; (void)err; (void)cap;
    snprintf(g_mg_wifi_profile, sizeof g_mg_wifi_profile, "%s", profile ? profile : "");
    return ++g_mg_jobs;
}
static int  g_mg_modem;             /* a USB modem plugged in */
static char g_mg_lte_profile[48];
static void fm_lte_get(ui_lte_t *l, ui_net_status_t *st)
{
    memset(l, 0, sizeof *l);
    memset(st, 0, sizeof *st);
    snprintf(l->profile, sizeof l->profile, "%s", g_mg_lte_profile);
    if (!g_mg_modem) return;
    l->n_modems = 2;
    l->modems[0] = (ui_radio_t){ .id = "12d1:1f01", .name = "HUAWEI_MOBILE", .driver = "usb_storage",
                                 .profile = "rndis-e3372h", .bound = 1 };
    l->modems[1] = (ui_radio_t){ .id = "2c7c:0125", .name = "Quectel EC25", .driver = "qmi_wwan" };
}
static int  fm_lte_profile(const char *profile, char *msg, size_t cap)
{
    snprintf(g_mg_lte_profile, sizeof g_mg_lte_profile, "%s", profile);
    snprintf(msg, cap, "wlandev is %s: the modem starts at the next boot", profile);
    return 0;
}
static void fm_services(ui_svc_t *d, ui_svc_t *m, ui_svc_t *a, ui_svc_t *n)
{
    *d = (ui_svc_t){ .last = 1795702800, .ok = 1, .result = "updated: good 203.0.113.7" };
    *m = (ui_svc_t){ 0 };
    *a = (ui_svc_t){ .last = 1795703400, .ok = 1, .result = "sent" };
    *n = (ui_svc_t){ 0 };
}
static int  fm_job_start(void) { return ++g_mg_jobs; }
static int  fm_nas_support(const char *type, char *why, size_t cap)
{
    if (!strcmp(type, "nfs")) {
        snprintf(why, cap, "the kernel has NFS without version 3: it needs CONFIG_NFS_V3");
        return 0;
    }
    return 1;
}
static int  fm_nas_mounted(void) { return 0; }
static int  fm_curl(void) { return 1; }
static int  fm_job(int id, ui_mjob_t *j)
{
    memset(j, 0, sizeof *j);
    j->id = id;
    j->state = UI_JOBST_DONE;
    snprintf(j->msg, sizeof j->msg, "done");
    return 0;
}
static int  fm_disks(ui_disk_t *o, int max)
{
    static const ui_disk_t d[] = {
        { .name = "sda", .size = 1000204886016ull, .model = "WDC WD10PURZ" },
        { .name = "sda1", .parent = "sda", .is_part = 1, .size = 1000203837440ull, .fs = "exfat",
          .mnt = "/mnt/sda1", .total = 1000170000000ull, .avail = 612400000000ull, .state = "recording" },
        { .name = "sdb", .size = 31914983424ull, .model = "SanDisk Ultra", .removable = 1 },
        { .name = "sdb1", .parent = "sdb", .is_part = 1, .size = 31913934848ull, .fs = "vfat",
          .state = "not mounted" },
    };
    int n = (int)(sizeof d / sizeof d[0]);
    if (n > max) n = max;
    memcpy(o, d, (size_t)n * sizeof *o);
    if (g_mg_formatted && n > 3) {
        snprintf(o[3].fs, sizeof o[3].fs, "ext4");
        snprintf(o[3].mnt, sizeof o[3].mnt, "/mnt/sdb1");
        snprintf(o[3].state, sizeof o[3].state, "mounted");
        o[3].total = 31900000000ull;
        o[3].avail = 31850000000ull;
    }
    return n;
}
static int  fm_formats(char out[][8], int max)
{
    (void)max;                          /* the firmware's: e2fsprogs, exfatprogs, busybox */
    snprintf(out[0], 8, "ext4");
    snprintf(out[1], 8, "exfat");
    snprintf(out[2], 8, "vfat");
    return 3;
}
static int  fm_disk_format(const char *name, const char *fs, const char *confirm, char *err, size_t cap)
{
    (void)fs;
    if (strcmp(name, confirm)) {
        snprintf(err, cap, "type the disk's name (%s) to confirm", name);
        return UI_MGMT_EINVAL;
    }
    g_mg_formatted = 1;
    return ++g_mg_jobs;
}
static int  fm_disk_act(const char *name, char *err, size_t cap) { (void)name; (void)err; (void)cap; return ++g_mg_jobs; }

/* Disk Maintenance: sda a WD Purple with a few remapped and pending
 * sectors and a loose cable (what sys_smart makes of such a drive), sdb
 * a USB stick without SMART; a scan as the fixture says. */
static int       g_mg_selftest = -1;
static ui_scan_t g_mg_scan;
static int  fm_smart_read(const char *disk, char *err, size_t cap)
{
    (void)disk, (void)err, (void)cap;
    return ++g_mg_jobs;
}
static int  fm_smart_get(const char *disk, ui_smart_t *o)
{
    memset(o, 0, sizeof *o);
    o->read_at = 1795000000;
    o->temp = -1;
    o->hours = -1;
    o->health = -1;
    if (strcmp(disk, "sda")) {
        o->verdict = UI_SMART_UNKNOWN;
        snprintf(o->reason, sizeof o->reason, "SMART not available: %s does not pass ATA commands (a USB stick, "
                 "a memory card, or a USB enclosure without SAT)", disk);
        return 0;
    }
    static const ui_smart_attr_t A[] = {
        { 1, 200, 200, 51, 0, 0, "Raw read error rate", "0", "" },
        { 3, 172, 169, 21, 0, 0, "Spin-up time", "2383 ms", "" },
        { 4, 100, 100, 0, 0, 0, "Start/stop count", "312", "" },
        { 5, 199, 199, 140, 1, 1, "Reallocated sectors", "24", "Sectors moved to spares: the surface is wearing" },
        { 7, 200, 200, 0, 0, 0, "Seek error rate", "0", "" },
        { 9, 78, 78, 0, 0, 0, "Power-on hours", "16234 h", "" },
        { 10, 100, 100, 0, 0, 1, "Spin retry count", "0", "" },
        { 12, 100, 100, 0, 0, 0, "Power cycles", "310", "" },
        { 192, 200, 200, 0, 0, 0, "Power-off retracts", "41", "" },
        { 193, 200, 200, 0, 0, 0, "Load/unload cycles", "1043", "" },
        { 194, 113, 102, 0, 0, 0, "Temperature", "38 C (min 20, max 45)", "" },
        { 196, 199, 199, 0, 1, 1, "Reallocation events", "6", "Times the drive moved sectors to spares" },
        { 197, 200, 200, 0, 1, 1, "Current pending sectors", "8",
          "Unreadable sectors waiting to be remapped: data on them may be lost" },
        { 198, 100, 253, 0, 0, 1, "Offline uncorrectable", "0", "" },
        { 199, 200, 200, 0, 1, 1, "Interface CRC errors", "3", "Usually the cable or the connection, not the disk" },
        { 200, 100, 253, 0, 0, 0, "Multi-zone error rate", "0", "" },
    };
    o->available = 1;
    snprintf(o->transport, sizeof o->transport, "SATA");
    snprintf(o->model, sizeof o->model, "WDC WD10PURZ-85U8XY0");
    snprintf(o->serial, sizeof o->serial, "WD-WCC4J5HZ3V7K");
    snprintf(o->firmware, sizeof o->firmware, "01.01A01");
    o->bytes = 1000204886016ull;
    o->rpm = 5400;
    o->sata_max = 3;
    o->sata_cur = 3;
    o->smart_enabled = 1;
    o->health = 1;
    o->temp = 38;
    o->hours = 16234;
    o->verdict = UI_SMART_WARN;
    snprintf(o->reason, sizeof o->reason, "24 reallocated sectors, 6 reallocation events, 8 pending sectors, ...");
    o->test_supported = 1;
    o->short_min = 2;
    o->ext_min = 124;
    snprintf(o->test_status, sizeof o->test_status, "Completed without error");
    o->n_attrs = (int)(sizeof A / sizeof A[0]);
    memcpy(o->attrs, A, sizeof A);
    o->n_tests = 2;
    o->tests[0] = (ui_smart_test_t){ "Short", "Completed without error", 16230, 0, 0 };
    o->tests[1] = (ui_smart_test_t){ "Extended", "Aborted by host", 16101, 0, 0 };
    return 0;
}
static int  fm_selftest(const char *disk, int type, char *err, size_t cap)
{
    (void)disk, (void)err, (void)cap;
    g_mg_selftest = type;
    return 0;
}
/* A read of sda a third of the way: mostly fast, a few slow blocks and
 * two bad patches, as a tired disk shows. */
static void fm_scan_fixture(int mode, const char *disk, int state, int done_cells)
{
    ui_scan_t *s = &g_mg_scan;
    memset(s, 0, sizeof *s);
    s->state = state;
    s->mode = mode;
    snprintf(s->disk, sizeof s->disk, "%s", disk);
    snprintf(s->model, sizeof s->model, "%s", strcmp(disk, "sda") ? "SanDisk Ultra" : "WDC WD10PURZ-85U8XY0");
    s->end = strcmp(disk, "sda") ? 62333952ull : 1953525168ull;
    s->block = 256;
    s->sector = 512;
    s->pass = s->passes = 1;
    s->cells = UI_SCAN_MAP;
    s->blocks_total = (s->end + 255) / 256;
    s->blocks_done = s->blocks_total * (unsigned long long)done_cells / UI_SCAN_MAP;
    s->lba = s->blocks_done * 256;
    unsigned long long per = s->blocks_total / UI_SCAN_MAP;
    unsigned r = 12345;
    for (int i = 0; i < done_cells; ++i) {
        r = r * 1103515245u + 12345u;
        unsigned v = (r >> 16) % 1000;
        int c = v < 860 ? 0 : v < 950 ? 1 : v < 985 ? 2 : v < 995 ? 3 : 4;
        if (i >= 412 && i < 418) c = 5;
        if (i == 415 || i == 416 || i == 571) c = 6;
        s->map[i] = (unsigned char)(c + 1);
    }
    for (int c = 0; c < UI_SCAN_CLASSES; ++c) s->counts[c] = 0;
    s->counts[0] = s->blocks_done * 86 / 100;
    s->counts[1] = s->blocks_done * 9 / 100;
    s->counts[2] = s->blocks_done * 35 / 1000;
    s->counts[3] = s->blocks_done / 100;
    s->counts[4] = 214;
    s->counts[5] = 31;
    s->counts[6] = 3;
    s->counts[0] = s->blocks_done - s->counts[1] - s->counts[2] - s->counts[3] - 214 - 31 - 3;
    s->mbps = 112.4;
    s->elapsed = 1520;
    s->eta = 3100;
    snprintf(s->msg, sizeof s->msg, "%s", mode == UI_SCAN_READ ? "sda1 is mounted: reading at the idle I/O priority"
                                                               : "Pass 1 of 2: writing the pattern");
    s->n_bad = 2;
    s->bad_lba[0] = 415 * per * 256;
    s->bad_count[0] = 512;
    s->bad_lba[1] = 571 * per * 256 + 1024;
    s->bad_count[1] = 256;
}
static int  fm_scan_start(const char *disk, int mode, unsigned long long start, unsigned long long end,
                          unsigned block, const char *confirm, char *err, size_t cap)
{
    (void)start, (void)end, (void)block;
    if (mode == UI_SCAN_WRITE && (!confirm || strcmp(confirm, disk))) {
        snprintf(err, cap, "type the disk's name (%s) to confirm that everything on it is erased", disk);
        return UI_MGMT_EINVAL;
    }
    fm_scan_fixture(mode, disk, UI_SCAN_RUNNING, mode == UI_SCAN_WRITE ? 20 : 740);
    return 0;
}
static void fm_scan_get(ui_scan_t *o) { *o = g_mg_scan; }
static int  fm_scan_pause(int on)
{
    if (g_mg_scan.state != UI_SCAN_RUNNING && g_mg_scan.state != UI_SCAN_PAUSED) return UI_MGMT_EINVAL;
    g_mg_scan.state = on ? UI_SCAN_PAUSED : UI_SCAN_RUNNING;
    return 0;
}
static int  fm_scan_stop(void)
{
    if (g_mg_scan.state != UI_SCAN_RUNNING && g_mg_scan.state != UI_SCAN_PAUSED) return UI_MGMT_EINVAL;
    g_mg_scan.state = UI_SCAN_STOPPED;
    snprintf(g_mg_scan.msg, sizeof g_mg_scan.msg, "read of sda stopped: 3 errors in 2 bad ranges, 245 slow (>= 150 ms)");
    return 0;
}
static void fm_time_get(ui_time_t *t)
{
    memset(t, 0, sizeof *t);
    snprintf(t->zone, sizeof t->zone, "%s", g_mg_zone);
    snprintf(t->tz, sizeof t->tz, "%s", g_mg_tz);
    t->rtc = 1;
    snprintf(t->servers[0], 64, "0.pool.ntp.org");
    snprintf(t->servers[1], 64, "1.pool.ntp.org");
    t->n_servers = 2;
    t->ntp = 1;
    t->ntp_interval = 60;
    t->ntp_last_try = t->ntp_last_ok = 1795703100;
    snprintf(t->ntp_result, sizeof t->ntp_result, "the clock was set from 0.pool.ntp.org");
}
static int  fm_time_set(const char *zone, const char *tz, int set_clock, long long t, char *err, size_t cap)
{
    (void)set_clock; (void)t; (void)err; (void)cap;
    if (zone && *zone) snprintf(g_mg_zone, sizeof g_mg_zone, "%s", zone);
    if (tz && *tz) {
        snprintf(g_mg_tz, sizeof g_mg_tz, "%s", tz);
        snprintf(g_mg_zone, sizeof g_mg_zone, "custom");
    }
    return 0;
}
static int  fm_ntp_set(int en, const char *srv, int iv, char *err, size_t cap)
{
    (void)en; (void)srv; (void)iv; (void)err; (void)cap;
    return 0;
}
static void fm_host_get(char *b, size_t cap) { snprintf(b, cap, "%s", g_mg_host); }
static int  fm_host_set(const char *n, char *err, size_t cap)
{
    if (!*n || strchr(n, ' ')) {
        snprintf(err, cap, "1 to 63 letters, digits and '-'");
        return UI_MGMT_EINVAL;
    }
    snprintf(g_mg_host, sizeof g_mg_host, "%s", n);
    return 0;
}

/* Accounts, in memory: the rules users.c enforces that the pages show. */
static ui_user_t  g_fu[8];
static int        g_nfu;
static ui_group_t g_fg[8] = { { "admin", 0xff, 1, 0, 0 }, { "user", 0x3, 1, 1, 0 } };
static int        g_nfg = 2;
static unsigned   g_fgen = 1;
static const char *const k_fperms[] = { "live", "playback", "ptz", "cameras", "storage", "network", "system",
                                        "users" };

static unsigned fa_gen(void) { return g_fgen; }
static int fa_users(ui_user_t *o, int max)
{
    int n = g_nfu < max ? g_nfu : max;
    memcpy(o, g_fu, (size_t)n * sizeof *o);
    return n;
}
static int fa_groups(ui_group_t *o, int max)
{
    for (int i = 0; i < g_nfg; ++i) {
        g_fg[i].members = 0;
        for (int k = 0; k < g_nfu; ++k) g_fg[i].members += !strcmp(g_fu[k].group, g_fg[i].name);
    }
    int n = g_nfg < max ? g_nfg : max;
    memcpy(o, g_fg, (size_t)n * sizeof *o);
    return n;
}
static int fa_find_user(const char *n)
{
    for (int i = 0; i < g_nfu; ++i)
        if (!strcmp(g_fu[i].name, n)) return i;
    return -1;
}
static int fa_find_group(const char *n)
{
    for (int i = 0; i < g_nfg; ++i)
        if (!strcmp(g_fg[i].name, n)) return i;
    return -1;
}
#define FA_ERR(f, ...) do { snprintf(field, fcap, "%s", f); snprintf(err, cap, __VA_ARGS__); return UI_MGMT_EINVAL; } while (0)
static int fa_user_add(const char *name, const char *group, const char *pass, int en, char *field, size_t fcap,
                       char *err, size_t cap)
{
    if (!*name || !strcmp(name, "root")) FA_ERR("name", "not a user name");
    if (fa_find_user(name) >= 0) FA_ERR("name", "the name is taken");
    if (fa_find_group(group) < 0) FA_ERR("group", "no such group");
    if (strlen(pass) < 4) FA_ERR("password", "at least 4 characters");
    if (g_nfu == 8) FA_ERR("", "too many users");
    ui_user_t *u = &g_fu[g_nfu++];
    memset(u, 0, sizeof *u);
    snprintf(u->name, sizeof u->name, "%s", name);
    snprintf(u->group, sizeof u->group, "%s", group);
    u->enabled = en;
    u->password_set = 1;
    ++g_fgen;
    return 0;
}
static int fa_user_update(const char *name, const char *group, int en, const char *pass, char *field, size_t fcap,
                          char *err, size_t cap)
{
    int i = fa_find_user(name);
    if (i < 0) FA_ERR("name", "no such user");
    if (group && fa_find_group(group) < 0) FA_ERR("group", "no such group");
    if (pass && strlen(pass) < 4) FA_ERR("password", "at least 4 characters");
    if (group) snprintf(g_fu[i].group, sizeof g_fu[i].group, "%s", group);
    if (en >= 0) g_fu[i].enabled = en;
    ++g_fgen;
    return 0;
}
static int fa_user_delete(const char *name, char *err, size_t cap)
{
    int i = fa_find_user(name);
    if (i < 0) {
        snprintf(err, cap, "no such user");
        return UI_MGMT_EINVAL;
    }
    memmove(&g_fu[i], &g_fu[i + 1], (size_t)(g_nfu - i - 1) * sizeof g_fu[0]);
    --g_nfu;
    ++g_fgen;
    return 0;
}
static int fa_group_set(const char *name, unsigned perms, int create, char *field, size_t fcap, char *err, size_t cap)
{
    int i = fa_find_group(name);
    if (create) {
        if (!*name || strchr(name, ' ')) FA_ERR("name", "not a group name");
        if (i >= 0) FA_ERR("name", "the name is taken");
        if (g_nfg == 8) FA_ERR("", "too many groups");
        i = g_nfg++;
        memset(&g_fg[i], 0, sizeof g_fg[i]);
        snprintf(g_fg[i].name, sizeof g_fg[i].name, "%s", name);
        g_fg[i].editable = 1;
    }
    if (i < 0) FA_ERR("name", "no such group");
    if (!g_fg[i].editable) FA_ERR("permissions", "a built-in group cannot be changed so");
    g_fg[i].perms = perms;
    ++g_fgen;
    return 0;
}
static int fa_group_delete(const char *name, char *err, size_t cap)
{
    int i = fa_find_group(name);
    if (i < 0 || g_fg[i].builtin) {
        snprintf(err, cap, i < 0 ? "no such group" : "a built-in group");
        return UI_MGMT_EINVAL;
    }
    memmove(&g_fg[i], &g_fg[i + 1], (size_t)(g_nfg - i - 1) * sizeof g_fg[0]);
    --g_nfg;
    ++g_fgen;
    return 0;
}

/* Logins: root with "rootpw" (every permission), a user of the list with
 * its name and "pw" (its group's). */
static int fa_login(const char *name, const char *pass, unsigned *perms, char *why, size_t cap)
{
    (void)why;
    (void)cap;
    char want[48];
    snprintf(want, sizeof want, "%spw", name);
    if (!strcmp(name, "root")) {
        *perms = 0xff;
        return !strcmp(pass, "rootpw");
    }
    for (int i = 0; i < g_nfu; ++i)
        if (!strcmp(g_fu[i].name, name) && g_fu[i].enabled && !strcmp(pass, want)) {
            *perms = 0;
            for (int k = 0; k < g_nfg; ++k)
                if (!strcmp(g_fg[k].name, g_fu[i].group)) *perms = g_fg[k].perms;
            return 1;
        }
    return 0;
}
static void fa_system_user(char *buf, size_t cap) { snprintf(buf, cap, "root"); }
static int g_power = -1;                /* what Exit asked for: 1 reboot, 0 off */
static int fa_power(int reboot)
{
    g_power = reboot;
    return 0;
}

static void fake_mgmt(ui_model_t *m)
{
    g_mg = *m->mgmt;                    /* real: time zone parsing */
    g_mg.managed = fm_managed;
    g_mg.eth_get = fm_eth_get;
    g_mg.eth_apply = fm_eth_apply;
    g_mg.net_pending = fm_pending;
    g_mg.net_confirm = fm_confirm;
    g_mg.net_revert = fm_revert;
    g_mg.wifi_get = fm_wifi_get;
    g_mg.wifi_scan = fm_wifi_scan;
    g_mg.wifi_set = fm_wifi_set;
    g_mg.lte_get = fm_lte_get;
    g_mg.lte_profile = fm_lte_profile;
    g_mg.services = fm_services;
    g_mg.ddns_update = fm_job_start;
    g_mg.smtp_test = fm_job_start;
    g_mg.nas_test = fm_job_start;
    g_mg.nas_support = fm_nas_support;
    g_mg.nas_mounted = fm_nas_mounted;
    g_mg.curl = fm_curl;
    g_mg.job = fm_job;
    g_mg.disks = fm_disks;
    g_mg.disk_formats = fm_formats;
    g_mg.disk_format = fm_disk_format;
    g_mg.disk_mount = fm_disk_act;
    g_mg.disk_unmount = fm_disk_act;
    g_mg.smart_read = fm_smart_read;
    g_mg.smart_get = fm_smart_get;
    g_mg.selftest = fm_selftest;
    g_mg.scan_start = fm_scan_start;
    g_mg.scan_get = fm_scan_get;
    g_mg.scan_pause = fm_scan_pause;
    g_mg.scan_stop = fm_scan_stop;
    g_mg.time_get = fm_time_get;
    g_mg.time_set = fm_time_set;
    g_mg.ntp_set = fm_ntp_set;
    g_mg.ntp_sync = fm_job_start;
    g_mg.hostname_get = fm_host_get;
    g_mg.hostname_set = fm_host_set;
    /* Accounts: the real ones would read api.users (/etc). */
    g_mg.users_gen = fa_gen;
    g_mg.users = fa_users;
    g_mg.groups = fa_groups;
    g_mg.perm_names = k_fperms;
    g_mg.n_perms = 8;
    g_mg.user_add = fa_user_add;
    g_mg.user_update = fa_user_update;
    g_mg.user_delete = fa_user_delete;
    g_mg.group_set = fa_group_set;
    g_mg.group_delete = fa_group_delete;
    g_mg.login = fa_login;
    g_mg.system_user = fa_system_user;
    g_mg.power = fa_power;
    m->mgmt = &g_mg;
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
    if (lv_obj_check_type(o, &lv_image_class) && visible(o) &&
        (lv_image_get_src(o) == img || lv_obj_get_user_data(o) == img))     /* a hard-edged copy (ui_osd_icon) */
        return 1;
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

/* The tile showing `title` (its name's parent: the outlined name sits on
 * the tile). */
static lv_obj_t *tile(const char *title)
{
    lv_obj_t *l = find(title, 0);
    return l ? lv_obj_get_parent(l) : NULL;
}

/* ---- the steps ---------------------------------------------------------------------------- */

#define SAVE LV_SYMBOL_SAVE "  Save"

/* A section's schema form, as the menu used to open it (the management
 * screens show their own pages now). */
static void open_form(const char *key)
{
    for (int i = 0; i < g_model.n_sections; ++i)
        if (!strcmp(g_model.sections[i].key, key)) ui_form_open(i, -1);
    run(100);
}

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
    CHECK(ui_page_top() && find("Storage", 1) && find("Disk Manager", 0), "Record: the storage screen");
    button(BTN_RIGHT);
    button(BTN_RIGHT);
    click_text("Display");
    CHECK(ui_page_top() && find("Resolution", 0) && find("1280 x 720, 60 Hz", 0), "Display: the output");
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
    CHECK(find("Disk Manager", 0) && find("Network Storage", 0), "STORAGE: the storage screen");
    button(BTN_RIGHT);
    open_form("record");
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
    CHECK(find("Ethernet", 0) && find("Alarm center", 0) && find("P2P", 0), "NETWORK: the network screen");
    button(BTN_RIGHT);
    open_form("api");
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
    CHECK(find("Resolution", 0), "DISPLAY: the display page");
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
    quiet();
    click_text("CAMERA");
    g_dj_hold = 1;
    click_text(LV_SYMBOL_REFRESH "  Search");
    run(300);
    CHECK(find("Searching (40%)... 0 found", 1) && find(LV_SYMBOL_STOP "  Stop", 0), "Search: progress, and Stop");
    /* The devices show as they answer, while the search runs. */
    g_dj_devices = 2;
    g_dj.found = 2;
    g_dj.progress = 60;
    run(300);
    CHECK(find("Searching (60%)... 2 found", 1) && find("192.168.1.10", 0) && find("192.168.1.108", 0) &&
          !find("192.168.1.175", 0), "the first two, while it runs");
    shot("channels_searching");
    lv_obj_t *first_row = find("192.168.1.108", 0);
    g_dj_devices = 4;
    g_dj.found = 4;
    run(300);
    CHECK(find("192.168.1.108", 0) == first_row && find("192.168.1.22", 0), "two more; the rows heard first stay");
    fake_job_end();
    g_dj_hold = 0;
    run(300);
    CHECK(find("5 found", 0) && find("192.168.1.108", 0) && find("On channel 1", 0) && find("New", 0) &&
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

/* A search that finds more devices than the table shows: Stop keeps what
 * was found; the list scrolls with the wheel and with the keyboard's
 * focus; the channel table too. */
static void test_channels_scroll(void)
{
    quiet();
    click_text("CAMERA");
    g_dj_hold = 1;
    click_text(LV_SYMBOL_REFRESH "  Search");
    g_dj_many = 34;
    g_dj.found = 34;
    run(300);
    click_text(LV_SYMBOL_STOP "  Stop");
    run(300);
    CHECK(!g_dj.running && find(LV_SYMBOL_REFRESH "  Search", 0) && find("34 found", 0), "Stop: 34 kept");
    g_dj_hold = 0;
    lv_obj_t *top = find("10.0.0.10", 0), *last = find("10.0.0.43", 0);
    lv_obj_t *table = top ? lv_obj_get_parent(lv_obj_get_parent(top)) : NULL;
    CHECK(table && lv_obj_get_scroll_bottom(table) > 0, "34 rows: the table scrolls");
    if (!table || !last) return;
    lv_area_t ta, la;
    int x, y;
    center_of(top, &x, &y);
    move_to(x, y);
    shot("channels_scroll_top");
    for (int i = 0; i < 5; ++i) ev(EV_REL, REL_WHEEL, -1);
    ev(EV_SYN, SYN_REPORT, 0);
    run(100);
    CHECK(lv_obj_get_scroll_y(table) > 0, "the wheel scrolls the list under the pointer (%d)",
          (int)lv_obj_get_scroll_y(table));
    shot("channels_scroll_middle");
    for (int i = 0; i < 20; ++i) ev(EV_REL, REL_WHEEL, -1);
    ev(EV_SYN, SYN_REPORT, 0);
    run(100);
    lv_obj_get_coords(table, &ta);
    lv_obj_get_coords(last, &la);
    CHECK(lv_obj_get_scroll_bottom(table) <= 0 && la.y2 <= ta.y2, "... to its end");
    shot("channels_scroll_bottom");
    for (int i = 0; i < 30; ++i) ev(EV_REL, REL_WHEEL, 1);
    ev(EV_SYN, SYN_REPORT, 0);
    run(100);
    CHECK(lv_obj_get_scroll_y(table) == 0, "and back up");

    /* Keys as the keypad gives them to the focused table (gridnav): Down
     * through the rows, the focused one in view. */
    lv_group_focus_obj(table);
    uint32_t k = LV_KEY_DOWN;
    for (int i = 0; i < 20; ++i) lv_obj_send_event(table, LV_EVENT_KEY, &k);
    run(400);
    lv_obj_t *row = NULL;
    for (uint32_t i = 0; i < lv_obj_get_child_count(table); ++i)
        if (lv_obj_has_state(lv_obj_get_child(table, (int32_t)i), LV_STATE_FOCUSED)) row = lv_obj_get_child(table, (int32_t)i);
    lv_area_t ra;
    if (row) lv_obj_get_coords(row, &ra);
    lv_obj_get_coords(table, &ta);
    CHECK(row && lv_obj_get_index(row) == 20 && ra.y1 >= ta.y1 && ra.y2 <= ta.y2 + 1, "row 21 focused, in view (%d: %d..%d in %d..%d)",
          row ? (int)lv_obj_get_index(row) : -1, (int)ra.y1, (int)ra.y2, (int)ta.y1, (int)ta.y2);
    /* Space checks it: the table is updated in place, the focus stays. */
    k = ' ';
    lv_obj_send_event(table, LV_EVENT_KEY, &k);
    run(200);
    CHECK(row && lv_obj_has_state(row, LV_STATE_FOCUSED) && find("1 checked", 0), "checked; still focused");
    shot("channels_scroll_keyboard");
    lv_obj_send_event(table, LV_EVENT_KEY, &k);
    run(200);
    lv_group_focus_obj(NULL);
    g_dj_many = 0;
    button(BTN_RIGHT);
    run(100);
}

/* Manual add: the page per protocol, Test (OK, an authentication
 * error), Save into the first empty channel, and asking before replacing. */
/* The drop-down in the row labelled `label`. */
static lv_obj_t *dd_near(const char *label)
{
    lv_obj_t *dd = NULL;
    for (lv_obj_t *l = find(label, 0); l && !dd; l = lv_obj_get_parent(l))
        for (uint32_t i = 0; i < lv_obj_get_child_count(l) && !dd; ++i)
            if (lv_obj_check_type(lv_obj_get_child(l, (int32_t)i), &lv_dropdown_class)) dd = lv_obj_get_child(l, (int32_t)i);
    return dd;
}

static void man_proto(int kind)
{
    lv_obj_t *dd = dd_near("Protocol");
    if (!dd) return;
    lv_dropdown_set_selected(dd, (uint32_t)kind);
    lv_obj_send_event(dd, LV_EVENT_VALUE_CHANGED, NULL);
    run(50);
}

static void test_channels_manual(void)
{
    quiet();                                    /* the toasts of the steps before */
    click_text("CAMERA");
    click_text("Manual add");
    CHECK(find("Manual add", 0) && find("Device channel", 0) && !find("Main stream", 0) &&
          textarea(1) && !strcmp(lv_textarea_get_text(textarea(1)), "34567"), "Manual add: NETIP, port 34567");
    char sel[32] = "";
    if (dd_near("Channel")) lv_dropdown_get_selected_str(dd_near("Channel"), sel, sizeof sel);
    CHECK(!strcmp(sel, "3  empty"), "the first empty channel chosen: %s", sel);
    lv_textarea_set_text(textarea(0), "192.168.1.30");
    shot("manual_netip");
    click_text(LV_SYMBOL_REFRESH "  Test");
    run(400);
    CHECK(find("OK: H.265 1920x1080", 1) && !strcmp(g_dj_manual.host, "192.168.1.30") && g_dj_manual.port == 34567,
          "Test: OK with codec and size");
    shot("manual_test_ok");
    lv_textarea_set_text(textarea(3), "wrong");
    click_text(LV_SYMBOL_REFRESH "  Test");
    run(400);
    CHECK(find("Authentication error", 1) && find("login refused", 1), "Test: authentication error");
    shot("manual_test_auth");
    lv_textarea_set_text(textarea(3), "pw");    /* an edit: the result no longer applies */
    CHECK(!find("Authentication error", 1), "an edited password: the old result goes");
    click_text(LV_SYMBOL_REFRESH "  Test");
    run(400);
    CHECK(find("OK: H.265", 1), "tested again");
    lv_textarea_set_text(textarea(3), "wrong");
    man_proto(UI_MANUAL_ONVIF);
    CHECK(!strcmp(lv_textarea_get_text(textarea(1)), "80"), "ONVIF: port 80");
    CHECK(!find("Authentication error", 1) && !find("login refused", 1), "another protocol: the old result goes");
    shot("manual_onvif");
    man_proto(UI_MANUAL_DAHUA);
    CHECK(!strcmp(lv_textarea_get_text(textarea(1)), "554"), "Dahua: RTSP port 554");
    shot("manual_dahua");
    man_proto(UI_MANUAL_RTSP);
    CHECK(find("Main stream", 0) && !find("Device channel", 0), "RTSP: the paths, no channel");
    lv_textarea_set_text(textarea(3), "pw");
    lv_textarea_set_text(textarea(5), "/live/main");
    lv_textarea_set_text(textarea(6), "/live/sub");
    shot("manual_rtsp");
    click_text(LV_SYMBOL_SAVE "  Save");
    run(400);
    CHECK(g_dj_manual_ch == 3 && !g_dj_manual_replace && g_dj_manual.kind == UI_MANUAL_RTSP &&
          !strcmp(g_dj_manual.main_path, "/live/main") && !strcmp(g_dj_manual.sub_path, "/live/sub") &&
          !find("Device channel", 0) && find("Search settings", 1), "Save: channel 3, the paths; the page closes");
    click_text("Manual add");
    lv_obj_t *dd = dd_near("Channel");
    if (dd) lv_dropdown_set_selected(dd, 0);
    lv_textarea_set_text(textarea(0), "192.168.1.31");
    click_text(LV_SYMBOL_SAVE "  Save");
    CHECK(ui_popup_is_open() && find("Channel 1 has a camera. Replace it?", 1), "an occupied channel: asked");
    shot("manual_replace");
    lv_obj_t *yes = find_in(lv_layer_top(), "Replace", 0);
    int x, y;
    if (yes) {
        center_of(yes, &x, &y);
        click_at(x, y);
    }
    run(400);
    CHECK(g_dj_manual_ch == 1 && g_dj_manual_replace, "replaced after asking");
    button(BTN_RIGHT);
}

static void test_system(void)
{
    run(VG_DURATION_TOAST);
    click_text("OPERATION");
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
    open_form("record");
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

/* ---- the management screens -------------------------------------------------------------- */


/* The left pane's item `label`. */
static void nav_to(const char *label)
{
    click_text(label);
    run(200);
}

static void test_network_pages(void)
{
    run(VG_DURATION_TOAST);
    move_to(W / 2, H - 1);
    run(300);
    click_text("Main menu");
    click_text("NETWORK");
    CHECK(find("Network", 1) && find("Ethernet", 0) && find("up, 100 Mb/s full", 0) &&
          find("00:12:31:a5:7c:02", 0), "Ethernet: status");
    lv_obj_t *mask = textarea(1);
    CHECK(mask && !strcmp(lv_textarea_get_text(mask), "255.255.255.0"), "Ethernet: the stored netmask");
    lv_textarea_set_text(mask, "255.0.255.0");
    click_text(SAVE);
    run(100);
    CHECK(find("not a netmask", 1) && lv_obj_has_state(mask, LV_STATE_USER_1) && !g_mg_pending,
          "a bad netmask: refused, marked");
    shot("network_error");
    lv_textarea_set_text(mask, "255.255.0.0");
    click_text(SAVE);
    run(800);
    CHECK(g_mg_pending && ui_popup_is_open() && find("Keep these network settings?", 1) &&
          find("Undone in 57 s", 1), "applied: the countdown question");
    shot("network_keep");
    click_text(LV_SYMBOL_OK "  Keep");
    run(100);
    CHECK(!g_mg_pending && !ui_popup_is_open() && find("kept", 1), "Keep: confirmed");
    CHECK(!strcmp(g_mg_eth.netmask, "255.255.0.0"), "the change went to the model");

    nav_to("Wi-Fi");
    CHECK(find("No Wi-Fi adapter", 0) && !find(SAVE, 0), "Wi-Fi: no adapter, nothing to save");
    g_mg_wifi = 2;
    nav_to("DDNS");
    nav_to("Wi-Fi");
    CHECK(find("MediaTek 802.11 n WLAN (USB 148f:7601)", 0) && find("mt7601u-generic", 0) &&
          find("No wireless interface yet", 1) && find(SAVE, 0), "Wi-Fi: a USB adapter without wlan0, its profile");
    lv_obj_t *ssid = textarea(0), *wpass = textarea(1);
    if (ssid) lv_textarea_set_text(ssid, "HomeNet");
    if (wpass) lv_textarea_set_text(wpass, "longenough");
    click_text(SAVE);
    run(800);
    CHECK(!strcmp(g_mg_wifi_profile, "mt7601u-generic"), "joining sets wlandev to the adapter's profile: '%s'",
          g_mg_wifi_profile);
    g_mg_wifi = 0;
    g_mg_modem = 1;
    nav_to("LTE");
    CHECK(find("rndis-e3372h", 0) && find("qmi_wwan, not loaded yet", 0) && find("Use this profile", 1),
          "LTE: modems by USB id, their profiles");
    click_text("Use this profile");
    run(100);
    CHECK(!strcmp(g_mg_lte_profile, "rndis-e3372h") && find("next boot", 1), "Use this profile: wlandev");
    g_mg_modem = 0;
    g_mg_lte_profile[0] = 0;
    nav_to("DDNS");
    CHECK(find("Update now", 1) && find("good 203.0.113.7", 1), "DDNS: the form and the last update");
    lv_obj_t *host = textarea(0);
    if (host) lv_textarea_set_text(host, "nvr.example.org");
    nav_to("SMTP");
    CHECK(ui_popup_is_open() && find("Discard the changes", 1), "leaving with changes asks");
    click_text("Cancel");
    CHECK(!ui_popup_is_open() && find("Update now", 1), "Cancel: still on DDNS");
    click_text(SAVE);
    run(100);
    vigil_config_t c;
    config_get(&c);
    CHECK(!strcmp(c.ddns.host, "nvr.example.org"), "DDNS saved through the store: '%s'", c.ddns.host);
    nav_to("SMTP");
    CHECK(find("Send test mail", 1) && !ui_popup_is_open(), "SMTP: saved, so no question");
    click_text("Send test mail");
    run(800);
    CHECK(find("done", 1), "test mail: the job's result");
    nav_to("P2P");
    CHECK(find("P2P is not available", 0), "P2P: said so");
    button(BTN_RIGHT);
    CHECK(find("Main menu", 0), "back in the menu");
    button(BTN_RIGHT);
}

/* Every sub-page of a management screen, as a screenshot each. */
static void shots_settings(const char *tile, const char *prefix, const char *const *items, int n)
{
    click_text(tile);
    for (int i = 0; i < n; ++i) {
        nav_to(items[i]);
        char name[64], slug[32];
        size_t k = 0;
        for (const char *p = items[i]; *p && k + 1 < sizeof slug; ++p)
            slug[k++] = (*p >= 'A' && *p <= 'Z') ? (char)(*p + 32) : (*p == ' ' ? '-' : *p);
        slug[k] = 0;
        snprintf(name, sizeof name, "%s-%s", prefix, slug);
        shot(name);
    }
    button(BTN_RIGHT);
}

static void shots_network(void)
{
    static const char *const items[] = { "Ethernet", "Wi-Fi", "LTE", "DDNS", "UPnP", "SMTP", "Alarm center", "P2P" };
    shots_settings("NETWORK", "network", items, 8);
    g_mg_wifi = 1;
    click_text("NETWORK");
    nav_to("Wi-Fi");
    shot("network-wifi-adapter");
    click_text("Scan");
    shot("network-wifi-scan");
    nav_to("Ethernet");
    lv_obj_t *mask = textarea(1);
    if (mask) lv_textarea_set_text(mask, "255.0.255.0");
    click_text(SAVE);
    shot("network-ethernet-error");
    if (mask) lv_textarea_set_text(mask, "255.255.255.0");
    click_text(SAVE);
    run(800);
    shot("network-ethernet-keep");
    click_text(LV_SYMBOL_OK "  Keep");
    quiet();
    g_mg_wifi = 2;
    nav_to("Wi-Fi");
    shot("network-wifi-usb-adapter");
    g_mg_modem = 1;
    nav_to("LTE");
    shot("network-lte-modems");
    g_mg_modem = 0;
    g_mg_wifi = 1;
    nav_to("Ethernet");
    lv_obj_t *dns = textarea(3);
    if (dns) lv_textarea_set_text(dns, "1.1.1.1");
    nav_to("LTE");
    shot("settings-discard");
    click_text("Discard");
    button(BTN_RIGHT);
    g_mg_wifi = 0;
}

/* The schedule's grid: the object before the legend in its panel. */
static lv_obj_t *sched_grid(void)
{
    lv_obj_t *l = find("Continuous   ", 0);
    return l ? lv_obj_get_child(lv_obj_get_parent(lv_obj_get_parent(l)), 0) : NULL;
}

/* The centre of half hour `slot` of day `day` (0 = Monday) on the grid. */
static void slot_at(lv_obj_t *grid, int day, int slot, int *x, int *y)
{
    lv_area_t a;
    lv_obj_get_coords(grid, &a);
    *x = a.x1 + 52 + slot * 18 + 9;
    *y = a.y1 + 22 + day * 26 + 13;
}

/* Paint day `day` from slot `from` to `to` by dragging the mouse. */
static void drag_slots(lv_obj_t *grid, int day, int from, int to)
{
    int x, y;
    slot_at(grid, day, from, &x, &y);
    move_to(x, y);
    ev(EV_KEY, BTN_LEFT, 1);
    ev(EV_SYN, SYN_REPORT, 0);
    run(40);
    for (int k = from + 1; k <= to; ++k) {
        slot_at(grid, day, k, &x, &y);
        move_to(x, y);
    }
    ev(EV_KEY, BTN_LEFT, 0);
    ev(EV_SYN, SYN_REPORT, 0);
    run(60);
}

static void test_storage_pages(void)
{
    run(VG_DURATION_TOAST);
    move_to(W / 2, H - 1);
    run(300);
    click_text("Main menu");
    click_text("STORAGE");
    CHECK(find("When the disk is full", 0) && find("Segment length (min)", 0) && find("/mnt/sda1/rec", 1),
          "STORAGE: Basic, with the mounted disk as a shortcut");
    shot("storage_basic");
    lv_obj_t *seg = textarea(0);
    CHECK(seg && !strcmp(lv_textarea_get_text(seg), "1"), "60 s shown as 1 min: %s", seg ? lv_textarea_get_text(seg) : "");
    if (seg) lv_textarea_set_text(seg, "2");
    lv_obj_t *dd = NULL;
    for (lv_obj_t *o = lv_obj_get_parent(find("When the disk is full", 0)); o && !dd;) {
        for (uint32_t i = 0; i < lv_obj_get_child_count(o); ++i) {
            lv_obj_t *c = lv_obj_get_child(o, (int32_t)i);
            lv_obj_t *box = lv_obj_get_child_count(c) ? lv_obj_get_child(c, 0) : NULL;
            if (box && lv_obj_get_child_count(box) && lv_obj_check_type(lv_obj_get_child(box, 0), &lv_dropdown_class))
                dd = lv_obj_get_child(box, 0);
        }
        break;
    }
    CHECK(dd != NULL, "the on_full drop-down");
    if (dd) lv_dropdown_set_selected(dd, 1);
    click_text(SAVE);
    run(100);
    vigil_config_t c;
    config_get(&c);
    CHECK(c.record.segment == 120 && !strcmp(c.record.on_full, "stop"), "saved: segment %d s, on_full %s",
          c.record.segment, c.record.on_full);

    nav_to("Schedule");
    lv_obj_t *grid = sched_grid();
    CHECK(grid && find("Motion only records while", 1), "Schedule: the grid and the motion note");
    click_text("Off");
    if (grid) drag_slots(grid, 5, 0, 47);
    shot("storage_schedule");
    click_text(SAVE);
    run(100);
    config_get(&c);
    int found = 0;
    for (int i = 0; i < c.n_schedules; ++i)
        if (c.schedule[i].camera == 1)
            found = strspn(c.schedule[i].day[5], "-") == 48 && strspn(c.schedule[i].day[0], "C") == 48;
    CHECK(c.n_schedules == 1 && found, "saved: CAM1 Saturday off (%d entries)", c.n_schedules);
    /* The day last painted (Saturday) to every day. */
    click_text("Copy the day to all days");
    click_text(SAVE);
    run(100);
    config_get(&c);
    CHECK(c.n_schedules == 1 && strspn(c.schedule[0].day[0], "-") == 48 && strspn(c.schedule[0].day[6], "-") == 48,
          "Copy the day: every day off");
    click_text("Always");
    click_text(SAVE);
    run(100);
    config_get(&c);
    CHECK(c.n_schedules == 0, "Always: no entry needed (%d)", c.n_schedules);

    nav_to("Disk Manager");
    CHECK(find("sda1", 1) && find("recording", 0) && find("sdb1", 1) && find("WDC WD10PURZ", 0) && !find(SAVE, 0),
          "Disk Manager: the disks, their states, nothing to save");
    shot("storage_disks");
    /* sdb1's Format: the second row with a Format button after sda's two. */
    lv_obj_t *sdb1 = find("    sdb1", 0);
    lv_obj_t *fmt = NULL;
    if (sdb1) {
        lv_obj_t *row = lv_obj_get_parent(sdb1);
        for (uint32_t i = 0; i < lv_obj_get_child_count(row) && !fmt; ++i) {
            lv_obj_t *b = lv_obj_get_child(row, (int32_t)i);
            if (lv_obj_check_type(b, &lv_button_class)) fmt = b;
        }
    }
    CHECK(fmt != NULL, "sdb1 has a Format button");
    int x, y;
    if (fmt) {
        center_of(fmt, &x, &y);
        click_at(x, y);
    }
    lv_obj_t *go = find(LV_SYMBOL_TRASH "  Format", 0);
    CHECK(find("Format sdb1", 1) && go && lv_obj_has_state(lv_obj_get_parent(go), LV_STATE_DISABLED),
          "the question; Format disabled");
    lv_obj_t *name = textarea(0);
    if (name) lv_textarea_set_text(name, "sdb");
    CHECK(go && lv_obj_has_state(lv_obj_get_parent(go), LV_STATE_DISABLED), "a wrong name: still disabled");
    shot("storage_format");
    if (name) lv_textarea_set_text(name, "sdb1");
    CHECK(go && !lv_obj_has_state(lv_obj_get_parent(go), LV_STATE_DISABLED), "the right name: enabled");
    if (go) {
        center_of(go, &x, &y);
        click_at(x, y);
    }
    run(1000);
    CHECK(g_mg_formatted && find("/mnt/sdb1", 0), "formatted: mounted at /mnt/sdb1");
    g_mg_formatted = 0;

    /* Disk Maintenance: SMART of sda, the verdict and the highlighted rows. */
    nav_to("Disk Maintenance");
    run(1500);
    CHECK(find(LV_SYMBOL_WARNING "  Warning", 0) && find("24 reallocated sectors", 1) &&
          find("WD-WCC4J5HZ3V7K", 0) && find("Current pending sectors", 0) && find("Usually the cable", 1) &&
          !find(SAVE, 0), "SMART: the verdict, the identity, the attributes");
    lv_obj_t *pend = find("Current pending sectors", 0);
    CHECK(pend && lv_obj_get_style_bg_opa(lv_obj_get_parent(pend), 0) > 0, "the pending row is highlighted");
    CHECK(find("Aborted by host", 0) && find("Last: Completed without error (Short)", 0), "the self-test log");
    shot("storage_maint");
    click_text("Short test (2 min)");
    CHECK(g_mg_selftest == 1, "a short self-test asked for");
    /* The surface scan: Read starts at once, the map and the counts. */
    click_text("Surface scan");
    CHECK(find("No scan yet", 0) && find(LV_SYMBOL_PLAY "  Start", 0), "Surface scan: nothing yet");
    click_text(LV_SYMBOL_PLAY "  Start");
    run(1200);
    CHECK(g_mg_scan.state == UI_SCAN_RUNNING && find("Read of sda", 1) && find("37.0%", 1) &&
          find(LV_SYMBOL_PAUSE "  Pause", 0) && find("Bad blocks (LBA ranges):", 1), "a read under way: %s",
          g_mg_scan.msg);
    shot("storage_maint_scan");
    click_text(LV_SYMBOL_PAUSE "  Pause");
    run(1200);
    CHECK(g_mg_scan.state == UI_SCAN_PAUSED && find(LV_SYMBOL_PLAY "  Resume", 0), "paused");
    click_text(LV_SYMBOL_PLAY "  Resume");
    click_text(LV_SYMBOL_STOP "  Stop");
    run(1200);
    CHECK(g_mg_scan.state == UI_SCAN_STOPPED && find(LV_SYMBOL_PLAY "  Start", 0) && find("stopped", 1),
          "stopped: Start again");
    /* Leaving and coming back: a scan under way opens on its map; the
     * Disk Manager shows it too. */
    fm_scan_fixture(UI_SCAN_READ, "sda", UI_SCAN_RUNNING, 740);
    nav_to("Disk Manager");
    run(2200);
    CHECK(find("Surface scan (Read) of sda under way: 37.0%, 2 bad ranges", 1) && find("scanning", 0),
          "Disk Manager: the scan");
    nav_to("Disk Maintenance");
    run(1200);
    CHECK(find(LV_SYMBOL_STOP "  Stop", 0) && find("Read of sda", 1), "back on the scan under way");
    click_text(LV_SYMBOL_STOP "  Stop");
    run(1200);
    /* Write: sdb, the question names it, its name typed. */
    lv_obj_t *ddisk = NULL;
    for (int i = 0; !ddisk; ++i) {
        lv_obj_t *o = nth_of(&lv_dropdown_class, i);
        if (!o) break;
        if (strstr(lv_dropdown_get_options(o), "sdb")) ddisk = o;
    }
    CHECK(ddisk != NULL, "the disk drop-down");
    if (ddisk) {
        lv_dropdown_set_selected(ddisk, 1);
        lv_obj_send_event(ddisk, LV_EVENT_VALUE_CHANGED, NULL);
    }
    run(1200);
    lv_obj_t *mode = NULL;
    for (int i = 0; !mode; ++i) {
        lv_obj_t *o = nth_of(&lv_dropdown_class, i);
        if (!o) break;
        if (strstr(lv_dropdown_get_options(o), "Write")) mode = o;
    }
    CHECK(mode != NULL, "the mode drop-down");
    if (mode) {
        lv_dropdown_set_selected(mode, UI_SCAN_WRITE);
        lv_obj_send_event(mode, LV_EVENT_VALUE_CHANGED, NULL);
    }
    click_text(LV_SYMBOL_PLAY "  Start");
    lv_obj_t *erase = find(LV_SYMBOL_TRASH "  Erase and test", 0);
    CHECK(find("Erase this disk and test it?", 1) && find("SanDisk Ultra, serial unknown", 1) && erase &&
          lv_obj_has_state(lv_obj_get_parent(erase), LV_STATE_DISABLED), "the write question; Erase disabled");
    lv_obj_t *nm = NULL;
    for (int i = 0; (nm = textarea(i)) != NULL; ++i)
        if (!strcmp(lv_textarea_get_placeholder_text(nm), "sdb")) break;
    CHECK(nm != NULL, "the name field");
    if (nm) lv_textarea_set_text(nm, "sdb");
    CHECK(erase && !lv_obj_has_state(lv_obj_get_parent(erase), LV_STATE_DISABLED), "the name typed: enabled");
    shot("storage_maint_write");
    if (erase) {
        int ex, ey;
        center_of(erase, &ex, &ey);
        click_at(ex, ey);
    }
    run(1200);
    CHECK(g_mg_scan.state == UI_SCAN_RUNNING && g_mg_scan.mode == UI_SCAN_WRITE && find("Write of sdb", 1),
          "write under way");
    fm_scan_stop();
    run(1200);
    click_text("SMART");
    run(1200);
    CHECK(find("SMART not available", 1) && find("A USB stick or a memory card", 1), "sdb: no SMART, said so");
    memset(&g_mg_scan, 0, sizeof g_mg_scan);

    nav_to("Disk Quota");
    lv_obj_t *gb = textarea(0);
    CHECK(gb && find("Max size (GB)", 0), "Disk Quota: a row per camera");
    if (gb) lv_textarea_set_text(gb, "50");
    click_text(SAVE);
    run(100);
    config_get(&c);
    CHECK(c.n_quotas == 1 && c.quota[0].camera == 1 && c.quota[0].max_gb == 50 && !c.quota[0].max_days,
          "saved: CAM1 50 GB (%d entries)", c.n_quotas);

    nav_to("Network Storage");
    CHECK(find("CONFIG_NFS_V3", 1) && find("Not mounted", 1), "Network Storage: NFS needs a kernel option");
    int jobs = g_mg_jobs;
    click_text(LV_SYMBOL_UPLOAD "  Test");
    run(800);
    CHECK(g_mg_jobs == jobs + 1 && find("done", 1), "Test: a job, followed");
    click_text("Record here");
    run(100);
    config_get(&c);
    CHECK(!strcmp(c.record.path, "/mnt/nas/rec"), "Record here: %s", c.record.path);
    button(BTN_RIGHT);
    CHECK(find("Main menu", 0), "back in the menu");
    button(BTN_RIGHT);
}

/* Disk Maintenance: SMART scrolled to the attributes, a scan under way
 * (the map), the Disk Manager meanwhile, the write question, a stick
 * without SMART. */
static void shots_maint(void)
{
    nav_to("Disk Maintenance");
    run(1500);
    lv_obj_t *at = find("Attributes", 0);
    if (at) lv_obj_scroll_to_view_recursive(at, LV_ANIM_OFF);
    shot("storage-maint-smart-attributes");
    fm_scan_fixture(UI_SCAN_READ, "sda", UI_SCAN_RUNNING, 740);
    nav_to("Disk Manager");
    run(2200);
    shot("storage-disks-scanning");
    nav_to("Disk Maintenance");
    run(1500);
    shot("storage-maint-scan");
    lv_obj_t *bad = find("Bad blocks (LBA ranges):", 1);
    if (bad) lv_obj_scroll_to_view_recursive(bad, LV_ANIM_OFF);
    shot("storage-maint-scan-bottom");
    fm_scan_stop();
    run(1200);
    lv_obj_t *dd = nth_of(&lv_dropdown_class, 0);
    if (dd) {
        lv_dropdown_set_selected(dd, 1);
        lv_obj_send_event(dd, LV_EVENT_VALUE_CHANGED, NULL);
    }
    run(1500);
    click_text("SMART");
    run(300);
    shot("storage-maint-nosmart");
    click_text("Surface scan");
    run(300);
    for (int i = 0;; ++i) {
        lv_obj_t *o = nth_of(&lv_dropdown_class, i);
        if (!o) break;
        if (strstr(lv_dropdown_get_options(o), "Write")) {
            lv_dropdown_set_selected(o, UI_SCAN_WRITE);
            lv_obj_send_event(o, LV_EVENT_VALUE_CHANGED, NULL);
        }
    }
    click_text(LV_SYMBOL_PLAY "  Start");
    lv_obj_t *nm = NULL;
    for (int i = 0; (nm = textarea(i)) != NULL; ++i)
        if (!strcmp(lv_textarea_get_placeholder_text(nm), "sdb")) break;
    if (nm) lv_textarea_set_text(nm, "sdb");
    shot("storage-maint-write-confirm");
    memset(&g_mg_scan, 0, sizeof g_mg_scan);
}

static void shots_storage(void)
{
    static const char *const items[] = { "Basic", "Schedule", "Disk Manager", "Disk Maintenance", "Disk Quota",
                                         "Network Storage" };
    shots_settings("STORAGE", "storage", items, 6);
    click_text("STORAGE");
    nav_to("Schedule");
    lv_obj_t *grid = sched_grid();
    click_text("Off");
    if (grid) {
        drag_slots(grid, 5, 0, 47);
        drag_slots(grid, 6, 0, 47);
        for (int d = 0; d < 5; ++d) drag_slots(grid, d, 18, 35);
    }
    click_text("Motion only");
    if (grid) for (int d = 0; d < 5; ++d) drag_slots(grid, d, 0, 11);
    shot("storage-schedule-painted");
    click_text("Always");
    nav_to("Disk Manager");
    lv_obj_t *sdb1 = find("    sdb1", 0);
    if (sdb1) {
        lv_obj_t *row = lv_obj_get_parent(sdb1);
        for (uint32_t i = 0; i < lv_obj_get_child_count(row); ++i) {
            lv_obj_t *b = lv_obj_get_child(row, (int32_t)i);
            if (!lv_obj_check_type(b, &lv_button_class)) continue;
            int x, y;
            center_of(b, &x, &y);
            click_at(x, y);
            break;
        }
    }
    lv_obj_t *name = textarea(0);
    if (name) lv_textarea_set_text(name, "sdb1");
    shot("storage-disks-format");
    click_text(LV_SYMBOL_CLOSE "  Cancel");
    shots_maint();
    quiet();
    button(BTN_RIGHT);
}

/* The `nth` object of `cls` on the screen (hidden ones counted). */
static lv_obj_t *nth_of_in(lv_obj_t *o, const lv_obj_class_t *cls, int nth, int *seen)
{
    if (lv_obj_check_type(o, cls) && (*seen)++ == nth) return o;
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); ++i) {
        lv_obj_t *r = nth_of_in(lv_obj_get_child(o, (int32_t)i), cls, nth, seen);
        if (r) return r;
    }
    return NULL;
}

static lv_obj_t *nth_of(const lv_obj_class_t *cls, int nth)
{
    int seen = 0;
    return nth_of_in(lv_screen_active(), cls, nth, &seen);
}

static void dd_pick(lv_obj_t *dd, int sel)
{
    if (!dd) return;
    lv_dropdown_set_selected(dd, (uint32_t)sel);
    lv_obj_send_event(dd, LV_EVENT_VALUE_CHANGED, NULL);
    run(50);
}

static void click_obj(lv_obj_t *o)
{
    int x, y;
    lv_obj_scroll_to_view_recursive(o, LV_ANIM_OFF);
    run(50);
    center_of(o, &x, &y);
    click_at(x, y);
}

static int zone_index(const char *name)
{
    for (int i = 0; i < g_mg.zones(); ++i)
        if (!strcmp(g_mg.zone_name(i), name)) return i;
    return -1;
}

static void test_system_pages(void)
{
    run(VG_DURATION_TOAST);
    move_to(W / 2, H - 1);
    run(300);
    click_text("Main menu");
    click_text("SYSTEM");
    CHECK(find("System", 1) && find("General", 0) && find("Device name", 0) && find("Time zone", 0), "System: General");
    CHECK(find("UTC+01:00; daylight saving time UTC+02:00 from the last Sunday of March 02:00 to the last Sunday "
               "of October 03:00", 1), "the zone described");
    lv_obj_t *host = textarea(0);
    CHECK(host && !strcmp(lv_textarea_get_text(host), "nvr-hall"), "the host name");
    lv_textarea_set_text(host, "nvr-gate");
    dd_pick(nth_of(&lv_dropdown_class, 1), 1);         /* DD.MM.YYYY */
    click_text(SAVE);
    run(300);
    vigil_config_t c;
    config_get(&c);
    CHECK(!strcmp(g_mg_host, "nvr-gate"), "host name saved: %s", g_mg_host);
    CHECK(!strcmp(c.ui.date_format, "DD.MM.YYYY"), "date format saved: %s", c.ui.date_format);
    CHECK(find("26.11.2026", 1), "the clock in the new format");
    shot("system_general");

    lv_textarea_set_text(host, "bad name");
    click_text(SAVE);
    run(100);
    CHECK(find("1 to 63 letters", 1) && lv_obj_has_state(host, LV_STATE_USER_1) && !strcmp(g_mg_host, "nvr-gate"),
          "a bad host name: refused under the field");
    lv_textarea_set_text(host, "nvr-gate");

    dd_pick(nth_of(&lv_dropdown_class, 3), zone_index("Asia/Tokyo"));
    CHECK(find("UTC+09:00, no daylight saving time", 1), "Tokyo described");
    click_text(SAVE);
    run(100);
    CHECK(!strcmp(g_mg_zone, "Asia/Tokyo"), "zone saved: %s", g_mg_zone);

    /* Custom: UTC+1 with DST, the last Sunday of March 02:00 to the last
     * Sunday of October 03:00. */
    dd_pick(nth_of(&lv_dropdown_class, 3), g_mg.zones());
    dd_pick(nth_of(&lv_dropdown_class, 4), 15);         /* UTC+01:00 */
    lv_obj_t *dst = nth_of(&lv_switch_class, 0);
    if (dst) {
        lv_obj_add_state(dst, LV_STATE_CHECKED);
        lv_obj_send_event(dst, LV_EVENT_VALUE_CHANGED, NULL);
    }
    dd_pick(nth_of(&lv_dropdown_class, 6), 4);          /* last */
    dd_pick(nth_of(&lv_dropdown_class, 7), 0);          /* Sunday */
    dd_pick(nth_of(&lv_dropdown_class, 8), 2);          /* March */
    dd_pick(nth_of(&lv_dropdown_class, 9), 4);
    dd_pick(nth_of(&lv_dropdown_class, 10), 0);
    dd_pick(nth_of(&lv_dropdown_class, 11), 9);         /* October */
    lv_textarea_set_text(textarea(3), "02:00");
    lv_textarea_set_text(textarea(4), "03:00");
    lv_obj_send_event(textarea(4), LV_EVENT_VALUE_CHANGED, NULL);
    run(50);
    CHECK(find("POSIX: <+01>-1<+02>,M3.5.0,M10.5.0/3", 1), "the custom zone's preview");
    shot("system_custom_tz");
    click_text(SAVE);
    run(100);
    CHECK(!strcmp(g_mg_tz, "<+01>-1<+02>,M3.5.0,M10.5.0/3") && !strcmp(g_mg_zone, "custom"), "custom saved: %s",
          g_mg_tz);

    int jobs = g_mg_jobs;
    lv_obj_t *sync = find(LV_SYMBOL_REFRESH "  Sync now", 0);
    if (sync) click_obj(sync);
    run(800);
    CHECK(g_mg_jobs > jobs && find("Clock synchronised", 1), "Sync now: a job");
    button(BTN_RIGHT);
    CHECK(find("Main menu", 0), "back in the menu");
}

/* The live screen's label with `text` (exactly), wherever the UI is. */
/* The live view's clock (a label on the screen itself; the navigation
 * bar has one of its own). */
static lv_obj_t *osd_clock(void)
{
    lv_obj_t *f = lv_group_get_focused(ui_live_group()), *scr = f ? lv_obj_get_screen(f) : NULL;
    for (uint32_t i = 0; scr && i < lv_obj_get_child_count(scr); ++i) {
        lv_obj_t *o = lv_obj_get_child(scr, (int32_t)i);
        if (lv_obj_check_type(o, &lv_label_class) && strstr(lv_label_get_text(o), ":32:05")) return o;
    }
    return NULL;
}

/* The live view's first tile (its grid is the screen's first child). */
static lv_obj_t *first_tile(void)
{
    lv_obj_t *grid = lv_obj_get_child(lv_screen_active(), 0);
    return grid ? lv_obj_get_child(grid, 0) : NULL;
}

static int put_display(const char *json)
{
    schema_errors_t e;
    memset(&e, 0, sizeof e);
    int rc = config_put_json("display", json, strlen(json), &e);
    run(300);                                   /* the generation is polled */
    return rc;
}

/* A platform with a cursor plane: the pointer is put there instead of
 * drawn by LVGL, shown while a mouse is plugged in, kept on the screen, and
 * clicks still land where it is. Returns the new pointer. */
static lv_indev_t *test_cursor_plane(lv_display_t *disp)
{
    static const input_hooks_t hooks = { ui_app_back, ui_app_key, ui_app_keyboards, ui_app_wheel };
    uint32_t drawn = lv_obj_get_child_count(lv_layer_sys());
    input_evdev_delete();                       /* closes the test mouse's end */
    close(g_mouse[1]);
    CHECK(lv_obj_get_child_count(lv_layer_sys()) == drawn - 1, "LVGL's arrow gone with its reader");
    g_plane = 1;
    lv_indev_t *ptr = input_evdev_create(disp, &hooks, 0);
    ui_app_set_pointer(ptr);
    ui_app_set_keypad(input_evdev_keypad());
    CHECK(g_plane_made && g_plane_arrow, "the arrow handed to the cursor plane");
    CHECK(lv_obj_get_child_count(lv_layer_sys()) == drawn - 1, "and not drawn by LVGL");
    CHECK(__atomic_load_n(&g_plane_shown, __ATOMIC_SEQ_CST) == 0 &&
              __atomic_load_n(&g_plane_x, __ATOMIC_SEQ_CST) == W / 2 &&
              __atomic_load_n(&g_plane_y, __ATOMIC_SEQ_CST) == H / 2,
          "hidden at the centre with no mouse");
    if (pipe(g_mouse) < 0) return ptr;
    fcntl(g_mouse[0], F_SETFL, O_NONBLOCK);
    input_evdev_attach(g_mouse[0], "test mouse", 0, 0, 0, 0, 0);
    run(50);
    CHECK(__atomic_load_n(&g_plane_shown, __ATOMIC_SEQ_CST) == 1, "shown with a mouse");
    g_px = W / 2;
    g_py = H / 2;
    move_to(100, 200);
    CHECK(__atomic_load_n(&g_plane_x, __ATOMIC_SEQ_CST) == 100 &&
              __atomic_load_n(&g_plane_y, __ATOMIC_SEQ_CST) == 200,
          "the plane follows the mouse");
    move_to(W + 50, -20);
    CHECK(__atomic_load_n(&g_plane_x, __ATOMIC_SEQ_CST) == W - 1 &&
              __atomic_load_n(&g_plane_y, __ATOMIC_SEQ_CST) == 0,
          "kept on the screen");
    button(BTN_RIGHT);
    click_text("Display");
    CHECK(ui_page_top() && find("Resolution", 0), "a click lands where the plane shows the pointer");
    button(BTN_RIGHT);
    CHECK(!ui_page_top(), "back on the live view");
    close(g_mouse[1]);
    run(100);
    CHECK(__atomic_load_n(&g_plane_shown, __ATOMIC_SEQ_CST) == 0, "hidden when the mouse is gone");
    if (pipe(g_mouse) == 0) {
        fcntl(g_mouse[0], F_SETFL, O_NONBLOCK);
        input_evdev_attach(g_mouse[0], "test mouse", 0, 0, 0, 0, 0);
    }
    return ptr;
}

static void test_display_pages(void)
{
    run(VG_DURATION_TOAST);
    move_to(300, 200);
    button(BTN_RIGHT);
    click_text("Display");
    CHECK(find("Display", 1) && find("Output", 0) && find("Clock", 0) && find("Camera names", 0) &&
          find("Status icons", 0) && find("Video", 0), "Display: its five parts");

    /* A new resolution, kept: switched, then saved. */
    int sets = g_mode_sets;
    dd_pick(nth_of(&lv_dropdown_class, 0), 1);
    click_text(SAVE);
    run(100);
    CHECK(g_mode.w == 1920 && g_mode.h == 1080 && g_mode_sets == sets + 1, "Save: 1920x1080 at once");
    CHECK(ui_popup_is_open() && find("Keep this resolution (1920 x 1080, 60 Hz)?", 0) &&
          find("Going back to the old resolution in 15 s.", 0), "asked to keep it, 15 s");
    vigil_config_t c;
    config_get(&c);
    CHECK(!strcmp(c.display.output, "1280x720@60"), "not saved before it is kept: %s", c.display.output);
    run(3000);
    CHECK(find("in 12 s", 1), "the countdown");
    click_text(LV_SYMBOL_OK "  Keep");
    run(100);
    config_get(&c);
    CHECK(!ui_popup_is_open() && !strcmp(c.display.output, "1920x1080@60") && g_mode.w == 1920,
          "Keep: saved, %s", c.display.output);
    CHECK(find("1920 x 1080, 60 Hz", 0), "in use: 1920 x 1080");

    /* No answer: the old one comes back by itself, nothing saved. */
    dd_pick(nth_of(&lv_dropdown_class, 0), 2);
    click_text(SAVE);
    run(100);
    CHECK(g_mode.w == 1024 && ui_popup_is_open(), "1024x768 tried");
    run(14000);
    CHECK(g_mode.w == 1024, "still there after 14 s");
    run(1500);
    config_get(&c);
    CHECK(g_mode.w == 1920 && g_mode.h == 1080 && !ui_popup_is_open() && !strcmp(c.display.output, "1920x1080@60"),
          "15 s without an answer: back to 1920x1080 (%ux%u), %s", g_mode.w, g_mode.h, c.display.output);
    CHECK(lv_dropdown_get_selected(nth_of(&lv_dropdown_class, 0)) == 1, "the list shows the one in use");

    /* The right button (Esc) on the question: back at once. Undo: the same. */
    dd_pick(nth_of(&lv_dropdown_class, 0), 0);
    click_text(SAVE);
    run(100);
    CHECK(g_mode.w == 1280, "1280x720 tried");
    button(BTN_RIGHT);
    run(100);
    CHECK(g_mode.w == 1920 && !ui_popup_is_open() && ui_page_top(), "the right button: back to 1920x1080");
    dd_pick(nth_of(&lv_dropdown_class, 0), 0);
    click_text(SAVE);
    run(100);
    click_text(LV_SYMBOL_REFRESH "  Undo");
    run(100);
    CHECK(g_mode.w == 1920 && !ui_popup_is_open(), "Undo: back to 1920x1080");
    dd_pick(nth_of(&lv_dropdown_class, 0), 0);
    click_text(SAVE);
    run(100);
    click_text(LV_SYMBOL_OK "  Keep");
    run(100);
    config_get(&c);
    CHECK(g_mode.w == 1280 && !strcmp(c.display.output, "1280x720@60"), "back to 1280x720, kept");

    /* The picture, the margins and the HDMI sound: in force and saved as
     * they change, without Save. */
    CHECK(find("Picture", 0) && find("Margins (overscan)", 0) && find("Sound (HDMI)", 0),
          "Output: picture, margins, sound");
    dd_pick(nth_of(&lv_dropdown_class, 1), 1);          /* Bright */
    config_get(&c);
    CHECK(!strcmp(c.display.picture, "bright") && g_picture.gamma == 80 && g_picture.luma == 54,
          "Bright: in force and saved (%s, gamma %u)", c.display.picture, g_picture.gamma);
    lv_obj_t *sl = nth_of(&lv_slider_class, 0);         /* top */
    if (sl) {
        lv_obj_add_state(sl, LV_STATE_PRESSED);         /* dragged: in force, not saved */
        lv_slider_set_value(sl, 4, LV_ANIM_OFF);
        lv_obj_send_event(sl, LV_EVENT_VALUE_CHANGED, NULL);
        config_get(&c);
        CHECK(g_margins.top == 4 && c.display.margin_top == 0 && find("4 %", 0), "dragged: 4 %% shown, not saved");
        lv_obj_remove_state(sl, LV_STATE_PRESSED);
        lv_obj_send_event(sl, LV_EVENT_RELEASED, NULL);
        run(50);
        config_get(&c);
        CHECK(c.display.margin_top == 4, "let go: saved (%d)", c.display.margin_top);
    } else {
        CHECK(0, "the margins' sliders");
    }
    sl = nth_of(&lv_slider_class, 3);                   /* right, from the keyboard: saved at once */
    if (sl) {
        lv_slider_set_value(sl, 2, LV_ANIM_OFF);
        lv_obj_send_event(sl, LV_EVENT_VALUE_CHANGED, NULL);
        run(50);
        config_get(&c);
        CHECK(g_margins.right == 2 && g_margins.top == 4 && c.display.margin_right == 2,
              "a key: in force and saved (%u, %d)", g_margins.right, c.display.margin_right);
    }
    sl = nth_of(&lv_slider_class, 4);                   /* volume */
    lv_obj_t *mute = nth_of(&lv_switch_class, 0);
    if (sl && mute) {
        lv_slider_set_value(sl, 55, LV_ANIM_OFF);
        lv_obj_send_event(sl, LV_EVENT_VALUE_CHANGED, NULL);
        lv_obj_add_state(mute, LV_STATE_CHECKED);
        lv_obj_send_event(mute, LV_EVENT_VALUE_CHANGED, NULL);
        run(50);
        config_get(&c);
        CHECK(g_volume == 55 && g_mute && c.display.hdmi_volume == 55 && c.display.hdmi_mute,
              "volume 55, muted: in force and saved (%u %d, %d %d)", g_volume, g_mute, c.display.hdmi_volume, c.display.hdmi_mute);
    } else {
        CHECK(0, "the volume slider and mute");
    }
    if (mute) lv_obj_scroll_to_view_recursive(mute, LV_ANIM_OFF);
    shot("display_output");
    lv_obj_t *none = find(LV_SYMBOL_REFRESH "  No margins", 0);
    CHECK(none, "No margins");
    if (none) click_obj(none);
    run(50);
    config_get(&c);
    CHECK(overscan_none(&g_margins) && !c.display.margin_top && !c.display.margin_right, "No margins: all 0");
    dd_pick(nth_of(&lv_dropdown_class, 1), 0);          /* back to Standard */
    config_get(&c);
    CHECK(!strcmp(c.display.picture, "standard") && g_picture.gamma == 100, "Standard again");

    /* The clock: bottom centre, large, on a box; then the format. */
    click_text("Clock");
    CHECK(find("Position", 0) && find("Background", 0) && find("Date", 0), "Clock: its settings");
    dd_pick(nth_of(&lv_dropdown_class, 0), 5);
    dd_pick(nth_of(&lv_dropdown_class, 1), 2);
    dd_pick(nth_of(&lv_dropdown_class, 2), 2);
    dd_pick(nth_of(&lv_dropdown_class, 4), 1);          /* 12-hour */
    click_text(SAVE);
    run(300);
    config_get(&c);
    CHECK(!strcmp(c.display.clock_position, "bottom-center") && !strcmp(c.display.clock_size, "large") &&
          !strcmp(c.display.clock_background, "box") && !strcmp(c.ui.time_format, "12h"),
          "clock saved: %s %s %s %s", c.display.clock_position, c.display.clock_size, c.display.clock_background,
          c.ui.time_format);
    lv_obj_t *clk = osd_clock();
    if (clk) {
        int x, y;
        center_of(clk, &x, &y);
        CHECK(x > W / 2 - 20 && x < W / 2 + 20 && y > H - 40, "the clock at the bottom centre (%d, %d)", x, y);
        CHECK(lv_obj_get_style_text_font(clk, 0) == VG_FONT_LG_LV && lv_obj_get_style_bg_opa(clk, 0) == LV_OPA_COVER,
              "large, on an opaque box");
    } else {
        CHECK(0, "the clock in 12-hour time on the live view");
    }

    click_text("Camera names");
    dd_pick(nth_of(&lv_dropdown_class, 0), 2);          /* top right */
    dd_pick(nth_of(&lv_dropdown_class, 2), 1);          /* no outline */
    click_text(SAVE);
    run(300);
    click_text("Status icons");
    dd_pick(nth_of(&lv_dropdown_class, 0), 1);          /* top right: next to the name */
    dd_pick(nth_of(&lv_dropdown_class, 1), 2);          /* box */
    click_text(SAVE);
    run(300);
    config_get(&c);
    CHECK(!strcmp(c.display.name_position, "top-right") && !strcmp(c.display.name_background, "none") &&
          !strcmp(c.display.icons_position, "top-right") && !strcmp(c.display.icons_background, "box"),
          "names and icons saved");
    click_text("Video");
    CHECK(find("Video aspect", 0) && lv_dropdown_get_selected(nth_of(&lv_dropdown_class, 0)) == 0,
          "Video: fit by default");
    dd_pick(nth_of(&lv_dropdown_class, 0), 1);          /* stretch */
    click_text(SAVE);
    run(300);
    config_get(&c);
    CHECK(!strcmp(c.display.video_aspect, "stretch"), "video aspect saved: %s", c.display.video_aspect);
    dd_pick(nth_of(&lv_dropdown_class, 0), 0);
    click_text(SAVE);
    run(300);
    config_get(&c);
    CHECK(!strcmp(c.display.video_aspect, "fit"), "and back to fit: %s", c.display.video_aspect);
    button(BTN_RIGHT);
    CHECK(!ui_page_top(), "back on the live view");
    lv_obj_t *gate = find("Gate", 0);
    if (gate) {
        int x, y;
        center_of(gate, &x, &y);
        CHECK(x > 500 && x < 640 && y < 40, "Gate's name at its tile's top right (%d, %d)", x, y);
    }
    lv_obj_t *t = tile("Gate"), *row = NULL;
    for (uint32_t i = 0; t && i < lv_obj_get_child_count(t); ++i)
        if (icon_shown(lv_obj_get_child(t, (int32_t)i), &ui_icon_rec)) row = lv_obj_get_child(t, (int32_t)i);
    if (row && gate) {
        lv_area_t a, b;
        lv_obj_get_coords(gate, &a);
        lv_obj_get_coords(row, &b);
        CHECK(b.y1 > a.y2 && b.x2 > 600 && lv_obj_get_style_bg_opa(row, 0) == LV_OPA_COVER,
              "the icons below the name, on a box (%d > %d)", b.y1, a.y2);
    } else {
        CHECK(0, "Gate's icons");
    }

    /* The clock at the top left, the names there too: the first tile's
     * name moves below the clock, the others stay at the top. */
    CHECK(put_display("{\"clock_position\":\"top-left\",\"name_position\":\"top-left\"}") == CONFIG_OK,
          "clock and names top left");
    clk = osd_clock();
    lv_obj_t *yard = find("Yard", 0);
    gate = find("Gate", 0);
    if (clk && gate && yard) {
        lv_area_t a, g, y;
        lv_obj_get_coords(clk, &a);
        lv_obj_get_coords(gate, &g);
        lv_obj_get_coords(yard, &y);
        CHECK(g.y1 > a.y2 && y.y1 < a.y2 && lv_obj_get_style_bg_opa(clk, 0) == LV_OPA_TRANSP,
              "Gate below the outlined clock (%d > %d), Yard at the top (%d)", g.y1, a.y2, y.y1);
    } else {
        CHECK(0, "clock, Gate and Yard");
    }
    /* The clock at the top centre, a name at its tile's top right: moved. */
    CHECK(put_display("{\"clock_position\":\"top-center\",\"clock_size\":\"xlarge\","
                      "\"name_position\":\"top-right\"}") == CONFIG_OK, "clock top centre");
    clk = osd_clock();
    gate = find("Gate", 0);
    yard = find("Yard", 0);
    if (clk && gate && yard) {
        lv_area_t a, g, y;
        lv_obj_get_coords(clk, &a);
        lv_obj_get_coords(gate, &g);
        lv_obj_get_coords(yard, &y);
        CHECK(a.x1 < 640 && a.x2 > 640 && g.y1 > a.y2 && y.y1 < a.y2, "Gate below it (%d > %d), Yard not (%d)",
              g.y1, a.y2, y.y1);
    } else {
        CHECK(0, "clock, Gate and Yard");
    }
    CHECK(put_display("{\"clock_position\":\"off\",\"name_position\":\"off\"}") == CONFIG_OK, "no clock");
    CHECK(!osd_clock() && !find("Gate", 0) && icon_shown(first_tile(), &ui_icon_rec),
          "no clock, no names; the icons stay");
    CHECK(put_display("{}") == CONFIG_OK, "the defaults");
    clk = osd_clock();
    CHECK(clk && lv_obj_get_style_bg_opa(clk, 0) == LV_OPA_TRANSP, "an outlined clock by default");
    const char *fmt = "{\"language\":\"en\",\"date_format\":\"YYYY-MM-DD\",\"time_format\":\"24h\"}";
    schema_errors_t e;
    memset(&e, 0, sizeof e);
    config_put_json("ui", fmt, strlen(fmt), &e);
    run(300);
}

static void test_account_pages(void)
{
    run(VG_DURATION_TOAST);
    click_text("ACCOUNT");
    CHECK(find("Account", 1) && find("No users yet", 1) && !find(SAVE, 0), "Users: none, no Save");
    click_text(LV_SYMBOL_PLUS "  Add user");
    lv_textarea_set_text(textarea(0), "anna");
    lv_textarea_set_text(textarea(1), "anna1");
    lv_textarea_set_text(textarea(2), "anna1");
    shot("account_user_editor");
    click_obj(find(LV_SYMBOL_OK "  Add user", 0));
    run(100);
    CHECK(g_nfu == 1 && !strcmp(g_fu[0].name, "anna") && !strcmp(g_fu[0].group, "user") && find("anna", 0),
          "anna added and listed");
    shot("account_users");
    click_text(LV_SYMBOL_PLUS "  Add user");
    lv_textarea_set_text(textarea(0), "anna");
    lv_textarea_set_text(textarea(1), "other1");
    lv_textarea_set_text(textarea(2), "other1");
    click_obj(find(LV_SYMBOL_OK "  Add user", 0));
    run(100);
    CHECK(g_nfu == 1 && find("the name is taken", 1), "a taken name: refused under the field");
    lv_textarea_set_text(textarea(1), "x1");
    click_obj(find(LV_SYMBOL_CLOSE "  Cancel", 0));
    run(100);
    click_obj(find(LV_SYMBOL_EDIT "  Edit", 0));
    lv_obj_t *en = nth_of(&lv_switch_class, 0);
    if (en) lv_obj_remove_state(en, LV_STATE_CHECKED);
    click_obj(find(LV_SYMBOL_OK "  Save user", 0));
    run(100);
    CHECK(g_nfu == 1 && !g_fu[0].enabled && find("no", 0), "anna disabled");
    click_obj(find(LV_SYMBOL_TRASH "  Remove", 0));
    CHECK(ui_popup_is_open() && find("Remove the user anna?", 1), "Remove asks");
    click_text("Remove");
    run(100);
    CHECK(g_nfu == 0 && find("No users yet", 1), "anna removed");

    click_text("Groups");
    run(200);
    lv_obj_t *admin = find("admin", 0);
    lv_obj_t *arow = admin ? lv_obj_get_parent(admin) : NULL;
    CHECK(arow && lv_obj_has_state(lv_obj_get_child(arow, 2), LV_STATE_DISABLED), "admin: read only");
    lv_textarea_set_text(textarea(0), "viewer");
    click_obj(find(LV_SYMBOL_PLUS "  Add", 0));
    run(100);
    lv_obj_t *v = find("viewer", 0);
    CHECK(v && g_nfg == 3, "viewer added");
    if (v) click_obj(lv_obj_get_child(lv_obj_get_parent(v), 2));     /* Live */
    shot("account_groups");
    click_text(SAVE);
    run(100);
    CHECK(g_nfg == 3 && g_fg[2].perms == 1, "viewer: live only (%x)", g_nfg == 3 ? g_fg[2].perms : 0);
    button(BTN_RIGHT);
    button(BTN_RIGHT);
    CHECK(!ui_page_top(), "back on the live view");
}

static void shots_system(void)
{
    static const char *const items[] = { "General", "HTTP API", "Zero channel" };
    shots_settings("SYSTEM", "system", items, 3);
    click_text("SYSTEM");
    dd_pick(nth_of(&lv_dropdown_class, 3), g_mg.zones());
    lv_obj_t *dst = nth_of(&lv_switch_class, 0);
    if (dst) {
        lv_obj_add_state(dst, LV_STATE_CHECKED);
        lv_obj_send_event(dst, LV_EVENT_VALUE_CHANGED, NULL);
    }
    run(50);
    lv_obj_t *pv = find("POSIX:", 1);
    if (pv) lv_obj_scroll_to_view_recursive(pv, LV_ANIM_OFF);
    shot("system-general-custom-tz");
    lv_obj_t *ls = find("Last synchronised", 1);
    if (ls) lv_obj_scroll_to_view_recursive(ls, LV_ANIM_OFF);
    shot("system-general-ntp");
    click_text("Cancel");
    button(BTN_RIGHT);
}

static void shots_account(void)
{
    g_nfu = 0;
    fa_user_add("anna", "user", "anna1", 1, (char[16]){ 0 }, 16, (char[64]){ 0 }, 64);
    fa_user_add("guard", "user", "guard1", 0, (char[16]){ 0 }, 16, (char[64]){ 0 }, 64);
    fa_group_set("viewer", 1, 1, (char[16]){ 0 }, 16, (char[64]){ 0 }, 64);
    static const char *const items[] = { "Users", "Groups" };
    shots_settings("ACCOUNT", "account", items, 2);
    click_text("ACCOUNT");
    click_text(LV_SYMBOL_PLUS "  Add user");
    shot("account-users-add");
    click_obj(find(LV_SYMBOL_EDIT "  Edit", 0));
    shot("account-users-edit");
    button(BTN_RIGHT);
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
    CHECK(find("Resolution", 0), "a click on DISPLAY opens it");
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
    CHECK(find("Disk Manager", 0), "Enter: the storage screen");
    key(KEY_ESC);
    CHECK(find("Main menu", 0), "Esc: back");
    open_form("record");
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
    open_form("record");
    CHECK(find("Recording directory", 0), "the form again");
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

/* An empty slot's "+": Channels with that channel marked; its row's Add
 * opens the camera form for it; the on-screen keyboard (mouse only) next
 * to the field; a camera added at channel 4 with 2 and 3 left empty. Then
 * Channel info. */
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
    CHECK(find("Channel 4: check a device above", 1), "\"+\": Channels, for channel 4");
    shot("tile_add_channels");
    /* The toolbar's Add, then the empty channels' (2, 3, 4). */
    lv_obj_t *add4 = find_nth(lv_screen_active(), LV_SYMBOL_PLUS "  Add", 3);
    CHECK(add4 != NULL, "channel 4: Add");
    if (!add4) return;
    center_of(add4, &x, &y);
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
    CHECK(find("Channel 4: check a device above", 1), "back on Channels");
    button(BTN_RIGHT);
    run(100);

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
    click_text("OPERATION");
    g_xm_chan4 = 1;
    click_text("Channel info");
    run(1100);
    CHECK(find("Channel info", 0) && find("1920x1080", 0) && find("motion, human", 0) &&
          find("Connecting", 0), "Channel info: state, size, events");
    CHECK(find("Front door", 0) && find("monitor refused (Ret 103)", 0) && find("640x360", 0),
          "Channel info: the sub-stream live, the main stream's failure in its own column");
    shot("channel_info");
    g_xm_chan4 = 0;
    button(BTN_RIGHT);
    button(BTN_RIGHT);
    button(BTN_RIGHT);
    CHECK(!ui_page_top(), "back on the live view");
}

/* Pick `text` in drop-down `dd`. */
static void dd_pick_text(lv_obj_t *dd, const char *text)
{
    char opts[512];
    snprintf(opts, sizeof opts, "%s", lv_dropdown_get_options(dd));
    int i = 0;
    for (char *p = strtok(opts, "\n"); p; p = strtok(NULL, "\n"), ++i)
        if (!strcmp(p, text)) {
            lv_dropdown_set_selected(dd, (uint32_t)i);
            return;
        }
}

/* Exit: reboot and shutdown asked, then done; the on-screen login: off by
 * default, then on: the menu needs a user, a wrong password is refused,
 * a user without the permission is told, Exit > Logout and idle end it. */
static void test_session(void)
{
    CHECK(!ui_login_required(), "the on-screen login is off by default");
    move_to(300, 200);
    button(BTN_RIGHT);
    click_text("Exit");
    CHECK(ui_popup_is_open() && find("Shutdown", 1) && find("the on-screen login is off", 1), "Exit: the chooser");
    lv_obj_t *out = find("Logout", 1);
    CHECK(out && lv_obj_has_state(lv_obj_get_parent(out), LV_STATE_DISABLED), "Logout: off without a login");
    shot("exit");
    click_text("Reboot");
    CHECK(ui_popup_is_open() && find("Reboot the recorder?", 1), "Reboot asks first");
    shot("reboot_confirm");
    click_text("Cancel");
    CHECK(g_power == -1 && !ui_popup_is_open(), "Cancel: nothing");
    button(BTN_RIGHT);
    click_text("Exit");
    click_text("Shutdown");
    lv_obj_t *yes = find_in(lv_layer_top(), "Shutdown", 0);
    if (yes) {
        int x, y;
        center_of(yes, &x, &y);
        click_at(x, y);
    }
    CHECK(g_power == 0 && find("Shutting down", 1), "Shutdown, confirmed: asked of the system");
    run(VG_DURATION_TOAST + 100);

    /* The login on, out after a minute without use. */
    schema_errors_t se = { 0 };
    static const char on[] = "{\"login\":true,\"idle_logout\":1}", off[] = "{\"login\":false}";
    CHECK(config_put_json("ui", on, strlen(on), &se) == CONFIG_OK, "login on");
    ui_app_refresh();
    run(300);
    CHECK(ui_login_required() && !ui_logged_in(), "required, nobody in");
    move_to(W / 2, H - 1);
    run(300);
    click_text("Main menu");
    CHECK(ui_popup_is_open() && find("Log in", 1) && !ui_page_top(), "the menu asks for a user");
    lv_obj_t *dd = NULL, *pw = NULL;
    int seen = 0;
    pw = find_textarea(lv_layer_top(), 0, &seen);
    shot("login");
    CHECK(pw && lv_textarea_get_password_mode(pw), "a password field");
    if (pw) lv_textarea_set_text(pw, "nope");
    click_text("Log in");                       /* the title: nothing */
    lv_obj_t *btn = find_nth(lv_layer_top(), "Log in", 1);
    int bx, by;
    if (btn) {
        center_of(btn, &bx, &by);
        click_at(bx, by);
    }
    CHECK(find("Wrong user or password", 1) && !ui_logged_in(), "a wrong password: refused");
    shot("login_wrong");
    if (pw) lv_textarea_set_text(pw, "rootpw");
    if (btn) click_at(bx, by);
    CHECK(ui_logged_in() && !strcmp(ui_session_user(), "root") && ui_page_top(), "root: in, the menu opens");
    button(BTN_RIGHT);
    move_to(300, 200);
    button(BTN_RIGHT);
    click_text("Exit");
    out = find("Logout", 1);
    CHECK(out && !lv_obj_has_state(lv_obj_get_parent(out), LV_STATE_DISABLED), "Logout: there now");
    click_text("Logout");
    CHECK(!ui_logged_in() && find("Logged out", 1), "logged out");
    run(VG_DURATION_TOAST + 100);

    /* A user with the live view only: in, but not into Camera. */
    g_fu[g_nfu] = (ui_user_t){ .name = "liv", .group = "watch", .enabled = 1, .password_set = 1 };
    g_fg[g_nfg] = (ui_group_t){ .name = "watch", .perms = UI_PERM_LIVE, .editable = 1 };
    ++g_nfu;
    ++g_nfg;
    move_to(W / 2, H - 1);
    run(300);
    click_text("Main menu");
    dd = NULL;
    seen = 0;
    pw = find_textarea(lv_layer_top(), 0, &seen);
    /* the drop-down: the first one on the top layer */
    lv_obj_t *stack[64];
    int sp = 0;
    stack[sp++] = lv_layer_top();
    while (sp && !dd) {
        lv_obj_t *o = stack[--sp];
        if (lv_obj_check_type(o, &lv_dropdown_class)) dd = o;
        for (uint32_t i = 0; i < lv_obj_get_child_count(o) && sp < 64; ++i) stack[sp++] = lv_obj_get_child(o, (int32_t)i);
    }
    CHECK(dd && pw, "the dialog again");
    if (dd) dd_pick_text(dd, "liv");
    if (pw) lv_textarea_set_text(pw, "livpw");
    btn = find_nth(lv_layer_top(), "Log in", 1);
    if (btn) {
        center_of(btn, &bx, &by);
        click_at(bx, by);
    }
    CHECK(ui_logged_in() && !strcmp(ui_session_user(), "liv") && ui_page_top(), "liv: in, the menu");
    click_text("CAMERA");
    CHECK(find("no permission", 1) && find("Main menu", 0), "liv: not into Camera");
    button(BTN_RIGHT);

    /* A minute without a key or the mouse: out. */
    run(61 * 1000);
    CHECK(!ui_logged_in() && !ui_page_top(), "idle: logged out, the live view");
    CHECK(config_put_json("ui", off, strlen(off), &se) == CONFIG_OK, "login off again");
    ui_app_refresh();
    run(300);
    --g_nfu;
    --g_nfg;
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

    /* Channels opened for channel 14 (an empty tile's "+"): its row is
     * scrolled into view in the channel table. */
    ui_channels_open_at(13);
    run(100);
    lv_obj_t *n14 = find("14", 0);
    lv_obj_t *table = n14 ? lv_obj_get_parent(lv_obj_get_parent(n14)) : NULL;
    lv_area_t ra, ta;
    if (table) {
        lv_obj_get_coords(lv_obj_get_parent(n14), &ra);
        lv_obj_get_coords(table, &ta);
    }
    CHECK(find("Channel 14: check a device above", 1) && table && lv_obj_get_scroll_y(table) > 0 &&
          ra.y1 >= ta.y1 && ra.y2 <= ta.y2 + 1, "Channels for channel 14: its row scrolled into view");
    shot("channels16_target");
    button(BTN_RIGHT);
    run(100);
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
    if (!g_live) fake_discovery(&g_model);
    fake_mgmt(&g_model);
    ui_app_create(&g_model);
    if (pipe(g_mouse) < 0 || pipe(g_kbd) < 0) return -1;
    fcntl(g_mouse[0], F_SETFL, O_NONBLOCK);
    fcntl(g_kbd[0], F_SETFL, O_NONBLOCK);
    static const input_hooks_t hooks = { ui_app_back, ui_app_key, ui_app_keyboards, ui_app_wheel };
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

/* Display, its five parts and the resolution question (the page open). */
static void shots_display(void)
{
    shot("display-output");
    lv_dropdown_open(nth_of(&lv_dropdown_class, 0));
    shot("display-output-list");
    lv_dropdown_close(nth_of(&lv_dropdown_class, 0));
    dd_pick(nth_of(&lv_dropdown_class, 0), 1);
    click_text(SAVE);
    run(2000);
    shot("display-keep");
    click_text(LV_SYMBOL_REFRESH "  Undo");
    quiet();
    click_text("Clock");
    shot("display-clock");
    click_text("Camera names");
    shot("display-names");
    click_text("Status icons");
    shot("display-icons");
    click_text("Video");
    shot("display-video");
    click_text("Output");
}

/* The live view's text and icons over a picture, as display.* places
 * and dresses them (the live view shown, 2x2). */
static void shots_osd(void)
{
    static const struct { const char *name, *json; } k[] = {
        { "osd-default", "{}" },
        { "osd-clock-top-left-box", "{\"clock_position\":\"top-left\",\"clock_background\":\"box\"}" },
        { "osd-clock-top-center-xl", "{\"clock_position\":\"top-center\",\"clock_size\":\"xlarge\","
                                     "\"name_position\":\"top-right\"}" },
        { "osd-clock-bottom-center-none", "{\"clock_position\":\"bottom-center\",\"clock_size\":\"large\","
                                          "\"clock_background\":\"none\",\"name_background\":\"none\","
                                          "\"icons_background\":\"none\"}" },
        { "osd-all-top-left", "{\"clock_position\":\"top-left\",\"name_position\":\"top-left\","
                              "\"icons_position\":\"top-left\",\"name_size\":\"large\"}" },
        { "osd-boxes", "{\"clock_background\":\"box\",\"name_background\":\"box\",\"icons_background\":\"box\"}" },
        { "osd-names-off-clock-bottom-left", "{\"clock_position\":\"bottom-left\",\"name_position\":\"off\"}" },
    };
    g_video_all = 1;
    for (size_t i = 0; i < sizeof k / sizeof k[0]; ++i) {
        put_display(k[i].json);
        lv_obj_invalidate(lv_screen_active());
        shot(k[i].name);
    }
    put_display("{}");
    lv_obj_invalidate(lv_screen_active());
    g_video_all = 0;
}

static int shots_4(void)
{
    char path[512];
    if (setup(4, k_shots_json, path, sizeof path) < 0) return 1;
    shot("live-2x2");
    shots_osd();
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
    open_form("record");
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
    shots_display();
    button(BTN_RIGHT);
    quiet();
    shots_network();
    quiet();
    shots_storage();
    quiet();
    shots_system();
    quiet();
    shots_account();
    quiet();
    click_text("OPERATION");
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
    key(KEY_ESC);
    key(KEY_ESC);
    key(KEY_ESC);

    /* Exit, a reboot asked, and the on-screen login (turned on here);
     * the keyboard unplugged: the on-screen keyboard is back. */
    close(g_kbd[1]);
    run(200);
    move_to(300, 200);
    button(BTN_RIGHT);
    shot("context-menu-exit");
    click_text("Exit");
    shot("exit-chooser");
    click_text("Reboot");
    shot("reboot-confirm");
    click_text("Cancel");
    button(BTN_RIGHT);
    click_text("Exit");
    click_text("Shutdown");
    shot("shutdown-confirm");
    click_text("Cancel");
    schema_errors_t se = { 0 };
    static const char on[] = "{\"login\":true,\"idle_logout\":10}";
    config_put_json("ui", on, strlen(on), &se);
    ui_app_refresh();
    run(300);
    move_to(W / 2, H - 1);
    run(300);
    click_text("Main menu");
    shot("login");
    int seen = 0;
    lv_obj_t *pw = find_textarea(lv_layer_top(), 0, &seen);
    if (pw) {
        int x, y;
        center_of(pw, &x, &y);
        click_at(x, y);
        shot("login-keyboard");
    }
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
    g_video_all = 1;
    lv_obj_invalidate(lv_screen_active());
    shot("live-16-text-over-video");            /* the outlined text over a picture */
    g_video_all = 0;
    move_to(W / 2, H - 1);
    run(300);
    click_text("Main menu");
    click_text("OPERATION");
    click_text("Channel info");
    shot("channel-info-16");
    input_evdev_delete();
    unlink(path);
    return g_fail ? 1 : 0;
}

/* ---- a live search, for people (tests/live_search_shots.sh) ------------------------------ */

/* The real discovery and bridge against fake_discovery.py's devices,
 * which answer late (0.2 s to 3 s): screenshots while the search runs
 * show the rows coming in. The search asks only 127.0.0.1 on the fake's
 * ports, by unicast: nothing leaves the machine. */
static int g_live_port[3];

static int live_scan(unsigned protos, char *err, size_t cap)
{
    disc_scan_opts_t o;
    memset(&o, 0, sizeof o);
    o.protos = protos;
    o.seconds = 4;
    o.no_broadcast = 1;
    snprintf(o.targets, sizeof o.targets, "127.0.0.1");
    memcpy(o.port, g_live_port, sizeof o.port);
    int id = disc_scan(&o);
    if (id < 0) snprintf(err, cap, "cannot start");
    return id;
}

static long long real_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

/* The UI in step with the clock (the search's thread runs on it) until
 * `ms` after `t0`. */
static void run_until(long long t0, int ms)
{
    while (real_ms() - t0 < ms) {
        usleep(5000);
        g_ms += 5;
        lv_timer_handler();
    }
}

static void live_shot(long long t0, const char *what)
{
    char name[64];
    snprintf(name, sizeof name, "live-%05.2fs-%s", (double)(real_ms() - t0) / 1000.0, what);
    shot(name);
    printf("%s\n", name);
}

static int live_search(const char *ports)
{
    if (sscanf(ports, "%d,%d,%d", &g_live_port[0], &g_live_port[1], &g_live_port[2]) != 3) return 2;
    char path[512];
    g_live = 1;
    /* Channel 1's camera is one of the fakes: its row says so. */
    if (setup(4, "{\"version\":1,\"cameras\":[{\"url\":\"rtsp://10.20.0.31/stream1\",\"name\":\"Gate\"}]}", path,
              sizeof path) < 0)
        return 1;
    g_model.disc_scan = live_scan;
    ui_channels_open();
    run(300);
    move_to(640, 600);
    click_text(LV_SYMBOL_REFRESH "  Search");
    long long t0 = real_ms();
    static const struct { int ms; const char *what; } at[] = {
        { 100, "started" }, { 450, "first" }, { 1100, "more" }, { 1700, "merged" }, { 2300, "filling" },
        { 3200, "late" }, { 4600, "done" },
    };
    for (size_t i = 0; i < sizeof at / sizeof at[0]; ++i) {
        run_until(t0, at[i].ms);
        live_shot(t0, at[i].what);
    }
    lv_obj_t *top = find("10.20.0.31", 0);
    if (top) {
        int x, y;
        center_of(top, &x, &y);
        move_to(x + 300, y);
        for (int i = 0; i < 4; ++i) ev(EV_REL, REL_WHEEL, -1);
        ev(EV_SYN, SYN_REPORT, 0);
        run(100);
        live_shot(t0, "wheel-middle");
        for (int i = 0; i < 30; ++i) ev(EV_REL, REL_WHEEL, -1);
        ev(EV_SYN, SYN_REPORT, 0);
        run(100);
        live_shot(t0, "wheel-bottom");
        for (int i = 0; i < 40; ++i) ev(EV_REL, REL_WHEEL, 1);
        ev(EV_SYN, SYN_REPORT, 0);
        run(100);
    }
    /* Again: what the last search found is dimmed until it answers. */
    move_to(640, 600);
    click_text(LV_SYMBOL_REFRESH "  Search");
    t0 = real_ms();
    run_until(t0, 700);
    live_shot(t0, "again");
    run_until(t0, 4600);
    live_shot(t0, "again-done");
    disc_shutdown();
    input_evdev_delete();
    unlink(path);
    return 0;
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
    if (argc > 3 && !strcmp(argv[1], "--live-search")) {
        g_out = argv[2];
        mkdir(g_out, 0755);
        return live_search(argv[3]);
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
    fake_mgmt(&g_model);
    ui_app_create(&g_model);

    if (pipe(g_mouse) < 0) return 1;
    fcntl(g_mouse[0], F_SETFL, O_NONBLOCK);
    static const input_hooks_t hooks = { ui_app_back, ui_app_key, ui_app_keyboards, ui_app_wheel };
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
    test_channels_scroll();
    test_channels_manual();
    test_system();
    test_same_redacted_urls();
    test_stale_and_cancel();
    test_two_mice(ptr);
    test_keyboard_live();
    test_keyboard_settings();
    test_keyboard_dropdown();
    test_keyboard_plug();
    test_empty_slot();
    test_session();
    test_network_pages();
    test_storage_pages();
    test_system_pages();
    test_account_pages();
    test_display_pages();
    test_cursor_plane(disp);

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
