# Adding a platform

This guide shows how to bring vigil to a new SoC family, using SigmaStar
as the running example. A port touches only `src/platform/<name>/`,
`cmake/platforms/<name>.cmake` and, if the board needs set-up, a script
under `scripts/<name>/`. The core, the sources and the UI stay as they
are. If a port seems to need a change there, the HAL is missing something:
change the HAL header for every platform instead of adding an `#ifdef`.

Read [architecture.md](architecture.md) first for the terms used here.

The SigmaStar port now exists for infinity2m
([platforms/sigmastar.md](platforms/sigmastar.md),
`src/platform/sigmastar/`). The steps below are the general recipe; where
they differ from it (it takes the headers and shared libraries as the
firmware ships them, not an SDK root, and binds the decoder straight to the
display), the finished port is the reference.

## 1. Start from the generic platform

`src/platform/generic/hal_generic.c` implements every HAL function as
"not available". Copy it to `src/platform/sigmastar/hal_sigmastar.c` and
add `cmake/platforms/sigmastar.cmake`:

```cmake
# SigmaStar SSD20x / SSC33x: …  Vendor SDK: give its root with
# -DSIGMASTAR_SDK=… (never a default path).
set(SIGMASTAR_SDK "$ENV{SIGMASTAR_SDK}" CACHE PATH "SigmaStar SDK root")
if(NOT EXISTS "${SIGMASTAR_SDK}/include/mi_sys.h")
  message(FATAL_ERROR "set SIGMASTAR_SDK to the SDK root; now '${SIGMASTAR_SDK}'")
endif()

set(VIGIL_HAS_UI OFF)                 # ON once hal_ui is implemented
set(_ss ${CMAKE_SOURCE_DIR}/src/platform/sigmastar)
set(VIGIL_PLATFORM_SOURCES ${_ss}/hal_sigmastar.c)
set(VIGIL_PLATFORM_INCLUDES ${SIGMASTAR_SDK}/include ${_ss})
set(VIGIL_PLATFORM_LIBS -L${SIGMASTAR_SDK}/lib mi_sys mi_vdec mi_disp)
```

Add the name to the `STRINGS` list in `CMakeLists.txt`, then configure and
build:

```sh
cmake -B build-ss --toolchain cmake/toolchains/cross.cmake \
      -DCROSS_PREFIX=<toolchain>/bin/arm-linux-gnueabihf- \
      -DVIGIL_PLATFORM=sigmastar -DSIGMASTAR_SDK=<sdk>
cmake --build build-ss
```

At this point `vigild` already ingests and records cameras on the board,
because recording never depends on the HAL. That is the first check: run
it with `-c rtsp://…` and play back a segment.

The variables a platform file sets:

| Variable | Meaning |
|---|---|
| `VIGIL_PLATFORM_SOURCES` | The platform's `.c` files |
| `VIGIL_PLATFORM_INCLUDES` | Include paths (SDK headers, the platform directory) |
| `VIGIL_PLATFORM_DEFS` | Compile definitions, for code shared by several SoCs of one vendor |
| `VIGIL_PLATFORM_LIBS` | What to link: SDK libraries, linker flags |
| `VIGIL_HAS_UI` | `ON` builds the LVGL UI and requires `hal_ui.h` |
| `VIGIL_UI_COLOR_DEPTH` | 32 (ARGB8888) or 16 (RGB565), whatever the UI layer takes |
| `vigil_platform_extras()` | Optional function that adds extra tools (e.g. `fbtest`) |

## 2. Capabilities: `hal.h`

`hal_caps()` returns a static `hal_caps_t`. Set only what works. The core
decodes only when `.decoder` is set and runs at most `.max_channels`
channels; the UI labels the board "DVR" when `.capture` is set:

```c
static const hal_caps_t g_caps = {
    .name = "sigmastar", .display = 1, .decoder = 1,
    .dec_max_w = 2560, .dec_max_h = 1920,  /* hal_dec_open refuses larger */
    .max_channels = 4,
};
```

`.max_channels` also decides the UI's layouts (2x2 up to 4 channels, 3x3
from 5, 4x4 from 10). Set `.fixed_tiles` when the platform places the
tiles itself and `hal_display_set_tile` cannot move them: the UI then
offers only the 2x2 view.

`hal_init(w, h, out)` brings up the vendor media system and the display:
a w×h screen (1280×720), the UI's and the tiles' coordinates, sent out at
mode `out` (`display.output`) if the platform has it, else at its own
mode. If it fails, it must undo what it already
brought up itself: vigild then exits without calling `hal_shutdown()`.
`hal_shutdown()` undoes a successful `hal_init` in reverse order.

## 3. Decoder: `hal_decoder.h`

These functions are called from the channel's own thread, one thread per
channel. Different channels may call them at the same time, so they must
not share unlocked state.

- `hal_dec_open(ch, codec, si)`: create channel `ch`'s decoder for
  `si->width × si->height` with `si->refs` reference frames, or return -1
  if the hardware cannot (too large: the channel is then only recorded).
  Size the
  picture memory from these values instead of the maximum: on small boards
  that is the difference between fitting three cameras or one. When the
  stream changes, the core calls `hal_dec_close` and then opens again; a
  failed open is retried after a few seconds. Connect the decoder's output
  to tile `ch`.
