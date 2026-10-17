# Architecture

vigil is one program, `vigild`, that turns a cheap recorder board into a
DVR/NVR: it takes video from cameras, shows it on the HDMI output in a
2×2 grid ("quad view") with a small UI on top, and records the streams to
disk. The same source tree builds for very different hardware. This page
explains how the code is split so that this works.

## Terms

- **DVR** (digital video recorder): cameras are wired straight into the
  board (analog, through a video decoder chip); the board digitises,
  shows and *encodes* the video.
- **NVR** (network video recorder): cameras are on the network and already
  send compressed H.264/H.265; the board *decodes* them for display and
  stores the compressed stream without re-encoding.
- **Access unit (AU)**: one compressed picture plus the units that belong
  with it, in Annex-B form (each unit prefixed by `00 00 00 01`).
- **Keyframe**: an AU that decodes on its own (IDR). Every other picture
  depends on earlier ones, so decoding and recording can only start at a
  keyframe.
- **Parameter sets**: SPS/PPS (H.264) and VPS/SPS/PPS (H.265), small units
  that describe the stream. A decoder cannot start without them, and the SPS
  also gives the picture size and how many reference frames the decoder must
  keep.
- **HAL** (hardware abstraction layer): the small set of C functions every
  platform implements (`src/hal/`).
- **Platform**: one implementation of the HAL for a SoC family, picked when
  the program is built.

vigil does not have fixed "DVR" and "NVR" modes. Each tile is a **channel**
with one source. A network source is decoded if the platform has a decoder.
A local input is recorded if the platform has an encoder. So the role
follows from the hardware plus the configuration, and a board that has both
decoder and capture inputs can mix the two.

## Layers

```
            src/app      vigild: command line, channels, UI or status loop
               |
   +-----------+------------+
   |                        |
src/core                 src/ui            LVGL screens; draws through hal_ui
 channel  source/*  au_norm  sps
 seg_writer  events  net  md5
   |                        |
   +-----------+------------+
               |
            src/hal        hal.h, hal_decoder.h, hal_display.h,
               |           hal_capture.h, hal_ui.h — declarations only
               |
          src/platform/<name>   generic | hisilicon/hi3536dv100 |
                                hisilicon/hi3520dv200 | rockchip
```

Rules that keep the layers apart:

- `src/core` is plain C11 + POSIX. It never includes a vendor header and
  never uses `#ifdef` to test which platform it is on. It builds and runs
  its tests on any Linux host. CI runs them on x86.
- `src/ui` only knows LVGL and `hal_ui.h`.
- A platform provides every HAL function. Functions it cannot support
  return an error, and `hal_caps()` reports what it can do. The core reads
  those capabilities at run time and does not assume them.
- CMake picks exactly one platform (`-DVIGIL_PLATFORM=…`) and links its
  files into `vigild`. There is no plugin loading.
- Vendor SDKs, kernel modules and libraries stay outside the repository.
  Their paths are given to CMake or to the scripts.

## Data path of one channel

```
 source (rtsp:// | netip:// | capture:N)
    |  Annex-B access units, as the protocol delivers them
    v
 au_norm  -- parameter-set cache, waits for a keyframe, SPS -> size/refs
    |
    +--> hal_dec_* (if the platform decodes and the source is not shown
    |               by the platform itself) --> tile on screen
    |
    +--> seg_writer (if recording) --> <dir>/CAMn_<date>_<time>.h264|h265
```

Each channel runs on its own thread (`src/core/channel.c`). The thread:

1. Creates the source from the URL. The scheme picks the protocol driver
   (`src/core/source/source.c`).
2. Opens it. On failure it retries with a back-off growing from 1 to 10
   seconds.
3. Calls the driver's `poll` in a loop. The driver hands back complete
   access units.
4. Runs each AU through the **normalizer** (`au_norm.c`). It makes AUs from
   every protocol look the same:
   - Each keyframe carries all parameter sets. Some cameras send them only
     once or out of band (in the RTSP SDP), so the normalizer caches them
     and adds the missing ones.
   - Units without a picture, which some cameras send as their own
     "frame", are merged into the next picture.
   - Nothing is passed on before the first keyframe, or after a gap until
     the next one.
   - The SPS gives the stream's size and reference count.
5. Feeds the result to the decoder and the recorder independently.
   Recording does not depend on decoding: a stream the board cannot decode
   is still stored.

The decoder is created from the SPS information (size, references), so
each channel gets only the memory it needs. When the SPS changes, the
decoder is rebuilt. If the HAL drops an AU (`hal_dec_send` fails), the
channel stops feeding the decoder until the next keyframe, because the
pictures after it would refer to a picture the decoder never got.

### Recording

