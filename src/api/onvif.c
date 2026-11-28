/* SPDX-License-Identifier: GPL-3.0-or-later */
/* onvif.c — vigild as an ONVIF device (onvif.h, docs/onvif.md).
 *
 * Profile S, the part a client needs to find the recorder and play its
 * cameras: the device service (identity, time, capabilities, services,
 * network, users read-only, reboot), media ver10 and media2 ver20 (a
 * profile per camera and stream, ch<N>_main / ch<N>_sub, and their RTSP
 * addresses on the recorder's own RTSP server), and WS-Discovery.
 *
 * Its own CivetWeb server at onvif_server.port (8899 by default, as on
 * Xiongmai devices): port 80 belongs to the zero channel (web.c), which
 * only runs on boards with a screen and has its own logins and headers.
 * Discovery tells clients the port, so they never have to guess it.
 *
 * SOAP without a SOAP library: onvif_proto.h finds the operation and the
 * UsernameToken, the answers are templates. Logins are vigil's users
 * (users.h): a WS-UsernameToken digest or an HTTP Digest is checked
 * against the user's sealed password (users.h), PasswordText and Basic against the
 * crypt(3) hash; failures count with the API's (api_login).
 *
 * One thread (supervisor) follows the configuration: it starts, moves
 * and stops the web server, answers WS-Discovery probes and sends Hello
 * and Bye. The web server's workers only read snapshots (config_get,
 * channels_status), like the API's.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netpacket/packet.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>

#include "civetweb.h"

#include "api_int.h"
#include "auth.h"
#include "channel.h"
#include "config.h"
#include "discovery.h"
#include "hal.h"
#include "log.h"
#include "md5.h"
#include "net.h"
#include "mgmt.h"
#include "onvif.h"
#include "onvif_proto.h"
#include "sys_net.h"
#include "users.h"

#ifndef VIGIL_VERSION
#define VIGIL_VERSION "dev"
#endif

#define THREADS      "4"
#define BODY_MAX     (64u << 10)
#define REALM        "vigil"
#define NONCE_SECS   300            /* an HTTP Digest nonce is good this long */
#define WSSE_SKEW    900            /* a UsernameToken's Created may be this far off, s */
#define SEEN_N       64             /* recent UsernameToken nonces, against replays */
#define IFACE_MAX    8
#define PROBE_RATE   20             /* probe answers a second, at most */

/* ---- state ------------------------------------------------------------------------- */

static pthread_t         g_thread;
static int               g_thread_up;
static atomic_int        g_stop;
static struct mg_context *g_ctx;            /* the supervisor's */
static atomic_int        g_port;            /* where g_ctx listens; 0 none */
static unsigned char     g_key[16];         /* HTTP Digest nonces */

static pthread_mutex_t   g_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned char     g_seen[SEEN_N][16];    /* md5 of UsernameToken nonces */
static int               g_seen_at;
static onvif_nc_t        g_nc;                  /* HTTP Digest nonce counts used */
static atomic_uint       g_nonce_serial;

typedef struct {
    char mac[18];
    char serial[16];
    char uuid[48];                  /* urn:uuid:... */
    char chip[24];
    char model[48];
    char firmware[96];
    char hostname[64];
    char scopes[768];
} ident_t;

static ident_t g_id;                    /* set before the server starts */

static long mono(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec;
}

static void random_bytes(unsigned char *b, size_t n)
{
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    ssize_t r = fd >= 0 ? read(fd, b, n) : -1;
    if (fd >= 0) close(fd);
    if (r != (ssize_t)n) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        unsigned v = (unsigned)ts.tv_nsec ^ (unsigned)getpid() << 16;
        for (size_t i = 0; i < n; ++i) {
            v = v * 1103515245u + 12345u;
            b[i] = (unsigned char)(v >> 16);
        }
    }
}

static void new_uuid(char out[46])
{
    unsigned char r[16];
    random_bytes(r, sizeof r);
    r[6] = (unsigned char)((r[6] & 0x0f) | 0x40);
    r[8] = (unsigned char)((r[8] & 0x3f) | 0x80);
    snprintf(out, 46, "urn:uuid:%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", r[0], r[1],
             r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15]);
}

/* ---- who we are -------------------------------------------------------------------- */

/* A value of /etc/os-release, unquoted. */
static void os_release(const char *key, char *out, size_t cap)
{
    out[0] = 0;
    FILE *f = fopen("/etc/os-release", "r");
    if (!f) return;
    char line[256];
    size_t kl = strlen(key);
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, key, kl) != 0 || line[kl] != '=') continue;
        char *v = line + kl + 1;
        v[strcspn(v, "\r\n")] = 0;
        size_t n = strlen(v);
        if (n >= 2 && v[0] == '"' && v[n - 1] == '"') {
            v[n - 1] = 0;
            ++v;
        }
        snprintf(out, cap, "%s", v);
        break;
    }
    fclose(f);
}

/* The MAC of interface `name`, else (NULL) of eth0, else of the first
 * interface that has one. */
static void if_mac(const char *name, char out[18])
{
    out[0] = 0;
    struct ifaddrs *ifa;
    if (getifaddrs(&ifa) < 0) return;
    for (int pass = 0; pass < 2 && !out[0]; ++pass)
        for (struct ifaddrs *i = ifa; i; i = i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_PACKET || (i->ifa_flags & IFF_LOOPBACK)) continue;
            if (name ? strcmp(i->ifa_name, name) != 0 : pass == 0 && strcmp(i->ifa_name, "eth0") != 0) continue;
            const struct sockaddr_ll *l = (const struct sockaddr_ll *)i->ifa_addr;
            if (l->sll_halen != 6 || !(l->sll_addr[0] | l->sll_addr[1] | l->sll_addr[2] | l->sll_addr[3] |
                                       l->sll_addr[4] | l->sll_addr[5]))
                continue;
            snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", l->sll_addr[0], l->sll_addr[1], l->sll_addr[2],
                     l->sll_addr[3], l->sll_addr[4], l->sll_addr[5]);
            break;
        }
    freeifaddrs(ifa);
}

static void ident_init(ident_t *id)
{
    memset(id, 0, sizeof *id);
    if_mac(NULL, id->mac);
    for (const char *p = id->mac; *p; ++p)
        if (*p != ':' && strlen(id->serial) < sizeof id->serial - 1) {
            size_t n = strlen(id->serial);
            id->serial[n] = (char)(*p >= 'a' && *p <= 'f' ? *p - 32 : *p);
            id->serial[n + 1] = 0;
        }
    if (gethostname(id->hostname, sizeof id->hostname) < 0 || !id->hostname[0])
        snprintf(id->hostname, sizeof id->hostname, "vigil");
    id->hostname[sizeof id->hostname - 1] = 0;
    /* The endpoint reference stays across restarts: from the MAC (else the
     * host name), as a name-based UUID. */
    unsigned char d[16];
    char seed[96];
    snprintf(seed, sizeof seed, "vigil-onvif:%s", id->mac[0] ? id->mac : id->hostname);
    md5_raw(seed, strlen(seed), d);
    d[6] = (unsigned char)((d[6] & 0x0f) | 0x30);
    d[8] = (unsigned char)((d[8] & 0x3f) | 0x80);
    snprintf(id->uuid, sizeof id->uuid,
             "urn:uuid:%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", d[0], d[1], d[2],
             d[3], d[4], d[5], d[6], d[7], d[8], d[9], d[10], d[11], d[12], d[13], d[14], d[15]);
    /* The chip: OpenIPC's BUILD_PLATFORM ("ssr621q_lite"), else the HAL's
     * family name. */
    char plat[48], ver[48];
    os_release("BUILD_PLATFORM", plat, sizeof plat);
    plat[strcspn(plat, "_")] = 0;
    snprintf(id->chip, sizeof id->chip, "%.23s", plat[0] ? plat : hal_caps()->name);
    char up[24];
    size_t i = 0;
    for (; id->chip[i] && i < sizeof up - 1; ++i)
        up[i] = (char)(id->chip[i] >= 'a' && id->chip[i] <= 'z' ? id->chip[i] - 32 : id->chip[i]);
    up[i] = 0;
    snprintf(id->model, sizeof id->model, "%s NVR", up);
    os_release("OPENIPC_VERSION", ver, sizeof ver);
    if (ver[0]) snprintf(id->firmware, sizeof id->firmware, "vigild %s, OpenIPC %s", VIGIL_VERSION, ver);
    else snprintf(id->firmware, sizeof id->firmware, "vigild %s", VIGIL_VERSION);
    /* name and hardware as ONVIF Core 7.3.2.2 has them; manufacturer and
     * MAC as many devices add (and disc_proto.c reads). */
    char name[192], hw[96];
    onvif_scope_encode(id->hostname, name, sizeof name);
    onvif_scope_encode(id->model, hw, sizeof hw);
    snprintf(id->scopes, sizeof id->scopes,
             "onvif://www.onvif.org/type/NetworkVideoTransmitter onvif://www.onvif.org/type/video_encoder "
             "onvif://www.onvif.org/Profile/Streaming onvif://www.onvif.org/name/%s "
             "onvif://www.onvif.org/hardware/%s onvif://www.onvif.org/manufacturer/OpenIPC%s%s",
             name, hw, id->mac[0] ? " onvif://www.onvif.org/MAC/" : "", id->mac);
}

/* ---- the answer -------------------------------------------------------------------- */

typedef struct {
    char  *p;
    size_t n, cap;
    int    oom;
} buf_t;

