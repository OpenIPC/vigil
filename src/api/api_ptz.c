/* SPDX-License-Identifier: GPL-3.0-or-later */
/* api_ptz.c — a channel's PTZ over the API (docs/api.md, PTZ):
 *
 *   GET  /api/v1/channels/N/ptz[?wait=MS]   state, capabilities, presets
 *   POST /api/v1/channels/N/ptz             a move, a stop, a preset, home, a tour
 *
 * The core's worker sends the commands (ptz.h); a request only queues
 * them and answers at once (202 with the state then). A continuous move
 * lasts its timeout (1 s by default, at most 10 s) unless sent again: a
 * client that goes away leaves nothing moving. Permission: ptz. */
#define _DEFAULT_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "api_int.h"
#include "cJSON.h"
#include "civetweb.h"
#include "ptz.h"

static cJSON *info_json(int ch)
{
    ptz_info_t i;
    if (ptz_info(ch - 1, &i) < 0) return NULL;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "channel", ch);
    cJSON_AddStringToObject(o, "state", ptz_state_name(i.state));
    cJSON_AddStringToObject(o, "backend", ptz_backend_name(i.backend));
    cJSON *caps = cJSON_AddArrayToObject(o, "capabilities");
    for (int b = 0; b < PTZ_N_CAPS; ++b)
        if (i.caps & (1u << b)) cJSON_AddItemToArray(caps, cJSON_CreateString(ptz_cap_name(b)));
    cJSON *p = cJSON_AddArrayToObject(o, "presets");
    for (int k = 0; k < i.n_presets; ++k) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "id", i.presets[k].token);
        cJSON_AddStringToObject(e, "name", i.presets[k].name);
        cJSON_AddItemToArray(p, e);
    }
    cJSON_AddNumberToObject(o, "tours", i.tours);
    cJSON_AddBoolToObject(o, "moving", i.moving);
    cJSON_AddStringToObject(o, "error", i.error);
    return o;
}

static int send_rc(struct mg_connection *conn, int rc, const char *what)
{
    char msg[128];
    snprintf(msg, sizeof msg, "%s: %s", what, ptz_rc_text(rc));
    int status = rc == PTZ_EINVAL ? 400 : rc == PTZ_EBUSY ? 503 : 409;
    return send_error(conn, status, msg, NULL);
}

/* -1..1, or -2 when not a number in that range. */
static double unit(const cJSON *v)
{
    if (!v) return 0;
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble < -1 || v->valuedouble > 1) return -2;
    return v->valuedouble;
}

/* A preset's id: a number or a string. */
static int preset_id(const cJSON *v, char *out, size_t cap)
{
    if (cJSON_IsNumber(v) && v->valuedouble == (double)(int)v->valuedouble && v->valueint >= 0)
        snprintf(out, cap, "%d", v->valueint);
    else if (cJSON_IsString(v) && v->valuestring[0] && strlen(v->valuestring) < cap)
        snprintf(out, cap, "%s", v->valuestring);
    else
        return -1;
    return 0;
}

static int do_get(struct mg_connection *conn, int ch)
{
    const struct mg_request_info *ri = mg_get_request_info(conn);
    char v[16] = "";
    int wait = 0;
    if (ri->query_string && mg_get_var(ri->query_string, strlen(ri->query_string), "wait", v, sizeof v) > 0)
        wait = atoi(v);
    wait = wait < 0 ? 0 : wait > 10000 ? 10000 : wait;
    /* ?wait: until the camera has said what it can do (or that long). */
    for (int t = 0;; t += 50) {
        ptz_info_t i;
        if (ptz_info(ch - 1, &i) < 0) break;
        if (i.state != PTZ_PROBING || t >= wait) break;
        struct timespec ts = { 0, 50 * 1000000L };
        nanosleep(&ts, NULL);
    }
    cJSON *o = info_json(ch);
    return o ? send_json(conn, 200, o) : send_error(conn, 404, "no such channel", NULL);
}

