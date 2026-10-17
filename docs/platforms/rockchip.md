# Rockchip

`VIGIL_PLATFORM=rockchip` targets RK3588 (Orange Pi 5 / 5 Plus, Rock 5,
…) and, by design, RK356x, running Rockchip's vendor kernel (for example
the Armbian "vendor" images with kernel 6.1). It is an NVR: network
cameras are decoded and shown on HDMI and recorded as received.

## How it works

```
channel thread:  hal_dec_send -> MPP decoder -> newest picture in the channel's slot
compositor:      30 times a second, RGA scales each new picture into its
                 tile of an NV12 "mosaic" buffer -> flip onto the video plane
UI (main):       LVGL -> ARGB8888 buffer -> flip onto the UI plane above
```

- **MPP** (Media Process Platform) is Rockchip's library for the hardware
  video decoder. Each channel has its own MPP decoder. Decoded pictures stay
  in DMA buffers and are never copied by the CPU.
- **RGA** is Rockchip's 2D engine. It scales each picture into its tile,
  keeping the aspect ratio, inside one 1280×720 NV12 buffer. Why compose at
  all, instead of giving each camera its own hardware plane? On RK3588 only
  the "Esmart" planes accept NV12, and only two of them can be attached to
  one output. So the four tiles are combined into one mosaic, shown on one
  Esmart plane. The mosaic is double-buffered: RGA writes one buffer while
  the other is on screen.
- **DRM/KMS** (the Linux display API) drives the output with atomic
  commits. The UI is on a separate ARGB8888 plane above the video (a
  "Cluster" plane) with per-pixel alpha. Tiles are fully transparent in
  the UI, so no colour key is needed. The UI plane is double-buffered too.
- **Writeback**: VOP2 (the display controller) can write the blended output
  (video and UI, as a monitor would get it) back into memory. vigil uses
  this to run and to check the output with no monitor attached.

Display selection at start:

- If an HDMI or DP connector reports a monitor, vigil sets 1280×720@60 on
  it, using a CRTC (display pipe) that the connector can be driven from and
  that has the planes it needs.
- With no monitor, vigil drives the writeback connector alone ("headless").
  Everything is composed as usual and can be captured.

vigild needs the display to itself: run it on a console image, or stop the
desktop / display manager first. Only one process can drive DRM at a time.

## Dependencies

Neither MPP nor RGA is in the stock Debian/Ubuntu repositories, so a
script builds both from pinned source revisions, natively on the board,
into `/usr/local`:

- rockchip-linux/mpp (Apache-2.0)
- librga from the `jellyfin-rga` branch of nyanmisaka/rk-mirrors
  (Apache-2.0). This is the source Jellyfin's ffmpeg builds use; the
  airockchip/librga repository ships binaries only.

```sh
sudo apt install cmake meson ninja-build pkg-config libdrm-dev git build-essential
sudo scripts/rockchip/build-deps.sh        # PREFIX=/usr/local, WORK=/tmp/vigil-deps
```

The script installs into `PREFIX` and runs `ldconfig`, so it needs root
for the default `/usr/local`.

CI runs the same script on a GitHub arm64 runner and compiles the platform
there. CI cannot run it: the runners have no VPU, RGA or display.

## Build and run

Natively on the board:

```sh
git clone --recursive <repo> vigil && cd vigil
cmake -B build -DVIGIL_PLATFORM=rockchip && cmake --build build -j
./build/vigild -o /mnt/rec -c rtsp://user:pass@cam1/stream=0 -c netip://admin:@cam2:34567/
```

The log lists the chosen display and planes, for example:

```
rockchip: headless (writeback only) 1280x720@60, video plane 194, UI plane 57, writeback available
rockchip: decoder 0: H.265 1920x1080
```

## Capture the output

Send `SIGUSR1` to capture the next composed frame through the writeback
connector. It is saved as raw ARGB8888 to `$VIGIL_SNAPSHOT` (default
`/tmp/vigil-snapshot.argb`). Convert it to PNG:

```sh
kill -USR1 $(pidof vigild)
ffmpeg -f rawvideo -pix_fmt bgra -s 1280x720 -i /tmp/vigil-snapshot.argb snapshot.png
```

On boards without a writeback connector, the request is logged as skipped.

## Verified

On an Orange Pi 5 Plus (RK3588, Armbian vendor kernel 6.1.115), headless
through writeback. Four cameras: RTSP 1080p H.265, two RTSP 5 MP H.265,
and a NETIP H.264 stream. All four decoded and were composed with the UI,
using about 5% CPU in total, and the recordings decoded cleanly.

Not verified yet: output to a physical monitor (mode setting with a real
connector), and RK356x boards.
