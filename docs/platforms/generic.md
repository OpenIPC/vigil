# Generic (any Linux host)

`VIGIL_PLATFORM=generic` is the default. It has no media hardware: no
decoder, no display, no capture inputs. `vigild` connects to the cameras,
records their streams without re-encoding, and prints each channel's status
to the log instead of drawing a UI.

It serves three purposes:

- **CI and development**: the whole core (protocols, normalizer, recorder)
  builds and runs its tests on an ordinary x86 machine.
- **A plain recorder** on any Linux server, VM or cloud instance. The CPU
  only copies compressed data, so the cost per camera is small.
- **Proof that the core needs no hardware**. If something in `src/core`
  stops building here, it has picked up a platform dependency.

## Build and test

```sh
git clone --recursive <repo> vigil && cd vigil
cmake -B build && cmake --build build -j
(cd build && ctest --output-on-failure)
```

Building needs CMake and libyaml (`libyaml-dev`). The end-to-end tests
need Python 3 and ffmpeg. The RTSP tests also need
the GStreamer RTSP server. On Debian/Ubuntu:

```sh
apt install cmake libyaml-dev ffmpeg python3-gi gir1.2-gst-rtsp-server-1.0 \
    gstreamer1.0-plugins-ugly gstreamer1.0-plugins-good gstreamer1.0-plugins-bad
```

Without the GStreamer server, the RTSP tests are reported as skipped.

## Run

```sh
mkdir -p ~/rec
./build/vigild -o ~/rec -S 300 -K 0 \
    -c rtsp://user:pass@cam1/stream=0 \
    -c 'netip://admin:@dvr:34567/?ch=1&stream=extra'
```

| Option | Meaning |
|---|---|
| `-f FILE` | The configuration file (default `/etc/vigil.yaml`; JSON when the name ends in `.json`; see [api.md](../api.md)) |
| `-c URL` | A camera, repeated for each one, in tile order (formats in [architecture.md](../architecture.md#sources-camera-protocols)); replaces the file's cameras |
| `-o DIR` | Where to record; `none` disables recording (default: `record.path` from the file, which is empty — no recording — unless set) |
| `-S SECS` | Segment length, 10..3600 (default 60) |
| `-K N` | Segments kept per camera, 0..64 (default 3; 0 keeps all, limited only by the free-space reserve) |
| `-t SECS` | Exit after SECS seconds (default: run until SIGINT/SIGTERM) |
| `-s` | Log to syslog (tag `vigil`, facility daemon) instead of stderr, including what the platform and vendor libraries print |
| `-w OUT` | Write the configuration (the file with the options applied) to OUT and exit: YAML, or JSON when OUT ends in `.json`; `-` prints it |

Options override the configuration file for that run. The same settings,
and the HTTP API that reads and changes them while `vigild` runs (on port
8080 unless `api.listen` says otherwise), are described in
[api.md](../api.md).

Recordings are named `CAM<n>_<YYYYmmdd>_<HHMMSS>.h264` or `.h265`. They
are raw elementary streams that ffmpeg and VLC play directly, and that
`ffmpeg -i … -c copy out.mp4` puts into a container.

## NETIP emulator

`tests/netip_server.py` is a minimal NETIP camera that serves an H.264 or
H.265 file. It is useful for trying vigil, or another NETIP client,
without an XM camera:

```sh
# terminal 1: the emulated camera (stop it with Ctrl-C)
python3 tests/netip_server.py tests/data/testsrc_640x360.h264 --port 34567
# terminal 2, from the repository root
./build/vigild -t 20 -c netip://admin:@127.0.0.1:34567/
```
