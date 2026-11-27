/* SPDX-License-Identifier: GPL-3.0-or-later */
/* api_none.c — vigild built without the API (-DVIGIL_API=OFF). */
#include <stdio.h>

#include "api.h"
#include "config.h"
#include "log.h"
#include "netip_srv.h"
#include "rtsp_server.h"
#include "onvif.h"
#include "web.h"

int api_start(void)
{
    vigil_config_t c;
    config_get(&c);
    if (c.api.listen[0]) vg_log("api.listen is set, but this vigild is built without the API");
    return 0;
}

void api_stop(void) {}

int web_start(void)
{
    vigil_config_t c;
    config_get(&c);
    if (c.web.enabled && c.web.listen[0]) vg_log("web.enabled is on, but this vigild is built without the web server");
    return 0;
}

void web_stop(void) {}

int api_restart(const char *listen, char *err, size_t cap)
{
    (void)listen;
    (void)err;
    (void)cap;
    return 0;
}

int api_listening(char *listen, size_t cap, char *why, size_t wcap)
{
    if (listen && cap) listen[0] = 0;
    if (why) snprintf(why, wcap, "this vigild is built without the API");
    return 0;
}

int web_restart(const char *listen, char *err, size_t cap)
{
    (void)listen;
    (void)err;
    (void)cap;
    return 0;
}

int web_listening(char *listen, size_t cap, char *why, size_t wcap)
{
    if (listen && cap) listen[0] = 0;
    if (why) snprintf(why, wcap, "this vigild is built without the web server");
    return 0;
}

int rtsp_server_start(void)
{
    vigil_config_t c;
    config_get(&c);
    if (c.rtsp_server.enabled) vg_log("rtsp_server.enabled is on, but this vigild is built without the API's users");
    return 0;
}

void rtsp_server_stop(void) {}
void rtsp_server_apply(void) {}

int rtsp_server_listening(char *listen, size_t cap, char *why, size_t wcap)
{
    if (listen && cap) listen[0] = 0;
    if (why) snprintf(why, wcap, "this vigild is built without the RTSP server");
    return 0;
}

int onvif_start(void)
{
    vigil_config_t c;
    config_get(&c);
    if (c.onvif_server.enabled) vg_log("onvif_server.enabled is on, but this vigild is built without the API");
    return 0;
}

void onvif_stop(void) {}

int onvif_listening(char *listen, size_t cap, char *why, size_t wcap)
{
    if (listen && cap) listen[0] = 0;
    if (why) snprintf(why, wcap, "this vigild is built without the API");
    return 0;
}

int netip_server_start(const config_netip_server_t *cfg, const char *platform)
{
    (void)platform;
    if (cfg->enabled) vg_log("netip_server.enabled is on, but this vigild is built without the API, "
                             "whose users it logs in");
    return 0;
}

void netip_server_stop(void) {}

void netip_server_apply(const config_netip_server_t *cfg) { (void)cfg; }

int netip_server_listening(char *listen, size_t cap, char *why, size_t wcap)
{
    if (listen && cap) listen[0] = 0;
    if (why) snprintf(why, wcap, "this vigild is built without the API");
    return 0;
}
