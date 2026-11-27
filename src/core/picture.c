/* SPDX-License-Identifier: GPL-3.0-or-later */
/* picture.c — the output's picture presets (picture.h has the table and
 * why). */
#include "picture.h"

#include <math.h>
#include <string.h>

const char *const picture_presets[] = { "standard", "bright", "soft", "vivid", NULL };

static const vg_picture_t k_values[] = {
    { 50, 50, 50, 50, 50, 100 },        /* standard */
    { 54, 52, 50, 50, 50, 80 },         /* bright */
    { 50, 44, 50, 45, 25, 100 },        /* soft */
    { 50, 56, 50, 64, 60, 110 },        /* vivid */
};

int picture_preset(const char *name, vg_picture_t *out)
{
    for (int i = 0; picture_presets[i]; ++i)
        if (name && !strcmp(name, picture_presets[i])) {
            *out = k_values[i];
            return 0;
        }
    *out = k_values[0];
    return -1;
}

void picture_gamma_curve(unsigned gamma, unsigned n, unsigned char *out)
{
    if (n < 2) {
        if (n) out[0] = 0;
        return;
    }
    double g = gamma ? gamma / 100.0 : 1.0;
    for (unsigned i = 0; i < n; ++i) {
        double v = gamma == 100 ? 256.0 * i / (n - 1) : 256.0 * pow((double)i / (n - 1), g);
        long r = lround(v);
        out[i] = (unsigned char)(r > 255 ? 255 : r < 0 ? 0 : r);
    }
}