static void bput(buf_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void bput(buf_t *b, const char *fmt, ...)
{
    if (b->oom) return;
    for (;;) {
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(b->p ? b->p + b->n : NULL, b->p ? b->cap - b->n : 0, fmt, ap);
        va_end(ap);
        if (n < 0) {
            b->oom = 1;
            return;
        }
        if (b->p && b->n + (size_t)n < b->cap) {
            b->n += (size_t)n;
            return;
        }
        size_t cap = (b->cap ? b->cap : 2048) * 2;
        while (cap < b->n + (size_t)n + 1) cap *= 2;
        char *p = realloc(b->p, cap);
        if (!p) {
            b->oom = 1;
            return;
        }
        b->p = p;
        b->cap = cap;
    }
}

/* `s` as XML text. */
static void besc(buf_t *b, const char *s)
{
    char e[512];
    if (onvif_xml_escape(s, e, sizeof e) == 0) bput(b, "%s", e);
    else bput(b, "?");
}

typedef struct {
    struct mg_connection *conn;
    const char           *xml;
    size_t                len;
    onvif_soap_t          soap;
    vigil_config_t       *cfg;
    char                  host[80];     /* as the client reached us: "10.0.0.5:8899" */
    char                  hostname[64]; /* the same without the port */
    char                  user[USERS_NAME_MAX];
    /* A fault instead of an answer: its HTTP status, codes and reason. */
    int                   f_status;
    const char           *f_code, *f_sub, *f_sub2, *f_text;
} req_t;

static int fault(req_t *q, int status, const char *code, const char *sub, const char *sub2, const char *text)
{
    q->f_status = status;
    q->f_code = code;
    q->f_sub = sub;
    q->f_sub2 = sub2;
    q->f_text = text;
    return -1;
}

static int bad_arg(req_t *q, const char *sub2, const char *text)
{
    return fault(q, 400, "s:Sender", "ter:InvalidArgVal", sub2, text);
}

/* The operation's argument `name` (its first element of that local name). */
static int arg(const req_t *q, const char *name, char *out, size_t cap)
{
    return disc_xml_text(q->xml + q->soap.arg0, q->soap.arg1 - q->soap.arg0, name, out, cap, NULL);
}

/* ---- cameras as profiles ------------------------------------------------------------ */

typedef struct {
    vg_codec_t codec;
    unsigned   w, h, fps;
} stream_t;

typedef struct {
    int      ch;                    /* 1-based */
    int      has_sub;
    char     name[40];
    stream_t s[2];                  /* main, sub */
} chan_t;

/* What each stream was last seen as: a stream not decoded or connected
 * just now still has its profile as it was. */
static stream_t g_seen_stream[CHANNEL_MAX][2];

/* Channel idx's stream: what the channel says now (channels_status
 * reports each stream once its parameter sets came), else what it said
 * last, else a guess a client can live with until the camera connects
 * (the stream's real values are in its RTSP description anyway). When
 * the RTSP server lands, its stream_tap_info is the better source. */
static stream_t stream_info(int idx, int sub, const channel_status_t *st)
{
    stream_t s = { st->streams[sub].codec, st->streams[sub].width, st->streams[sub].height, st->streams[sub].fps };
    pthread_mutex_lock(&g_lock);
    stream_t *last = &g_seen_stream[idx][sub];
    if (s.codec && s.w && s.h) {
        if (!s.fps) s.fps = last->fps;
        *last = s;
    } else if (last->codec) {
        s = *last;
    }
    pthread_mutex_unlock(&g_lock);
    if (!s.codec) s.codec = sub ? st->streams[0].codec : VG_CODEC_NONE;
    if (!s.codec) s.codec = VG_CODEC_H264;
    if (!s.w || !s.h) {
        s.w = sub ? 640 : 1920;
        s.h = sub ? 360 : 1080;
    }
    if (!s.fps) s.fps = 25;
    return s;
}

/* The cameras that are set up and on, in channel order. */
static int chans(const vigil_config_t *c, chan_t *out)
{
    int n = 0;
    for (int i = 0; i < c->n_cameras && i < CHANNEL_MAX; ++i) {
        const config_camera_t *cam = &c->cameras[i];
        if (!cam->url[0] || !cam->enabled) continue;
        channel_status_t st;
        channels_status(i, &st);
        chan_t *o = &out[n++];
        memset(o, 0, sizeof *o);
        o->ch = i + 1;
        o->has_sub = st.sub_status != CH_EMPTY || cam->sub_url[0];
        if (cam->name[0]) snprintf(o->name, sizeof o->name, "%s", cam->name);
        else if (st.name[0]) snprintf(o->name, sizeof o->name, "%s", st.name);
        else snprintf(o->name, sizeof o->name, "CAM%d", i + 1);
        o->s[0] = stream_info(i, 0, &st);
        if (o->has_sub) o->s[1] = stream_info(i, 1, &st);
    }
    return n;
}

/* "ch3_sub" -> the channel in `cs` and 1. */
static const chan_t *by_token(const chan_t *cs, int n, const char *tok, const char *prefix, int *sub)
{
    size_t pl = strlen(prefix);
    if (strncmp(tok, prefix, pl) != 0) return NULL;
    char *end;
    long ch = strtol(tok + pl, &end, 10);
    if (end == tok + pl) return NULL;
    if (!strcmp(end, "_main")) *sub = 0;
    else if (!strcmp(end, "_sub")) *sub = 1;
    else return NULL;
    for (int i = 0; i < n; ++i)
        if (cs[i].ch == ch && (!*sub || cs[i].has_sub)) return &cs[i];
    return NULL;
}

static const chan_t *by_source(const chan_t *cs, int n, const char *tok)
{
    char *end;
    if (strncmp(tok, "src", 3) != 0) return NULL;
    long ch = strtol(tok + 3, &end, 10);
    if (end == tok + 3 || *end) return NULL;
    for (int i = 0; i < n; ++i)
        if (cs[i].ch == ch) return &cs[i];
    return NULL;
}

static const char *sub_name(int sub) { return sub ? "sub" : "main"; }

static void put_vsc(buf_t *b, const chan_t *c, const char *el)
{
    bput(b, "<%s token=\"src%d\"><tt:Name>", el, c->ch);
    besc(b, c->name);
    bput(b, "</tt:Name><tt:UseCount>%d</tt:UseCount><tt:SourceToken>vs%d</tt:SourceToken>"
            "<tt:Bounds x=\"0\" y=\"0\" width=\"%u\" height=\"%u\"/></%s>",
         c->has_sub ? 2 : 1, c->ch, c->s[0].w, c->s[0].h, el);
}

/* Media ver10's encoder configuration. Its Encoding has no H.265 (only
 * JPEG, MPEG4, H264), and a strict client fails on a value outside the
 * schema: an H.265 stream says H264 here, as other devices do; media2
 * says H265, and the RTSP description has the truth. */
static void put_vec1(buf_t *b, const chan_t *c, int sub, const char *el)
{
    const stream_t *s = &c->s[sub];
    bput(b, "<%s token=\"enc%d_%s\"><tt:Name>", el, c->ch, sub_name(sub));
    besc(b, c->name);
    bput(b, " %s</tt:Name><tt:UseCount>1</tt:UseCount><tt:Encoding>H264</tt:Encoding>"
            "<tt:Resolution><tt:Width>%u</tt:Width><tt:Height>%u</tt:Height></tt:Resolution>"
            "<tt:Quality>5</tt:Quality><tt:RateControl><tt:FrameRateLimit>%u</tt:FrameRateLimit>"
            "<tt:EncodingInterval>1</tt:EncodingInterval><tt:BitrateLimit>0</tt:BitrateLimit></tt:RateControl>"
            "<tt:H264><tt:GovLength>%u</tt:GovLength><tt:H264Profile>Main</tt:H264Profile></tt:H264>"
            "<tt:Multicast><tt:Address><tt:Type>IPv4</tt:Type><tt:IPv4Address>0.0.0.0</tt:IPv4Address></tt:Address>"
            "<tt:Port>0</tt:Port><tt:TTL>0</tt:TTL><tt:AutoStart>false</tt:AutoStart></tt:Multicast>"
            "<tt:SessionTimeout>PT60S</tt:SessionTimeout></%s>",
         sub_name(sub), s->w, s->h, s->fps, s->fps * 2, el);
}

static void put_vec2(buf_t *b, const chan_t *c, int sub, const char *el)
{
    const stream_t *s = &c->s[sub];
    bput(b, "<%s token=\"enc%d_%s\"><tt:Name>", el, c->ch, sub_name(sub));
    besc(b, c->name);
    bput(b, " %s</tt:Name><tt:UseCount>1</tt:UseCount><tt:Encoding>%s</tt:Encoding>"
            "<tt:Resolution><tt:Width>%u</tt:Width><tt:Height>%u</tt:Height></tt:Resolution>"
            "<tt:RateControl ConstantBitRate=\"false\"><tt:FrameRateLimit>%u</tt:FrameRateLimit>"
            "<tt:BitrateLimit>0</tt:BitrateLimit></tt:RateControl><tt:Quality>5</tt:Quality></%s>",
         sub_name(sub), s->codec == VG_CODEC_H265 ? "H265" : "H264", s->w, s->h, s->fps, el);
}

static void put_profile1(buf_t *b, const chan_t *c, int sub, const char *el)
{
    bput(b, "<%s token=\"ch%d_%s\" fixed=\"true\"><tt:Name>", el, c->ch, sub_name(sub));
    besc(b, c->name);
    bput(b, " %s</tt:Name>", sub_name(sub));
    put_vsc(b, c, "tt:VideoSourceConfiguration");
    put_vec1(b, c, sub, "tt:VideoEncoderConfiguration");
    bput(b, "</%s>", el);
}

static void put_profile2(buf_t *b, const chan_t *c, int sub, int vs, int ve)
{
    bput(b, "<tr2:Profiles token=\"ch%d_%s\" fixed=\"true\"><tr2:Name>", c->ch, sub_name(sub));
    besc(b, c->name);
    bput(b, " %s</tr2:Name>", sub_name(sub));
    if (vs || ve) {
        bput(b, "<tr2:Configurations>");
        if (vs) put_vsc(b, c, "tr2:VideoSource");
        if (ve) put_vec2(b, c, sub, "tr2:VideoEncoder");
        bput(b, "</tr2:Configurations>");
    }
    bput(b, "</tr2:Profiles>");
}

/* No stream URI while the RTSP server is off: a URI nothing answers at
 * would only send the client into retries. */
static int rtsp_off(req_t *q)
{
    return fault(q, 500, "s:Receiver", "ter:ActionNotSupported", NULL,
                 "The RTSP server is off (rtsp_server.enabled)");
}

static void put_uri(buf_t *b, const req_t *q, int ch, int sub)
{
    bput(b, "rtsp://");
    besc(b, q->hostname);
    bput(b, ":%d/ch%d/%s", q->cfg->rtsp_server.port, ch, sub_name(sub));
}

/* ---- device ------------------------------------------------------------------------- */

static void put_dt(buf_t *b, const char *el, const struct tm *t)
{
    bput(b, "<tt:%s><tt:Time><tt:Hour>%d</tt:Hour><tt:Minute>%d</tt:Minute><tt:Second>%d</tt:Second></tt:Time>"
            "<tt:Date><tt:Year>%d</tt:Year><tt:Month>%d</tt:Month><tt:Day>%d</tt:Day></tt:Date></tt:%s>",
         el, t->tm_hour, t->tm_min, t->tm_sec, t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, el);
}

static int dev_time(req_t *q, buf_t *b)
{
    time_t now = time(NULL);
    struct tm u, l;
    gmtime_r(&now, &u);
    localtime_r(&now, &l);
    char tz[64] = "";
    FILE *f = fopen("/etc/TZ", "r");
    if (f) {
        if (fgets(tz, sizeof tz, f)) tz[strcspn(tz, "\r\n")] = 0;
        fclose(f);
    }
    if (!tz[0] && getenv("TZ")) snprintf(tz, sizeof tz, "%s", getenv("TZ"));
    if (!tz[0]) snprintf(tz, sizeof tz, "UTC0");
    bput(b, "<tds:GetSystemDateAndTimeResponse><tds:SystemDateAndTime><tt:DateTimeType>%s</tt:DateTimeType>"
            "<tt:DaylightSavings>%s</tt:DaylightSavings><tt:TimeZone><tt:TZ>",
         q->cfg->time.ntp ? "NTP" : "Manual", l.tm_isdst > 0 ? "true" : "false");
    besc(b, tz);
    bput(b, "</tt:TZ></tt:TimeZone>");
    put_dt(b, "UTCDateTime", &u);
    put_dt(b, "LocalDateTime", &l);
    bput(b, "</tds:SystemDateAndTime></tds:GetSystemDateAndTimeResponse>");
    return 0;
}

static int dev_info(req_t *q, buf_t *b)
{
    (void)q;
    bput(b, "<tds:GetDeviceInformationResponse><tds:Manufacturer>OpenIPC</tds:Manufacturer><tds:Model>");
    besc(b, g_id.model);
    bput(b, "</tds:Model><tds:FirmwareVersion>");
    besc(b, g_id.firmware);
    bput(b, "</tds:FirmwareVersion><tds:SerialNumber>%s</tds:SerialNumber><tds:HardwareId>",
         g_id.serial[0] ? g_id.serial : "0");
    besc(b, g_id.chip);
    bput(b, "</tds:HardwareId></tds:GetDeviceInformationResponse>");
    return 0;
}

static void put_xaddr(buf_t *b, const req_t *q, const char *svc)
{
    bput(b, "http://");
    besc(b, q->host);
    bput(b, "/onvif/%s", svc);
}

static void put_dev_caps(buf_t *b)
{
    bput(b, "<tds:Capabilities><tds:Network IPFilter=\"false\" ZeroConfiguration=\"false\" IPVersion6=\"false\" "
            "DynDNS=\"false\" Dot11Configuration=\"false\" HostnameFromDHCP=\"false\" NTP=\"0\"/>"
            "<tds:Security TLS1.0=\"false\" TLS1.1=\"false\" TLS1.2=\"false\" OnboardKeyGeneration=\"false\" "
            "AccessPolicyConfig=\"false\" DefaultAccessPolicy=\"false\" Dot1X=\"false\" RemoteUserHandling=\"false\" "
            "X.509Token=\"false\" SAMLToken=\"false\" KerberosToken=\"false\" UsernameToken=\"true\" "
            "HttpDigest=\"true\" RELToken=\"false\"/>"
            "<tds:System DiscoveryResolve=\"false\" DiscoveryBye=\"true\" RemoteDiscovery=\"false\" "
            "SystemBackup=\"false\" SystemLogging=\"false\" FirmwareUpgrade=\"false\" HttpFirmwareUpgrade=\"false\" "
            "HttpSystemBackup=\"false\" HttpSystemLogging=\"false\" HttpSupportInformation=\"false\"/>"
            "</tds:Capabilities>");
}

/* Snapshots are the API's (api_snap.c): there while the API listens. */
static int api_port(const req_t *q) { return config_listen_port(q->cfg->api.listen); }
static const char *snap_cap(const req_t *q) { return api_port(q) > 0 ? "true" : "false"; }

static void put_media_caps(buf_t *b, const req_t *q)
{
    bput(b, "<trt:Capabilities SnapshotUri=\"%s\" Rotation=\"false\" VideoSourceMode=\"false\" OSD=\"false\">"
            "<trt:ProfileCapabilities MaximumNumberOfProfiles=\"%d\"/>"
            "<trt:StreamingCapabilities RTPMulticast=\"false\" RTP_TCP=\"true\" RTP_RTSP_TCP=\"true\" "
            "NonAggregateControl=\"false\" NoRTSPStreaming=\"false\"/></trt:Capabilities>", snap_cap(q), 2 * CHANNEL_MAX);
}

static void put_media2_caps(buf_t *b, const req_t *q)
{
    bput(b, "<tr2:Capabilities SnapshotUri=\"%s\" Rotation=\"false\" VideoSourceMode=\"false\" OSD=\"false\" "
            "Mask=\"false\" SourceMask=\"false\"><tr2:ProfileCapabilities MaximumNumberOfProfiles=\"%d\" "
            "ConfigurationsSupported=\"VideoSource VideoEncoder\"/><tr2:StreamingCapabilities RTSPStreaming=\"true\" "
            "RTPMulticast=\"false\" RTP_RTSP_TCP=\"true\" NonAggregateControl=\"false\" AutoStartMulticast=\"false\"/>"
            "</tr2:Capabilities>", snap_cap(q), 2 * CHANNEL_MAX);
}

static int dev_caps(req_t *q, buf_t *b)
{
    /* Category is a list; none (or All) is everything we have. */
    char cat[128] = "";
    arg(q, "Category", cat, sizeof cat);
    int all = !cat[0] || strstr(cat, "All"), dev = all || strstr(cat, "Device"), media = all || strstr(cat, "Media");
    if (!dev && !media) return fault(q, 500, "s:Receiver", "ter:ActionNotSupported", "ter:NoSuchService",
                                     "This device has only the device and media services");
    bput(b, "<tds:GetCapabilitiesResponse><tds:Capabilities>");
    if (dev) {
        bput(b, "<tt:Device><tt:XAddr>");
        put_xaddr(b, q, "device_service");
        bput(b, "</tt:XAddr><tt:Network><tt:IPFilter>false</tt:IPFilter><tt:ZeroConfiguration>false</tt:ZeroConfiguration>"
                "<tt:IPVersion6>false</tt:IPVersion6><tt:DynDNS>false</tt:DynDNS></tt:Network>"
                "<tt:System><tt:DiscoveryResolve>false</tt:DiscoveryResolve><tt:DiscoveryBye>true</tt:DiscoveryBye>"
                "<tt:RemoteDiscovery>false</tt:RemoteDiscovery><tt:SystemBackup>false</tt:SystemBackup>"
                "<tt:SystemLogging>false</tt:SystemLogging><tt:FirmwareUpgrade>false</tt:FirmwareUpgrade>"
                "<tt:SupportedVersions><tt:Major>2</tt:Major><tt:Minor>60</tt:Minor></tt:SupportedVersions>"
                "<tt:SupportedVersions><tt:Major>20</tt:Major><tt:Minor>12</tt:Minor></tt:SupportedVersions></tt:System>"
                "<tt:Security><tt:TLS1.1>false</tt:TLS1.1><tt:TLS1.2>false</tt:TLS1.2>"
                "<tt:OnboardKeyGeneration>false</tt:OnboardKeyGeneration><tt:AccessPolicyConfig>false</tt:AccessPolicyConfig>"
                "<tt:X.509Token>false</tt:X.509Token><tt:SAMLToken>false</tt:SAMLToken>"
                "<tt:KerberosToken>false</tt:KerberosToken><tt:RELToken>false</tt:RELToken></tt:Security></tt:Device>");
    }
    if (media) {
        bput(b, "<tt:Media><tt:XAddr>");
        put_xaddr(b, q, "media_service");
        bput(b, "</tt:XAddr><tt:StreamingCapabilities><tt:RTPMulticast>false</tt:RTPMulticast>"
                "<tt:RTP_TCP>true</tt:RTP_TCP><tt:RTP_RTSP_TCP>true</tt:RTP_RTSP_TCP></tt:StreamingCapabilities>"
                "</tt:Media>");
    }
    bput(b, "</tds:Capabilities></tds:GetCapabilitiesResponse>");
    return 0;
}

static int dev_services(req_t *q, buf_t *b)
{
    char inc[16] = "";
    arg(q, "IncludeCapability", inc, sizeof inc);
    int caps = !strcmp(inc, "true") || !strcmp(inc, "1");
    static const struct { const char *ns, *path; int major, minor; } S[] = {
        { ONVIF_NS_DEVICE, "device_service", 2, 60 },
        { ONVIF_NS_MEDIA, "media_service", 2, 60 },
        { ONVIF_NS_MEDIA2, "media2_service", 20, 12 },
    };
    bput(b, "<tds:GetServicesResponse>");
    for (size_t i = 0; i < sizeof S / sizeof S[0]; ++i) {
        bput(b, "<tds:Service><tds:Namespace>%s</tds:Namespace><tds:XAddr>", S[i].ns);
        put_xaddr(b, q, S[i].path);
        bput(b, "</tds:XAddr>");
        if (caps) {
            bput(b, "<tds:Capabilities>");
            if (i == 0) put_dev_caps(b);
            else if (i == 1) put_media_caps(b, q);
            else put_media2_caps(b, q);
            bput(b, "</tds:Capabilities>");
        }
        bput(b, "<tds:Version><tt:Major>%d</tt:Major><tt:Minor>%d</tt:Minor></tds:Version></tds:Service>",
             S[i].major, S[i].minor);
    }
    bput(b, "</tds:GetServicesResponse>");
    return 0;
}

static int dev_service_caps(req_t *q, buf_t *b)
{
    (void)q;
    bput(b, "<tds:GetServiceCapabilitiesResponse>");
    put_dev_caps(b);
    bput(b, "</tds:GetServiceCapabilitiesResponse>");
    return 0;
}

static int dev_wsdl(req_t *q, buf_t *b)
{
    (void)q;
    bput(b, "<tds:GetWsdlUrlResponse><tds:WsdlUrl>https://www.onvif.org/profiles/specifications/</tds:WsdlUrl>"
            "</tds:GetWsdlUrlResponse>");
    return 0;
}

static int dev_hostname(req_t *q, buf_t *b)
{
    (void)q;
    char h[64] = "";
    if (gethostname(h, sizeof h) < 0) h[0] = 0;
    h[sizeof h - 1] = 0;
    bput(b, "<tds:GetHostnameResponse><tds:HostnameInformation><tt:FromDHCP>false</tt:FromDHCP><tt:Name>");
    besc(b, h);
    bput(b, "</tt:Name></tds:HostnameInformation></tds:GetHostnameResponse>");
    return 0;
}

static int dev_scopes(req_t *q, buf_t *b)
{
    (void)q;
    bput(b, "<tds:GetScopesResponse>");
    for (const char *p = g_id.scopes; *p;) {
        size_t n = strcspn(p, " ");
        if (n) bput(b, "<tds:Scopes><tt:ScopeDef>Fixed</tt:ScopeDef><tt:ScopeItem>%.*s</tt:ScopeItem></tds:Scopes>",
                    (int)n, p);
        p += n;
        while (*p == ' ') ++p;
    }
    bput(b, "</tds:GetScopesResponse>");
    return 0;
}

static int dev_discovery_mode(req_t *q, buf_t *b)
{
    (void)q;
    bput(b, "<tds:GetDiscoveryModeResponse><tds:DiscoveryMode>Discoverable</tds:DiscoveryMode>"
            "</tds:GetDiscoveryModeResponse>");
    return 0;
}

static int dev_protocols(req_t *q, buf_t *b)
{
    bput(b, "<tds:GetNetworkProtocolsResponse>"
            "<tds:NetworkProtocols><tt:Name>HTTP</tt:Name><tt:Enabled>true</tt:Enabled><tt:Port>%d</tt:Port>"
            "</tds:NetworkProtocols>"
            "<tds:NetworkProtocols><tt:Name>RTSP</tt:Name><tt:Enabled>%s</tt:Enabled><tt:Port>%d</tt:Port>"
            "</tds:NetworkProtocols></tds:GetNetworkProtocolsResponse>",
         q->cfg->onvif_server.port, q->cfg->rtsp_server.enabled ? "true" : "false", q->cfg->rtsp_server.port);
    return 0;
}

static int prefix_len(uint32_t mask)
{
    int n = 0;
    while (mask & 0x80000000u) {
        ++n;
        mask <<= 1;
    }
    return n;
}

static int dev_ifaces(req_t *q, buf_t *b)
{
    (void)q;
    struct ifaddrs *ifa;
    bput(b, "<tds:GetNetworkInterfacesResponse>");
    if (getifaddrs(&ifa) == 0) {
        for (struct ifaddrs *i = ifa; i; i = i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK) ||
                !i->ifa_netmask)
                continue;
            net_status_t ns;
            net_status(i->ifa_name, &ns);
            char mac[18];
            if_mac(i->ifa_name, mac);
            net_cfg_t nc;
            char err[96];
            int dhcp = net_read(i->ifa_name, &nc, err, sizeof err) == 0 && nc.dhcp;
            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, ip, sizeof ip);
            uint32_t mask = ntohl(((struct sockaddr_in *)i->ifa_netmask)->sin_addr.s_addr);
            bput(b, "<tds:NetworkInterfaces token=\"");
            besc(b, i->ifa_name);
            bput(b, "\"><tt:Enabled>%s</tt:Enabled><tt:Info><tt:Name>", (i->ifa_flags & IFF_UP) ? "true" : "false");
            besc(b, i->ifa_name);
            bput(b, "</tt:Name><tt:HwAddress>%s</tt:HwAddress><tt:MTU>%d</tt:MTU></tt:Info>"
                    "<tt:IPv4><tt:Enabled>true</tt:Enabled><tt:Config>",
                 mac[0] ? mac : "00:00:00:00:00:00", ns.mtu > 0 ? ns.mtu : 1500);
            bput(b, dhcp ? "<tt:FromDHCP><tt:Address>%s</tt:Address><tt:PrefixLength>%d</tt:PrefixLength></tt:FromDHCP>"
                         : "<tt:Manual><tt:Address>%s</tt:Address><tt:PrefixLength>%d</tt:PrefixLength></tt:Manual>",
                 ip, prefix_len(mask));
            bput(b, "<tt:DHCP>%s</tt:DHCP></tt:Config></tt:IPv4></tds:NetworkInterfaces>", dhcp ? "true" : "false");
        }
        freeifaddrs(ifa);
    }
    bput(b, "</tds:GetNetworkInterfacesResponse>");
    return 0;
}

