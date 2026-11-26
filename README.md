# vigil

Open-source DVR/NVR software for cheap recorder hardware running
[OpenIPC](https://openipc.org). It shows cameras on HDMI in a 2×2 view
with a small status UI and records their streams to disk. One code base
runs on HiSilicon, Rockchip and SigmaStar boards; plain Linux / cloud hosts
are part of the design.

- **Network cameras** over RTSP or Xiongmai NETIP (DVRIP), H.264 and
  H.265. They are recorded without re-encoding on every platform, and
  decoded in hardware for display where the board has a decoder and a
  display (see the table below).
- **Local analog inputs** on DVR boards: shown, then encoded in hardware
  for recording.
- **Recording** into segments that play on their own, with a free-space
  reserve: the oldest recordings are deleted when the disk fills up.
- **Configuration** in one JSON file (`/etc/vigil.json`) and an
  **HTTP/WebSocket API** to read and change it while video keeps running,
  with live status and events; login with the system account
  ([docs/api.md](docs/api.md)).

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
cmake -B build && cmake --build build -j && (cd build && ctest)
./build/vigild -o /tmp/rec -c rtsp://user:pass@camera/stream -c netip://admin:@dvr:34567/
```

Or put the cameras in a configuration file
([example](docs/vigil.example.json)) and run `vigild -f vigil.json`;
then `curl -u root:PASSWORD http://localhost:8080/api/v1/status`.

For a board, see its page under [docs/platforms](docs/platforms/).

## Documentation

- [Architecture](docs/architecture.md): how the code is split into core,
  protocol sources, HAL and platforms, and how a stream flows through it.
- [Configuration and HTTP API](docs/api.md): the configuration file, the
  endpoints, the WebSocket, authentication and password redaction.
- [Adding a platform](docs/porting.md): what a new SoC family has to
  implement.
- Platforms: [HiSilicon](docs/platforms/hisilicon.md),
  [Rockchip](docs/platforms/rockchip.md),
  [SigmaStar](docs/platforms/sigmastar.md),
  [generic](docs/platforms/generic.md).

## License

GPL-3.0-or-later. [LVGL](https://lvgl.io), [cJSON](https://github.com/DaveGamble/cJSON)
and [CivetWeb](https://github.com/civetweb/civetweb) (all MIT) are included
as submodules.
Vendor SDKs, libraries and kernel modules are not part of this repository.
They are used from wherever you keep them.
