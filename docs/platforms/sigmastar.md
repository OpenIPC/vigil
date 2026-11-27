# SigmaStar

| | infinity2m |
|---|---|
| SoCs | SSR621Q (NVRs) |
| Role | NVR: decodes network cameras |
| `VIGIL_PLATFORM` | `sigmastar` |
| SDK | the i2m NVR SDK's MI libraries (2020-03), shared |
| C library | glibc |
| UI format | ARGB8888, colour key |
| Status | **experimental**: run only in emulation ([qemu-bench](https://github.com/OpenIPC/qemu-bench)'s `ssr621q` machine), not on hardware |

Up to 16 channels. Channel *i* is shown through input port *i* of
`MI_DISP` video layer 0, which the display scans out on HDMI and VGA at the
same timing; its decoder is whichever `MI_VDEC` channel is free when it
opens (see below). The picture goes into a window fitted inside the tile at
the camera's display aspect ratio (a sub-stream at its main stream's; see
[architecture](../architecture.md), "The picture's shape"), centred, with
black bars in the rest of the tile (`display.video_aspect: stretch`: the
whole tile), and the decoder shrinks it to about the window's size (below,
"Picture geometry"). In `fill` the window is the whole tile, the decoder
shrinks the picture to about the size that covers it, and the display's
zoom-in window shows the middle of that (below).

The UI is drawn on the GOP framebuffer `/dev/fb0` above the video layer.
fbdev.ko sets its size and format from `fbdev.ini` (1280x720, ARGB8888 on
OpenIPC); vigild checks them and paints tiles magenta, which the GOP's
colour key makes see-through.

The mouse pointer is fbdev.ko's hardware cursor (`FBIOSET_CURSOR_ATTRIBUTE`),
a window on GOP 0 above the UI's GOP 1, set up by `fbdev.ini`'s
`[FB_CURSOR]` section (memory `E_MMAP_ID_HW_CURSOR`). A move is one ioctl
and no redraw, so the pointer follows the mouse once a frame, at the
output's 60 or 50 Hz, however busy the UI is ([ui.md](../ui.md), Input).
What vigild does with it follows from fbdev.ko and the region code under
it (opensigmastar `kernel/rgn`): GOP 0 cannot scale, so fbdev.ko places it
1:1 in output pixels, and vigild scales the position and the arrow from
the UI's 1280x720 to the output; GOP 0 has no ARGB8888, so the arrow goes
as ARGB1555 (alpha 0 and 255), `[FB_CURSOR]`'s own `FB_HWWIN_FORMAT` being
unused; and each call waits for the next vsync, so a thread of vigild's own
makes the calls with the newest position. Without `[FB_CURSOR]` in the
file fbdev.ko loaded (`default_config_path_file`) LVGL draws the pointer
instead: the cursor ioctl would dereference a NULL pointer in fbdev.ko.

Layouts and full screen ([ui.md](../ui.md)) move a channel's input port
window (`hal_display_set_tile`). A smaller tile shrinks the decoder's
output at once; a larger one is enlarged by the display until the next
keyframe, where a new decoder takes over at the new size (making the
output larger on a running decoder scrambled the pictures in emulation).