/* Read-only: vigil's users are changed through the API and the UI. */
static int dev_users(req_t *q, buf_t *b)
{
    users_user_t *u = calloc(USERS_MAX, sizeof *u);
    users_group_t *g = calloc(GROUPS_MAX, sizeof *g);
    if (!u || !g) {
        free(u);
        free(g);
        return fault(q, 500, "s:Receiver", "ter:Action", NULL, "out of memory");
    }
    int nu = users_list(u, USERS_MAX), ng = users_groups(g, GROUPS_MAX), has_admin = 0;
    bput(b, "<tds:GetUsersResponse>");
    for (int i = 0; i < nu; ++i) {
        unsigned perms = 0;
        for (int k = 0; k < ng; ++k)
            if (!strcmp(g[k].name, u[i].group)) perms = g[k].perms;
        has_admin |= !strcmp(u[i].name, "admin");
        const char *level = (perms & (PERM_USERS | PERM_SYSTEM)) == (PERM_USERS | PERM_SYSTEM) ? "Administrator"
                          : perms & (PERM_CAMERAS | PERM_STORAGE | PERM_NETWORK | PERM_SYSTEM) ? "Operator"
                                                                                               : "User";
        bput(b, "<tds:User><tt:Username>");
        besc(b, u[i].name);
        bput(b, "</tt:Username><tt:UserLevel>%s</tt:UserLevel></tds:User>", level);
    }
    bput(b, "<tds:User><tt:Username>");
    besc(b, q->cfg->api.user);
    bput(b, "</tt:Username><tt:UserLevel>Administrator</tt:UserLevel></tds:User>");
    if (!has_admin)
        bput(b, "<tds:User><tt:Username>admin</tt:Username><tt:UserLevel>Administrator</tt:UserLevel></tds:User>");
    bput(b, "</tds:GetUsersResponse>");
    free(u);
    free(g);
    return 0;
}

