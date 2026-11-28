/* SPDX-License-Identifier: GPL-3.0-or-later */
/* test_ptz.c — the PTZ core (ptz.h): NETIP's command encoding, the
 * worker's moves and their safety stops (a hold that runs out, a stop
 * that does not get through, a camera replaced or vigild stopping while
 * it moves), presets and their names kept across a restart, with a fake
 * backend that records what it is asked; then the real NETIP backend
 * against a NETIP camera in a thread, over a socket.
 *
 *   test_ptz [TMPDIR]
 */
#define _DEFAULT_SOURCE
#include <math.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>

#include "cJSON.h"
#include "hal_capture.h"
#include "netip_proto.h"
#include "ptz.h"

static int g_fail, g_checks;
#define CHECK(cond, ...) do { ++g_checks; if (!(cond)) { ++g_fail; \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* The configuration's URL check can create a capture: source; no HAL here. */
int hal_cap_start(int in, int ch, hal_cap_cb cb, void *user) { (void)in; (void)ch; (void)cb; (void)user; return -1; }
void hal_cap_stop(int in) { (void)in; }

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* ---- NETIP's encoding ------------------------------------------------------------------- */

static void test_netip_commands(void)
{
    static const struct { double p, t; const char *cmd; } dirs[] = {
        { 1, 0, "DirectionRight" },     { 0.7, 0.7, "DirectionRightUp" }, { 0, 1, "DirectionUp" },
        { -0.7, 0.7, "DirectionLeftUp" }, { -1, 0, "DirectionLeft" },   { -0.7, -0.7, "DirectionLeftDown" },
        { 0, -1, "DirectionDown" },     { 0.7, -0.7, "DirectionRightDown" }, { 1, 0.3, "DirectionRight" },
        { 0.3, 1, "DirectionUp" },
    };
    for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; ++i) {
        double v[PTZ_AXES] = { dirs[i].p, dirs[i].t, 0, 0, 0 };
        int step;
        const char *c = ptz_netip_command(v, &step);
        CHECK(c && !strcmp(c, dirs[i].cmd), "pan %.1f tilt %.1f: %s (got %s)", dirs[i].p, dirs[i].t, dirs[i].cmd,
              c ? c : "none");
    }
    double v[PTZ_AXES] = { 0 };
    int step = 0;
    CHECK(!ptz_netip_command(v, &step), "nothing: no command");
    v[0] = 0.1;
    CHECK(!ptz_netip_command(v, &step), "inside the dead zone: no command");
    static const struct { double mag; int step; } steps[] = { { 0.15, 1 }, { 0.5, 4 }, { 0.75, 6 }, { 1, 8 } };
    for (size_t i = 0; i < sizeof steps / sizeof steps[0]; ++i) {
        double w[PTZ_AXES] = { -steps[i].mag, 0, 0, 0, 0 };
        ptz_netip_command(w, &step);
        CHECK(step == steps[i].step, "speed %.2f: Step %d (got %d)", steps[i].mag, steps[i].step, step);
    }
    static const struct { int ax; double d; const char *cmd; } lens[] = {
        { PTZ_AX_ZOOM, 1, "ZoomTile" },  { PTZ_AX_ZOOM, -1, "ZoomWide" }, { PTZ_AX_FOCUS, 1, "FocusFar" },
        { PTZ_AX_FOCUS, -1, "FocusNear" }, { PTZ_AX_IRIS, 1, "IrisLarge" }, { PTZ_AX_IRIS, -1, "IrisSmall" },
    };
    for (size_t i = 0; i < sizeof lens / sizeof lens[0]; ++i) {
        double w[PTZ_AXES] = { 0 };
        w[lens[i].ax] = lens[i].d;
        const char *c = ptz_netip_command(w, &step);
        CHECK(c && !strcmp(c, lens[i].cmd), "%s", lens[i].cmd);
    }
    double both[PTZ_AXES] = { 1, 0, 1, 0, 0 };
    const char *c = ptz_netip_command(both, &step);
    CHECK(c && !strcmp(c, "DirectionRight"), "pan and zoom at once: the pan (one command at a time)");

    char body[512];
    CHECK(ptz_netip_body(body, sizeof body, 0x1234ABCD, "DirectionLeftUp", 2, 65535, 6, 0) > 0, "body");
    cJSON *doc = cJSON_Parse(body);
    const cJSON *op = cJSON_GetObjectItemCaseSensitive(doc, "OPPTZControl");
    const cJSON *par = cJSON_GetObjectItemCaseSensitive(op, "Parameter");
    CHECK(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(doc, "Name")), "OPPTZControl"), "Name");
    CHECK(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(doc, "SessionID")), "0x1234ABCD"), "SessionID");
    CHECK(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(op, "Command")), "DirectionLeftUp"), "Command");
    CHECK(cJSON_GetObjectItemCaseSensitive(par, "Channel")->valueint == 2, "Channel");
    CHECK(cJSON_GetObjectItemCaseSensitive(par, "Preset")->valueint == 65535, "Preset 65535 starts");
    CHECK(cJSON_GetObjectItemCaseSensitive(par, "Step")->valueint == 6, "Step");
    CHECK(cJSON_GetObjectItemCaseSensitive(par, "Tour")->valueint == 0, "Tour");
    CHECK(cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(par, "AUX")) &&
              !strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(par, "MenuOpts")), "Enter"),
          "the fields python-dvr sends");
    cJSON_Delete(doc);
    CHECK(ptz_netip_body(body, sizeof body, 1, "DirectionUp", 0, -1, 1, 0) > 0 && strstr(body, "\"Preset\":-1"),
          "Preset -1 stops");
    CHECK(ptz_netip_body(body, 64, 1, "DirectionUp", 0, -1, 1, 0) < 0, "too small: -1");
}