Whenever a decoder is replaced (a larger tile, the main stream in place of
the sub-stream full screen, a reconnect), the port keeps showing the old
decoder's last picture, enlarged, until the new one's first picture: the
new decoder is made beside the old one when the heap and `mi_vdec` have
room for both, else the old one goes without the port being cleared, and
the port keeps the old picture's size until a picture of the new size has
reached it (`RecvBuf_W/H` in `/proc/mi_modules/mi_disp/mi_disp0`; set at
once, the display read the old picture at the wrong size). A decoder made
beside the old one holds a second `MI_VDEC` channel number until it goes
on screen; when another channel needs a number and none is free, that
takeover is finished at once (its port keeps the old picture until the
new decoder's first), rather than the other channel saying "no decoder"
until the core asks again 10 s later: with 4 numbers, a 2x2 page coming
back from full screen ran into it every time. In emulation
the new decoder's first picture comes about 5 s after the switch; an
`MI_VDEC` channel bound to nothing decodes nothing there.

## Zero channel

The [zero channel](../api.md#zero-channel) (the screen in a browser) needs
the picture the monitor shows, and this display cannot give it: it has no
writeback, and `MI_DISP_GetScreenFrame`, though in the SDK's API, is
answered by the SDK's `mi_disp.ko` with an error without looking (its
`MI_DISP_IMPL_GetScreenFrame` is `return 31`); the display HAL whose source
the boot release has (opensigmastar `kernel/disp`) reads nothing back
either. A JPEG encoder in `MI_VENC` would still need the picture made
first, and the firmware ships no `libmi_venc`. So the picture is made in
software: the UI from LVGL's own pixels as it draws them, and each tile
from its decoder's newest picture (`hal_display_grab`): while a viewer is
connected each decoder's output port keeps a user queue of one
(`MI_SYS_SetChnOutputPortDepth(1, 4)`, back to `(0, 4)` when the last
viewer goes), read with `MI_SYS_ChnOutputPortGetBuf` a row at a time after
`MI_SYS_FlushInvCache`, scaled to the port's window and handed back at
once. The decoders' pictures are linear NV12 in emulation; any other
layout (tiled, compressed) is logged once and the tile stays black.

## Picture geometry

Each tile is mapped from the UI's 1280x720 to the output's own size, edge by
edge (`aspect_map`, even pixels), and the picture's window is fitted inside
the mapped tile (`aspect_fit`), so on a 5:4 or 16:10 output, where the UI is
stretched, the picture keeps its shape: a 16:9 camera full screen at
1280x1024 is 1280x720 with bars above and below. What the SDK's modules do,
read off them:

- **mi_disp**'s zoom-in window (`MI_DISP_SetZoomInWindow`) is a rectangle
  of the port's source picture (`u16SrcWidth` x `u16SrcHeight`, the
  decoder's output), every edge even (open_mhal's
  `_HalDispIfGetInfoInputPortCrop` refuses odd ones); one outside the
  source is dropped with only a log line. mi_disp keeps it and crops every
  picture to it on its way to the MOP (`MHAL_DISP_InputPortSetCropAttr`:
  the plane's address moved to the corner, the low four bits of x in the
  window's ADDR16 field, the source size and the scaling ratios the
  part's), whatever the picture's size, until it is given another: in
  emulation the top left of a larger picture showed, enlarged, after a
  fill tile went back to fit and then full screen. So vigild sets it at
  every placement of a port, to all of the picture outside fill, as the
  SDK's NVR demo ends a zoom (`st_main_vdec.c`). In fill the part is never
  larger than the window, as the display only enlarges (next point).
- **mi_disp** (open_mhal's `_HalDispIfSetInputPortAttr`, the MOP windows)
  only enlarges: a window larger than the port's source size gets the ratio
  source / window, a smaller one 1:1, so the picture's top left part shows
  and the rest is cut. In emulation (the SDK's modules on the emulated
  hardware) it shows the frame's content size, not the buffer's: for a
  1080-line stream scaled to 720 lines mi_vdec marks 714 of them as content
  (below) and mi_disp spreads those over the window.
- **mi_vdec**'s output port size (`MI_VDEC_SetOutputPortAttr`) is checked
  for 8..8192, even, and no larger than the channel's picture
  (`_MI_VDEC_ChkOutputPortAttr`); nothing about 16. The scaler's frames are
  sized by `CalcScaleDown`: the size asked for rounded **up** to a multiple
  of 8, and no less than an eighth of the picture (rounded up to 8). The
  scaler takes the whole decoded frame, the lines below the crop included:
  a 1920x1080 stream (1088 coded) scaled to 1280x720 has content 1280x714
  (`Content_W/H` in `/proc/mi_modules/mi_disp/mi_disp0`).

So vigild asks the decoder for the window's size rounded **down** to a
multiple of 8 (never larger than the window, which would be cut) and no
smaller than an eighth of the picture (`aspect_scaled_size`); the display
enlarges that by the few pixels left. Where the window is smaller than an
eighth of the picture (a 5 MP main stream in a 4x4 tile at 720p, 240x180
for 2592x1944: the decoder gives 328x248 at least) the picture is cut, not
squeezed: give such cameras a sub-stream. The SDK's decoder sample uses DIVP
behind the decoder there.

## Channels, decoders and memory

| | limit | evidence |
|---|---|---|
| tiles | 16 | `MI_DISP_INPUTPORT_MAX 16` (`mi_disp_datatype.h`); the SDK's display test binds layer 0's ports as DISP channels 0..15 |
| `MI_VDEC` channels | **4 or 32** | the SDK's `mi_vdec.ko` (2020-03) sets its `MaxChnNum` in `_MI_VDEC_CreateDevice`: 32 when the chip's efuse UUID (bits 16..47, efuse regs 0x17/0x18) reads zero, **4 otherwise**; `/proc/mi_modules/mi_vdec/mi_vdec0` shows it. A `mi_vdec.ko` from a vendor's 16-channel i2m NVR firmware has a fixed limit of 32 instead |
| picture | 2592 wide | `dec_max_w`, the VPU's 5 MP |
| media heap | 160 MiB | `mma_heap=...,sz=0xA000000` on 256 MiB boards; about 157 MiB free with the display up |

The 4-channel case is what the emulated SSR621Q shows (its efuse holds a
real chip's UUID): `MI_VDEC_CreateChn` refuses channel 4 with
`0xA0082002` (invalid channel). Whether real boards are limited the same
way with OpenIPC's modules is not known yet; it would take a `mi_vdec`
built without the check. vigild copes with either: it hands out `MI_VDEC`
channel numbers as decoders open (the one free longest), learns the limit
from that refusal, and decodes only the channels on screen
(`dec_shown_only`), so with 4 decoder channels a 2x2 page and paging
through all 16 cameras work, and a 3x3 or 4x4 layout decodes 4 of its tiles
(the others say "no decoder" and get one as soon as one is free).

**Memory.** `MI_VDEC` allocates its frame buffers only when the first
picture decodes, after `MI_VDEC_CreateChn` has succeeded, so vigild keeps
its own account of the heap: what is free with no decoder open (from
`/proc/mi_modules/mi_sys_mma/mma_heap_name0`), less 4 MiB, against an
estimate per decoder, and refuses a decoder that would not fit; the
channel reports "no decoder" and tries again when another decoder closes.
What the decoders took in emulation, read from that file:

| stream | refs | heap per decoder |
|---|---|---|
| 1280x720 H.264 | 3 | 8.2 MiB + output pictures |
| 1280x720 H.265 | 3 | 5.6 MiB + output pictures |
| 640x360 H.264 | 3 | 3.0 MiB + output pictures |
| 352x288 H.264 / H.265 (sub-streams) | 3 | about 1.5 MiB |

Sixteen 720p decoders fit (6 H.264, 5 H.265 and five 640x360 in a 4x4 view
took 95 MiB, 64 MiB were left); 1080p or 4 MP main streams fit far fewer.
Sixteen CIF sub-streams took 24 MiB.

**Sub-streams.** The 9- and 16-window views are meant for the cameras'
sub-streams: a tile of a quarter of the screen or less shows the camera's
`sub_url` (or, for a NETIP camera, the same URL with `stream=extra`), a
larger tile and full screen its main stream, and recording always takes
the main stream ([architecture](../architecture.md)). Give RTSP cameras a
`sub_url` (the vendor's second stream, e.g. `.../stream2` or
`.../h264Preview_01_sub`): without one they decode their main stream in
every tile. Both streams of every camera stay connected, and a switch
between them (full screen and back, another page) opens the decoder at
once from the GOP the stream kept, the whole GOP so far sent in one go
([architecture](../architecture.md), "Main stream and sub-stream").
`MI_VDEC_SendStream` takes it into the bitstream buffer in a few
milliseconds (a 720p GOP of 500 KiB in a 1 MiB buffer); the decoder is in
`E_MI_VDEC_DISPLAY_MODE_PREVIEW` and the display port in free-run, so what
is decoded faster than the display shows is not shown. In emulation the
first picture after the switch was the live one, without artefacts; what
the board's VPU makes of a GOP burst (a few pictures shown fast, or only
the last) is unverified. The caches take about 1 MiB per 720p main stream
at 1.5 Mbit/s with a 3 s GOP and 128 KiB per sub-stream; all of them at
most 24 MiB of the about 80 MiB Linux has.

The bitstream buffer is sized as the SDK's decoder sample does (512 KiB up
to D1, 1 MiB up to 1080p, 2 MiB above) and grows, with a new decoder, at a
keyframe that does not fit, up to 4 MiB.

The SDK has no `mi_hdmi`. The HDMI transmitter is
[opensigmastar](https://github.com/OpenIPC/opensigmastar)'s
`open_hdmitx.ko`, built from SigmaStar's source, and vigild switches it on
by writing the display's timing to `/sys/kernel/open_hdmi/output`. Without
that module only VGA has a picture.

## What you need (none of it is in this repository)

- **Headers**: opensigmastar's `include/infinity2m`, the MI headers that
  match the SDK's libraries.
- **Libraries**: `libmi_sys.so`, `libmi_vdec.so`, `libmi_disp.so` from the
  i2m NVR SDK. OpenIPC firmware ships them in `/usr/lib`
  (`sigmastar-osdrv-infinity2m`).
- **A cross toolchain** for `arm-openipc-linux-gnueabihf` (glibc).
- **libyaml for the target** (`yaml.h`, `libyaml.so`): the configuration
  file is YAML. `cmake/toolchains/cross.cmake` looks in the toolchain's
  sysroot (OpenIPC firmware builds with `yaml-cli` have it); otherwise add a
  firmware build's staging directory with `-DCMAKE_FIND_ROOT_PATH=<firmware>/output/staging`,
  or give `-DLIBYAML_INCLUDE_DIR=… -DLIBYAML_LIBRARY=…`.
- **On the board**: the media modules and `/config` (panel timings, PQ
  tables, memory map, decoder firmware), loaded by the firmware's
  `load_sigmastar`, and opensigmastar's `open_hdmitx.ko` and `open_hdmi.ko`
  (`sigmastar-opensdk`).

## Build

```sh
cmake -B build-ss --toolchain cmake/toolchains/cross.cmake \
      -DCROSS_PREFIX=<toolchain>/bin/arm-openipc-linux-gnueabihf- \
      -DVIGIL_PLATFORM=sigmastar \
      -DSIGMASTAR_INCLUDE_DIR=<opensigmastar>/include/infinity2m \
      -DSIGMASTAR_LIB_DIR=<dir with libmi_*.so>
cmake --build build-ss
```

## Run

```sh
load_sigmastar             # the firmware runs it at boot
vigild -o /mnt/disk/rec -c rtsp://user:pass@camera/stream -c netip://admin:@dvr:34567/
```

## Output resolution

The UI is laid out on a 1280x720 screen, always; `display.output` (Display
→ Output on screen, `PUT /api/v1/config/display`) picks the timing the
display sends to HDMI and VGA:

| `display.output` | `MI_DISP` timing | open_hdmi |
|---|---|---|
| `1280x720@60` (default) | `E_MI_DISP_OUTPUT_720P60` | `720p60` |
| `1280x720@50` | `E_MI_DISP_OUTPUT_720P50` | `720p50` |
| `1920x1080@60` | `E_MI_DISP_OUTPUT_1080P60` | `1080p60` |
| `1920x1080@50` | `E_MI_DISP_OUTPUT_1080P50` | `1080p50` |
| `1280x1024@60` | `E_MI_DISP_OUTPUT_1280x1024_60` | `1280x1024p60` |
| `1440x900@60` | `E_MI_DISP_OUTPUT_1440x900_60` | `1440x900p60` |

These are the modes both `MI_DISP` and open_hdmi have, at least as wide and
as tall as the screen. The GOP scales the UI layer to the output in
hardware: fbdev.ko keeps its buffer at `fbdev.ini`'s size, and vigild sets
the screen (the timing's size) and the stretch window's destination with
`FBIOSET_DISPLAYLAYER_ATTRIBUTES` (`MASK_SCREEN_SIZE | MASK_DISP_POS |
MASK_DISP_SIZE`); fbdev.ko then programs the GOP's scale registers
(`HSTRCH`/`VSTRCH`, 0x1000 = 1:1, 0xaaa for 720 → 1080). It refuses a
destination smaller than the buffer (EINVAL), so the GOP does not shrink:
1024x768 is not offered. The UI's 16:9 layout is stretched on 5:4 and 16:10
monitors. The video layer runs at the output's own size and each tile is
scaled to it (`to_output`), so a camera is decoded for its tile at 1080p
(960x540 in 2x2) rather than enlarged twice.

A switch (`hal_display_set_mode`) turns HDMI off, disables the display,
sets the new timing on HDMI and VGA, enables it, resizes the video layer,
moves every input port window (a decoder whose window grew is made again
at its next keyframe, as for a larger tile), scales the UI layer, and turns
HDMI on at the new timing; the ports and their bindings stay. If any step
fails, the old mode is set again the same way. vigild starts at the
configured mode directly; a mode this board does not have is logged and
1280x720@60 used. `fbdev.ini` needs no change: its `FB_TIMMING_*` is only
the start-up screen, which vigild sets anyway (a stretch left by an earlier
run stays in fbdev.ko).

On screen a new resolution must be kept within 15 s ("Keep this
resolution?"): with no answer, Undo, Esc or the right button, the old mode
comes back and nothing is saved. Through the API or the file it applies at
once, without that question.

## Margins, picture and HDMI sound

**Margins** (`display.margin_*`, `hal_display_set_margins`) put the screen
into the output's inner rectangle (`overscan_inner`: each margin a
percentage of the output, even). Each layer follows:

- *Video*: every tile is mapped into the inner rectangle (`to_output`,
  `overscan_map`) and its input port window placed there, as for a new
  mode; the video layer stays the whole output, its black background
  around. The zero channel reads the windows back into screen coordinates
  (`overscan_unmap`), so its picture is the 1280x720 screen, undistorted.
  In fill the zoom-in window is cut from the tile's window as placed in
  the inner rectangle, so the crop shows the same part with margins, and
  the zero channel's copy follows it (seen in emulation at 720p and
  1080p, 5 %).
- *UI layer*: the GOP's stretch window stays the whole output and the UI
  is shrunk into the buffer as LVGL flushes it (`hal_ui_fbdev.c`,
  `soft_flush`: nearest pixel, which keeps the colour key exact; the
  margins black). The GOP cannot do it: GOP 1 never shrinks (open_mhal's
  `HalGopSetStretchWindowSize`, "GOP1 Not Support H/V scaling down"; any
  margin at 720p needs that), and a placed destination smaller than the
  screen (`FBIOSET_DISPLAYLAYER_ATTRIBUTES` with `u32Xpos/u32YPos` and a
  smaller `u32dstWidth/Height`, which fbdev.ko takes) is undone by fbdev.ko
  itself: its vsync interrupt (`_sstar_FB_VsyncISR`) compares the
  destination with the display's timing and queues
  `GOPTimingSync_WorkHandler`, which sets it back to the whole timing,
  keeping the position (switched on at open, nothing switches it off; seen
  in emulation). The UI's coordinates do not change: LVGL, the mouse, the
  zero channel's input and picture all stay 1280x720.
- *Pointer*: the hardware cursor's position and arrow are scaled into the
  inner rectangle (`cursor_output`), so it reaches every UI pixel and
  nothing outside.

**Picture** (`display.picture`, `hal_display_set_picture`): MI_DISP's HDMI
and VGA parameters (`MI_DISP_SetHdmiParam`/`SetVgaParam`: luma, contrast,
hue, saturation 0..100, 50 the non-linear curves' unity in open_mhal, which
build the colour engine's matrix from them; sharpness to the peaking
filter, which needs the PQ tables, "Peaking is not set" without them) and
the gamma table (`MI_DISP_DeviceSetGammaParam`, 33 points a colour, off at
1.0 as the display starts):

| preset | luma | contrast | hue | saturation | sharpness | gamma |
|---|---|---|---|---|---|---|
| standard | 50 | 50 | 50 | 50 | as started | 1.0 (off) |
| bright | 54 | 52 | 50 | 50 | as started | 0.8 |
| soft | 50 | 44 | 50 | 45 | -25 | 1.0 (off) |
| vivid | 50 | 56 | 50 | 64 | +10 | 1.1 |

The display starts with the BT.709 matrix though `MI_DISP_GetHdmiParam`
answers BT.601: set with BT.709 and 50s, the colour engine's registers are
the ones it starts with, with BT.601 they differ, so vigild sets BT.709.
HDMI and VGA share one colour engine (`HAL_DISP_COLOR_CSC_ID`) and get the
same values. Until a preset other than standard is chosen vigild sets
nothing. The values show in `/proc/mi_modules/mi_disp/mi_disp0` (the
`Luma Contrast Hue Saturation Sharpness` columns per interface).

**HDMI sound** (`display.hdmi_volume`, `display.hdmi_mute`,
`hal_audio_set_volume`): vigild plays no sound yet, the setting is kept
(and logged with its level, 0 muted, 1..100 as (v - 100) / 2 dB, 100 being
0 dB) for the audio output to set as it starts. The path, read off the
sources and binaries (none of it run on a board):

- The i2m's audio block feeds the HDMI transmitter itself; there is no
  I2S/SPDIF input to choose in open_hdmitx (its audio source and FIFO
  calls are empty). open_mhal's `HalAud2Hdmi` routes the audio DMA to it
  when an output device with the HDMI flag opens (`MHAL_AUDIO_OpenPcmOut`).
- MI_AO devices (the SDK's audio demo, `open_mi_ao.ko`'s strings,
  `open_mhal.ko`'s device table): 0 line out (DAC), 1 I2S out, 2 HDMI,
  3 HDMI and line out, all on one DMA channel and one digital gain stage.
  `MI_AO_Enable` takes 32, 44.1 or 48 kHz on HDMI.
- The volume is `MI_AO_SetVolume` on that device, whole dB -60..+30 (the
  shared gain, 0.5 dB steps); `MI_AO_SetMute` zeroes the samples in
  `SendFrame`. Both answer "has not been enabled" until `MI_AO_Enable`.
  The HDMI transmitter itself has only a mute, which also stops the audio
  packets (TVs resynchronise): not for the user's mute.
- open_hdmi sets `bAudioEn = false` (48 kHz, 2 channels, PCM attributes
  otherwise). With it on, open_hdmitx writes N/CTS from a fixed table
  (48 kHz: N 6144, CTS 27000), the channel status and the audio infoframe.

Camera sound to HDMI would take: the audio from RTSP/NETIP (G.711 A/µ-law is
a table to 16-bit PCM; AAC needs a decoder, none in the rootfs),
resampling to 48 kHz (`MI_AO_EnableReSmp` or in software), MI_AO device 2
(or 3) set up, enabled and fed (`SetPubAttr`, `Enable`, `EnableChn`,
`SetVolume`, `SendFrame`), and open_hdmi switched on with audio at the same
rate, its attributes set again after an off/on (it clears its audio flag on
off). Unknown until a board: whether the HDMI tap is after the gain stage,
whether CTS is measured by the transmitter at 1080p (bank 0x1125 reg 0x30
bit 0, never written), the audio block's clocks, and whether keeping the
device open with silence avoids the TV's resync clicks.

## Verified

Only in emulation: [qemu-bench](https://github.com/OpenIPC/qemu-bench)'s
`ssr621q` machine, whose decoder and display are emulated (OpenIPC firmware
image, four cameras over RTSP and NETIP, H.264 and H.265; see the pull
request that added the platform). The UI (live view, navigation bar,
context menu, full screen and back, settings saved and applied) was driven
there with the machine's emulated USB mouse. Nothing has run on an SSR621Q
board yet.

Output modes, also only in emulation, on that machine with its GOP model
scaling the UI layer as fbdev.ko programs it (qemu-bench branch
`sigmastar/i2m-gop-stretch`; the stock model ignored the scale registers
and showed the 1280x720 layer unscaled): every mode in the table set
through the API and the Display page, the video layer and the decoders
following (960x540 decoders at 1080p in 2x2), vigild started at 1080p, a
mode left unanswered back after 15 s, and the UI layer free of pixels
blended with the key in every clock and name style. The emulator scales
nearest-neighbour; what the GOP's own stretch filter does at the edge of a
keyed pixel (whether it blends the key colour into the text's edge at
1080p) and how monitors take each timing are unverified.

The hardware cursor, also only in emulation, on a GOP 0 model added for it
(qemu-bench, `mstar-disp`'s fourth region, banks 0x1231/0x1232: the window,
its ARGB1555 alphas, blended above GOP 1): the arrow at the UI's position at
720p and at 1080p (1.5 times larger, at 1.5 times the coordinates), hidden
with no mouse, and the pointer moved in 55 to 60 of every 60 frames while
the emulated mouse reported about 100 times a second. That GOP 0 is mixed
above GOP 1, how the real GOP 0 blends the ARGB1555 arrow, and the rate
fbdev.ko's vsync wait gives on a board (the emulator raises its vsync
interrupt several times a frame) are unverified.

Sixteen channels, also only in emulation (OpenIPC firmware image, 16 fake
cameras: ten NETIP with a CIF sub-stream on `stream=extra`, six RTSP with a
`sub_url`; 720p main streams, H.264 and H.265): with the stock emulated
efuse, `mi_vdec` takes 4 channels, 2x2 pages through all 16 cameras and
4x4 decodes 4 of them; with the efuse UUID zeroed in the emulator (32
channels), 4x4 decodes all 16 sub-streams and full screen switches to the
main stream. In emulation a decoder made again right after another one on
the same port took 10-40 s to show a picture (the emulated VPU); one on a
fresh `MI_VDEC` channel shows it within about 5 s, the old picture staying
up meanwhile. An `MI_VDEC` channel reused right after another camera gave
it up showed a few of that camera's pictures first (only 4 numbers, none
free for longer).

The picture's shape, also only in emulation (OpenIPC firmware image, four
NETIP cameras with a circle-and-grid test picture: 2592x1944 4:3 with a
704x576 sub-stream drawn at 4:3, 2560x1440 with 800x448, 1920x1080 with a
704x576 sub-stream drawn at 16:9, 2560x1440 H.265 with a 704x576 H.265 one
at 16:9): the circle's width over its height measured in screendumps of
2x2 and each camera full screen at 1280x720, 1920x1080, 1280x1024 and
1440x900, and of 3x3 and 4x4 at 1280x720, was 1.00 within 1 % in fit (the
two nearest-neighbour scalers' rounding of a thick line), and the tile's
own shape in stretch. Full screen and back switched between the
sub-stream and the main stream without the picture changing shape. That
took a fix to qemu-bench's VPU model, which scaled the cropped picture
where mi_vdec's content size says the firmware scales the whole decoded
frame: with the stock model a 1080-line stream came out 0.8 % too tall.
Whether mi_disp on a board crops to the content as it does in emulation is
unverified.

Fill, also only in emulation (OpenIPC firmware image, four NETIP cameras
with a test picture of a circle, a grid and a coloured band along each
edge: 2592x1944 4:3 with a 704x576 sub-stream drawn at 4:3, 2560x1440 with
800x448, 2048x1536 4:3 with a sub-stream coded 800x448 but drawn at 4:3,
1920x1080 H.265 with a 704x576 H.265 one at 16:9), on qemu-bench's machine
with a MOP model that reads the ADDR16 field (branch
`sigmastar/i2m-mop-crop`; the stock model ignored it, and a crop whose x
was not a multiple of 16 showed shifted by up to 15 source pixels, 4 to 10
screen pixels at 1280x1024): in 2x2 and full screen at 1280x720 the 4:3
cameras lost their top and bottom bands and the 16:9 ones nothing; at
1280x1024 (5:4 tiles) the 16:9 cameras lost the sides, x offsets 20, 104,
118 and 268. The circle stayed round (width over height 1.00 within 1 %)
and centred in its tile to 2 pixels, mi_disp's `/proc` file listed the
crops, and the zero channel's picture matched the screendump. Back to fit
and stretch, the whole picture came back, full screen included. What a
board's MOP does with the crop, and the zero channel's on a board, are
unverified.

The zero channel, also only in emulation (OpenIPC firmware image, the four
NETIP cameras above at 10 pictures a second, a 2x2 live view at 720p, on
qemu-bench's machine with the cursor plane model): headless Chrome logged
in, showed the picture (the tiles where the screendump has them, the
names, clock and bars), opened the context menu with the right button,
went through the menus, typed into a text field, and the monitor's
screendump showed the remote pointer on the cursor plane. With one viewer
at the screen's size the pictures came at 8 to 14 a second, each made in
13 to 65 ms (a whole 1280x720 picture of the four tiles about 100 KB, a
changed part a few KB), the context menu was on the page 130 to 220 ms
after the click; at half size 11 to 12 a second; vigild took about 45 %
of one emulated core with a viewer (35 % at half size) and 4 % with none,
its memory 10 MB more while one watched, and the display took as many
pictures from each decoder with a viewer as without. Emulated cores are
not a board's: the time a picture takes on a Cortex-A7, whether the
decoders' pictures are linear NV12 on a board too, and whether keeping
one of them aside costs a board's decoder anything are unverified.

Both streams connected and the switch from the GOP so far, also only in
emulation (OpenIPC firmware image, four NETIP cameras with 720p main
streams and 640x360 sub-streams, 10 pictures a second, a keyframe every
3 s, three H.264 and one H.265; mi_vdec with 4 channel numbers; each of
the four tiles of a 2x2 view full screen with Enter and back, twice): the
eight connections stayed up through every switch (none was made again),
the main stream's decoder opened 70 to 320 ms after the key (a median of
1.9 s before, waiting for the keyframe) and back on the grid all four
tiles had a new picture within 0.24 to 0.4 s (once 1.9 s; before, every
sub-stream connected again, and the last channel back often had no
decoder number for 10 s). The full-screen picture itself still came about
6 s after the key, as before: the emulated machine stops for 4 to 6 s
whenever a 720p decoder is made. vigild's resident memory went from 3.5
to 7.8 MiB.

The HDMI and VGA signals, hot-plug and decoder limits are unverified on
hardware.

Margins and picture presets, also only in emulation, on that machine with
the GOP and cursor models plus, for this, the display timing from the OP2
timing generator (bank 0x1129: the output's size no longer comes from the
GOP's window), the GOP stretch window's position, and the colour engine
and gamma applied to the video plane as open_mhal programs them (qemu-bench
local branch `sigmastar/i2m-gop-offset`): at 720p and 1080p with 0 and 5 %
margins the video, the UI and the pointer inside the inner rectangle,
black around, a slider dragged on the Display page moving everything as it
went and saved when let go, the zero channel's picture the whole screen;
each preset's values in `/proc/mi_modules/mi_disp/mi_disp0` and in the
colour engine's and gamma's registers. Whether the GOP is mixed before or
after the colour engine and gamma on the chip (the model leaves the UI
alone), how the picture looks on a TV, and anything of the HDMI sound are
unverified.