static int dev_reboot(req_t *q, buf_t *b)
{
    if (mgmt_power_request(MGMT_POWER_REBOOT) != MGMT_OK)
        return fault(q, 500, "s:Receiver", "ter:ActionNotSupported", NULL,
                     "vigild does not reboot this system (it does not manage it)");
    vg_log("onvif: %s asked for a reboot", q->user);
    bput(b, "<tds:SystemRebootResponse><tds:Message>Rebooting</tds:Message></tds:SystemRebootResponse>");
    return 0;
}

/* ---- media ver10 ------------------------------------------------------------------------ */

typedef struct {
    chan_t c[CHANNEL_MAX];
    int    n;
} chans_t;

static chans_t *load_chans(req_t *q)
{
    chans_t *cs = malloc(sizeof *cs);
    if (cs) cs->n = chans(q->cfg, cs->c);
    else fault(q, 500, "s:Receiver", "ter:Action", NULL, "out of memory");
    return cs;
}

static int m1_caps(req_t *q, buf_t *b)
{
    bput(b, "<trt:GetServiceCapabilitiesResponse>");
    put_media_caps(b, q);
    bput(b, "</trt:GetServiceCapabilitiesResponse>");
    return 0;
}

static int m1_profiles(req_t *q, buf_t *b)
{
    chans_t *cs = load_chans(q);
    if (!cs) return -1;
    bput(b, "<trt:GetProfilesResponse>");
    for (int i = 0; i < cs->n; ++i)
        for (int s = 0; s <= cs->c[i].has_sub; ++s) put_profile1(b, &cs->c[i], s, "trt:Profiles");
    bput(b, "</trt:GetProfilesResponse>");
    free(cs);
    return 0;
}

static int m1_profile(req_t *q, buf_t *b)
{
    char tok[64] = "";
    arg(q, "ProfileToken", tok, sizeof tok);
    chans_t *cs = load_chans(q);
    if (!cs) return -1;
    int sub;
    const chan_t *c = by_token(cs->c, cs->n, tok, "ch", &sub);
    if (c) {
        bput(b, "<trt:GetProfileResponse>");
        put_profile1(b, c, sub, "trt:Profile");
        bput(b, "</trt:GetProfileResponse>");
    }
    free(cs);
    return c ? 0 : bad_arg(q, "ter:NoProfile", "No such profile");
}

static int m1_uri(req_t *q, buf_t *b)
{
    char tok[64] = "", stream[32] = "";
    arg(q, "ProfileToken", tok, sizeof tok);
    arg(q, "Stream", stream, sizeof stream);
    if (!strcmp(stream, "RTP-Multicast"))
        return bad_arg(q, "ter:InvalidStreamSetup", "Only unicast streams (RTSP over UDP or TCP)");
    if (!q->cfg->rtsp_server.enabled) return rtsp_off(q);
    chans_t *cs = load_chans(q);
    if (!cs) return -1;
    int sub;
    const chan_t *c = by_token(cs->c, cs->n, tok, "ch", &sub);
    if (c) {
        bput(b, "<trt:GetStreamUriResponse><trt:MediaUri><tt:Uri>");
        put_uri(b, q, c->ch, sub);
        bput(b, "</tt:Uri><tt:InvalidAfterConnect>false</tt:InvalidAfterConnect>"
                "<tt:InvalidAfterReboot>false</tt:InvalidAfterReboot><tt:Timeout>PT0S</tt:Timeout></trt:MediaUri>"
                "</trt:GetStreamUriResponse>");
    }
    free(cs);
    return c ? 0 : bad_arg(q, "ter:NoProfile", "No such profile");
}

/* GetSnapshotUri: the API's picture of the channel (the camera's own JPEG
 * when it has one, else the recorder's), with the API's logins (HTTP
 * Digest or Basic, as ONVIF clients send them). A profile of either
 * stream is the channel's picture. */
