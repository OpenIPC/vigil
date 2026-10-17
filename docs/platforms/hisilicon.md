# HiSilicon

Two HiSilicon recorder SoCs are supported. They share the display code in
`src/platform/hisilicon/common/`:

- `hi_display.c`: the MPP system (SYS/VB), the VO display device, the
  video layer and HDMI.
- `hal_ui_hifb.c`: the UI on the hifb framebuffer.

| | hi3536dv100 | hi3520dv200 |
|---|---|---|
| Role | NVR: decodes network cameras | DVR: four analog inputs, encodes them |
| `VIGIL_PLATFORM` | `hi3536dv100` | `hi3520dv200` |
| SDK | Hi3536DV100_SDK_V2.0.2.0, linked statically | MPP V2.0.5.1 from the OpenIPC toolchain |
| C library | glibc | musl |
| UI format | ARGB8888 | RGB565 |
| Status | verified on hardware | **experimental**: builds, not verified |

On both, video and UI are separate hardware layers. VO shows the video
(decoder or input → VPSS scaler → VO channel per tile). The UI is drawn in
hifb `/dev/fb0` above it, and tiles are made see-through with hifb's
colour key: the UI paints them magenta (`0xFF00FF`), and the hardware shows
the video wherever it sees that colour.

## hi3536dv100 (NVR)

### What you need (none of it is in this repository)

- **The SDK** `Hi3536DV100_SDK_V2.0.2.0`: headers and static libraries
  (`mpp/include`, `mpp/lib`), and `mpp/ko/hi3536dv100_vfmw.ko`.
- **A cross toolchain** for `arm-openipc-linux-gnueabi` (glibc), e.g. the
  one an OpenIPC firmware build for hi3536dv100 produces in
  `output-hi3536dv100/host/`.
- **Kernel modules**: the OpenIPC lite image ships none. Build them with
  [openhisilicon](https://github.com/OpenIPC/openhisilicon) for this SoC
  (`open_*.ko`: OSAL from source, the MPP modules relinked from the SDK).
- **A patched `hi3536dv100_vdec.ko`** if the chip is revision 2. Check
  with `devmem 0x12050EE0`: bits 31:24 are the revision. The stock SDK
  module reads `0x12060024` and silently refuses every frame when the value
  there is above 1. Decoding then never starts, with no error. A copy of
  the module with that check bypassed works.

### Build

```sh
cmake -B build-hi3536 --toolchain cmake/toolchains/cross.cmake \
      -DCROSS_PREFIX=<toolchain>/bin/arm-openipc-linux-gnueabi- \
      -DVIGIL_PLATFORM=hi3536dv100 -DHI3536DV100_SDK=<sdk root>
cmake --build build-hi3536
```

This gives `vigild` and `fbtest`, a framebuffer smoke test that draws
colour bars through hifb. Both link the SDK statically, so the board only
needs glibc.

### Load the modules

From the build machine:

```sh
KMOD_DIR=<openhisilicon>/kernel \
HI3536DV100_SDK=<sdk root> \
VDEC_KO=<patched>/hi3536dv100_vdec.ko \
CROSS_PREFIX=<toolchain>/bin/arm-openipc-linux-gnueabi- \
scripts/hisilicon/stage_mpp_hi3536dv100.sh root@<nvr>
```

The script strips the modules, copies them with
`load_mpp_hi3536dv100.sh` to `/tmp/mpp/kmod` on the board, and loads
them. Nothing is written to flash, so after a reboot you run it again. If
`/dev/fb0` already exists, it does nothing.

Two settings in the load script matter:

- **MMZ** (the memory the media hardware uses) is one zone covering all
  RAM above the kernel's `mem=`. On a 256 MiB board with `mem=128M`, that
  is 128 MiB.
- **`vdec.ko VBSource=1`**: each decoder channel gets its own picture
  pool, sized from that camera's SPS (resolution and reference count). One
  shared pool sized for the largest camera does not fit: with two 5 MP
  cameras it already exceeds 128 MiB. Sized per camera, 1080p + two 5 MP
  H.265 + an H.264 stream use about 87–113 MiB.

### Run

```sh
scp -O build-hi3536/vigild root@<nvr>:/tmp/
ssh root@<nvr> '/tmp/vigild -o /mnt/rec \
    -c rtsp://user:pass@cam1/stream=0 -c rtsp://user:pass@cam2/stream=0 \
    -c netip://admin:@cam3:34567/'
```

`-o` should point at real storage (a disk or SD card). If you record to
`/tmp`, the RAM fills up, and the writer starts deleting old segments to
keep 10% free.

To check health while it runs:

- `cat /proc/umap/vdec`: the decoder error counters should stay at 0.
- `cat /proc/umap/vpss`: `VdecLost` should stay at 0.
- `cat /proc/media-mem`: how much MMZ is in use.

### Verified

On a revision-2 Hi3536DV100 NVR running the OpenIPC lite image, with four
cameras: RTSP 1080p H.265, two RTSP 5 MP H.265, and a NETIP H.264 stream.
The decoder error counters stayed at 0, MMZ use was 113 MiB, and the
recordings decoded cleanly. The HDMI output was captured with an HDMI
capture input and showed live video in all four tiles with the UI on top.

## hi3520dv200 (DVR) — experimental

The DVR path captures four analog cameras through the NVP6114 video
decoder chip (VI), shows them in the tiles (VI → VPSS → VO), and encodes
them to H.264 (VPSS → VENC) for the core's recorder, through
`hal_capture.h`. Use the inputs as `-c capture:0 … -c capture:3`.

This SoC also has a hardware decoder, but it is not connected to the HAL
yet: network cameras are recorded but not shown.

**Known problem:** creating the encoder channel fails with `0xa007800c`
(out of memory) on the lab DVR. The inputs show video, but no encoded
frames arrive, so local recording does not work yet. `hal_init` logs this.
The code has not been run on hardware since it moved into vigil.

### Build

The OpenIPC hi3520dv200 SDK toolchain (musl) carries the MPP headers and
libraries in `sdk/include` and `sdk/lib`. That includes
`libglibc-compat-static.a`: the vendor `libmpi.so` was built against
glibc, and this static shim supplies the `mmap` entry points it expects
from a musl process.

```sh
cmake -B build-hi3520 --toolchain cmake/toolchains/cross.cmake \
      -DCROSS_PREFIX=<toolchain>/bin/arm-openipc-linux-musleabi- \
      -DVIGIL_PLATFORM=hi3520dv200 -DHI3520DV200_TOOLCHAIN_SDK=<toolchain>
cmake --build build-hi3520
```

`vigild` links `libmpi.so` dynamically, so the board needs the SDK's
shared libraries. The OpenIPC firmware for this SoC ships them.
