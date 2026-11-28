/* SPDX-License-Identifier: GPL-3.0-or-later */
/* netip_proto.c — NETIP's header and media framing (see netip_proto.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "cam_events.h"
#include "channel.h"
#include "netip_proto.h"

static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static uint32_t get32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

void netip_hdr_put(uint8_t out[NETIP_HDR_LEN], const netip_hdr_t *h)
{
    out[0] = 0xff;
    out[1] = h->version;
    out[2] = out[3] = 0;
    put32(out + 4, h->session);
    put32(out + 8, h->seq);
    out[12] = h->total;
    out[13] = h->cur;
    out[14] = h->msgid & 0xff;
    out[15] = h->msgid >> 8;
    put32(out + 16, h->len);
}

int netip_hdr_get(const uint8_t in[NETIP_HDR_LEN], netip_hdr_t *h)
{
    if (in[0] != 0xff) return -1;
    h->version = in[1];
    h->session = get32(in + 4);
    h->seq = get32(in + 8);
    h->total = in[12];
    h->cur = in[13];
    h->msgid = (uint16_t)(in[14] | in[15] << 8);
    h->len = get32(in + 16);
    return 0;
}

uint32_t netip_time_pack(const struct tm *t)
{
    unsigned year = t->tm_year >= 100 ? (unsigned)t->tm_year - 100 : 0;    /* from 2000 */
    if (year > 63) year = 63;
    return (uint32_t)(t->tm_sec & 63) | (uint32_t)(t->tm_min & 63) << 6 | (uint32_t)(t->tm_hour & 31) << 12 |
           (uint32_t)(t->tm_mday & 31) << 17 | (uint32_t)((t->tm_mon + 1) & 15) << 22 | year << 26;
}

void netip_time_unpack(uint32_t v, struct tm *t)
{
    memset(t, 0, sizeof *t);
    t->tm_sec = v & 63;
    t->tm_min = v >> 6 & 63;
    t->tm_hour = v >> 12 & 31;
    t->tm_mday = v >> 17 & 31;
    t->tm_mon = (int)(v >> 22 & 15) - 1;
    t->tm_year = (int)(v >> 26) + 100;
}

size_t netip_frame_head(uint8_t out[NETIP_FRAME_HEAD], int key, vg_codec_t codec, unsigned width,
                        unsigned height, unsigned fps, const struct tm *t, uint32_t len)
{
    if (codec != VG_CODEC_H264 && codec != VG_CODEC_H265) return 0;
    out[0] = out[1] = 0;
    out[2] = 1;
    if (!key) {
        out[3] = 0xfd;
        put32(out + 4, len);
        return 8;
    }
    /* The size in units of 8 pixels, its bits 8-9 in the codec byte: a
     * 2560x1440 H.265 picture says 0x13 (source_netip.c, xm_codec). */
    unsigned w = width / 8, h = height / 8;
    if (w > 1023) w = 1023;
    if (h > 1023) h = 1023;
    out[3] = 0xfc;
    out[4] = (uint8_t)((codec == VG_CODEC_H265 ? 3 : 2) | (w >> 8 & 3) << 4 | (h >> 8 & 3) << 6);
    out[5] = (uint8_t)(fps > 31 ? 31 : fps);
    out[6] = (uint8_t)(w & 255);
    out[7] = (uint8_t)(h & 255);
    put32(out + 8, t ? netip_time_pack(t) : 0);
    put32(out + 12, len);
    return 16;
}

int netip_session_parse(const char *s, uint32_t *v)
{
    if (!s || !*s) return -1;
    char *end;
    unsigned long n = strtoul(s, &end, 0);
    if (*end || n > 0xffffffffUL) return -1;
    *v = (uint32_t)n;
    return 0;
}

int netip_stream_type(const char *s)
{
    if (!s) return -1;
    if (!strcasecmp(s, "Main")) return 0;
    if (!strncasecmp(s, "Extra", 5) || !strcasecmp(s, "Sub")) return 1;
    return -1;
}

/* ---- alarms -------------------------------------------------------------- */