static void put_snap_uri(buf_t *b, const req_t *q, int ch)
{
    bput(b, "http://");
    besc(b, q->hostname);
    bput(b, ":%d/api/v1/channels/%d/snapshot.jpg", api_port(q), ch);
}

static int snap_off(req_t *q)
{
    return fault(q, 500, "s:Receiver", "ter:ActionNotSupported", NULL,
                 "Snapshots are served by the HTTP API, which is off (api.listen)");
}

static int m1_snap_uri(req_t *q, buf_t *b)
{
    char tok[64] = "";
    arg(q, "ProfileToken", tok, sizeof tok);
    if (api_port(q) <= 0) return snap_off(q);
    chans_t *cs = load_chans(q);
    if (!cs) return -1;
    int sub;
    const chan_t *c = by_token(cs->c, cs->n, tok, "ch", &sub);
    if (c) {
        bput(b, "<trt:GetSnapshotUriResponse><trt:MediaUri><tt:Uri>");
        put_snap_uri(b, q, c->ch);
        bput(b, "</tt:Uri><tt:InvalidAfterConnect>false</tt:InvalidAfterConnect>"
                "<tt:InvalidAfterReboot>false</tt:InvalidAfterReboot><tt:Timeout>PT0S</tt:Timeout></trt:MediaUri>"
                "</trt:GetSnapshotUriResponse>");
    }
    free(cs);
    return c ? 0 : bad_arg(q, "ter:NoProfile", "No such profile");
}

static int m2_snap_uri(req_t *q, buf_t *b)
{
    char tok[64] = "";
    arg(q, "ProfileToken", tok, sizeof tok);
    if (api_port(q) <= 0) return snap_off(q);
    chans_t *cs = load_chans(q);
    if (!cs) return -1;
    int sub;
    const chan_t *c = by_token(cs->c, cs->n, tok, "ch", &sub);
    if (c) {
        bput(b, "<tr2:GetSnapshotUriResponse><tr2:Uri>");
        put_snap_uri(b, q, c->ch);
        bput(b, "</tr2:Uri></tr2:GetSnapshotUriResponse>");
    }
    free(cs);
    return c ? 0 : bad_arg(q, "ter:NoProfile", "No such profile");
}

static int m1_sources(req_t *q, buf_t *b)
{
    chans_t *cs = load_chans(q);
    if (!cs) return -1;
    bput(b, "<trt:GetVideoSourcesResponse>");
    for (int i = 0; i < cs->n; ++i)
        bput(b, "<trt:VideoSources token=\"vs%d\"><tt:Framerate>%u</tt:Framerate><tt:Resolution><tt:Width>%u</tt:Width>"
                "<tt:Height>%u</tt:Height></tt:Resolution></trt:VideoSources>",
             cs->c[i].ch, cs->c[i].s[0].fps, cs->c[i].s[0].w, cs->c[i].s[0].h);
    bput(b, "</trt:GetVideoSourcesResponse>");
    free(cs);
    return 0;
}

static int m1_vscs(req_t *q, buf_t *b)
{
    chans_t *cs = load_chans(q);
    if (!cs) return -1;
    bput(b, "<trt:GetVideoSourceConfigurationsResponse>");
    for (int i = 0; i < cs->n; ++i) put_vsc(b, &cs->c[i], "trt:Configurations");
    bput(b, "</trt:GetVideoSourceConfigurationsResponse>");
    free(cs);
    return 0;
}

static int m1_vsc(req_t *q, buf_t *b)
{
    char tok[64] = "";
    arg(q, "ConfigurationToken", tok, sizeof tok);
    chans_t *cs = load_chans(q);
    if (!cs) return -1;
    const chan_t *c = by_source(cs->c, cs->n, tok);
    if (c) {
        bput(b, "<trt:GetVideoSourceConfigurationResponse>");
        put_vsc(b, c, "trt:Configuration");
        bput(b, "</trt:GetVideoSourceConfigurationResponse>");
    }
    free(cs);
    return c ? 0 : bad_arg(q, "ter:NoConfig", "No such configuration");
}

static int m1_vecs(req_t *q, buf_t *b)
{
    chans_t *cs = load_chans(q);
    if (!cs) return -1;
    bput(b, "<trt:GetVideoEncoderConfigurationsResponse>");
    for (int i = 0; i < cs->n; ++i)
        for (int s = 0; s <= cs->c[i].has_sub; ++s) put_vec1(b, &cs->c[i], s, "trt:Configurations");
    bput(b, "</trt:GetVideoEncoderConfigurationsResponse>");
    free(cs);
    return 0;
}

static int m1_vec(req_t *q, buf_t *b)
{
    char tok[64] = "";
    arg(q, "ConfigurationToken", tok, sizeof tok);
    chans_t *cs = load_chans(q);
    if (!cs) return -1;
    int sub;
    const chan_t *c = by_token(cs->c, cs->n, tok, "enc", &sub);
    if (c) {
        bput(b, "<trt:GetVideoEncoderConfigurationResponse>");
        put_vec1(b, c, sub, "trt:Configuration");
        bput(b, "</trt:GetVideoEncoderConfigurationResponse>");
    }
    free(cs);
    return c ? 0 : bad_arg(q, "ter:NoConfig", "No such configuration");
}

/* ---- media2 ver20 ------------------------------------------------------------------------ */

static int m2_caps(req_t *q, buf_t *b)
{
    bput(b, "<tr2:GetServiceCapabilitiesResponse>");
    put_media2_caps(b, q);
    bput(b, "</tr2:GetServiceCapabilitiesResponse>");
    return 0;
}

/* Every element `name` of the arguments, as one string. */
static void args_all(const req_t *q, const char *name, char *out, size_t cap)
{
    const char *x = q->xml + q->soap.arg0;
    size_t len = q->soap.arg1 - q->soap.arg0, from = 0, o = 0;
    char v[64];
    out[0] = 0;
    while (disc_xml_text(x, len, name, v, sizeof v, &from) == 0 && o + strlen(v) + 2 < cap)
        o += (size_t)snprintf(out + o, cap - o, "%s ", v);
}

static int m2_profiles(req_t *q, buf_t *b)
{
    char tok[64] = "", types[256];
    arg(q, "Token", tok, sizeof tok);
    args_all(q, "Type", types, sizeof types);
    int all = strstr(types, "All") != NULL;
    int vs = all || strstr(types, "VideoSource"), ve = all || strstr(types, "VideoEncoder");
    chans_t *cs = load_chans(q);
    if (!cs) return -1;
    int sub, found = 0;
    const chan_t *only = tok[0] ? by_token(cs->c, cs->n, tok, "ch", &sub) : NULL;
    bput(b, "<tr2:GetProfilesResponse>");
    for (int i = 0; i < cs->n; ++i)
        for (int s = 0; s <= cs->c[i].has_sub; ++s)
            if (!tok[0] || (only == &cs->c[i] && sub == s)) {
                put_profile2(b, &cs->c[i], s, vs, ve);
                found = 1;
            }
    bput(b, "</tr2:GetProfilesResponse>");
    free(cs);
    return found || !tok[0] ? 0 : bad_arg(q, "ter:NoProfile", "No such profile");
}

static int m2_uri(req_t *q, buf_t *b)
{
    char tok[64] = "", proto[32] = "";
    arg(q, "ProfileToken", tok, sizeof tok);
    arg(q, "Protocol", proto, sizeof proto);
    if (!strcmp(proto, "RtspMulticast"))
        return bad_arg(q, "ter:InvalidStreamSetup", "Only unicast streams (RTSP over UDP or TCP)");
    if (!q->cfg->rtsp_server.enabled) return rtsp_off(q);
    chans_t *cs = load_chans(q);
    if (!cs) return -1;
    int sub;
    const chan_t *c = by_token(cs->c, cs->n, tok, "ch", &sub);
    if (c) {
        bput(b, "<tr2:GetStreamUriResponse><tr2:Uri>");
        put_uri(b, q, c->ch, sub);
        bput(b, "</tr2:Uri></tr2:GetStreamUriResponse>");
    }
    free(cs);
    return c ? 0 : bad_arg(q, "ter:NoProfile", "No such profile");
}

static int m2_vscs(req_t *q, buf_t *b)
{
    char ctok[64] = "", ptok[64] = "";
    arg(q, "ConfigurationToken", ctok, sizeof ctok);
    arg(q, "ProfileToken", ptok, sizeof ptok);
    chans_t *cs = load_chans(q);
    if (!cs) return -1;
    int sub, n = 0;
    const chan_t *only = ptok[0] ? by_token(cs->c, cs->n, ptok, "ch", &sub)
                       : ctok[0]  ? by_source(cs->c, cs->n, ctok) : NULL;
    bput(b, "<tr2:GetVideoSourceConfigurationsResponse>");
    for (int i = 0; i < cs->n; ++i)
        if ((!ptok[0] && !ctok[0]) || only == &cs->c[i]) {
            put_vsc(b, &cs->c[i], "tr2:Configurations");
            ++n;
        }
    bput(b, "</tr2:GetVideoSourceConfigurationsResponse>");
    free(cs);
    if (!n && ptok[0]) return bad_arg(q, "ter:NoProfile", "No such profile");
    if (!n && ctok[0]) return bad_arg(q, "ter:NoConfig", "No such configuration");
    return 0;
}

static int m2_vecs(req_t *q, buf_t *b)
{
    char ctok[64] = "", ptok[64] = "";
    arg(q, "ConfigurationToken", ctok, sizeof ctok);
    arg(q, "ProfileToken", ptok, sizeof ptok);
    chans_t *cs = load_chans(q);
    if (!cs) return -1;
    int sub = 0, n = 0;
    const chan_t *only = ptok[0] ? by_token(cs->c, cs->n, ptok, "ch", &sub)
                       : ctok[0]  ? by_token(cs->c, cs->n, ctok, "enc", &sub) : NULL;
    bput(b, "<tr2:GetVideoEncoderConfigurationsResponse>");
    for (int i = 0; i < cs->n; ++i)
        for (int s = 0; s <= cs->c[i].has_sub; ++s)
            if ((!ptok[0] && !ctok[0]) || (only == &cs->c[i] && sub == s)) {
                put_vec2(b, &cs->c[i], s, "tr2:Configurations");
                ++n;
            }
    bput(b, "</tr2:GetVideoEncoderConfigurationsResponse>");
    free(cs);
    if (!n && ptok[0]) return bad_arg(q, "ter:NoProfile", "No such profile");
    if (!n && ctok[0]) return bad_arg(q, "ter:NoConfig", "No such configuration");
    return 0;
}

