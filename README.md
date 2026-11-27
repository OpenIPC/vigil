# vigil

Open-source DVR/NVR software for cheap recorder hardware running
[OpenIPC](https://openipc.org). It shows cameras on HDMI with a
mouse and keyboard UI and records their streams to disk. One code base
runs on HiSilicon, Rockchip and SigmaStar boards; plain Linux / cloud hosts
are part of the design.

- **Network cameras** over RTSP or Xiongmai NETIP (DVRIP), H.264 and
  H.265. They are recorded without re-encoding on every platform, and
  decoded in hardware for display where the board has a decoder and a
  display (see the table below). Multi-window views show the cameras'
  sub-streams (`sub_url`, derived for NETIP), full screen the main stream;
  recording always takes the main stream.
- **Local analog inputs** on DVR boards: shown, then encoded in hardware
  for recording.
- **Recording** into segments that play on their own, with a free-space
  reserve: the oldest recordings are deleted when the disk fills up. Only
  onto a disk: with none mounted where the path leads, nothing is written
  to the root filesystem (a board's flash) or to RAM.
- **Configuration** in one YAML file (`/etc/vigil.yaml`, like majestic's,
  every setting explained in a comment) and an
  **HTTP/WebSocket API** to read and change it while video keeps running,
  with live status and events; login with the system account
  ([docs/api.md](docs/api.md)).
- **On-screen UI** with a USB mouse and/or keyboard: a full-screen live
  view (1 to 64 windows, as many as the platform shows) with page arrows at
  the screen's edges and double-click full screen; on each tile the
  camera's name, REC and event icons, and its state when there is no
  picture (offline, authentication error, ...); a "+" on an empty channel
  to add a camera there; a clock, a context menu, an auto-hiding navigation
  bar, a channel info page, a compact on-screen keyboard, and settings
  pages built from the same schema as the API ([docs/ui.md](docs/ui.md)).
- **Zero channel**: that screen, live, in a browser at `http://<recorder>/`,
  with its mouse and keyboard, for users given the `remote` permission
  ([docs/api.md](docs/api.md#zero-channel)).
- **RTSP server**: every camera's main and sub-stream to a VMS, VLC or
  another recorder at `rtsp://<recorder>/ch1/main`, as the camera sends it
  (no re-encoding), over TCP or UDP, starting at once at a keyframe, with
  Digest logins of vigil's users ([docs/rtsp.md](docs/rtsp.md)).

## Platforms

| Platform | Hardware | Role | Status |
|---|---|---|---|
| [`hi3536dv100`](docs/platforms/hisilicon.md#hi3536dv100-nvr) | HiSilicon Hi3536DV100 NVRs | decode + HDMI + record | verified on hardware |
| [`rockchip`](docs/platforms/rockchip.md) | RK3588 (Orange Pi 5 / 5 Plus, …) | decode + HDMI + record | verified headless (writeback) |
| [`hi3520dv200`](docs/platforms/hisilicon.md#hi3520dv200-dvr--experimental) | HiSilicon Hi3520DV200 DVRs | capture + HDMI + encode | experimental, builds |
| [`sigmastar`](docs/platforms/sigmastar.md) | SigmaStar SSR621Q NVRs (infinity2m) | decode + HDMI/VGA + record | experimental, qemu only |
| [`generic`](docs/platforms/generic.md) | any Linux host | record only | CI-tested |

## Quick start (any Linux host)

```sh
git clone --recursive https://github.com/OpenIPC/vigil && cd vigil
sudo apt install cmake libyaml-dev           # Debian/Ubuntu
cmake -B build && cmake --build build -j && (cd build && ctest)
./build/vigild -o /tmp/rec -c rtsp://user:pass@camera/stream -c netip://admin:@dvr:34567/
```

Or put the cameras in a configuration file
([example](docs/vigil.example.yaml)) and run `vigild -f vigil.yaml`;
then `curl -u root:PASSWORD http://localhost:8080/api/v1/status`.

For a board, see its page under [docs/platforms](docs/platforms/).

## Documentation

- [Architecture](docs/architecture.md): how the code is split into core,
  protocol sources, HAL and platforms, and how a stream flows through it.
- [Configuration and HTTP API](docs/api.md): the configuration file, the
  endpoints, the WebSocket, authentication and password redaction.
- [RTSP server](docs/rtsp.md): the cameras' streams to other clients.
- [On-screen UI](docs/ui.md): screens, data binding, schema-driven
  settings, design tokens, mouse and keyboard input, headless tests.
- [Adding a platform](docs/porting.md): what a new SoC family has to
  implement.
- Platforms: [HiSilicon](docs/platforms/hisilicon.md),
  [Rockchip](docs/platforms/rockchip.md),
  [SigmaStar](docs/platforms/sigmastar.md),
  [generic](docs/platforms/generic.md).

## License

GPL-3.0-or-later. [LVGL](https://lvgl.io), [cJSON](https://github.com/DaveGamble/cJSON)
and [CivetWeb](https://github.com/civetweb/civetweb) (all MIT) are included
as submodules; [libyaml](https://github.com/yaml/libyaml) (MIT) is used from
the system.
Vendor SDKs, libraries and kernel modules are not part of this repository.
They are used from wherever you keep them.