/* ---- a fake backend ------------------------------------------------------------------------ */

static pthread_mutex_t g_flock = PTHREAD_MUTEX_INITIALIZER;
static char g_flog[64][96];
static int  g_fn, g_fail_motion, g_hangups, g_fake_presets;
static unsigned g_fake_caps = PTZ_CAP_PAN_TILT | PTZ_CAP_ZOOM | PTZ_CAP_FOCUS | PTZ_CAP_PRESETS | PTZ_CAP_TOURS;

static void flog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void flog(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&g_flock);
    if (g_fn < 64) vsnprintf(g_flog[g_fn++], sizeof g_flog[0], fmt, ap);
    pthread_mutex_unlock(&g_flock);
    va_end(ap);
}

static int fcount(const char *prefix)
{
    int n = 0;
    pthread_mutex_lock(&g_flock);
    for (int i = 0; i < g_fn; ++i) n += !strncmp(g_flog[i], prefix, strlen(prefix));
    pthread_mutex_unlock(&g_flock);
    return n;
}

static void freset(void)
{
    pthread_mutex_lock(&g_flock);
    g_fn = 0;
    g_fail_motion = g_hangups = 0;
    pthread_mutex_unlock(&g_flock);
}

static const char *flast(void)
{
    static char b[96];
    pthread_mutex_lock(&g_flock);
    snprintf(b, sizeof b, "%s", g_fn ? g_flog[g_fn - 1] : "");
    pthread_mutex_unlock(&g_flock);
    return b;
}

