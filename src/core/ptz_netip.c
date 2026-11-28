/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ptz_netip.c — PTZ over Xiongmai NETIP (DVRIP): OPPTZControl, message
 * 1400 (answered 1401), on a connection of its own logged in with the
 * camera URL's user and password (the stream's session is the channel
 * thread's, and a control connection that hangs never holds up the
 * picture).
 *
 *   {"Name":"OPPTZControl","SessionID":"0x...","OPPTZControl":{
 *     "Command":"DirectionUp","Parameter":{"AUX":{"Number":0,"Status":"On"},
 *     "Channel":0,"MenuOpts":"Enter","POINT":{"bottom":0,"left":0,"right":0,
 *     "top":0},"Pattern":"SetBegin","Preset":65535,"Step":5,"Tour":0}}}
 *
 * Commands: DirectionUp/Down/Left/Right, DirectionLeftUp/LeftDown/
 * RightUp/RightDown, ZoomTile (in) / ZoomWide (out), FocusNear/FocusFar,
 * IrisLarge/IrisSmall, GotoPreset/SetPreset/ClearPreset (Preset: its
 * number), StartTour/StopTour (Tour: its number). A move starts with
 * Preset 65535 and runs until the same command comes with Preset -1;
 * Step 1..8 is its speed (stock XM firmware turns it into the Pelco speed
 * byte round(Step * 63 / 8); its zoom has one speed). Every request is
 * answered Ret 100; 118 ("PTZ protocol not set") and majestic's 607 say
 * the camera has no PTZ. Sources: OpenIPC python-dvr (dvrip.py: ptz(),
 * ptz_step(): "Preset 65535 starts, -1 stops"), Xiongmai's "DVR interface
 * protocol" V1.0.0 (PTZ_REQ 1400, PTZ_RSP 1401), and stock XM and
 * majestic firmware as measured: 0 and 65535 start, -1 stops; majestic
 * also ends a move when the client hangs up.
 *
 * XM cameras keep no preset names and list none: vigild keeps the list
 * (ptz.c). There is no home position (a camera's preset can be one).
 */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>

#include "cJSON.h"
#include "md5.h"
#include "net.h"
#include "netip_proto.h"
#include "ptz.h"
#include "source.h"

#define NETIP_PORT     34567
#define MSG_PTZ        1400
#define CALL_SECS      3
#define IDLE_CLOSE_MS  60000        /* the connection goes after a minute without a command */
#define PRESET_START   65535
#define PRESET_STOP    (-1)
#define DEAD_ZONE      0.15

typedef struct {
    char user[64], pass[64], host[128];
    int  port, channel;
    const volatile int *quit;
    int  fd;
    uint32_t session, seq;
    int  alive;                     /* keepalive interval, s */
    time_t last_sent;
} netip_ptz_t;

static void wipe(void *p, size_t n)
{
    volatile unsigned char *q = p;
    while (n--) *q++ = 0;
}

/* ---- the encoding (pure, tested) --------------------------------------------------------- */

static int step_of(double mag)
{
    int s = (int)lround(fabs(mag) * 8.0);
    return s < 1 ? 1 : s > 8 ? 8 : s;
}

const char *ptz_netip_command(const double v[PTZ_AXES], int *step)
{
    double p = v[PTZ_AX_PAN], t = v[PTZ_AX_TILT];
    *step = 1;
    if (fabs(p) >= DEAD_ZONE || fabs(t) >= DEAD_ZONE) {
        *step = step_of(fmax(fabs(p), fabs(t)));
        /* Eight directions: a diagonal within 22.5 degrees either side. */
        double a = atan2(t, p) * 180.0 / M_PI;     /* 0 right, 90 up */
        static const char *const dir[8] = { "DirectionRight", "DirectionRightUp", "DirectionUp", "DirectionLeftUp",
                                            "DirectionLeft", "DirectionLeftDown", "DirectionDown",
                                            "DirectionRightDown" };
        int k = (int)lround(a / 45.0);
        return dir[(k % 8 + 8) % 8];
    }
    if (fabs(v[PTZ_AX_ZOOM]) >= DEAD_ZONE) {
        *step = step_of(v[PTZ_AX_ZOOM]);
        return v[PTZ_AX_ZOOM] > 0 ? "ZoomTile" : "ZoomWide";
    }
    if (fabs(v[PTZ_AX_FOCUS]) >= DEAD_ZONE) {
        *step = step_of(v[PTZ_AX_FOCUS]);
        return v[PTZ_AX_FOCUS] > 0 ? "FocusFar" : "FocusNear";
    }
    if (fabs(v[PTZ_AX_IRIS]) >= DEAD_ZONE) {
        *step = step_of(v[PTZ_AX_IRIS]);
        return v[PTZ_AX_IRIS] > 0 ? "IrisLarge" : "IrisSmall";
    }
    return NULL;
}

int ptz_netip_body(char *out, size_t cap, unsigned session, const char *cmd, int ch, int preset, int step,
                   int tour)
{
    int n = snprintf(out, cap,
                     "{\"Name\":\"OPPTZControl\",\"SessionID\":\"0x%08X\",\"OPPTZControl\":{\"Command\":\"%s\","
                     "\"Parameter\":{\"AUX\":{\"Number\":0,\"Status\":\"On\"},\"Channel\":%d,\"MenuOpts\":\"Enter\","
                     "\"POINT\":{\"bottom\":0,\"left\":0,\"right\":0,\"top\":0},\"Pattern\":\"SetBegin\","
                     "\"Preset\":%d,\"Step\":%d,\"Tour\":%d}}}",
                     session, cmd, ch, preset, step, tour);
    return n > 0 && (size_t)n < cap ? n : -1;
}

/* ---- the connection ----------------------------------------------------------------------- */

static void hangup(void *v)
{
    netip_ptz_t *n = v;
    if (n->fd >= 0) close(n->fd);
    n->fd = -1;
}

static int send_msg(netip_ptz_t *n, int msgid, const char *json)
{
    size_t jl = strlen(json);
    uint8_t buf[NETIP_HDR_LEN + 1024];
    if (jl + NETIP_HDR_LEN + 2 > sizeof buf) return -1;
    netip_hdr_t h = { .version = 0, .session = n->session, .seq = n->seq++, .msgid = (uint16_t)msgid,
                      .len = (uint32_t)(jl + 2) };
    netip_hdr_put(buf, &h);
    memcpy(buf + NETIP_HDR_LEN, json, jl);
    buf[NETIP_HDR_LEN + jl] = '\n';
    buf[NETIP_HDR_LEN + jl + 1] = 0;
    size_t total = NETIP_HDR_LEN + jl + 2;
    ssize_t w = send(n->fd, buf, total, MSG_NOSIGNAL);
    wipe(buf, sizeof buf);
    if (w != (ssize_t)total) return -1;
    n->last_sent = time(NULL);
    return 0;
}

static int recv_all(netip_ptz_t *n, uint8_t *b, size_t len, time_t deadline)
{
    size_t got = 0;
    while (got < len) {
        if ((n->quit && *n->quit) || time(NULL) >= deadline) return -2;
        ssize_t r = recv(n->fd, b + got, len - got, 0);
        if (r > 0) {
            got += (size_t)r;
            continue;
        }
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
        return -1;
    }
    return 0;
}

/* Send and wait for msgid + 1 (CALL_SECS), whatever comes before it. The
 * reply parsed, or NULL with `err`. */
static cJSON *call(netip_ptz_t *n, int msgid, const char *json, char *err, size_t cap)
{
    if (n->fd < 0 || send_msg(n, msgid, json) < 0) {
        snprintf(err, cap, "cannot send to the camera");
        hangup(n);
        return NULL;
    }
    time_t deadline = time(NULL) + CALL_SECS;
    for (;;) {
        uint8_t hb[NETIP_HDR_LEN];
        netip_hdr_t h;
        int rc = recv_all(n, hb, sizeof hb, deadline);
        if (rc == 0 && netip_hdr_get(hb, &h) < 0) rc = -1;
        if (rc == 0 && h.len > 64 * 1024) rc = -1;
        char *b = rc == 0 ? malloc(h.len + 1) : NULL;
        if (rc == 0 && !b) rc = -1;
        if (rc == 0) rc = recv_all(n, (uint8_t *)b, h.len, deadline);
        if (rc < 0) {
            free(b);
            snprintf(err, cap, rc == -2 ? "no answer from the camera within %d s" : "the camera closed the connection",
                     CALL_SECS);
            hangup(n);
            return NULL;
        }
        b[h.len] = 0;
        if (h.msgid == msgid + 1) {
            if (h.session) n->session = h.session;
            cJSON *doc = cJSON_Parse(b);
            free(b);
            if (!doc) snprintf(err, cap, "the camera's answer is not JSON");
            return doc;
        }
        free(b);
    }
}

static int ret_of(const cJSON *doc)
{
    const cJSON *r = cJSON_GetObjectItemCaseSensitive(doc, "Ret");
    return cJSON_IsNumber(r) ? r->valueint : -1;
}

static int login(netip_ptz_t *n, char *err, size_t cap)
{
    if (n->fd >= 0) return 0;
    n->session = n->seq = 0;
    n->fd = net_connect(n->host, n->port, 3000, n->quit, err, cap);
    if (n->fd < 0) return -1;
    char hash[9], req[256];
    md5_sofia(n->pass, hash);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "EncryptType", "MD5");
    cJSON_AddStringToObject(o, "LoginType", "DVRIP-Web");
    cJSON_AddStringToObject(o, "PassWord", hash);
    cJSON_AddStringToObject(o, "UserName", n->user);
    int ok = o && cJSON_PrintPreallocated(o, req, sizeof req, 0);
    cJSON_Delete(o);
    wipe(hash, sizeof hash);
    if (!ok) {
        snprintf(err, cap, "the user name is too long");
        hangup(n);
        return -1;
    }
    cJSON *r = call(n, NETIP_LOGIN, req, err, cap);
    wipe(req, sizeof req);
    if (!r) return -1;
    int ret = ret_of(r);
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(r, "AliveInterval");
    const cJSON *sid = cJSON_GetObjectItemCaseSensitive(r, "SessionID");
    uint32_t sv;
    if (cJSON_IsString(sid) && netip_session_parse(sid->valuestring, &sv) == 0) n->session = sv;
    n->alive = cJSON_IsNumber(a) && a->valueint > 0 ? a->valueint : 20;
    cJSON_Delete(r);
    if (ret != 100 && ret != 515) {
        snprintf(err, cap, "login refused (Ret %d)%s", ret, ret == 203 || ret == 205 || ret == 206 ? ": user/password" : "");
        hangup(n);
        return -1;
    }
    return 0;
}

