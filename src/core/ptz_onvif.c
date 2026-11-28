/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ptz_onvif.c — PTZ over ONVIF: the PTZ service for pan, tilt, zoom,
 * presets, tours and home, the Imaging service for focus. Every call
 * through disc_ops.h's ONVIF client (SOAP 1.2, WS-UsernameToken in the
 * camera's own clock, HTTP Digest when asked for), with the user and
 * password of the camera's URL.
 *
 * Finding it: the device service at cameras[].onvif_url, else the address
 * discovery heard, else http://host/onvif/device_service, then port 8899
 * (XM's), as onvif_events.c does; GetCapabilities (All) for the Media, PTZ
 * and Imaging addresses (GetServices when it has no GetCapabilities);
 * Media GetProfiles: the first profile with a PTZConfiguration (the main
 * stream's, on a camera), its video source and PTZ node; PTZ GetNode:
 * ContinuousPanTiltVelocitySpace, ContinuousZoomVelocitySpace,
 * MaximumNumberOfPresets, HomeSupported, SupportedPresetTour; Imaging
 * GetMoveOptions (Continuous: focus) and GetOptions (AutoFocusModes AUTO).
 *
 * Moving: ContinuousMove with Velocity PanTilt x (+ right), y (+ up) and
 * Zoom x (+ in), and Timeout PT2S, sent again every second while the move
 * lasts, so a camera that stops hearing from vigild stops by itself (a
 * camera that refuses a Timeout gets none); Stop (PanTilt and Zoom).
 * Imaging Move Continuous Speed (+ far) and Stop. Presets: GetPresets,
 * GotoPreset, SetPreset (PresetName; the camera gives the token),
 * RemovePreset; GotoHomePosition; GetPresetTours, OperatePresetTour
 * Start/Stop. Iris has no continuous move in ONVIF: not offered.
 * Sources: ONVIF PTZ Service Specification 2.x, Imaging Service
 * Specification, Media Service Specification.
 */
#define _DEFAULT_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "disc_ops.h"
#include "disc_proto.h"
#include "discovery.h"
#include "ptz.h"
#include "source.h"

#define CALL_MS      4000
#define MOVE_TIMEOUT 2              /* s: the camera's own end of a move */
#define TPTZ "xmlns:tptz=\"http://www.onvif.org/ver20/ptz/wsdl\""
#define TIMG "xmlns:timg=\"http://www.onvif.org/ver20/imaging/wsdl\""
#define NS_PTZ  "http://www.onvif.org/ver20/ptz/wsdl"
#define NS_IMG  "http://www.onvif.org/ver20/imaging/wsdl"
#define NS_MEDIA "http://www.onvif.org/ver10/media/wsdl"

typedef struct {
    char host[128], user[64], pass[64], onvif_url[160];
    const volatile int *quit;
    disc_onvif_t dev, ptz, img, media;
    int  have_ptz, have_img;
    char profile[64], vsrc[64], node[64];
    unsigned caps;
    int  no_timeout;                /* the camera refused ContinuousMove's Timeout */
    char tours[PTZ_TOURS_MAX][64];
    int  n_tours;
} onvif_ptz_t;

static void wipe(void *p, size_t n)
{
    volatile unsigned char *q = p;
    while (n--) *q++ = 0;
}

static void xml_escape(const char *s, char *out, size_t cap)
{
    size_t o = 0;
    for (; *s && o + 7 < cap; ++s) {
        const char *e = *s == '<' ? "&lt;" : *s == '>' ? "&gt;" : *s == '&' ? "&amp;" : *s == '"' ? "&quot;"
                      : *s == '\'' ? "&apos;" : NULL;
        if (e) {
            size_t n = strlen(e);
            memcpy(out + o, e, n);
            o += n;
        } else {
            out[o++] = *s;
        }
    }
    out[o] = 0;
}

/* ---- the wire (pure, tested) ----------------------------------------------------------- */

/* The service at `ns` (GetServices), or the capability element `cap_el`'s
 * XAddr (GetCapabilities). */
static int service_addr(const char *xml, size_t len, const char *cap_el, const char *ns, char *out, size_t cap)
{
    size_t c0, c1, after, from = 0;
    out[0] = 0;
    if (disc_xml_elem(xml, len, cap_el, 0, NULL, &c0, &c1, NULL) == 0 &&
        disc_xml_text(xml + c0, c1 - c0, "XAddr", out, cap, NULL) == 0 && !strncasecmp(out, "http://", 7))
        return 0;
    while (disc_xml_elem(xml, len, "Service", from, NULL, &c0, &c1, &after) == 0) {
        char n[96];
        if (disc_xml_text(xml + c0, c1 - c0, "Namespace", n, sizeof n, NULL) == 0 && !strcmp(n, ns) &&
            disc_xml_text(xml + c0, c1 - c0, "XAddr", out, cap, NULL) == 0 && !strncasecmp(out, "http://", 7))
            return 0;
        from = after;
    }
    out[0] = 0;
    return -1;
}

int ptz_onvif_services(const char *xml, size_t len, char *media, char *ptz, char *imaging, size_t cap)
{
    int m = service_addr(xml, len, "Media", NS_MEDIA, media, cap);
    int p = service_addr(xml, len, "PTZ", NS_PTZ, ptz, cap);
    int i = service_addr(xml, len, "Imaging", NS_IMG, imaging, cap);
    return m == 0 && (p == 0 || i == 0) ? 0 : -1;
}

int ptz_onvif_profile(const char *xml, size_t len, char *profile, char *vsrc, char *node, size_t cap)
{
    size_t tag, c0, c1, after, from = 0;
    int first = 1;
    profile[0] = vsrc[0] = node[0] = 0;
    while (disc_xml_elem(xml, len, "Profiles", from, &tag, &c0, &c1, &after) == 0) {
        char tok[64] = "", src[64] = "", nd[64] = "";
        size_t at = tag;
        disc_xml_attr(xml, len, "Profiles", "token", tok, sizeof tok, &at);
        size_t v0, v1;
        if (disc_xml_elem(xml + c0, c1 - c0, "VideoSourceConfiguration", 0, NULL, &v0, &v1, NULL) == 0)
            disc_xml_text(xml + c0 + v0, v1 - v0, "SourceToken", src, sizeof src, NULL);
        int has_ptz = disc_xml_elem(xml + c0, c1 - c0, "PTZConfiguration", 0, NULL, &v0, &v1, NULL) == 0;
        if (has_ptz) disc_xml_text(xml + c0 + v0, v1 - v0, "NodeToken", nd, sizeof nd, NULL);
        if (tok[0] && (first || has_ptz)) {
            snprintf(profile, cap, "%s", tok);
            snprintf(vsrc, cap, "%s", src);
            snprintf(node, cap, "%s", nd);
            first = 0;
            if (has_ptz) return 0;
        }
        from = after;
    }
    return profile[0] ? 1 : -1;
}

static int has_elem(const char *xml, size_t len, const char *local)
{
    return disc_xml_elem(xml, len, local, 0, NULL, NULL, NULL, NULL) == 0;
}

unsigned ptz_onvif_node_caps(const char *xml, size_t len, int *max_tours)
{
    unsigned caps = 0;
    char v[32];
    if (has_elem(xml, len, "ContinuousPanTiltVelocitySpace")) caps |= PTZ_CAP_PAN_TILT;
    if (has_elem(xml, len, "ContinuousZoomVelocitySpace")) caps |= PTZ_CAP_ZOOM;
    if (disc_xml_text(xml, len, "MaximumNumberOfPresets", v, sizeof v, NULL) == 0 && atoi(v) > 0)
        caps |= PTZ_CAP_PRESETS;
    if (disc_xml_text(xml, len, "HomeSupported", v, sizeof v, NULL) == 0 && !strcasecmp(v, "true"))
        caps |= PTZ_CAP_HOME;
    *max_tours = 0;
    size_t c0, c1;
    if (disc_xml_elem(xml, len, "SupportedPresetTour", 0, NULL, &c0, &c1, NULL) == 0 &&
        disc_xml_text(xml + c0, c1 - c0, "MaximumNumberOfPresetTour", v, sizeof v, NULL) == 0)
        *max_tours = atoi(v);
    return caps;
}

int ptz_onvif_presets(const char *xml, size_t len, ptz_preset_t *out, int max)
{
    size_t tag, c0, c1, after, from = 0;
    int n = 0;
    while (n < max && disc_xml_elem(xml, len, "Preset", from, &tag, &c0, &c1, &after) == 0) {
        size_t at = tag;
        ptz_preset_t p = { "", "" };
        if (disc_xml_attr(xml, len, "Preset", "token", p.token, sizeof p.token, &at) == 0 && p.token[0]) {
            disc_xml_text(xml + c0, c1 - c0, "Name", p.name, sizeof p.name, NULL);
            out[n++] = p;
        }
        from = after;
    }
    return n;
}

static void num(char *out, size_t cap, double v)
{
    if (fabs(v) < 0.0005) v = 0;
    snprintf(out, cap, "%.3f", v);
}

int ptz_onvif_move_body(char *out, size_t cap, const char *profile, double pan, double tilt, double zoom,
                        unsigned caps, int timeout_s)
{
    char tok[96], x[16], y[16], z[16], pt[96] = "", zm[64] = "", to[48] = "";
    xml_escape(profile, tok, sizeof tok);
    num(x, sizeof x, pan);
    num(y, sizeof y, tilt);
    num(z, sizeof z, zoom);
    if (caps & PTZ_CAP_PAN_TILT) snprintf(pt, sizeof pt, "<tt:PanTilt x=\"%s\" y=\"%s\"/>", x, y);
    if (caps & PTZ_CAP_ZOOM) snprintf(zm, sizeof zm, "<tt:Zoom x=\"%s\"/>", z);
    if (timeout_s > 0) snprintf(to, sizeof to, "<tptz:Timeout>PT%dS</tptz:Timeout>", timeout_s);
    int n = snprintf(out, cap,
                     "<tptz:ContinuousMove " TPTZ "><tptz:ProfileToken>%s</tptz:ProfileToken>"
                     "<tptz:Velocity>%s%s</tptz:Velocity>%s</tptz:ContinuousMove>",
                     tok, pt, zm, to);
    return n > 0 && (size_t)n < cap ? n : -1;
}

/* ---- calls ---------------------------------------------------------------------------- */

static int call(onvif_ptz_t *o, disc_onvif_t *svc, const char *body, const char *what, disc_http_t *r, char *err,
                size_t cap)
{
    disc_http_t tmp;
    disc_http_t *rr = r ? r : &tmp;
    disc_rc_t rc = disc_onvif_call(svc, NULL, NULL, body, CALL_MS, rr, what, err, cap);
    (void)o;
    if (rc != DISC_OK) return rc == DISC_EUNSUPPORTED ? PTZ_ENOTSUP : -1;
    if (!r) free(tmp.body);
    return 0;
}

/* The service at xaddr, as the device service is reached. */
static int service(onvif_ptz_t *o, disc_onvif_t *svc, const char *xaddr)
{
    if (disc_onvif_init(svc, xaddr, o->onvif_url[0] ? NULL : o->host, o->user, o->pass) < 0) return -1;
    svc->clock_offset = o->dev.clock_offset;
    svc->stop = o->quit;
    return 0;
}

/* The device service, and from it the Media, PTZ and Imaging ones. */
static int find(onvif_ptz_t *o, char *err, size_t cap)
{
    char cand[3][192];
    int n = 0;
    if (o->onvif_url[0]) {
        snprintf(cand[n++], sizeof cand[0], "%s", o->onvif_url);
    } else {
        char x[160];
        if (disc_xaddr_of(o->host, x, sizeof x) == 0 && !strncasecmp(x, "http://", 7))
            snprintf(cand[n++], sizeof cand[0], "%s", x);
        snprintf(cand[n++], sizeof cand[0], "http://%s/onvif/device_service", o->host);
        snprintf(cand[n++], sizeof cand[0], "http://%s:8899/onvif/device_service", o->host);
    }
    snprintf(err, cap, "no ONVIF device service");
    for (int i = 0; i < n && !(o->quit && *o->quit); ++i) {
        if (disc_onvif_init(&o->dev, cand[i], o->onvif_url[0] ? NULL : o->host, o->user, o->pass) < 0) continue;
        o->dev.stop = o->quit;
        disc_onvif_clock(&o->dev);
        disc_http_t r;
        char e[128];
        int rc = call(o, &o->dev, "<tds:GetCapabilities><tds:Category>All</tds:Category></tds:GetCapabilities>",
                      "GetCapabilities", &r, e, sizeof e);
        char media[192], ptz[192], img[192];
        int ok = rc == 0 && ptz_onvif_services(r.body, r.len, media, ptz, img, sizeof media) == 0;
        if (rc == 0) free(r.body);
        if (!ok && rc != -1) {
            /* No GetCapabilities, or not all in it: GetServices. */
            if (call(o, &o->dev, "<tds:GetServices><tds:IncludeCapability>false</tds:IncludeCapability>"
                                 "</tds:GetServices>", "GetServices", &r, e, sizeof e) == 0) {
                ok = ptz_onvif_services(r.body, r.len, media, ptz, img, sizeof media) == 0;
                free(r.body);
                rc = 0;
            }
        }
        if (rc != 0) {
            snprintf(err, cap, "%.70s: %s", cand[i], e);
            if (strstr(e, "not authorized")) return -1;     /* the answer at every address */
            continue;
        }
        if (!ok) {
            snprintf(err, cap, "no PTZ or Imaging service");
            return 1;
        }
        if (service(o, &o->media, media) < 0) continue;
        o->have_ptz = ptz[0] && service(o, &o->ptz, ptz) == 0;
        o->have_img = img[0] && service(o, &o->img, img) == 0;
        return 0;
    }
    return -1;
}

/* ---- the backend ---------------------------------------------------------------------- */

static void *create(const ptz_camera_t *cam, const volatile int *quit, char *err, size_t cap)
{
    onvif_ptz_t *o = calloc(1, sizeof *o);
    if (!o) return NULL;
    char scheme[16];
    const char *sep = strstr(cam->url, "://"), *path;
    int port = 0;
    if (!sep || (size_t)(sep - cam->url) >= sizeof scheme) {
        snprintf(err, cap, "not a camera URL ONVIF can use");
        free(o);
        return NULL;
    }
    memcpy(scheme, cam->url, (size_t)(sep - cam->url));
    scheme[sep - cam->url] = 0;
    if (url_split(cam->url, scheme, o->user, sizeof o->user, o->pass, sizeof o->pass, o->host, sizeof o->host, &port,
                  &path) < 0 || !o->host[0]) {
        snprintf(err, cap, "not a camera URL ONVIF can use");
        free(o);
        return NULL;
    }
    if (!o->user[0]) snprintf(o->user, sizeof o->user, "admin");
    snprintf(o->onvif_url, sizeof o->onvif_url, "%s", cam->onvif_url);
    o->quit = quit;
    return o;
}

static void destroy(void *v)
{
    wipe(v, sizeof(onvif_ptz_t));
    free(v);
}

static int presets(void *v, ptz_preset_t *p, int *n, char *err, size_t cap)
{
    onvif_ptz_t *o = v;
    char body[256], tok[96];
    disc_http_t r;
    xml_escape(o->profile, tok, sizeof tok);
    snprintf(body, sizeof body, "<tptz:GetPresets " TPTZ "><tptz:ProfileToken>%s</tptz:ProfileToken></tptz:GetPresets>",
             tok);
    int rc = call(o, &o->ptz, body, "GetPresets", &r, err, cap);
    if (rc) return rc;
    *n = ptz_onvif_presets(r.body, r.len, p, PTZ_PRESETS_MAX);
    free(r.body);
    return 0;
}

static int probe(void *v, unsigned *caps, int *tours, ptz_preset_t *p, int *n_presets, char *err, size_t cap)
{
    onvif_ptz_t *o = v;
    *caps = 0;
    *tours = 0;
    *n_presets = -1;
    int rc = find(o, err, cap);
    if (rc) return rc;
    disc_http_t r;
    if (call(o, &o->media, "<trt:GetProfiles/>", "GetProfiles", &r, err, cap) != 0) return -1;
    int has_ptz_cfg = ptz_onvif_profile(r.body, r.len, o->profile, o->vsrc, o->node, sizeof o->profile) == 0;
    free(r.body);
    if (!o->profile[0]) {
        snprintf(err, cap, "GetProfiles: no media profile");
        return 1;
    }
    char body[512], tok[96];
    if (o->have_ptz && has_ptz_cfg) {
        int max_tours = 0;
        if (o->node[0]) {
            xml_escape(o->node, tok, sizeof tok);
            snprintf(body, sizeof body, "<tptz:GetNode " TPTZ "><tptz:NodeToken>%s</tptz:NodeToken></tptz:GetNode>", tok);
            rc = call(o, &o->ptz, body, "GetNode", &r, err, cap);
        } else {
            rc = call(o, &o->ptz, "<tptz:GetNodes " TPTZ "/>", "GetNodes", &r, err, cap);
        }
        if (rc == 0) {
            *caps |= ptz_onvif_node_caps(r.body, r.len, &max_tours);
            free(r.body);
        }
        if ((*caps & PTZ_CAP_PRESETS) && presets(o, p, n_presets, err, cap) != 0) *n_presets = 0;
        if (max_tours > 0) {
            xml_escape(o->profile, tok, sizeof tok);
            snprintf(body, sizeof body,
                     "<tptz:GetPresetTours " TPTZ "><tptz:ProfileToken>%s</tptz:ProfileToken></tptz:GetPresetTours>",
                     tok);
            if (call(o, &o->ptz, body, "GetPresetTours", &r, err, cap) == 0) {
                size_t tag, from = 0, after;
                while (o->n_tours < PTZ_TOURS_MAX &&
                       disc_xml_elem(r.body, r.len, "PresetTour", from, &tag, NULL, NULL, &after) == 0) {
                    size_t at = tag;
                    if (disc_xml_attr(r.body, r.len, "PresetTour", "token", o->tours[o->n_tours],
                                      sizeof o->tours[0], &at) == 0 && o->tours[o->n_tours][0])
                        ++o->n_tours;
                    from = after;
                }
                free(r.body);
            }
            if (o->n_tours) {
                *caps |= PTZ_CAP_TOURS;
                *tours = o->n_tours;
            }
        }
    }
    if (o->have_img && o->vsrc[0]) {
        xml_escape(o->vsrc, tok, sizeof tok);
        snprintf(body, sizeof body, "<timg:GetMoveOptions " TIMG "><timg:VideoSourceToken>%s</timg:VideoSourceToken>"
                                    "</timg:GetMoveOptions>", tok);
        char e2[96];
        if (call(o, &o->img, body, "GetMoveOptions", &r, e2, sizeof e2) == 0) {
            if (has_elem(r.body, r.len, "Continuous")) *caps |= PTZ_CAP_FOCUS;
            free(r.body);
        }
        snprintf(body, sizeof body, "<timg:GetOptions " TIMG "><timg:VideoSourceToken>%s</timg:VideoSourceToken>"
                                    "</timg:GetOptions>", tok);
        if (call(o, &o->img, body, "GetOptions", &r, e2, sizeof e2) == 0) {
            size_t c0, c1;
            if (disc_xml_elem(r.body, r.len, "Focus", 0, NULL, &c0, &c1, NULL) == 0) {
                char m[16];
                size_t f = c0;
                while (disc_xml_text(r.body, c1, "AutoFocusModes", m, sizeof m, &f) == 0)
                    if (!strcasecmp(m, "AUTO")) *caps |= PTZ_CAP_FOCUS_AUTO;
            }
            free(r.body);
        }
    }
    o->caps = *caps;
    if (!*caps) {
        if (o->have_ptz) snprintf(err, cap, "its profile %s has no PTZ configuration", o->profile);
        else snprintf(err, cap, "no PTZ service");
        return 1;
    }
    err[0] = 0;
    return 0;
}

static int ptz_stop_call(onvif_ptz_t *o, char *err, size_t cap)
{
    char body[320], tok[96];
    xml_escape(o->profile, tok, sizeof tok);
    snprintf(body, sizeof body,
             "<tptz:Stop " TPTZ "><tptz:ProfileToken>%s</tptz:ProfileToken><tptz:PanTilt>true</tptz:PanTilt>"
             "<tptz:Zoom>true</tptz:Zoom></tptz:Stop>", tok);
    return call(o, &o->ptz, body, "Stop", NULL, err, cap);
}

static int motion(void *v, const double from[PTZ_AXES], const double to[PTZ_AXES], char *err, size_t cap)
{
    onvif_ptz_t *o = v;
    char body[512], tok[96];
    int ptz_from = fabs(from[0]) > 0.01 || fabs(from[1]) > 0.01 || fabs(from[2]) > 0.01;
    int ptz_to = fabs(to[0]) > 0.01 || fabs(to[1]) > 0.01 || fabs(to[2]) > 0.01;
    int rc = 0;
    if (fabs(from[PTZ_AX_FOCUS]) > 0.01 && fabs(to[PTZ_AX_FOCUS]) <= 0.01) {
        xml_escape(o->vsrc, tok, sizeof tok);
        snprintf(body, sizeof body, "<timg:Stop " TIMG "><timg:VideoSourceToken>%s</timg:VideoSourceToken></timg:Stop>",
                 tok);
        rc = call(o, &o->img, body, "Imaging Stop", NULL, err, cap);
    }
    if (ptz_from && !ptz_to) {
        int r2 = ptz_stop_call(o, err, cap);
        if (r2) rc = r2;
    }
    if (ptz_to) {
        ptz_onvif_move_body(body, sizeof body, o->profile, to[0], to[1], to[2], o->caps, o->no_timeout ? 0 : MOVE_TIMEOUT);
        rc = call(o, &o->ptz, body, "ContinuousMove", NULL, err, cap);
        if (rc == -1 && !o->no_timeout && (strstr(err, "Timeout") || strstr(err, "InvalidArgVal"))) {
            o->no_timeout = 1;
            ptz_onvif_move_body(body, sizeof body, o->profile, to[0], to[1], to[2], o->caps, 0);
            rc = call(o, &o->ptz, body, "ContinuousMove", NULL, err, cap);
        }
        if (rc) return rc;
    }
    if (fabs(to[PTZ_AX_FOCUS]) > 0.01) {
        char sp[16];
        num(sp, sizeof sp, to[PTZ_AX_FOCUS]);
        xml_escape(o->vsrc, tok, sizeof tok);
        snprintf(body, sizeof body,
                 "<timg:Move " TIMG "><timg:VideoSourceToken>%s</timg:VideoSourceToken><timg:Focus><tt:Continuous>"
                 "<tt:Speed>%s</tt:Speed></tt:Continuous></timg:Focus></timg:Move>", tok, sp);
        rc = call(o, &o->img, body, "Imaging Move", NULL, err, cap);
    }
    return rc;
}

static int preset(void *v, int op, const char *token, const char *name, char *out, size_t ocap, char *err,
                  size_t cap)
{
    onvif_ptz_t *o = v;
    char body[640], tok[96], pt[96], nm[160] = "";
    xml_escape(o->profile, tok, sizeof tok);
    xml_escape(token, pt, sizeof pt);
    if (op == PTZ_PRESET_GOTO) {
        snprintf(body, sizeof body,
                 "<tptz:GotoPreset " TPTZ "><tptz:ProfileToken>%s</tptz:ProfileToken><tptz:PresetToken>%s"
                 "</tptz:PresetToken></tptz:GotoPreset>", tok, pt);
        snprintf(out, ocap, "%s", token);
        return call(o, &o->ptz, body, "GotoPreset", NULL, err, cap);
    }
    if (op == PTZ_PRESET_REMOVE) {
        snprintf(body, sizeof body,
                 "<tptz:RemovePreset " TPTZ "><tptz:ProfileToken>%s</tptz:ProfileToken><tptz:PresetToken>%s"
                 "</tptz:PresetToken></tptz:RemovePreset>", tok, pt);
        snprintf(out, ocap, "%s", token);
        return call(o, &o->ptz, body, "RemovePreset", NULL, err, cap);
    }
    char esc[96];
    if (name && name[0]) {
        xml_escape(name, esc, sizeof esc);
        snprintf(nm, sizeof nm, "<tptz:PresetName>%s</tptz:PresetName>", esc);
    }
    char ptok[160] = "";
    if (token[0]) snprintf(ptok, sizeof ptok, "<tptz:PresetToken>%s</tptz:PresetToken>", pt);
    snprintf(body, sizeof body,
             "<tptz:SetPreset " TPTZ "><tptz:ProfileToken>%s</tptz:ProfileToken>%s%s</tptz:SetPreset>", tok, nm, ptok);
    disc_http_t r;
    int rc = call(o, &o->ptz, body, "SetPreset", &r, err, cap);
    if (rc) return rc;
    if (disc_xml_text(r.body, r.len, "PresetToken", out, ocap, NULL) < 0) snprintf(out, ocap, "%s", token);
    free(r.body);
    if (!out[0]) {
        snprintf(err, cap, "SetPreset: no token in the answer");
        return -1;
    }
    return 0;
}

static int home(void *v, char *err, size_t cap)
{
    onvif_ptz_t *o = v;
    char body[256], tok[96];
    xml_escape(o->profile, tok, sizeof tok);
    snprintf(body, sizeof body,
             "<tptz:GotoHomePosition " TPTZ "><tptz:ProfileToken>%s</tptz:ProfileToken></tptz:GotoHomePosition>", tok);
    return call(o, &o->ptz, body, "GotoHomePosition", NULL, err, cap);
}

static int tour(void *v, int n, int start, char *err, size_t cap)
{
    onvif_ptz_t *o = v;
    if (n < 1 || n > o->n_tours) {
        snprintf(err, cap, "no tour %d", n);
        return -1;
    }
    char body[512], tok[96], tt[96];
    xml_escape(o->profile, tok, sizeof tok);
    xml_escape(o->tours[n - 1], tt, sizeof tt);
    snprintf(body, sizeof body,
             "<tptz:OperatePresetTour " TPTZ "><tptz:ProfileToken>%s</tptz:ProfileToken><tptz:PresetTourToken>%s"
             "</tptz:PresetTourToken><tptz:Operation>%s</tptz:Operation></tptz:OperatePresetTour>",
             tok, tt, start ? "Start" : "Stop");
    return call(o, &o->ptz, body, "OperatePresetTour", NULL, err, cap);
}

static int focus_auto(void *v, char *err, size_t cap)
{
    onvif_ptz_t *o = v;
    char body[512], tok[96];
    xml_escape(o->vsrc, tok, sizeof tok);
    snprintf(body, sizeof body,
             "<timg:SetImagingSettings " TIMG "><timg:VideoSourceToken>%s</timg:VideoSourceToken><timg:ImagingSettings>"
             "<tt:Focus><tt:AutoFocusMode>AUTO</tt:AutoFocusMode></tt:Focus></timg:ImagingSettings>"
             "<timg:ForcePersistence>false</timg:ForcePersistence></timg:SetImagingSettings>", tok);
    return call(o, &o->img, body, "SetImagingSettings", NULL, err, cap);
}

const ptz_drv_t ptz_drv_onvif = {
    .name = "onvif",
    .create = create,
    .destroy = destroy,
    .probe = probe,
    .motion = motion,
    .preset = preset,
    .presets = presets,
    .home = home,
    .tour = tour,
    .focus_auto = focus_auto,
    .refresh_ms = 1000,
};
