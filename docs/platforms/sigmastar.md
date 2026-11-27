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
its aspect ratio, centred, with black bars in the rest of the tile, and the
decoder outputs it at the window's size: mi_disp enlarges a picture but does
not shrink it across (a wider source lost its right part), and the decoder's
scaler shrinks at most 8 times (the SDK's decoder sample puts DIVP behind it
beyond that).

The UI is drawn on the GOP framebuffer `/dev/fb0` above the video layer.
fbdev.ko sets its size and format from `fbdev.ini` (1280x720, ARGB8888 on
OpenIPC); vigild checks them and paints tiles magenta, which the GOP's
colour key makes see-through.

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
once, the display read the old picture at the wrong size). In emulation
the new decoder's first picture comes about 5 s after the switch; an
`MI_VDEC` channel bound to nothing decodes nothing there.

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
every tile.

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

The HDMI and VGA signals, hot-plug and decoder limits are unverified on
hardware.