static int g_dummy;
static void *f_create(const ptz_camera_t *c, const volatile int *quit, char *err, size_t cap)
{
    (void)c; (void)quit; (void)err; (void)cap;
    flog("create");
    return &g_dummy;
}
static void f_destroy(void *l) { (void)l; flog("destroy"); }
static int f_probe(void *l, unsigned *caps, int *tours, ptz_preset_t *p, int *n, char *err, size_t cap)
{
    (void)l; (void)err; (void)cap;
    *caps = g_fake_caps;
    *tours = 2;
    *n = -1;
    if (g_fake_presets) {
        *n = 2;
        snprintf(p[0].token, sizeof p[0].token, "Preset_1");
        snprintf(p[0].name, sizeof p[0].name, "Door");
        snprintf(p[1].token, sizeof p[1].token, "Preset_2");
        snprintf(p[1].name, sizeof p[1].name, "Window");
    }
    flog("probe");
    return 0;
}
static int f_motion(void *l, const double from[PTZ_AXES], const double to[PTZ_AXES], char *err, size_t cap)
{
    (void)l; (void)from;
    int fail;
    pthread_mutex_lock(&g_flock);
    fail = g_fail_motion;
    pthread_mutex_unlock(&g_flock);
    int stop = 1;
    for (int i = 0; i < PTZ_AXES; ++i) stop &= to[i] == 0;
    if (stop) flog("stop");
    else flog("move %.2f %.2f %.2f %.2f %.2f", to[0], to[1], to[2], to[3], to[4]);
    if (fail) {
        snprintf(err, cap, "no answer");
        return -1;
    }
    return 0;
}
static int f_preset(void *l, int op, const char *token, const char *name, char *out, size_t ocap, char *err,
                    size_t cap)
{
    (void)l; (void)err; (void)cap;
    flog("preset %d %s %s", op, token, name ? name : "-");
    snprintf(out, ocap, "%s", token[0] ? token : "Preset_9");
    return 0;
}
static int f_home(void *l, char *err, size_t cap) { (void)l; (void)err; (void)cap; flog("home"); return 0; }
static int f_tour(void *l, int n, int start, char *err, size_t cap)
{
    (void)l; (void)err; (void)cap;
    flog("tour %d %d", n, start);
    return 0;
}
static void f_hangup(void *l) { (void)l; flog("hangup"); ++g_hangups; }

static const ptz_drv_t k_fake = { .name = "fake", .create = f_create, .destroy = f_destroy, .probe = f_probe,
                                  .motion = f_motion, .preset = f_preset, .home = f_home, .tour = f_tour,
                                  .hangup = f_hangup };

static int wait_for(int (*cond)(void), int ms)
{
    for (int t = 0; t < ms; t += 10) {
        if (cond()) return 1;
        sleep_ms(10);
    }
    return cond();
}

static int ready_ch0(void)
{
    ptz_info_t i;
    ptz_info(0, &i);
    return i.state == PTZ_READY;
}
static int has_stop(void) { return fcount("stop") > 0; }
static int has_move(void) { return fcount("move") > 0; }
static int not_moving(void)
{
    ptz_info_t i;
    ptz_info(0, &i);
    return !i.moving;
}

static void cams(const char *url0, const char *mode)
{
    ptz_camera_t c[2];
    memset(c, 0, sizeof c);
    snprintf(c[0].url, sizeof c[0].url, "%s", url0);
    snprintf(c[0].mode, sizeof c[0].mode, "%s", mode);
    c[0].enabled = 1;
    snprintf(c[1].url, sizeof c[1].url, "capture:0");
    snprintf(c[1].mode, sizeof c[1].mode, "auto");
    c[1].enabled = 1;
    ptz_apply(c, 2);
}

static void test_backend_choice(void)
{
    ptz_camera_t c = { .url = "netip://10.0.0.2/", .mode = "auto", .enabled = 1 };
    CHECK(ptz_backend_for(&c) == PTZ_BACKEND_NETIP, "netip:// auto: NETIP");
    snprintf(c.url, sizeof c.url, "rtsp://10.0.0.2/s");
    CHECK(ptz_backend_for(&c) == PTZ_BACKEND_ONVIF, "rtsp:// auto: ONVIF");
    snprintf(c.mode, sizeof c.mode, "netip");
    CHECK(ptz_backend_for(&c) == PTZ_BACKEND_NETIP, "rtsp:// forced netip");
    snprintf(c.mode, sizeof c.mode, "off");
    CHECK(ptz_backend_for(&c) == PTZ_BACKEND_NONE, "off");
    snprintf(c.url, sizeof c.url, "capture:1");
    snprintf(c.mode, sizeof c.mode, "auto");
    CHECK(ptz_backend_for(&c) == PTZ_BACKEND_NONE, "capture: none");
    snprintf(c.url, sizeof c.url, "netip://h/");
    c.enabled = 0;
    CHECK(ptz_backend_for(&c) == PTZ_BACKEND_NONE, "disabled: none");
}

