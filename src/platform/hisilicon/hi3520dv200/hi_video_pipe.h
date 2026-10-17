/* SPDX-License-Identifier: GPL-3.0-or-later */
/* hi_video_pipe.h — Phase 2: VI (NVP6114, BT656 2MUX, 720p25) → VPSS →
 * VO_CHN windows positioned at the 4 tile slots of the LVGL home screen.
 *
 * Hand-rolled minimal port — does NOT pull in the upstream vendor
 * sample_comm_vi.c (~1350 lines of multi-sensor conditionals); only the
 * code paths we actually need for this DVR (NVP6114 4-camera @ 720p25).
 *
 * The bring-up is best-effort: if NVP6114 reports zero cameras detected
 * (the chip is on the I2C bus but no analog signal is wired), VI returns
 * errors. We log them and continue with VPSS + VO_CHN setup so the
 * pipeline is fully configured for when cameras are eventually plugged
 * in — and so that the VO bg colour fills the tile windows in the
 * meantime.
 */
#ifndef HI_VIDEO_PIPE_H
#define HI_VIDEO_PIPE_H

#include "hi_type.h"
#include "hi_comm_vo.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HI_VIDEO_PIPE_CHANNELS  4

/* Bring up VI -> VPSS -> VO channels, in that order. The 4 video-plane
 * channels are positioned at the 4 quadrants of the screen (matches the
 * LVGL tile layout). Returns 0 on overall success, <0 if VPSS or VO
 * setup fails (VI failure is logged but tolerated). */
int hi_video_pipe_start(int screen_w, int screen_h,
                        int header_h, int strip_h);

/* Tear down in reverse order. */
void hi_video_pipe_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* HI_VIDEO_PIPE_H */