/* One OPPTZControl, logged in first; tried once more on a new connection
 * when the old one has gone. The camera's Ret, or -1 with `err`. */
static int ptz_cmd(netip_ptz_t *n, const char *cmd, int preset, int step, int tour, char *err, size_t cap)
{
    for (int attempt = 0; attempt < 2; ++attempt) {
        int fresh = n->fd < 0;
        if (login(n, err, cap) < 0) return -1;
        char body[512];
        if (ptz_netip_body(body, sizeof body, n->session, cmd, n->channel, preset, step, tour) < 0) {
            snprintf(err, cap, "request too long");
            return -1;
        }
        cJSON *r = call(n, MSG_PTZ, body, err, cap);
        if (!r) {
            if (fresh) return -1;
            continue;                           /* a connection that had gone stale */
        }
        int ret = ret_of(r);
        cJSON_Delete(r);
        return ret;
    }
    return -1;
}

static int ptz_ok(netip_ptz_t *n, const char *cmd, int preset, int step, int tour, char *err, size_t cap)
{
    int ret = ptz_cmd(n, cmd, preset, step, tour, err, cap);
    if (ret < 0) return -1;
    if (ret == 100) return 0;
    if (ret == 118 || ret == 607) {
        snprintf(err, cap, "the camera has no PTZ (Ret %d)", ret);
        return PTZ_ENOTSUP;
    }
    snprintf(err, cap, "%s refused (Ret %d)", cmd, ret);
    return -1;
}