static void test_safety(void)
{
    ptz_set_drivers(&k_fake, &k_fake);
    freset();
    cams("netip://admin:x@10.0.0.2/", "auto");
    ptz_info_t info;
    CHECK(ptz_info(1, &info) == 0 && info.state == PTZ_OFF, "capture: no PTZ");
    CHECK(ptz_move(1, 1, 0, 0, 0) == PTZ_ENOPTZ, "capture: a move is refused");
    CHECK(ptz_info(0, &info) == 0, "info");
    CHECK(wait_for(ready_ch0, 2000), "ready after the probe");
    ptz_info(0, &info);
    CHECK(info.backend == PTZ_BACKEND_NETIP && info.caps == g_fake_caps && info.tours == 2, "caps from the probe");

    /* A hold that runs out: stopped without anyone asking. */
    CHECK(ptz_move(0, -1, 0, 0, 300) == PTZ_OK, "move");
    CHECK(wait_for(has_move, 1000), "the move is sent");
    sleep_ms(150);
    CHECK(fcount("stop") == 0, "not stopped while it holds");
    CHECK(wait_for(has_stop, 1000), "stopped when the hold ran out");
    CHECK(wait_for(not_moving, 500), "no longer moving");

    /* Repeats keep it going, once sent. */
    freset();
    for (int i = 0; i < 8; ++i) {
        ptz_move(0, 0, 0.5, 0, 300);
        sleep_ms(100);
    }
    CHECK(fcount("move") == 1 && fcount("stop") == 0, "repeated: sent once, still moving (%d moves, %d stops)",
          fcount("move"), fcount("stop"));
    ptz_move(0, 0, 1, 0, 300);
    sleep_ms(100);
    CHECK(fcount("move") == 2, "faster: sent again");
    CHECK(ptz_stop(0) == PTZ_OK && wait_for(has_stop, 1000), "an explicit stop");

    /* A stop that does not get through: tried again, then hung up. */
    freset();
    ptz_move(0, 1, 0, 0, 5000);
    wait_for(has_move, 1000);
    pthread_mutex_lock(&g_flock);
    g_fail_motion = 1;
    pthread_mutex_unlock(&g_flock);
    ptz_stop(0);
    for (int t = 0; t < 3000 && !g_hangups; t += 10) sleep_ms(10);
    CHECK(fcount("stop") == PTZ_STOP_TRIES, "the stop tried %d times (got %d)", PTZ_STOP_TRIES, fcount("stop"));
    CHECK(g_hangups == 1, "then the connection dropped");
    CHECK(wait_for(not_moving, 500), "and the move counted as ended");
    ptz_info(0, &info);
    CHECK(strstr(info.error, "no answer") != NULL, "the failure is said: '%s'", info.error);

    /* A move that fails is not tried again, and is stopped. */
    freset();
    pthread_mutex_lock(&g_flock);
    g_fail_motion = 1;
    pthread_mutex_unlock(&g_flock);
    ptz_move(0, 0, -1, 0, 5000);
    sleep_ms(1500);
    CHECK(fcount("move") == 1 && fcount("stop") >= 1, "a failed move: one try, then a stop (%d, %d)", fcount("move"),
          fcount("stop"));
    pthread_mutex_lock(&g_flock);
    g_fail_motion = 0;
    pthread_mutex_unlock(&g_flock);

    /* The camera replaced while it moves: the old one is stopped. */
    freset();
    ptz_move(0, 1, 1, 0, 5000);
    wait_for(has_move, 1000);
    cams("netip://admin:x@10.0.0.3/", "auto");
    CHECK(wait_for(has_stop, 1000), "the old camera stopped when replaced");
    CHECK(wait_for(ready_ch0, 2000), "the new one probed");

    /* Lens and presets, tours, home not offered. */
    CHECK(ptz_iris(0, 1, 0) == PTZ_ENOTSUP, "no iris: refused");
    CHECK(ptz_home(0) == PTZ_ENOTSUP, "no home: refused");
    freset();
    CHECK(ptz_focus(0, -1, 200) == PTZ_OK, "focus");
    CHECK(wait_for(has_stop, 1500) && fcount("move 0.00 0.00 0.00 -1.00") == 1, "focus near, then its stop");
    CHECK(ptz_tour(0, 2, 1) == PTZ_OK && ptz_tour(0, 0, 1) == PTZ_EINVAL, "tours");
    sleep_ms(100);
    CHECK(fcount("tour 2 1") == 1, "tour 2 started");

    /* vigild stops while it moves. */
    freset();
    ptz_move(0, -1, 0, 0, 5000);
    wait_for(has_move, 1000);
    ptz_shutdown();
    CHECK(fcount("stop") == 1 && fcount("destroy") == 1, "stopped and closed at shutdown");
}

