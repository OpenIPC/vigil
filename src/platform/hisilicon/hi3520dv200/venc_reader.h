/* SPDX-License-Identifier: GPL-3.0-or-later */
/* venc_reader.h — hi3520dv200: drain one VENC channel per capture input and
 * hand each encoded access unit to a callback (hal_capture.h). */
#ifndef VENC_READER_H
#define VENC_READER_H

#include "hal_capture.h"

#define VENC_READER_CHANNELS 4

int  venc_reader_start(int chn, hal_cap_cb cb, void *user);
void venc_reader_stop(int chn);
/* 1 if the channel produced data within the last 2 s. */
int  venc_reader_active(int chn);

#endif /* VENC_READER_H */