/* ---- the operations, and who may call them ------------------------------------------------ */

/* PRE_AUTH: no login, as ONVIF's default access policy has them (Core
 * Specification, 5.9.4.3): what a client needs before it knows how to
 * log in. Everything else needs a login whose group has `perm`. */
#define PRE_AUTH 0xffffffffu

typedef struct {
    const char *ns, *name;
    unsigned    perm;
    int       (*fn)(req_t *q, buf_t *b);
} op_t;

static const op_t OPS[] = {
    { ONVIF_NS_DEVICE, "GetSystemDateAndTime", PRE_AUTH, dev_time },
    { ONVIF_NS_DEVICE, "GetCapabilities", PRE_AUTH, dev_caps },
    { ONVIF_NS_DEVICE, "GetServices", PRE_AUTH, dev_services },
    { ONVIF_NS_DEVICE, "GetServiceCapabilities", PRE_AUTH, dev_service_caps },
    { ONVIF_NS_DEVICE, "GetWsdlUrl", PRE_AUTH, dev_wsdl },
    { ONVIF_NS_DEVICE, "GetHostname", PRE_AUTH, dev_hostname },
    { ONVIF_NS_DEVICE, "GetDeviceInformation", PERM_LIVE, dev_info },
    { ONVIF_NS_DEVICE, "GetScopes", PERM_LIVE, dev_scopes },
    { ONVIF_NS_DEVICE, "GetDiscoveryMode", PERM_LIVE, dev_discovery_mode },
    { ONVIF_NS_DEVICE, "GetNetworkInterfaces", PERM_LIVE, dev_ifaces },
    { ONVIF_NS_DEVICE, "GetNetworkProtocols", PERM_LIVE, dev_protocols },
    { ONVIF_NS_DEVICE, "GetUsers", PERM_USERS, dev_users },
    { ONVIF_NS_DEVICE, "SystemReboot", PERM_SYSTEM, dev_reboot },
    { ONVIF_NS_MEDIA, "GetServiceCapabilities", PRE_AUTH, m1_caps },
    { ONVIF_NS_MEDIA, "GetProfiles", PERM_LIVE, m1_profiles },
    { ONVIF_NS_MEDIA, "GetProfile", PERM_LIVE, m1_profile },
    { ONVIF_NS_MEDIA, "GetStreamUri", PERM_LIVE, m1_uri },
    { ONVIF_NS_MEDIA, "GetSnapshotUri", PERM_LIVE, m1_snap_uri },
    { ONVIF_NS_MEDIA, "GetVideoSources", PERM_LIVE, m1_sources },
    { ONVIF_NS_MEDIA, "GetVideoSourceConfigurations", PERM_LIVE, m1_vscs },
    { ONVIF_NS_MEDIA, "GetVideoSourceConfiguration", PERM_LIVE, m1_vsc },
    { ONVIF_NS_MEDIA, "GetVideoEncoderConfigurations", PERM_LIVE, m1_vecs },
    { ONVIF_NS_MEDIA, "GetVideoEncoderConfiguration", PERM_LIVE, m1_vec },
    { ONVIF_NS_MEDIA2, "GetServiceCapabilities", PRE_AUTH, m2_caps },
    { ONVIF_NS_MEDIA2, "GetProfiles", PERM_LIVE, m2_profiles },
    { ONVIF_NS_MEDIA2, "GetStreamUri", PERM_LIVE, m2_uri },
    { ONVIF_NS_MEDIA2, "GetSnapshotUri", PERM_LIVE, m2_snap_uri },
    { ONVIF_NS_MEDIA2, "GetVideoSourceConfigurations", PERM_LIVE, m2_vscs },
    { ONVIF_NS_MEDIA2, "GetVideoEncoderConfigurations", PERM_LIVE, m2_vecs },
    /* GetVideoSources is media ver10's; media2 clients ask it there too */
};

/* The operation: by its namespace, else (none declared) by the service
 * the request was posted to. */
static const op_t *find_op(const onvif_soap_t *s, const char *uri)
{
    const char *ns = s->ns;
    if (!ns[0]) ns = strstr(uri, "media2") ? ONVIF_NS_MEDIA2 : strstr(uri, "media") ? ONVIF_NS_MEDIA : ONVIF_NS_DEVICE;
    for (size_t i = 0; i < sizeof OPS / sizeof OPS[0]; ++i)
        if (!strcmp(OPS[i].ns, ns) && !strcmp(OPS[i].name, s->op)) return &OPS[i];
    return NULL;
}

/* ---- logging in ------------------------------------------------------------------------------ */

static int wsse_check(const char *pass, void *ctx)
{
    const onvif_soap_t *s = ctx;
    char want[32];
    return onvif_wsse_digest(s->nonce, s->created, pass, want) == 0 && onvif_equal(want, s->password);
}

typedef struct {
    const onvif_digest_t *d;
    const char           *method;
} digest_ctx_t;

static int digest_check(const char *pass, void *ctx)
{
    const digest_ctx_t *c = ctx;
    char want[33];
    onvif_digest_expect(c->d, c->method, pass, want);
    return onvif_equal(want, c->d->response);
}

/* Seen this UsernameToken nonce lately? Remembers it. */
static int nonce_replayed(const char *nonce)
{
    unsigned char d[16];
    md5_raw(nonce, strlen(nonce), d);
    pthread_mutex_lock(&g_lock);
    int seen = 0;
    for (int i = 0; i < SEEN_N && !seen; ++i) seen = !memcmp(g_seen[i], d, sizeof d);
    if (!seen) {
        memcpy(g_seen[g_seen_at], d, sizeof d);
        g_seen_at = (g_seen_at + 1) % SEEN_N;
    }
    pthread_mutex_unlock(&g_lock);
    return seen;
}

enum { AUTH_NONE = 0, AUTH_OK = 1, AUTH_BAD = -1, AUTH_LOCKED = -2, AUTH_STALE = -3 };

/* Who sent q: AUTH_OK with *perms and q->user; AUTH_NONE without
 * credentials; the others refused. */
static int authenticate(req_t *q, unsigned *perms)
{
    const struct mg_request_info *ri = mg_get_request_info(q->conn);
    const char *addr = ri->remote_addr;
    const onvif_soap_t *s = &q->soap;
    long retry = 0;
    int rc;
    *perms = 0;
    if (s->has_token) {
        if (!s->user[0]) return AUTH_BAD;
        snprintf(q->user, sizeof q->user, "%.*s", (int)sizeof q->user - 1, s->user);
        if (s->digest) {
            /* Replays: the nonce once, the Created near our clock -- unless
             * that clock was never set (no RTC, no NTP yet), when only the
             * nonce counts. */
            time_t created, now = time(NULL);
            if (!s->nonce[0] || onvif_time_parse(s->created, &created) < 0) return AUTH_BAD;
            if (now > 1577836800 && (created > now + WSSE_SKEW || created < now - WSSE_SKEW)) {
                vg_log("onvif: %s from %s: its Created %s is more than %d min off our clock", s->user, addr,
                       s->created, WSSE_SKEW / 60);
                return AUTH_BAD;
            }
            if (nonce_replayed(s->nonce)) {
                vg_log("onvif: %s from %s: a UsernameToken nonce used before (a replay?)", s->user, addr);
                return AUTH_BAD;
            }
            rc = api_login_with(addr, s->user, wsse_check, (void *)s, perms, &retry);
        } else {
            rc = api_login(addr, s->user, s->password, perms, &retry);
        }
        return rc == 1 ? AUTH_OK : rc == -2 ? AUTH_LOCKED : AUTH_BAD;
    }
    const char *hdr = mg_get_header(q->conn, "Authorization");
    if (!hdr) return AUTH_NONE;
    if (!strncasecmp(hdr, "Digest", 6)) {
        onvif_digest_t d;
        if (onvif_digest_parse(hdr, &d) < 0 || strcmp(d.realm, REALM) != 0) return AUTH_BAD;
        long now = mono(), age = onvif_digest_nonce_age(g_key, d.nonce, now);
        if (age < 0) return AUTH_BAD;
        if (age > NONCE_SECS) return AUTH_STALE;
        /* The uri it signed is the one it asked for. */
        const char *u = ri->request_uri ? ri->request_uri : "";
        if (strcmp(d.uri, u) != 0 && !(ri->local_uri_raw && !strcmp(d.uri, ri->local_uri_raw))) return AUTH_BAD;
        snprintf(q->user, sizeof q->user, "%.*s", (int)sizeof q->user - 1, d.username);
        digest_ctx_t c = { &d, ri->request_method };
        rc = api_login_with(addr, d.username, digest_check, &c, perms, &retry);
        if (rc == 1) {
            /* A request seen before, word for word (its nonce count used),
             * is a replay: it would do again what it did (SystemReboot).
             * Stale rather than refused: a client that sent its counts
             * out of order gets a new nonce and tries again. */
            pthread_mutex_lock(&g_lock);
            int fresh = onvif_nc_use(&g_nc, &d, now - age, now, NONCE_SECS);
            pthread_mutex_unlock(&g_lock);
            if (fresh == 0) vg_log("onvif: %s from %s: a Digest nonce count used before (a replay?)", d.username, addr);
            if (fresh <= 0) {
                *perms = 0;
                return AUTH_STALE;
            }
        }
        return rc == 1 ? AUTH_OK : rc == -2 ? AUTH_LOCKED : AUTH_BAD;
    }
    char user[USERS_NAME_MAX + 32], pass[256];
    if (auth_parse_basic(hdr, user, sizeof user, pass, sizeof pass) < 0) return AUTH_BAD;
    snprintf(q->user, sizeof q->user, "%.*s", (int)sizeof q->user - 1, user);
    rc = api_login(addr, user, pass, perms, &retry);
    volatile char *w = pass;
    for (size_t i = 0; i < sizeof pass; ++i) w[i] = 0;
    return rc == 1 ? AUTH_OK : rc == -2 ? AUTH_LOCKED : AUTH_BAD;
}

/* ---- the web server ------------------------------------------------------------------------------ */