static int n_presets(void)
{
    ptz_info_t i;
    ptz_info(0, &i);
    return i.n_presets;
}
static int presets_2(void) { return n_presets() == 2; }
static int presets_1(void) { return n_presets() == 1; }
static int presets_3(void) { return n_presets() == 3; }

static void test_presets(const char *dir)
{
    char path[512];
    snprintf(path, sizeof path, "%s/ptz-names.json", dir);
    unlink(path);
    ptz_set_drivers(&k_fake, &k_fake);
    g_fake_presets = 0;
    ptz_start(path);
    freset();
    cams("netip://admin:secret@10.0.0.2:34567/?ch=1", "auto");
    ptz_info_t info;
    ptz_info(0, &info);
    CHECK(wait_for(ready_ch0, 2000), "ready");
    CHECK(ptz_preset_set(0, NULL, "Gate") == PTZ_OK, "a new preset");
    CHECK(ptz_preset_set(0, NULL, NULL) == PTZ_OK, "another, no name");
    CHECK(wait_for(presets_2, 1000), "two presets");
    ptz_info(0, &info);
    CHECK(!strcmp(info.presets[0].token, "1") && !strcmp(info.presets[0].name, "Gate"), "1 Gate");
    CHECK(!strcmp(info.presets[1].token, "2") && !info.presets[1].name[0], "2, no name");
    CHECK(fcount("preset 1 1 Gate") == 1 && fcount("preset 1 2 -") == 1, "SetPreset 1 and 2");
    CHECK(ptz_preset_set(0, "300", NULL) == PTZ_EINVAL, "NETIP: 1..255");
    CHECK(ptz_preset_set(0, "x", NULL) == PTZ_EINVAL, "NETIP: a number");
    CHECK(ptz_preset_set(0, NULL, "a\nb") == PTZ_EINVAL, "no control characters in a name");
    CHECK(ptz_preset_rename(0, "2", "Yard") == PTZ_OK, "rename");
    CHECK(ptz_preset_goto(0, "1") == PTZ_OK, "goto");
    CHECK(ptz_preset_set(0, "7", "Road") == PTZ_OK, "set 7");
    CHECK(wait_for(presets_3, 1000), "three");
    CHECK(ptz_preset_remove(0, "1") == PTZ_OK, "remove 1");
    sleep_ms(200);
    ptz_info(0, &info);
    CHECK(info.n_presets == 2 && !strcmp(info.presets[0].token, "2") && !strcmp(info.presets[0].name, "Yard") &&
              !strcmp(info.presets[1].token, "7"),
          "2 Yard, 7 Road");
    CHECK(fcount("preset 0 1 -") == 1 && fcount("preset 2 1 -") == 1, "GotoPreset 1, ClearPreset 1");
    CHECK(ptz_preset_set(0, NULL, NULL) == PTZ_OK, "a new one takes the free number");
    sleep_ms(200);
    CHECK(fcount("preset 1 1 -") == 1, "1 again");
    ptz_preset_remove(0, "1");
    sleep_ms(200);
    ptz_shutdown();
    FILE *f = fopen(path, "r");
    char buf[2048] = "";
    if (f) {
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        buf[n] = 0;
        fclose(f);
    }
    CHECK(strstr(buf, "Yard") && !strstr(buf, "secret"), "kept in the names file, no password in it");

    /* A restart: the same list. */
    ptz_start(path);
    cams("netip://admin:secret@10.0.0.2:34567/?ch=1", "auto");
    ptz_info(0, &info);
    CHECK(wait_for(presets_2, 2000), "after a restart, the list is back");
    /* Another channel of the same NVR has its own. */
    cams("netip://admin:secret@10.0.0.2:34567/?ch=2", "auto");
    ptz_info(0, &info);
    wait_for(ready_ch0, 2000);
    CHECK(n_presets() == 0, "another device channel: none");
    ptz_shutdown();

    /* A camera that keeps a list (ONVIF): its own, with our renames over it. */
    g_fake_presets = 1;
    ptz_start(path);
    cams("rtsp://admin:secret@10.0.0.9/s", "auto");
    ptz_info(0, &info);
    CHECK(wait_for(presets_2, 2000), "the camera's two");
    ptz_info(0, &info);
    CHECK(info.backend == PTZ_BACKEND_ONVIF && !strcmp(info.presets[0].name, "Door"), "its names");
    ptz_preset_rename(0, "Preset_2", "Garden");
    sleep_ms(200);
    ptz_info(0, &info);
    CHECK(!strcmp(info.presets[1].name, "Garden"), "renamed here");
    CHECK(ptz_preset_set(0, NULL, "New") == PTZ_OK, "a new one: the camera says its token");
    sleep_ms(200);
    CHECK(fcount("preset 1  New") == 1, "SetPreset without a token");
    ptz_shutdown();
    unlink(path);
    g_fake_presets = 0;
}

