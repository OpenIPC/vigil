/* SPDX-License-Identifier: GPL-3.0-or-later */
/* api_none.c — vigild built without the API (-DVIGIL_API=OFF). */
#include "api.h"
#include "config.h"
#include "log.h"

int api_start(void)
{
    vigil_config_t c;
    config_get(&c);
    if (c.api.listen[0]) vg_log("api.listen is set, but this vigild is built without the API");
    return 0;
}

void api_stop(void) {}