#define ENV_HEAD                                                                                    \
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"                                                    \
    "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\""                               \
    " xmlns:tt=\"http://www.onvif.org/ver10/schema\" xmlns:tds=\"" ONVIF_NS_DEVICE "\""             \
    " xmlns:trt=\"" ONVIF_NS_MEDIA "\" xmlns:tr2=\"" ONVIF_NS_MEDIA2 "\""                           \
    " xmlns:ter=\"http://www.onvif.org/ver10/error\"><s:Body>"
#define ENV_TAIL "</s:Body></s:Envelope>"

static int send_soap(struct mg_connection *conn, int status, const char *body, size_t len, const char *h1,
                     const char *v1)
{
    char cl[24];
    snprintf(cl, sizeof cl, "%zu", strlen(ENV_HEAD) + len + strlen(ENV_TAIL));
    mg_response_header_start(conn, status);
    mg_response_header_add(conn, "Content-Type", "application/soap+xml; charset=utf-8", -1);
    mg_response_header_add(conn, "Content-Length", cl, -1);
    mg_response_header_add(conn, "Cache-Control", "no-store", -1);
    if (h1) mg_response_header_add(conn, h1, v1, -1);
    mg_response_header_send(conn);
    mg_write(conn, ENV_HEAD, strlen(ENV_HEAD));
    mg_write(conn, body, len);
    mg_write(conn, ENV_TAIL, strlen(ENV_TAIL));
    return status;
}

static int send_fault(req_t *q, const char *h1, const char *v1)
{
    buf_t b = { 0 };
    bput(&b, "<s:Fault><s:Code><s:Value>%s</s:Value>", q->f_code);
    if (q->f_sub) {
        bput(&b, "<s:Subcode><s:Value>%s</s:Value>", q->f_sub);
        if (q->f_sub2) bput(&b, "<s:Subcode><s:Value>%s</s:Value></s:Subcode>", q->f_sub2);
        bput(&b, "</s:Subcode>");
    }
    bput(&b, "</s:Code><s:Reason><s:Text xml:lang=\"en\">");
    besc(&b, q->f_text ? q->f_text : "");
    bput(&b, "</s:Text></s:Reason></s:Fault>");
    int rc = b.oom ? send_soap(q->conn, 500, "", 0, NULL, NULL) : send_soap(q->conn, q->f_status, b.p, b.n, h1, v1);
    free(b.p);
    return rc;
}

static int send_plain(struct mg_connection *conn, int status, const char *text, const char *h, const char *v)
{
    char cl[24];
    snprintf(cl, sizeof cl, "%zu", strlen(text));
    mg_response_header_start(conn, status);
    mg_response_header_add(conn, "Content-Type", "text/plain; charset=utf-8", -1);
    mg_response_header_add(conn, "Content-Length", cl, -1);
    if (h) mg_response_header_add(conn, h, v, -1);
    mg_response_header_send(conn);
    mg_write(conn, text, strlen(text));
    return status;
}

/* "Host: 10.0.0.5:8899" -> host "10.0.0.5:8899", hostname "10.0.0.5";
 * no usable header: the address of the first interface. */
static void req_host(req_t *q)
{
    const char *h = mg_get_header(q->conn, "Host");
    size_t n = h ? strlen(h) : 0;
    int ok = n > 0 && n < sizeof q->host && h[0] != '[';
    for (size_t i = 0; ok && i < n; ++i)
        ok = (h[i] >= 'a' && h[i] <= 'z') || (h[i] >= 'A' && h[i] <= 'Z') || (h[i] >= '0' && h[i] <= '9') ||
             h[i] == '.' || h[i] == '-' || h[i] == ':';
    if (ok) {
        snprintf(q->host, sizeof q->host, "%s", h);
        snprintf(q->hostname, sizeof q->hostname, "%.*s", (int)strcspn(h, ":"), h);
        return;
    }
    snprintf(q->hostname, sizeof q->hostname, "127.0.0.1");
    struct ifaddrs *ifa;
    if (getifaddrs(&ifa) == 0) {
        for (struct ifaddrs *i = ifa; i; i = i->ifa_next)
            if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && !(i->ifa_flags & IFF_LOOPBACK)) {
                inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, q->hostname, sizeof q->hostname);
                break;
            }
        freeifaddrs(ifa);
    }
    snprintf(q->host, sizeof q->host, "%s:%d", q->hostname, mg_get_request_info(q->conn)->server_port);
}

static int handler(struct mg_connection *conn, void *cbdata)
{
    (void)cbdata;
    const struct mg_request_info *ri = mg_get_request_info(conn);
    if (strcmp(ri->request_method, "POST") != 0)
        return send_plain(conn, 405, "ONVIF: SOAP over POST\n", "Allow", "POST");
    if (ri->content_length > (long long)BODY_MAX) return send_plain(conn, 413, "too large\n", NULL, NULL);
    char *body = malloc(BODY_MAX + 1);
    req_t *q = calloc(1, sizeof *q);
    vigil_config_t *cfg = malloc(sizeof *cfg);
    if (!body || !q || !cfg) {
        free(body);
        free(q);
        free(cfg);
        return send_plain(conn, 500, "out of memory\n", NULL, NULL);
    }
    size_t n = 0;
    int r;
    while (n < BODY_MAX && (r = mg_read(conn, body + n, BODY_MAX - n)) > 0) n += (size_t)r;
    body[n] = 0;
    config_get(cfg);
    q->conn = conn;
    q->xml = body;
    q->len = n;
    q->cfg = cfg;
    req_host(q);
    int rc;
    const op_t *op = NULL;
    int parsed = onvif_soap_parse(body, n, &q->soap) == 0;
    if (!cfg->onvif_server.enabled) {
        rc = send_plain(conn, 503, "ONVIF is turned off (onvif_server.enabled)\n", NULL, NULL);
    } else if (!parsed && !n && !mg_get_header(conn, "Authorization")) {
        /* An HTTP client that logs in first (curl --digest) sends no
         * body until it has been asked to. */
        char nonce[41], www[160];
        onvif_digest_nonce(g_key, mono(), atomic_fetch_add(&g_nonce_serial, 1), nonce);
        snprintf(www, sizeof www, "Digest realm=\"" REALM "\", qop=\"auth\", nonce=\"%s\", algorithm=MD5", nonce);
        fault(q, 401, "s:Sender", "ter:NotAuthorized", NULL, "Sender not authorized");
        rc = send_fault(q, "WWW-Authenticate", www);
    } else if (!parsed) {
        fault(q, 400, "s:Sender", "ter:WellFormed", NULL, "Not a SOAP request");
        rc = send_fault(q, NULL, NULL);
    } else if (!(op = find_op(&q->soap, ri->local_uri ? ri->local_uri : ""))) {
        char why[160];
        snprintf(why, sizeof why, "%.60s is not supported here", q->soap.op);
        fault(q, 500, "s:Receiver", "ter:ActionNotSupported", NULL, why);
        rc = send_fault(q, NULL, NULL);
    } else {
        unsigned perms = PERM_ALL;
        int a = op->perm == PRE_AUTH ? AUTH_OK : authenticate(q, &perms);
        if (a == AUTH_NONE || a == AUTH_STALE || (a == AUTH_BAD && !q->soap.has_token)) {
            /* No (or a stale) HTTP login: ask for one. A client that sent
             * a UsernameToken is told by the fault below instead. */
            char nonce[41], www[160];
            onvif_digest_nonce(g_key, mono(), atomic_fetch_add(&g_nonce_serial, 1), nonce);
            snprintf(www, sizeof www, "Digest realm=\"" REALM "\", qop=\"auth\", nonce=\"%s\", algorithm=MD5%s", nonce,
                     a == AUTH_STALE ? ", stale=true" : "");
            fault(q, 401, "s:Sender", "ter:NotAuthorized", NULL, "Sender not authorized");
            rc = send_fault(q, "WWW-Authenticate", www);
        } else if (a != AUTH_OK) {
            fault(q, 400, "s:Sender", "ter:NotAuthorized", NULL,
                  a == AUTH_LOCKED ? "Too many failed logins from this address; try again later"
                                   : "Sender not authorized");
            rc = send_fault(q, NULL, NULL);
        } else if (op->perm != PRE_AUTH && (perms & op->perm) != op->perm) {
            char why[160];
            snprintf(why, sizeof why, "%s's group does not have the %s permission", q->user,
                     op->perm == PERM_LIVE ? "live" : op->perm == PERM_USERS ? "users" : "system");
            fault(q, 400, "s:Sender", "ter:NotAuthorized", NULL, why);
            rc = send_fault(q, NULL, NULL);
        } else {
            buf_t b = { 0 };
            if (op->fn(q, &b) < 0) rc = send_fault(q, NULL, NULL);
            else if (b.oom) rc = send_plain(conn, 500, "out of memory\n", NULL, NULL);
            else rc = send_soap(conn, 200, b.p, b.n, NULL, NULL);
            free(b.p);
        }
    }
    free(body);
    free(q);
    free(cfg);
    return rc;
}

static int log_message(const struct mg_connection *conn, const char *msg)
{
    (void)conn;
    vg_log("onvif: %s", msg);
    return 1;
}

static void http_stop(void)
{
    if (!g_ctx) return;
    mg_stop(g_ctx);
    g_ctx = NULL;
    atomic_store(&g_port, 0);
}

static int http_start(int port)
{
    char ports[16];
    snprintf(ports, sizeof ports, "%d", port);
    const char *options[] = {
        "listening_ports", ports,
        "num_threads", THREADS,
        "request_timeout_ms", "10000",
        "max_request_size", "16384",
        "enable_keep_alive", "yes",
        NULL,
    };
    struct mg_callbacks cb;
    memset(&cb, 0, sizeof cb);
    cb.log_message = log_message;
    char why[128];
    if (net_listen_check(ports, why, sizeof why) < 0) {
        char t[160];
        snprintf(t, sizeof t, "%s; trying again every 30 s", why);
        api_server_state(2, "", t);
        return -1;
    }
    g_ctx = mg_start(&cb, NULL, options);
    if (!g_ctx) {
        api_server_state(2, "", "the web server library could not start there; trying again every 30 s");
        return -1;
    }
    /* Device management and the profiles: the management class (qos.h). */
    api_qos_listeners(g_ctx, QOS_MANAGEMENT);
    mg_set_request_handler(g_ctx, "/onvif", handler, NULL);
    atomic_store(&g_port, port);
    char l[24];
    snprintf(l, sizeof l, "0.0.0.0:%d", port);
    api_server_state(2, l, "");
    return 0;
}