/* ---- the real NETIP backend against a camera in a thread ----------------------------------- */

static int g_srv_fd, g_srv_port;
static char g_srv_log[32][160];
static int g_srv_n, g_srv_ret = 100, g_srv_conns;
static pthread_mutex_t g_slock = PTHREAD_MUTEX_INITIALIZER;

static int rd(int fd, uint8_t *b, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = recv(fd, b + got, n - got, 0);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

static void reply(int fd, int msgid, const char *json)
{
    uint8_t h[NETIP_HDR_LEN];
    size_t n = strlen(json) + 2;
    netip_hdr_t hd = { .version = 1, .session = 0x55, .msgid = (uint16_t)msgid, .len = (uint32_t)n };
    netip_hdr_put(h, &hd);
    char *b = malloc(NETIP_HDR_LEN + n);
    memcpy(b, h, NETIP_HDR_LEN);
    memcpy(b + NETIP_HDR_LEN, json, n - 2);
    b[NETIP_HDR_LEN + n - 2] = '\n';
    b[NETIP_HDR_LEN + n - 1] = 0;
    send(fd, b, NETIP_HDR_LEN + n, MSG_NOSIGNAL);
    free(b);
}

static void *srv_conn(void *arg)
{
    int fd = (int)(intptr_t)arg;
    for (;;) {
        uint8_t hb[NETIP_HDR_LEN];
        netip_hdr_t h;
        if (rd(fd, hb, sizeof hb) < 0 || netip_hdr_get(hb, &h) < 0 || h.len > 8192) break;
        char *b = calloc(1, h.len + 1);
        if (rd(fd, (uint8_t *)b, h.len) < 0) {
            free(b);
            break;
        }
        cJSON *j = cJSON_Parse(b);
        free(b);
        if (h.msgid == NETIP_LOGIN) {
            const char *pw = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "PassWord"));
            reply(fd, 1001, pw && !strcmp(pw, "tlJwpbo6") ? "{\"Ret\":100,\"SessionID\":\"0x00000055\",\"AliveInterval\":20}"
                                                            : "{\"Ret\":203}");
        } else if (h.msgid == 1400) {
            const cJSON *op = cJSON_GetObjectItemCaseSensitive(j, "OPPTZControl");
            const cJSON *p = cJSON_GetObjectItemCaseSensitive(op, "Parameter");
            pthread_mutex_lock(&g_slock);
            if (g_srv_n < 32)
                snprintf(g_srv_log[g_srv_n++], sizeof g_srv_log[0], "%s ch%d preset %d step %d tour %d sess %s",
                         cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(op, "Command")),
                         cJSON_GetObjectItemCaseSensitive(p, "Channel")->valueint,
                         cJSON_GetObjectItemCaseSensitive(p, "Preset")->valueint,
                         cJSON_GetObjectItemCaseSensitive(p, "Step")->valueint,
                         cJSON_GetObjectItemCaseSensitive(p, "Tour")->valueint,
                         cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "SessionID")));
            int ret = g_srv_ret;
            pthread_mutex_unlock(&g_slock);
            char r[64];
            snprintf(r, sizeof r, "{\"Ret\":%d,\"SessionID\":\"0x00000055\"}", ret);
            reply(fd, 1401, r);
        } else {
            reply(fd, h.msgid + 1, "{\"Ret\":100}");
        }
        cJSON_Delete(j);
    }
    close(fd);
    return NULL;
}