/* ---- the backend ------------------------------------------------------------------------- */

static void *create(const ptz_camera_t *cam, const volatile int *quit, char *err, size_t cap)
{
    netip_ptz_t *n = calloc(1, sizeof *n);
    if (!n) return NULL;
    const char *path = "";
    int is_netip = !strncmp(cam->url, "netip://", 8);
    n->port = is_netip ? NETIP_PORT : 0;
    if (url_split(cam->url, is_netip ? "netip" : "rtsp", n->user, sizeof n->user, n->pass, sizeof n->pass,
                  n->host, sizeof n->host, &n->port, &path) < 0) {
        snprintf(err, cap, "not a camera URL NETIP can use");
        free(n);
        return NULL;
    }
    /* ptz: netip on an RTSP camera: the same host, NETIP's port. */
    if (!is_netip) n->port = NETIP_PORT;
    if (!n->user[0]) snprintf(n->user, sizeof n->user, "admin");
    char v[16];
    n->channel = is_netip && url_query(path, "ch", v, sizeof v) ? atoi(v) : 0;
    n->fd = -1;
    n->quit = quit;
    return n;
}

static void destroy(void *v)
{
    netip_ptz_t *n = v;
    hangup(n);
    wipe(n, sizeof *n);
    free(n);
}

static int probe(void *v, unsigned *caps, int *tours, ptz_preset_t *presets, int *n_presets, char *err, size_t cap)
{
    netip_ptz_t *n = v;
    (void)presets;
    *n_presets = -1;
    /* A stop of nothing: harmless, and a camera without PTZ says so. */
    int rc = ptz_ok(n, "DirectionUp", PRESET_STOP, 1, 0, err, cap);
    if (rc == PTZ_ENOTSUP) return 1;
    if (rc < 0) return -1;
    *caps = PTZ_CAP_PAN_TILT | PTZ_CAP_ZOOM | PTZ_CAP_FOCUS | PTZ_CAP_IRIS | PTZ_CAP_PRESETS | PTZ_CAP_TOURS;
    *tours = PTZ_TOURS_MAX;
    return 0;
}