/* ---- WS-Discovery ------------------------------------------------------------------------------ */

typedef struct {
    unsigned index;
    uint32_t addr;                  /* host order */
} wif_t;

/* The IPv4 interfaces that can take multicast. */
static int wifs(wif_t *out, int max)
{
    struct ifaddrs *ifa;
    int n = 0;
    if (getifaddrs(&ifa) < 0) return 0;
    for (struct ifaddrs *i = ifa; i && n < max; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
        if ((i->ifa_flags & (IFF_UP | IFF_LOOPBACK | IFF_MULTICAST)) != (IFF_UP | IFF_MULTICAST)) continue;
        out[n].index = if_nametoindex(i->ifa_name);
        out[n].addr = ntohl(((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr);
        ++n;
    }
    freeifaddrs(ifa);
    return n;
}

typedef struct {
    int       fd;
    int       port;                 /* UDP; 3702 but in tests */
    wif_t     f[IFACE_MAX];
    int       nf;
    onvif_wsd_t w;
    long      bucket_at;
    int       bucket;
} wsd_t;

static void wsd_xaddr(const wsd_t *s, uint32_t addr, int port, char *out, size_t cap)
{
    (void)s;
    struct in_addr a = { htonl(addr) };
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &a, ip, sizeof ip);
    snprintf(out, cap, "http://%s:%d/onvif/device_service", ip, port);
}

static void wsd_send_all(wsd_t *s, int bye, int port)
{
    char *buf = malloc(4096), msgid[46], xa[96];
    if (!buf) return;
    for (int i = 0; i < s->nf; ++i) {
        wsd_xaddr(s, s->f[i].addr, port, xa, sizeof xa);
        s->w.xaddrs = xa;
        ++s->w.msgno;
        new_uuid(msgid);
        size_t n = bye ? onvif_wsd_bye(buf, 4096, &s->w, msgid) : onvif_wsd_hello(buf, 4096, &s->w, msgid);
        if (!n) continue;
        struct in_addr ia = { htonl(s->f[i].addr) };
        setsockopt(s->fd, IPPROTO_IP, IP_MULTICAST_IF, &ia, sizeof ia);
        struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons((uint16_t)s->port) };
        inet_pton(AF_INET, ONVIF_WSD_GROUP, &to.sin_addr);
        sendto(s->fd, buf, n, MSG_NOSIGNAL, (struct sockaddr *)&to, sizeof to);
    }
    free(buf);
}

/* Join the group on interfaces new since the last look. 1 when the set
 * of addresses changed. */
static int wsd_ifaces(wsd_t *s)
{
    wif_t f[IFACE_MAX];
    int n = wifs(f, IFACE_MAX), changed = n != s->nf;
    for (int i = 0; i < n; ++i) {
        int known = 0;
        for (int k = 0; k < s->nf && !known; ++k) known = s->f[k].index == f[i].index && s->f[k].addr == f[i].addr;
        if (known) continue;
        changed = 1;
        struct ip_mreqn m = { .imr_address.s_addr = htonl(f[i].addr), .imr_ifindex = (int)f[i].index };
        inet_pton(AF_INET, ONVIF_WSD_GROUP, &m.imr_multiaddr);
        if (setsockopt(s->fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof m) < 0 && errno != EADDRINUSE)
            vg_log("onvif: cannot join %s on interface %u: %s", ONVIF_WSD_GROUP, f[i].index, strerror(errno));
    }
    memcpy(s->f, f, sizeof f);
    s->nf = n;
    return changed;
}

static int wsd_open(wsd_t *s)
{
    s->fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s->fd < 0) return -1;
    int one = 1;
    /* Shared with other WS-Discovery tools on this box (multicast reaches
     * every socket bound to the port). */
    setsockopt(s->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(s->fd, IPPROTO_IP, IP_PKTINFO, &one, sizeof one);
    qos_mark(s->fd, QOS_MANAGEMENT, 1);
    unsigned char ttl = 1;
    setsockopt(s->fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)s->port) };
    if (bind(s->fd, (struct sockaddr *)&a, sizeof a) < 0) {
        vg_log("onvif: cannot listen for WS-Discovery on UDP %d: %s; clients must be given the address",
               s->port, strerror(errno));
        close(s->fd);
        s->fd = -1;
        return -1;
    }
    s->nf = 0;
    wsd_ifaces(s);
    return 0;
}

static void wsd_close(wsd_t *s)
{
    if (s->fd >= 0) close(s->fd);
    s->fd = -1;
    s->nf = 0;
}

/* Datagrams waiting on the socket: probes answered. */
static void wsd_read(wsd_t *s, int port)
{
    char *buf = malloc(8192), *out = malloc(4096);
    onvif_probe_t *p = malloc(sizeof *p);
    for (int burst = 0; buf && out && p && burst < 16; ++burst) {
        struct sockaddr_in from;
        char cbuf[CMSG_SPACE(sizeof(struct in_pktinfo))];
        struct iovec iov = { .iov_base = buf, .iov_len = 8191 };
        struct msghdr m = { .msg_name = &from, .msg_namelen = sizeof from, .msg_iov = &iov, .msg_iovlen = 1,
                            .msg_control = cbuf, .msg_controllen = sizeof cbuf };
        ssize_t n = recvmsg(s->fd, &m, MSG_DONTWAIT);
        if (n <= 0) break;
        buf[n] = 0;
        if (onvif_wsd_probe_parse(buf, (size_t)n, p) < 0 || !onvif_wsd_match(buf, (size_t)n, p, g_id.scopes))
            continue;
        /* Answered from the address it came to: the probe's interface. */
        uint32_t local = 0;
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
            if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_PKTINFO) {
                struct in_pktinfo pi;
                memcpy(&pi, CMSG_DATA(c), sizeof pi);
                local = ntohl(pi.ipi_spec_dst.s_addr);
            }
        if (!local) continue;
        long now = mono();
        if (now != s->bucket_at) {
            s->bucket_at = now;
            s->bucket = 0;
        }
        if (++s->bucket > PROBE_RATE) continue;     /* a flood is not answered in full */
        char xa[96], msgid[46];
        wsd_xaddr(s, local, port, xa, sizeof xa);
        s->w.xaddrs = xa;
        ++s->w.msgno;
        new_uuid(msgid);
        size_t len = onvif_wsd_probe_matches(out, 4096, &s->w, msgid, p->msgid);
        if (len) sendto(s->fd, out, len, MSG_NOSIGNAL, (struct sockaddr *)&from, sizeof from);
    }
    free(buf);
    free(out);
    free(p);
}

/* ---- the supervisor ------------------------------------------------------------------------------ */

static void *supervisor(void *arg)
{
    (void)arg;
    wsd_t s;
    memset(&s, 0, sizeof s);
    s.fd = -1;
    s.port = ONVIF_WSD_PORT;
    /* Tests run several vigild side by side: each on a port of its own. */
    const char *tp = getenv("VIGIL_ONVIF_WSD_PORT");
    if (tp && atoi(tp) > 0 && atoi(tp) < 65536) s.port = atoi(tp);
    s.w.uuid = g_id.uuid;
    s.w.scopes = g_id.scopes;
    s.w.instance = (unsigned)time(NULL);
    s.w.metadata = 1;
    int failed_port = 0;
    long failed_at = 0, ifaces_at = 0;
    vigil_config_t *c = malloc(sizeof *c);
    while (c && !atomic_load(&g_stop)) {
        config_get(c);
        int on = c->onvif_server.enabled, port = c->onvif_server.port;
        long now = mono();
        if (!on && !g_ctx) api_server_state(2, "", "off: onvif_server.enabled is off");
        if (g_ctx && (!on || port != atomic_load(&g_port))) {
            if (s.fd >= 0) wsd_send_all(&s, 1, atomic_load(&g_port));
            http_stop();
            vg_log("onvif: %s", on ? "moving to another port" : "off");
        }
        if (on && !g_ctx && (port != failed_port || now - failed_at >= 30)) {
            if (http_start(port) < 0) {
                if (port != failed_port) vg_log("onvif: cannot listen on port %d; trying again every 30 s", port);
                failed_port = port;
                failed_at = now;
            } else {
                failed_port = 0;
                if (s.fd < 0) wsd_open(&s);
                ++s.w.metadata;
                if (s.fd >= 0) wsd_send_all(&s, 0, port);
                vg_log("onvif: device service at http://<this recorder>:%d/onvif/device_service%s", port,
                       s.fd >= 0 ? ", discoverable" : "");
            }
        }
        if (!g_ctx && s.fd >= 0) wsd_close(&s);
        int want_port = atomic_load(&g_port);
        if (s.fd >= 0 && now - ifaces_at >= 10) {
            ifaces_at = now;
            if (wsd_ifaces(&s)) {
                ++s.w.metadata;
                wsd_send_all(&s, 0, want_port);
            }
        }
        struct pollfd pf = { .fd = s.fd, .events = POLLIN };
        if (s.fd >= 0) {
            if (poll(&pf, 1, 500) > 0) wsd_read(&s, want_port);
        } else {
            usleep(500 * 1000);
        }
    }
    if (s.fd >= 0 && g_ctx) wsd_send_all(&s, 1, atomic_load(&g_port));
    http_stop();
    wsd_close(&s);
    free(c);
    return NULL;
}

int onvif_start(void)
{
    if (g_thread_up) return 0;
    ident_init(&g_id);
    random_bytes(g_key, sizeof g_key);
    api_auth_init();
    disc_set_self(g_id.uuid);
    mg_init_library(0);
    atomic_store(&g_stop, 0);
    g_thread_up = pthread_create(&g_thread, NULL, supervisor, NULL) == 0;
    if (!g_thread_up) {
        vg_log("onvif: cannot start its thread; no ONVIF server");
        mg_exit_library();
        return -1;
    }
    return 0;
}

void onvif_stop(void)
{
    if (!g_thread_up) return;
    atomic_store(&g_stop, 1);
    pthread_join(g_thread, NULL);
    g_thread_up = 0;
    mg_exit_library();
}
