/* SPDX-License-Identifier: GPL-3.0-or-later */
/* picture.h — the output's picture presets (vigil.yaml display.picture):
 * the colour controls and the gamma curve the display applies to what it
 * sends to HDMI and VGA, the way a TV's picture modes do.
 *
 * Values are the vendor displays' common scale: 0..100, 50 the picture
 * unchanged (the SigmaStar display maps them onto its own ranges; HiSilicon
 * VO's CSC takes the same 0..100). Gamma is an exponent, times 100: 100 is
 * a straight line, below 100 lifts the mid-tones (brighter without washing
 * out black or clipping white), above 100 deepens them.
 *
 *              luma contrast hue saturation sharpness gamma
 *   standard    50     50     50     50        50      1.00   as the display starts
 *   bright      54     52     50     50        50      0.80   for a dim picture or a bright room
 *   soft        50     44     50     45        25      1.00   less contrast and edge, for long viewing
 *   vivid       50     56     50     64        60      1.10   punchy colour, deeper mid-tones
 */
#ifndef PICTURE_H
#define PICTURE_H

typedef struct {
    unsigned luma, contrast, hue, saturation;   /* 0..100, 50: unchanged */
    unsigned sharpness;                         /* 0..100, 50: the display's own */
    unsigned gamma;                             /* exponent x100, 100: linear */
} vg_picture_t;

/* The presets' names, as stored, NULL-terminated: "standard" first. */
extern const char *const picture_presets[];

/* Preset `name`'s values into `out`: 0, or -1 for an unknown name (`out`
 * then holds standard's). */
int picture_preset(const char *name, vg_picture_t *out);

/* `n` (>= 2) points of the gamma curve `gamma` on 0..255: point i is the
 * output for an input of i/(n-1) of the full scale (256), rounded, at most
 * 255. With 100 it is the straight line i * 256/(n-1) (33 points: 0, 8,
 * 16, ... 248, 255). */
void picture_gamma_curve(unsigned gamma, unsigned n, unsigned char *out);

#endif /* PICTURE_H */