`seg_writer.c` writes the compressed elementary stream (no re-encoding,
no container) into segments of `-S` seconds. What it writes is the
normalizer's output, so it is not a byte-for-byte copy of the network
data: cached parameter sets are added to keyframes that lack them,
picture-less units are merged into the next picture, and anything before
the first keyframe is dropped. The pictures themselves are untouched.
Every segment starts at a keyframe with its parameter sets, so each file
plays on its own (`ffplay CAM1_….h265`). `-K` limits how many segments each camera
keeps. Independently, the writer keeps 10% of the filesystem (at least
8 MiB) free: below that it deletes the oldest finished recording in the
directory, so a full disk rolls recordings over instead of stopping them.

### Status and events

Channel threads never touch the UI. They publish a status snapshot
(`channels_status()`: live, fps, codec, size, recording, last error) and
short events ("CAM2 offline") through a thread-safe queue (`events.c`).
The UI thread reads both. Without a display, `frontend_headless.c` prints
the same status to the log.

## Sources (camera protocols)

| URL | Protocol | Driver |
|---|---|---|
| `rtsp://[user:pass@]host[:554]/path` | RTSP, RTP interleaved over TCP; Basic and Digest auth; H.264 and H.265 | `source_rtsp.c`, `rtsp_client.c`, `rtp_depay.c` |
| `netip://[user:pass@]host[:34567]/[?ch=N&stream=main\|extra]` | Xiongmai NETIP / DVRIP: XM cameras and DVRs, and OpenIPC majestic with `netip` enabled | `source_netip.c` |
| `capture:N` | Local video input N of the board (DVR) | `source_capture.c` → `hal_capture.h` |

A driver implements `source_ops_t` (`source.h`): `create` parses the URL,
`open` connects, `poll` waits up to about a second and emits complete AUs,
and `close`/`destroy` clean up. Every driver watches the channel's stop
flag, so stopping never waits for a slow camera. Adding a protocol (ONVIF
discovery, RTMP, HTTP-FLV, …) means writing one new `source_<name>.c` and
listing it in `source.c`. Nothing else changes.

## The HAL

| Header | What the platform does |
|---|---|
| `hal.h` | `hal_init(w, h, fps)` brings up the media system and the output; `hal_caps()` says what is there (display, decoder and its maximum size, number of channels, capture inputs); `hal_shutdown()`. |
| `hal_decoder.h` | One hardware decoder per channel: open for a codec, size and reference count; send an AU; reset after a reconnect; close. Decoded pictures go to that channel's tile without passing through the core. |
| `hal_display.h` | Where each channel's tile is on the screen. The platform keeps the picture's aspect ratio inside it. |
| `hal_capture.h` | Local inputs: start input N in tile `ch` (the platform shows it itself) and deliver its *encoded* AUs to a callback for recording. |
| `hal_ui.h` | Create the LVGL display on the graphics layer above the video, and say how the UI makes a tile transparent (a colour key, or alpha). |

Video never passes through the core as raw pixels. Decoded and captured
pictures stay inside the platform (VDEC→VPSS→VO on HiSilicon, MPP→RGA→DRM
on Rockchip). The core only handles compressed data, which keeps it
portable and cheap on small CPUs.

## Screen layout

`frontend.h` fixes the layout at 1280×720: a 40-pixel header, a 2×2 grid
of tiles, and an 80-pixel event strip. The tile rectangles go to the
platform (`hal_display_set_tile`). LVGL draws the header, the strip and a
label on each tile, and paints the rest of each tile as a "hole" so the
video plane underneath shows through: a magenta colour key on HiSilicon
hifb, full transparency on Rockchip's alpha-blended plane.

## Build

```
CMakeLists.txt
  vigil_core      static library: src/core (+ host unit tests)
  vigil_platform  object library: the files listed by cmake/platforms/<p>.cmake
  vigild          src/app + LVGL UI (when the platform has a display)
cmake/platforms/<p>.cmake   sources, include paths, SDK/library paths
cmake/toolchains/cross.cmake   cross compiling: -DCROSS_PREFIX=…/bin/<triplet>-
```

Per-platform build and run steps are in [platforms/](platforms/), and
adding a platform is covered in [porting.md](porting.md).

## Tests

`ctest` in a generic build runs:

- `core`: unit tests for SPS parsing, the normalizer, RTP depacketizing,
  RTSP framing and helpers, the capture source (with a fake HAL) and the
  segment writer.
- `netip_*`: `vigild` against `tests/netip_server.py`, a NETIP camera
  emulator, with H.264 and H.265 clips and a wrong password.
- `fault_*`: misbehaving servers. A NETIP login that is never answered
  while other messages keep coming, a message body that stops halfway
  (vigild must not spin the CPU), a camera sending only audio (the channel
  must reconnect), and RTSP and NETIP servers that accept the connection
  and never answer (vigild must still stop promptly).
- `rtsp_*`: `vigild` against `tests/rtsp_server.py` (GStreamer) with no
  auth, Basic, Digest and a wrong password. These are skipped when the
  GStreamer RTSP server is not installed.

The `netip_*` and `rtsp_*` tests also check that the recorded files decode
cleanly with ffmpeg.