static int motion(void *v, const double from[PTZ_AXES], const double to[PTZ_AXES], char *err, size_t cap)
{
    netip_ptz_t *n = v;
    int fs, ts;
    const char *fc = ptz_netip_command(from, &fs), *tc = ptz_netip_command(to, &ts);
    /* Another command (or none): the one in force stops first; the same
     * at another speed is only sent again. */
    if (fc && (!tc || strcmp(fc, tc))) {
        int rc = ptz_ok(n, fc, PRESET_STOP, fs, 0, err, cap);
        if (rc && !tc) return rc;
    }
    if (tc) return ptz_ok(n, tc, PRESET_START, ts, 0, err, cap);
    return 0;
}

static int preset(void *v, int op, const char *token, const char *name, char *out, size_t ocap, char *err,
                  size_t cap)
{
    netip_ptz_t *n = v;
    (void)name;
    char *end;
    long num = strtol(token, &end, 10);
    if (!token[0] || *end || num < 1 || num > 255) {
        snprintf(err, cap, "a NETIP preset is a number 1..255");
        return -1;
    }
    const char *cmd = op == PTZ_PRESET_GOTO ? "GotoPreset" : op == PTZ_PRESET_SET ? "SetPreset" : "ClearPreset";
    snprintf(out, ocap, "%ld", num);
    return ptz_ok(n, cmd, (int)num, 5, 0, err, cap);
}

static int tour(void *v, int t, int start, char *err, size_t cap)
{
    return ptz_ok(v, start ? "StartTour" : "StopTour", PRESET_START, 5, t, err, cap);
}

static void idle(void *v, long long idle_ms)
{
    netip_ptz_t *n = v;
    if (n->fd < 0) return;
    if (idle_ms >= IDLE_CLOSE_MS) {
        hangup(n);
        return;
    }
    if (time(NULL) - n->last_sent >= n->alive) {
        char req[96], err[64];
        snprintf(req, sizeof req, "{\"Name\":\"KeepAlive\",\"SessionID\":\"0x%08X\"}", n->session);
        cJSON *r = call(n, NETIP_KEEPALIVE, req, err, sizeof err);
        cJSON_Delete(r);
    }
}

const ptz_drv_t ptz_drv_netip = {
    .name = "netip",
    .create = create,
    .destroy = destroy,
    .probe = probe,
    .motion = motion,
    .preset = preset,
    .tour = tour,
    .idle = idle,
    .hangup = hangup,
};