static int do_post(struct mg_connection *conn, int ch)
{
    size_t len = 0;
    int status = 0;
    char *body = read_body(conn, &len, &status);
    if (!body) return send_error(conn, status, status == 413 ? "body too large" : "out of memory", NULL);
    cJSON *b = len ? cJSON_ParseWithLength(body, len) : NULL;
    free(body);
    if (!cJSON_IsObject(b)) {
        cJSON_Delete(b);
        return send_error(conn, 400, "the body must be a JSON object", NULL);
    }
    const cJSON *move = cJSON_GetObjectItemCaseSensitive(b, "move");
    const cJSON *stop = cJSON_GetObjectItemCaseSensitive(b, "stop");
    const cJSON *focus = cJSON_GetObjectItemCaseSensitive(b, "focus");
    const cJSON *iris = cJSON_GetObjectItemCaseSensitive(b, "iris");
    const cJSON *preset = cJSON_GetObjectItemCaseSensitive(b, "preset");
    const cJSON *home = cJSON_GetObjectItemCaseSensitive(b, "home");
    const cJSON *tour = cJSON_GetObjectItemCaseSensitive(b, "tour");
    const cJSON *timeout = cJSON_GetObjectItemCaseSensitive(b, "timeout");
    int n_cont = (move != NULL) + (cJSON_IsTrue(stop)) + (focus != NULL) + (iris != NULL);
    int rc = PTZ_OK, any = 0;
    const char *what = "", *bad = NULL;
    int hold = 0;
    if (timeout) {
        if (!cJSON_IsNumber(timeout) || timeout->valuedouble < 100 || timeout->valuedouble > PTZ_HOLD_MAX_MS)
            bad = "timeout: 100..10000 ms";
        else hold = timeout->valueint;
    }
    for (const cJSON *k = b->child; k && !bad; k = k->next)
        if (strcmp(k->string, "move") && strcmp(k->string, "stop") && strcmp(k->string, "focus") &&
            strcmp(k->string, "iris") && strcmp(k->string, "preset") && strcmp(k->string, "home") &&
            strcmp(k->string, "tour") && strcmp(k->string, "timeout"))
            bad = "unknown member (move, stop, focus, iris, preset, home, tour, timeout)";
    if (!bad && n_cont > 1) bad = "one of move, stop, focus, iris at a time";
    if (!bad && stop && !cJSON_IsBool(stop)) bad = "stop: true";
    if (!bad && home && !cJSON_IsTrue(home)) bad = "home: true";

    if (!bad && cJSON_IsTrue(stop)) {
        what = "stop";
        rc = ptz_stop(ch - 1);
        any = 1;
    }
    if (!bad && move) {
        double p = unit(cJSON_GetObjectItemCaseSensitive(move, "pan"));
        double t = unit(cJSON_GetObjectItemCaseSensitive(move, "tilt"));
        double z = unit(cJSON_GetObjectItemCaseSensitive(move, "zoom"));
        const cJSON *mt = cJSON_GetObjectItemCaseSensitive(move, "timeout");
        if (!cJSON_IsObject(move) || p < -1.5 || t < -1.5 || z < -1.5) bad = "move: pan, tilt, zoom: -1..1";
        else if (mt && (!cJSON_IsNumber(mt) || mt->valuedouble < 100 || mt->valuedouble > PTZ_HOLD_MAX_MS))
            bad = "move.timeout: 100..10000 ms";
        else {
            what = "move";
            rc = ptz_move(ch - 1, p, t, z, mt ? mt->valueint : hold);
            any = 1;
        }
    }
    if (!bad && focus) {
        if (cJSON_IsString(focus) && !strcmp(focus->valuestring, "auto")) {
            what = "focus";
            rc = ptz_focus_auto(ch - 1);
        } else if (unit(focus) < -1.5) {
            bad = "focus: -1..1 (- near, + far) or \"auto\"";
        } else {
            what = "focus";
            rc = ptz_focus(ch - 1, unit(focus), hold);
        }
        any = 1;
    }
    if (!bad && iris) {
        if (unit(iris) < -1.5) bad = "iris: -1..1 (- close, + open)";
        else {
            what = "iris";
            rc = ptz_iris(ch - 1, unit(iris), hold);
            any = 1;
        }
    }
    if (!bad && rc == PTZ_OK && preset) {
        char id[PTZ_TOKEN_LEN] = "";
        const cJSON *go = cJSON_GetObjectItemCaseSensitive(preset, "goto");
        const cJSON *set = cJSON_GetObjectItemCaseSensitive(preset, "set");
        const cJSON *rm = cJSON_GetObjectItemCaseSensitive(preset, "remove");
        const cJSON *ren = cJSON_GetObjectItemCaseSensitive(preset, "rename");
        const cJSON *nm = cJSON_GetObjectItemCaseSensitive(preset, "name");
        int ops = (go != NULL) + (set != NULL) + (rm != NULL) + (ren != NULL);
        what = "preset";
        if (!cJSON_IsObject(preset) || ops != 1) bad = "preset: one of goto, set, remove, rename";
        else if (nm && !cJSON_IsString(nm)) bad = "preset.name: text";
        else if (go) rc = preset_id(go, id, sizeof id) < 0 ? PTZ_EINVAL : ptz_preset_goto(ch - 1, id);
        else if (rm) rc = preset_id(rm, id, sizeof id) < 0 ? PTZ_EINVAL : ptz_preset_remove(ch - 1, id);
        else if (ren) rc = !nm || preset_id(ren, id, sizeof id) < 0 ? PTZ_EINVAL
                                                                    : ptz_preset_rename(ch - 1, id, nm->valuestring);
        else rc = !cJSON_IsNull(set) && preset_id(set, id, sizeof id) < 0
                      ? PTZ_EINVAL
                      : ptz_preset_set(ch - 1, cJSON_IsNull(set) ? NULL : id, nm ? nm->valuestring : NULL);
        any = 1;
    }
    if (!bad && rc == PTZ_OK && home) {
        what = "home";
        rc = ptz_home(ch - 1);
        any = 1;
    }
    if (!bad && rc == PTZ_OK && tour) {
        const cJSON *st = cJSON_GetObjectItemCaseSensitive(tour, "start");
        const cJSON *sp = cJSON_GetObjectItemCaseSensitive(tour, "stop");
        what = "tour";
        if (!cJSON_IsObject(tour) || (st != NULL) + (sp != NULL) != 1 || !cJSON_IsNumber(st ? st : sp))
            bad = "tour: {\"start\": n} or {\"stop\": n}";
        else rc = ptz_tour(ch - 1, (st ? st : sp)->valueint, st != NULL);
        any = 1;
    }
    cJSON_Delete(b);
    if (bad) {
        char msg[160];
        snprintf(msg, sizeof msg, "invalid request: %s", bad);
        return send_error(conn, 400, msg, NULL);
    }
    if (!any) return send_error(conn, 400, "invalid request: nothing to do", NULL);
    if (rc != PTZ_OK) return send_rc(conn, rc, what);
    cJSON *o = info_json(ch);
    return o ? send_json(conn, 202, o) : send_error(conn, 404, "no such channel", NULL);
}

int api_ptz_request(struct mg_connection *conn, const char *uri, const char *method)
{
    int ch, used = 0;
    if (sscanf(uri, "/api/v1/channels/%d/ptz%n", &ch, &used) != 1 || !used || uri[used]) return 0;
    if (ch < 1 || ch > PTZ_CH_MAX) return send_error(conn, 404, "no such channel", NULL);
    if (!strcmp(method, "GET") || !strcmp(method, "HEAD")) return do_get(conn, ch);
    if (!strcmp(method, "POST")) return do_post(conn, ch);
    return method_not_allowed(conn, "GET, HEAD, POST");
}
