# Architecture

vigil is one program, `vigild`, that turns a cheap recorder board into a
DVR/NVR: it takes video from cameras, shows it on the HDMI output in a
grid of tiles with a mouse and keyboard UI on top ([ui.md](ui.md)), and records
the streams to disk. The same source tree builds for very different hardware. This page
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
   +-----------+------------+----------------+
   |                        |                |
src/core                 src/ui           src/api      HTTP/WebSocket API
 channel  source/*  au_norm  sps          (CivetWeb, crypt)
 seg_writer  events  net  md5
 config  schema                LVGL app: live view, menus, schema-driven
                               settings; sees only ui_model.h and hal_ui.h
   |                        |                |
   +-----------+------------+----------------+
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
- `src/ui` only knows LVGL and `hal_ui.h`. What it shows and changes comes
  through `ui_model.h`, which `src/app/ui_bridge.c` fills from the core.
- `src/api` knows CivetWeb, `crypt(3)` and the core's public headers. It
  reads snapshots and writes the configuration store; it never drives the
  channels or the UI itself. The zero channel's web server (`web.c`) gets
  the screen's pictures and hands the browser's input on through `kvm.h`,
  which the app implements (`src/app/kvm.c`): the pictures are composed
  from LVGL's flushes and the platform's `hal_display_grab`, the input
  goes into the evdev reader as one more mouse and keyboard.
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
   - Each keyframe gets every parameter set seen so far. Some cameras send
     them only once or out of band (in the RTSP SDP), so the normalizer
     caches them and adds the ones a keyframe lacks. It cannot invent sets
     the camera has not sent yet.
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

### Main stream and sub-stream

A camera can have a second session: its **sub-stream** (`sub_url`, or for
NETIP the same URL with `stream=extra`), a second thread with its own
source and normalizer. Both sessions run, and reconnect with the same
back-off, for as long as the channel does, whatever is on screen: the main
one carries the camera's events and the recording, the sub-stream is there
the moment a small tile wants it. Each session keeps its own state; the
channel's status is the state of the one whose picture is on screen (the
decoder's holder, else the one the tile waits for), and the status carries
the main and the sub-stream's states besides, so a main stream that fails
while a small tile shows the sub-stream is said there, not as "No video"
on the tile. The tile decides which one is
decoded (`channels_set_tiles`): a tile of a quarter of the screen or less
shows the sub-stream, a larger one or full screen the main stream; a
channel off screen, where the platform's decoders are scarce
(`dec_shown_only`), neither. The channel's one decoder belongs to one
session at a time, and the wanted one takes it over at once, without
waiting for its camera's next keyframe (1-4 s on most cameras): each
session keeps the GOP it is in (`gop_cache.c`: the access units since the
last keyframe, as `au_norm` hands them out), the sub-stream always and the
main stream while its tile is on screen (only such a tile can go full
screen). At its next access unit after the view changes the wanted session
opens the decoder at the cached keyframe, feeds it the GOP so far and goes
on live; the platform decodes the cached pictures as fast as it can, so
the picture runs fast up to the present for a moment. Full screen and
back, another page or layout all switch that way. Without a GOP to start
from (dropped, or the stream just connected) the wanted session takes the
decoder at its keyframe as before, and the other keeps feeding it until
then, so the picture stays up through a switch; a channel that comes on
screen full size, whose main stream's GOP was not kept, shows its
sub-stream's meanwhile. A GOP may take 1 MiB (sub-stream) or 4 MiB (main
stream), and all of them 24 MiB (`GOP_CACHE_BUDGET`): one that outgrows
either is dropped until the next keyframe, and a switch to it waits for
that keyframe. A camera has at most two connections; full screen, the
display and the recording share the main one. A view change never touches
the main session, so recording goes on in the same segment.

A decoder the platform cannot give (out of media memory or decoder
channels) is retried after 10 s, or as soon as another channel closes its
decoder.

### The picture's shape

A picture is shown at its camera's display aspect ratio, fitted inside its
tile (or the full screen) and centred, with black bars where the shapes
differ (`aspect.c`, the same geometry on every platform). The ratio is not
the coded size's: D1 and CIF sub-streams (704x576, 720x576, 704x480,
352x288) are anamorphic, so a sub-stream is shown at its **main stream's**
ratio, which the main session (always running) knows from its SPS; then
the sub-stream and the main stream look the same, and full screen and back
do not change the picture's shape. Before the main stream's SPS has been
seen, or for the main stream itself, the stream's own size times the
sample aspect ratio its SPS's VUI gives (1:1 when it gives none). A ratio
within 2 % of a common one (16:9, 4:3, 16:10, 5:4, 3:2, 1:1, 2:1, 32:9) is
taken as that one: 800x448 or an uncropped 1920x1088 is 16:9. The core
tells the platform the ratio before a decoder opens and whenever it
changes (`hal_display_set_aspect`), with how the picture fills the tile:
`display.video_aspect: stretch` stretches it over the tile whatever its
shape, and `fill` keeps its shape and covers the whole tile, the part that
overflows cut off, centred (`aspect_crop`: a 4:3 camera in a 16:9 tile
loses a strip at the top and at the bottom).

### Recording

`seg_writer.c` writes the compressed elementary stream (no re-encoding,
no container) into segments of `-S` seconds. What it writes is the
normalizer's output, so it is not a byte-for-byte copy of the network
data: cached parameter sets are added to keyframes that lack them,
picture-less units are merged into the next picture, and anything before
the first keyframe is dropped. The pictures themselves are untouched.
Every segment starts at a keyframe, carrying all the parameter sets the
camera has sent by then. So once a camera has supplied them, which is
normally with its first keyframe or in the SDP, each file plays on its own
(`ffplay CAM1_….h265`). `-K` limits how many segments each camera keeps.

Independently, the writer tries to keep 10% of the filesystem (at least
8 MiB) free. Below that it deletes the oldest finished recording in the
directory, so a disk filled by recordings rolls over instead of stopping
them. If nothing can be deleted (there are no finished recordings, or the
space is used by other files), writing fails: the channel reports the
failure and retries at the next keyframe.

Before a segment writer is set up, `rec_place.c` decides whether the
directory may be recorded to at all: only on a disk, never on the root
filesystem (a board's flash) unless `record.allow_rootfs` says it is a disk,
never on RAM or flash filesystems ([api.md](api.md#where-recordings-may-go)).
A channel whose directory is refused, or whose disk goes away, keeps its
video and tries again every 10 s.

### Status and events

Channel threads never touch the UI. They publish a status snapshot
(`channels_status()`: live, fps, codec, size, recording, last error, each
stream's state, the camera's own name for the channel) and
short events ("CAM2 offline") through a thread-safe ring (`events.c`).
The ring has many readers, each with its own cursor: the UI thread (tile
flashes, toasts) and the API's WebSocket feed both see every event, and a reader that
falls behind skips ahead and is told how many it missed. Without a display,
`frontend_headless.c` prints the status to the log.

## Management

The management pages (network, time, host name, disks, network storage,
mail, users) change the operating system, not only vigild's settings.
`src/sys` does that the OpenIPC firmware's way ([management.md](management.md)):
each backend (`sys_net`, `sys_time`, `sys_disk`, `sys_nas`, `sys_curl`) is
one kind of change, reading and writing files under a root and running
commands through replaceable actions (`sys.h`), so `test_sys` exercises
them on a fake system. `mgmt.c` sits between them and the two front ends:
slow changes run as jobs on worker threads (the API answers 202 and a job
id, the UI polls), and a timer thread undoes a network change nobody
confirmed, keeps NTP, DDNS and the network storage going and passes events
to the alarm center and the mail. It holds recording off a disk before it
is unmounted or formatted (`channels_rec_hold`).

## Configuration

The settings live in one YAML file (`/etc/vigil.yaml`, `-f FILE`; format
and API in [api.md](api.md)). Each setting is one row of a table,
`config_schema.c`: its place in `vigil_config_t`, type, default, limits,
label, help text, and whether it applies at run time. A generic engine
(`schema.c`) does every job from that table: defaults, parsing and
checking JSON (every error with its path), checking a struct built in C
(the command-line options), writing JSON with camera passwords redacted,
and describing the table itself so a UI can build its forms from it.
Adding a setting is a row and a struct member. Inside, a configuration is
always a JSON document (cJSON); the file is the one place it is YAML:
`schema_yaml.c` reads the YAML with libyaml into that document, typing
each scalar by the schema field it belongs to, and writes it back with
the schema's labels and help as comments. A file named `*.json` is read
and written as JSON instead.

`config.c` is the store: the settings in force behind a mutex, with a
generation counter. Readers take snapshots (`config_get`); a writer
(`config_put_json`) validates the new document, saves it atomically
(temporary file, `fsync`, `rename`, `fsync` of the directory) and only
then bumps the generation. Precedence at start: defaults < the file <
command-line options.

Applying is the main thread's job, so the UI is never touched from
elsewhere. The on-screen settings pages ([ui.md](ui.md)) write through
`config_put_json` too, so a change made on the screen is checked, saved
and applied exactly like one made through the API. The frontend loop (LVGL or headless) calls a tick several times
a second; when the generation moved, it hands the new camera list and
recording settings to `channels_apply()`. That compares each tile with
what runs there: a renamed camera keeps its stream, a changed URL or a
changed recording setting stops and restarts just those channels (all the
stops first, so their timeouts overlap). Each channel has its own stop
flag, and its status lock outlives restarts, so `channels_status()` is
safe from any thread at any time. The outcome goes out as events. A
changed `display.output` switches the output (`hal_display_set_mode`)
unless it already runs at that mode: the Display page switches first and
saves only a mode the user keeps.

## Threads

| Thread | Does |
|---|---|
| main | `hal_init`, the frontend loop (LVGL or headless), applying configurations, `hal_shutdown` |
| one per channel | main stream: source → `au_norm` → recorder, and the decoder for large tiles (`channel.c`) |
| one per channel in a small tile | sub-stream: source → `au_norm` → decoder (`channel.c`) |
| platform threads | e.g. capture readers (hi3520dv200), as each platform needs |
| CivetWeb master + up to 8 workers (started on demand) | HTTP requests and WebSocket sessions (`src/api`) |
| API pusher | gathers status, events and configuration changes for the WebSocket clients |
| API writers (4) | one per WebSocket client: writes its messages, so a stalled client stalls only itself |
| zero channel: CivetWeb master + up to 6 workers | its page, logins and WebSockets (`src/api/web.c`) |
| zero channel viewers (2) | one per viewer, at nice 10: composes, encodes and sends its pictures (`kvm_next`) |
| log reader (`-s`) | turns stdout/stderr lines into syslog messages |

## Sources (camera protocols)

| URL | Protocol | Driver |
|---|---|---|
| `rtsp://[user:pass@]host[:554]/path` | RTSP, RTP interleaved over TCP; Basic and Digest auth; H.264 and H.265 | `source_rtsp.c`, `rtsp_client.c`, `rtp_depay.c` |
| `netip://[user:pass@]host[:34567]/[?ch=N&stream=main\|extra]` | Xiongmai NETIP / DVRIP: XM cameras and DVRs, and OpenIPC majestic with `netip` enabled | `source_netip.c` |
| `capture:N` | Local video input N of the board (DVR) | `source_capture.c` → `hal_capture.h` |

A driver implements `source_ops_t` (`source.h`): `create` parses the URL,
`open` connects, `poll` waits up to about a second and emits complete AUs,
and `close`/`destroy` clean up. Every driver watches the channel's stop
flag, so stopping never waits for a slow camera. Adding a protocol (RTMP,
HTTP-FLV, …) means writing one new `source_<name>.c` and listing it in
`source.c`. Nothing else changes.

**Discovery** is not a source: `discovery.c` finds cameras (NETIP, ONVIF
WS-Discovery, Dahua DHIP), changes their addresses and adds them to the
cameras section through the configuration store, as the API and the
settings pages do. Its network work runs as jobs on a thread of its own;
the API (`api_disc.c`) and the UI (through `ui_model.h`) start them and
read their state. Formats, sources and the protocol matrix are in
`disc_proto.h`, `disc_ops.h` and [api.md](api.md#discovery).

The other way round, the **ONVIF server** (`src/api/onvif.c`,
[onvif.md](onvif.md)) makes the recorder an ONVIF device for VMS: like
the API it only reads snapshots (`channels_status` gives each stream's
codec and size, `config_get`), its wire formats are pure functions in
`src/core/onvif_proto.c`, and the camera search skips its WS-Discovery
answers by their endpoint reference (`disc_set_self`).

## The HAL

| Header | What the platform does |
|---|---|
| `hal.h` | `hal_init(w, h, out)` brings up the media system and the output (a w x h screen sent out at mode `out` where the platform can); `hal_caps()` says what is there (display, decoder and its maximum size, number of channels, capture inputs); `hal_shutdown()`. |
| `hal_decoder.h` | One hardware decoder per channel: open for a codec, size and reference count; send an AU; reset after a reconnect; close. Decoded pictures go to that channel's tile without passing through the core. |
| `hal_display.h` | Where each channel's tile is on the screen, and the display aspect ratio its picture is fitted inside it at (`aspect_fit`), stretched over it, or covering it with the overflow cut off (`aspect_crop`). The output modes it can switch to, the one in force, and a switch while running (the screen keeps its size, scaled to the mode). |
| `hal_capture.h` | Local inputs: start input N in tile `ch` (the platform shows it itself) and deliver its *encoded* AUs to a callback for recording. |
| `hal_ui.h` | Create the LVGL display on the graphics layer above the video, and say how the UI makes a tile transparent (a colour key, or alpha). |

Video never passes through the core as raw pixels. Decoded and captured
pictures stay inside the platform (VDEC→VPSS→VO on HiSilicon, MPP→RGA→DRM
on Rockchip). The core only handles compressed data, which keeps it
portable and cheap on small CPUs.

## Screen layout

The screen is 1280×720, all of it a grid of 1, 4, 9 or 16 tiles
(`src/ui/ui_layout.c`; tile edges on 16-pixel columns and even rows, which
every video layer places exactly). Channels start in the 2×2 grid (`frontend_tiles`); the UI moves
them for another layout, another page or one channel full screen through
`channels_set_tiles()`, which places each running channel with
`hal_display_set_tile` (or hides it with `hal_display_clear_tile`) from the
main thread. LVGL draws the clock, each tile's name and icons (where and
how: `display.*`) and, when asked for, the navigation bar and menus over
the video, and
paints the rest of each tile as a "hole" so the video plane underneath
shows through: a colour key on HiSilicon hifb and SigmaStar's GOP, full
transparency on Rockchip's alpha-blended plane. The screens, the
schema-driven settings pages, mouse and keyboard input and the UI tests are described in
[ui.md](ui.md).

## Build

```
CMakeLists.txt
  cjson           static library: third_party/cJSON (MIT)
  vigil_yaml      the system's libyaml (MIT), by pkg-config (yaml-0.1) or a
                  search; cross builds look in the toolchain's sysroot
  vigil_core      static library: src/core (+ host unit tests)
  vigil_platform  object library: the files listed by cmake/platforms/<p>.cmake
  vigil_api       static library: src/api + CivetWeb (cmake/api.cmake;
                  -DVIGIL_API=OFF leaves it and libcrypt out)
  lvgl, vigil_ui  LVGL and src/ui (platforms with a display; host builds
                  for the UI tests)
  vigild          src/app + vigil_ui (when the platform has a display)
cmake/platforms/<p>.cmake   sources, include paths, SDK/library paths
cmake/toolchains/cross.cmake   cross compiling: -DCROSS_PREFIX=…/bin/<triplet>-
```

Per-platform build and run steps are in [platforms/](platforms/), and
adding a platform is covered in [porting.md](porting.md).

## Tests

`ctest` in a generic build runs:

- `core`: unit tests for SPS parsing, the normalizer, RTP depacketizing,
  RTSP framing and helpers, the capture source (with a fake HAL), the
  segment writer, the settings schema (types, limits, enums, unknown keys),
  the configuration file (YAML round trip, the generated comments, quoting,
  boolean spellings, invalid YAML, aliases, depth, `.json` files, atomic
  save, `docs/vigil.example.yaml` being what vigild writes), the
  store (sections, password redaction and `***`), the event ring, and
  channels following a new layout (`channels_set_tiles`, with a fake
  HAL).
- `api_auth`: Basic credentials, the shadow/`crypt(3)` check, the accounts
  file (hashing, logging in, the root fallback, built-in groups) and the
  table of permissions per endpoint.
- `api_auth`: Basic credentials and the shadow/`crypt(3)` check.
- `discovery`: the discovery parsers on good, malformed, oversized and
  truncated replies, and on thousands of randomly changed ones (two ONVIF
  replies are real cameras'), the WS-Security and Dahua login hashes, the
  address plan; `discovery_e2e`: the discovery API against
  `tests/fake_discovery.py` (NETIP, ONVIF and Dahua devices on 127.0.0.x):
  scan, merge, address changes over each protocol with wrong and right
  passwords, a dry run, adding to a channel, auto-add, the rate limit and
  that no password leaks.
- `onvif`: SOAP requests, UsernameToken and HTTP Digest (published
  vectors), WS-Discovery probes and their matching, our ProbeMatches as
  the camera search reads them; each request cut at every length.
  `onvif_e2e`: `vigild`'s ONVIF server as a client sees it.
- `theme_tokens`: the generated `theme_tokens.h` and `web/theme.css` match
  `src/ui/theme.json`.
- `ui_screens`: the UI on an in-memory display, driven through the evdev
  reader with a fake mouse, saving through the real store; widget checks
  and screenshots compared with `tests/ui/*.png` ([ui.md](ui.md)).
- `api_e2e`: `vigild` with a hand-written YAML configuration, shadow file and port,
  driven over HTTP and a WebSocket by `tests/api_test.py`: authentication
  and lockout, a camera added from the NETIP emulator coming up live,
  invalid configurations refused with every error, redaction, the file
  saved as commented YAML (read back with `vigild -w`), and the pushed
  status, event and configuration messages.
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