static void *srv_main(void *arg)
{
    (void)arg;
    for (;;) {
        int c = accept(g_srv_fd, NULL, NULL);
        if (c < 0) break;
        pthread_mutex_lock(&g_slock);
        ++g_srv_conns;
        pthread_mutex_unlock(&g_slock);
        pthread_t t;
        pthread_create(&t, NULL, srv_conn, (void *)(intptr_t)c);
        pthread_detach(t);
    }
    return NULL;
}

static int srv_has(const char *s)
{
    int n = 0;
    pthread_mutex_lock(&g_slock);
    for (int i = 0; i < g_srv_n; ++i) n += strstr(g_srv_log[i], s) != NULL;
    pthread_mutex_unlock(&g_slock);
    return n;
}

static int srv_count(void)
{
    pthread_mutex_lock(&g_slock);
    int n = g_srv_n;
    pthread_mutex_unlock(&g_slock);
    return n;
}

static int g_want;
static int srv_reached(void) { return srv_count() >= g_want; }

static void test_netip_wire(void)
{
    g_srv_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t al = sizeof a;
    CHECK(bind(g_srv_fd, (struct sockaddr *)&a, sizeof a) == 0 && listen(g_srv_fd, 4) == 0, "listen");
    getsockname(g_srv_fd, (struct sockaddr *)&a, &al);
    g_srv_port = ntohs(a.sin_port);
    pthread_t th;
    pthread_create(&th, NULL, srv_main, NULL);

    ptz_set_drivers(NULL, NULL);
    ptz_start(NULL);
    char url[128];
    snprintf(url, sizeof url, "netip://admin:@127.0.0.1:%d/?ch=3&stream=main", g_srv_port);
    cams(url, "auto");
    ptz_info_t info;
    ptz_info(0, &info);
    CHECK(wait_for(ready_ch0, 3000), "the NETIP camera is ready");
    CHECK(srv_has("DirectionUp ch3 preset -1 step 1") == 1, "the probe: a stop of nothing");
    CHECK(srv_has("sess 0x00000055") == srv_count(), "with the login's session");
    ptz_info(0, &info);
    CHECK(info.caps & PTZ_CAP_PAN_TILT && info.caps & PTZ_CAP_PRESETS && !(info.caps & PTZ_CAP_HOME), "NETIP caps");

    ptz_move(0, -0.7, 0.7, 0, 300);
    g_want = 3;
    CHECK(wait_for(srv_reached, 2000), "start and stop of the move");
    CHECK(srv_has("DirectionLeftUp ch3 preset 65535 step 6") == 1, "DirectionLeftUp starts at Step 6");
    CHECK(srv_has("DirectionLeftUp ch3 preset -1 step 6") == 1, "... and stops with Preset -1");

    ptz_move(0, 1, 0, 0, 2000);
    g_want = 4;
    wait_for(srv_reached, 2000);
    ptz_move(0, 0, 0, 1, 300);              /* another command: the pan stops first */
    g_want = 7;
    CHECK(wait_for(srv_reached, 2000), "a change of command");
    CHECK(srv_has("DirectionRight ch3 preset -1") == 1 && srv_has("ZoomTile ch3 preset 65535 step 8") == 1 &&
              srv_has("ZoomTile ch3 preset -1") == 1,
          "the pan stopped, the zoom started and stopped");

    ptz_preset_set(0, "4", NULL);
    ptz_preset_goto(0, "4");
    ptz_preset_remove(0, "4");
    ptz_tour(0, 1, 1);
    ptz_tour(0, 1, 0);
    g_want = 12;
    CHECK(wait_for(srv_reached, 3000), "presets and tours");
    CHECK(srv_has("SetPreset ch3 preset 4") && srv_has("GotoPreset ch3 preset 4") && srv_has("ClearPreset ch3 preset 4"),
          "SetPreset, GotoPreset, ClearPreset 4");
    CHECK(srv_has("StartTour ch3 preset 65535 step 5 tour 1") && srv_has("StopTour ch3 preset 65535 step 5 tour 1"),
          "StartTour, StopTour 1");
    pthread_mutex_lock(&g_slock);
    int conns = g_srv_conns;
    g_srv_ret = 607;
    pthread_mutex_unlock(&g_slock);
    CHECK(conns == 1, "one control connection throughout (%d)", conns);
    ptz_shutdown();

    /* A camera without PTZ says so. */
    ptz_start(NULL);
    cams(url, "auto");
    ptz_info(0, &info);
    for (int t = 0; t < 3000; t += 10) {
        ptz_info(0, &info);
        if (info.state != PTZ_PROBING) break;
        sleep_ms(10);
    }
    CHECK(info.state == PTZ_NONE && strstr(info.error, "607"), "Ret 607: no PTZ ('%s')", info.error);
    CHECK(ptz_move(0, 1, 0, 0, 0) == PTZ_ENOPTZ, "and a move is refused");
    ptz_shutdown();

    /* A wrong password. */
    pthread_mutex_lock(&g_slock);
    g_srv_ret = 100;
    pthread_mutex_unlock(&g_slock);
    snprintf(url, sizeof url, "netip://admin:bad@127.0.0.1:%d/", g_srv_port);
    ptz_start(NULL);
    cams(url, "auto");
    for (int t = 0; t < 3000; t += 10) {
        ptz_info(0, &info);
        if (info.state != PTZ_PROBING && info.state != PTZ_OFF) break;
        sleep_ms(10);
    }
    CHECK(info.state == PTZ_ERROR && strstr(info.error, "203"), "a refused login: '%s'", info.error);
    ptz_shutdown();
    shutdown(g_srv_fd, SHUT_RDWR);
    close(g_srv_fd);
    pthread_join(th, NULL);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    test_netip_commands();
    test_backend_choice();
    ptz_start(NULL);
    test_safety();
    test_presets(dir);
    test_netip_wire();
    (void)flast;
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