- `hal_dec_send(ch, au)`: one complete Annex-B access unit. Keyframes
  already carry their parameter sets. Wait a bounded time if the decoder is
  full (around 100 ms), then return -1. After a -1, the core sends nothing
  more until the next keyframe.
- `hal_dec_reset(ch)`: called after a reconnect. Drop queued data, keep the
  decoder, and do not leave the old picture on screen.
- `hal_dec_close(ch)`: destroy the decoder and release everything it
  holds.

Pictures must not travel back to the core: bind decoder → scaler →
display inside the platform (on SigmaStar: MI_VDEC → MI_SCL or MI_DIVP →
MI_DISP, linked with MI_SYS_BindChnPort).

## 4. Display: `hal_display.h`

`hal_display_set_tile(ch, r)` puts channel `ch`'s video into rectangle `r`
(screen pixels). Keep the source's aspect ratio inside it, with black bars.
It is called when a channel starts (the 2x2 tiles of `frontend_tiles()`)
and again, from the main thread while the decoders run, whenever the UI
changes the layout or shows a channel full screen: the tile may move and
grow. `hal_display_clear_tile(ch)` hides the channel until it is placed
again; nothing of its picture may stay on screen, including in parts of
the screen no other tile covers now.

`hal_display_modes()` lists the output modes the board can run at,
`hal_display_mode()` the one in force, and `hal_display_set_mode()` switches
while running (the Display page tries a mode before saving it, and puts
the old one back unless the user keeps it). The screen stays w×h: the
platform scales the UI layer and the tiles to the mode (SigmaStar: the
GOP's stretch and a video layer at the output's size). A platform that
cannot switch lists the mode it runs and refuses any other.

## 5. UI: `hal_ui.h`

The UI is an LVGL display on a graphics layer above the video. Two parts:

- `hal_ui_create(w, h)` creates the `lv_display_t` and its flush callback,
  which writes LVGL's rendered areas to the graphics layer. Choose:
  - **Framebuffer** (`/dev/fbN`): LVGL's own `lv_linux_fbdev` driver
    (see `src/platform/hisilicon/common/hal_ui_hifb.c`). Set the pixel
    format before creating the display, and keep any fd that holds the
    mode open: some drivers reset the layer when the last fd closes.
  - **A DRM plane**: render into a buffer and flip it in. Use two buffers,
    or the screen can show a half-drawn frame
    (`src/platform/rockchip/hal_ui_drm.c`).
- `hal_ui_hole(&color, &opa)` says how a tile is left see-through:
  - With per-pixel alpha (ARGB plane, `GOP` with alpha on SigmaStar):
    any colour with `LV_OPA_TRANSP`.
  - With a colour key: the key colour with `LV_OPA_COVER`, and enable the
    key on the layer (hifb uses magenta `0xFF00FF`).

Set `VIGIL_HAS_UI ON` and the right `VIGIL_UI_COLOR_DEPTH` once this
works. The UI runs on the main thread only.

## 6. Capture: `hal_capture.h` (DVR boards)

Only for boards with local video inputs; otherwise leave the stubs. Set
`.capture` in the caps to the number of inputs.

- `hal_cap_start(in, ch, cb, user)`: start input `in`, show it in tile
  `ch` yourself (VI → scaler → display), and encode it. Call
  `cb(user, au)` from a platform thread with each encoded Annex-B AU
  (`au->key` set on keyframes, parameter sets included or sent just before).
- `hal_cap_signal(in)`: 1 while the input sees a video signal.
- `hal_cap_stop(in)`.

Users then add one `-c capture:N` per input, N from 0 to `.capture` − 1
(`capture:0` … `capture:3` on a four-input board). A `capture:N` beyond the
board's inputs fails to start and keeps retrying, like an unreachable
camera.

## 7. Bring-up scripts

If the board needs kernel modules or libraries, add a script under
`scripts/<vendor>/`. Paths to vendor files come from environment
variables and the script stops with a clear message when one is missing.
No default should point into anyone's home directory. Files staged on a
board go to RAM (`/tmp`) unless the user asks otherwise. See
`scripts/hisilicon/stage_mpp_hi3536dv100.sh`.

## 8. Verify

On the board, with real cameras of the sizes you expect:

1. Every tile shows video, and `vigild`'s status reports the right size
   and fps.
2. The recorded segments decode with
   `ffmpeg -v error -i CAM1_….h265 -f null -`, with no errors printed.
3. Unplug a camera and plug it back: the tile shows the reconnect and
   resumes, and the old picture does not stay up.
4. Watch the vendor's error counters (decoder and display) while it runs.
5. Capture the real output, not only the framebuffer: an HDMI capture
   device, or the display controller's writeback if it has one. This shows
   the video and the UI blended as a viewer sees them.

Finally add `docs/platforms/<name>.md` describing the build, the board
set-up and what was verified, and a CI job if the platform can at least be
compiled on a hosted runner (the Rockchip job in `.github/workflows/ci.yml`
builds its libraries from source on an arm64 runner).
