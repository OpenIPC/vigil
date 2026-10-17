/* SPDX-License-Identifier: GPL-3.0-or-later */
/* hal_hi3520dv200.c — Hi3520DV200 DVR: four analog cameras through the
 * NVP6114 (VI), shown in the HDMI tiles (VI -> VPSS -> VO, hi_video_pipe.c)
 * and encoded (VPSS -> VENC H.264, venc_reader.c) for recording by the
 * core. The decoder is not wired up yet, so network cameras are recorded
 * but not displayed on this SoC.
 *
 * Builds against the OpenIPC hi3520dv200 toolchain (musl) and its MPP
 * V2.0.5.1 headers/libraries.
 *
 * EXPERIMENTAL — not run on hardware since the move into vigil, and local
 * recording is known not to work yet: VENC channel creation fails with
 * 0xa007800c (NOMEM) on the lab DVR (hi_video_pipe.c, venc_bringup), so the
 * capture inputs show video but deliver no encoded frames. hal_init says so
 * in the log.
 */
#include <stdio.h>

#include "hi_type.h"
#include "hi_comm_vb.h"
#include "hi_comm_vo.h"
#include "mpi_vb.h"

#include "hal.h"
#include "hal_capture.h"
#include "hal_decoder.h"
#include "hal_display.h"
#include "hi_display.h"
#include "hi_video_pipe.h"
#include "venc_reader.h"

/* Header and footer heights of the quad view; the VI tiles sit between. */
#define UI_HEADER_H 40
#define UI_STRIP_H  80

static const hal_caps_t g_caps = {
    .name = "hi3520dv200", .display = 1, .decoder = 0,
    .max_channels = VENC_READER_CHANNELS, .capture = VENC_READER_CHANNELS,
};

const hal_caps_t *hal_caps(void) { return &g_caps; }

int hal_init(unsigned w, unsigned h, unsigned fps)
{
    (void)fps;
    /* u32MaxPoolCnt MUST be the vendor's 128: libmpi indexes pool storage
     * past the configured astCommPool[] entries. VI captures 1280x720
     * sp420 (1280 * 720 * 3/2 bytes, already 64-aligned) into this pool. */
    VB_CONF_S vb = {
        .u32MaxPoolCnt = 128,
        .astCommPool   = { { .u32BlkSize = 1280 * 720 * 3 / 2, .u32BlkCnt = 16, .acMmzName = "" } },
    };
    if (hi_display_start(&vb, VO_INTF_HDMI, VO_OUTPUT_720P60, w, h) < 0) return -1;

    /* A private pool before any VENC call, as the vendor's sample_venc does;
     * registering it with libmpi is what matters. */
    if (HI_MPI_VB_CreatePool(768 * 1024, 10, NULL) == VB_INVALID_POOLID)
        fprintf(stderr, "hi3520dv200: VB_CreatePool failed (VENC may fail)\n");

    if (hi_video_pipe_start((int)w, (int)h, UI_HEADER_H, UI_STRIP_H) < 0)
        fprintf(stderr, "hi3520dv200: video pipeline failed; continuing without capture\n");
    fprintf(stderr, "hi3520dv200: EXPERIMENTAL platform — capture inputs are shown, but local "
                    "recording is known to fail (VENC_CreateChn NOMEM on MPP V2.0.5.1)\n");
    return 0;
}

void hal_shutdown(void)
{
    for (int i = 0; i < VENC_READER_CHANNELS; ++i) venc_reader_stop(i);
    hi_video_pipe_stop();
    hi_display_stop();
}

/* The VI pipeline places its own tiles. */
int  hal_display_set_tile(int ch, const vg_rect_t *r) { (void)ch; (void)r; return 0; }
void hal_display_clear_tile(int ch) { (void)ch; }

int  hal_dec_open(int ch, vg_codec_t codec, const vg_stream_info_t *si) { (void)ch; (void)codec; (void)si; return -1; }
int  hal_dec_send(int ch, const vg_au_t *au) { (void)ch; (void)au; return -1; }
void hal_dec_reset(int ch) { (void)ch; }
void hal_dec_close(int ch) { (void)ch; }

int  hal_cap_start(int in, int ch, hal_cap_cb cb, void *user) { (void)ch; return venc_reader_start(in, cb, user); }
void hal_cap_stop(int in) { venc_reader_stop(in); }
int  hal_cap_signal(int in) { return venc_reader_active(in); }
