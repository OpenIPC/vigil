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

Per channel *i*, the decoder `MI_VDEC` channel *i* is bound to input port
*i* of `MI_DISP` video layer 0, which the display scans out on HDMI and VGA
at the same timing. The decoder's scaler only shrinks, so it outputs the
picture at the tile's size (or the picture's own, if smaller) and the
display scales that into a window fitted inside the tile at the picture's
aspect ratio, centred, with black bars in the rest of the tile.

The UI is drawn on the GOP framebuffer `/dev/fb0` above the video layer.
fbdev.ko sets its size and format from `/config/fbdev.ini` (1280x720,
ARGB8888 on OpenIPC); vigild checks them and paints tiles magenta,
which the GOP's colour key makes see-through.

Layouts and full screen ([ui.md](../ui.md)) move a channel's input port
window (`hal_display_set_tile`). The decoder keeps the output size it was
opened with, so a channel opened in a 2x2 tile and then shown full screen
is scaled up by the display (softer than a decode at full size) until its
decoder is opened again.

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

The display runs at 1280x720 (720p60), the size of vigil's layout.

## Verified

Only in emulation: [qemu-bench](https://github.com/OpenIPC/qemu-bench)'s
`ssr621q` machine, whose decoder and display are emulated (OpenIPC firmware
image, four cameras over RTSP and NETIP, H.264 and H.265; see the pull
request that added the platform). The UI (live view, navigation bar,
context menu, full screen and back, settings saved and applied) was driven
there with the machine's emulated USB mouse. Nothing has run on an SSR621Q
board yet.
The HDMI and VGA signals, hot-plug and decoder limits are unverified.
