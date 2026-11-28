/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ptz_onvif.c — PTZ over ONVIF (placeholder until the ONVIF client). */
#include <stdio.h>
#include <stdlib.h>

#include "ptz.h"

static void *create(const ptz_camera_t *cam, const volatile int *quit, char *err, size_t cap)
{
    (void)cam;
    (void)quit;
    snprintf(err, cap, "ONVIF PTZ is not there yet");
    return NULL;
}

static void destroy(void *l) { free(l); }

static int probe(void *l, unsigned *caps, int *tours, ptz_preset_t *p, int *n, char *err, size_t cap)
{
    (void)l; (void)caps; (void)tours; (void)p; (void)n; (void)err; (void)cap;
    return 1;
}

static int motion(void *l, const double from[PTZ_AXES], const double to[PTZ_AXES], char *err, size_t cap)
{
    (void)l; (void)from; (void)to; (void)err; (void)cap;
    return -1;
}

const ptz_drv_t ptz_drv_onvif = { .name = "onvif", .create = create, .destroy = destroy, .probe = probe,
                                  .motion = motion };
