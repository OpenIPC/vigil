/* SPDX-License-Identifier: GPL-3.0-or-later */
/* api_none.c — vigild built without the API (-DVIGIL_API=OFF). */
#include "api.h"
#include "config.h"
#include "log.h"
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

int onvif_start(void)
{
    vigil_config_t c;
    config_get(&c);
    if (c.onvif_server.enabled) vg_log("onvif_server.enabled is on, but this vigild is built without the API");
    return 0;
}

void onvif_stop(void) {}