unsigned netip_alarm_type(const char *event)
{
    static const struct { const char *name; unsigned type; } exact[] = {
        { "VideoMotion", CH_EV_MOTION },
        { "HumanDetect", CH_EV_HUMAN }, { "appEventHumanDetectAlarm", CH_EV_HUMAN },
        { "HumanDetection", CH_EV_HUMAN }, { "PersonDetect", CH_EV_HUMAN },
        { "FaceDetect", CH_EV_FACE }, { "FaceDetection", CH_EV_FACE }, { "FaceRecognition", CH_EV_FACE },
        { "CarShapeDetect", CH_EV_VEHICLE }, { "VehicleDetect", CH_EV_VEHICLE }, { "CarDetect", CH_EV_VEHICLE },
        { "VideoLoss", CH_EV_VIDEO_LOSS },
        { "VideoBlind", CH_EV_TAMPER },
        { "LocalAlarm", CH_EV_ALARM_IN }, { "AlarmIn", CH_EV_ALARM_IN },
        /* XM's IVA rules (tripwire and perimeter alike), a client's
         * OPNetAlarm, the device's own trouble. */
        { "VideoAnalyze", CH_EV_OTHER }, { "NetAlarm", CH_EV_OTHER },
        { "StorageNotExist", CH_EV_OTHER }, { "StorageFailure", CH_EV_OTHER },
        { "StorageLowSpace", CH_EV_OTHER }, { "NetAbort", CH_EV_OTHER }, { "NetIPConflict", CH_EV_OTHER },
    };
    /* Other firmwares' names, by what they contain. */
    static const struct { const char *part; unsigned type; } parts[] = {
        { "human", CH_EV_HUMAN }, { "person", CH_EV_HUMAN }, { "people", CH_EV_HUMAN },
        { "pedestrian", CH_EV_HUMAN }, { "face", CH_EV_FACE }, { "vehicle", CH_EV_VEHICLE },
        { "carshape", CH_EV_VEHICLE }, { "motor", CH_EV_VEHICLE }, { "motion", CH_EV_MOTION },
        { "videoloss", CH_EV_VIDEO_LOSS }, { "blind", CH_EV_TAMPER }, { "tamper", CH_EV_TAMPER },
        { "shelter", CH_EV_TAMPER }, { "cover", CH_EV_TAMPER }, { "tripwire", CH_EV_LINE },
        { "linecross", CH_EV_LINE }, { "crossline", CH_EV_LINE }, { "intrusion", CH_EV_INTRUSION },
        { "perimeter", CH_EV_INTRUSION }, { "localalarm", CH_EV_ALARM_IN }, { "alarmin", CH_EV_ALARM_IN },
    };
    if (!event || !*event) return 0;
    for (size_t i = 0; i < sizeof exact / sizeof exact[0]; ++i)
        if (!strcasecmp(event, exact[i].name)) return exact[i].type;
    char low[64];
    size_t n = 0;
    for (; event[n] && n + 1 < sizeof low; ++n)
        low[n] = (char)(event[n] >= 'A' && event[n] <= 'Z' ? event[n] - 'A' + 'a' : event[n]);
    low[n] = 0;
    for (size_t i = 0; i < sizeof parts / sizeof parts[0]; ++i)
        if (strstr(low, parts[i].part)) return parts[i].type;
    return 0;
}

int netip_alarm_parse(const char *body, size_t len, netip_alarm_t *a)
{
    memset(a, 0, sizeof *a);
    a->channel = -1;
    while (len && (body[len - 1] == 0 || body[len - 1] == '\n' || body[len - 1] == '\r')) --len;
    cJSON *doc = cJSON_ParseWithLength(body, len);
    if (!doc) return -1;
    /* The payload is under the message's Name (python-dvr reads it so),
     * which is "AlarmInfo"; a bare object is taken too. */
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(doc, "Name"), *o = NULL;
    if (cJSON_IsString(name)) o = cJSON_GetObjectItemCaseSensitive(doc, name->valuestring);
    if (!cJSON_IsObject(o)) o = cJSON_GetObjectItemCaseSensitive(doc, "AlarmInfo");
    if (!cJSON_IsObject(o)) o = doc;
    const cJSON *ev = cJSON_GetObjectItemCaseSensitive(o, "Event");
    const cJSON *st = cJSON_GetObjectItemCaseSensitive(o, "Status");
    const cJSON *ch = cJSON_GetObjectItemCaseSensitive(o, "Channel");
    int rc = -1;
    if (cJSON_IsString(ev) && ev->valuestring[0]) {
        snprintf(a->event, sizeof a->event, "%s", ev->valuestring);
        a->type = netip_alarm_type(ev->valuestring);
        a->known = a->type != 0;
        if (!a->type) a->type = CH_EV_OTHER;
        a->what = CAM_EV_PULSE;
        if (cJSON_IsString(st) && !strcasecmp(st->valuestring, "Start")) a->what = CAM_EV_START;
        else if (cJSON_IsString(st) && !strcasecmp(st->valuestring, "Stop")) a->what = CAM_EV_STOP;
        if (cJSON_IsNumber(ch)) a->channel = ch->valueint;
        else if (cJSON_IsString(ch)) a->channel = atoi(ch->valuestring);
        rc = 0;
    }
    cJSON_Delete(doc);
    return rc;
}
