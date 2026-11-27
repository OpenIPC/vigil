/* SPDX-License-Identifier: GPL-3.0-or-later */
/* hal_audio.h — the sound of the HDMI output.
 *
 * vigild plays no sound yet (the cameras' audio is not decoded); the
 * volume is set at start and when it changes, so it is in force for
 * whatever plays. */
#ifndef HAL_AUDIO_H
#define HAL_AUDIO_H

/* The HDMI output's volume, 0..100 (0 silent, 100 the most the platform
 * gives without distortion), and mute. 0, or -1 where the platform has no
 * such control (hal_caps()->audio_out 0). Any thread, before any sound
 * plays too. */
int hal_audio_set_volume(unsigned volume, int mute);

#endif /* HAL_AUDIO_H */
