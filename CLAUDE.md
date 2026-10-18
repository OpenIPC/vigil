# CLAUDE.md

Guidance for Claude Code (claude.ai/code) and other coding assistants working
in this repository. `AGENTS.md` is a symlink to this file.

Orientation only: what this is, how it builds, where things live, and the rules
that are specific to it. The design itself is in `docs/architecture.md`; read it
before changing anything structural.

## What this is

`vigil` is the DVR/NVR application for cheap recorder boards running OpenIPC:
one daemon, `vigild`, that takes cameras over RTSP or NETIP (or local analog
inputs on DVR boards), shows them on HDMI in a 2x2 view with an LVGL UI, and
records the compressed streams without re-encoding. One source tree builds for
HiSilicon (hi3536dv100, hi3520dv200), Rockchip (RK3588) and a hardware-free
`generic` platform used by CI and as a plain Linux recorder.

On OpenIPC firmware it is meant to ship as the `vigil` package
(`general/package/vigil` in OpenIPC/firmware, being added for hi3536dv100):
started at boot by `/etc/init.d/S95vigil`, a copy of majestic's supervisor
(restart on crash with back-off, give up after a crash loop), configured in
`/etc/vigil.yaml`, and run with `-s` so its log goes to syslog.

## The layering rule (the one that must not be broken)

- **`src/core` is pure C11 + POSIX.** No vendor header, no `#ifdef` on the
  platform, ever. It builds and runs its tests on an x86 host; CI proves it.
  If a feature seems to need platform knowledge in the core, the HAL is
  missing something: extend `src/hal/*.h` for every platform instead.
- **`src/ui` knows LVGL and `hal_ui.h` only.** The UI runs on the main thread;
  channel threads reach it only through `channels_status()` snapshots and the
  `events` queue.
- **Pixels never pass through the core.** Decoded or captured pictures stay
  inside the platform (VDEC→VPSS→VO, MPP→RGA→DRM). The core handles
  compressed access units only.
- **Vendor SDKs, kernel modules and libraries stay outside the repository.**
  Their locations come from CMake variables or the environment, with no
  default that points into anyone's home directory. Do not write local paths,
  machine names or repository/remote topology into any tracked file.

## Layout

- `src/core/` — channel threads, the access-unit normalizer (`au_norm`), SPS
  parsing, the segment writer, events, small net/md5 helpers.
- `src/core/source/` — camera protocols behind `source_ops_t`: RTSP
  (`rtsp_client`, `rtp_depay`), NETIP, `capture:N`. A new protocol is one
  `source_<name>.c` listed in `source.c`.
- `src/hal/` — the platform contract, declarations only.
- `src/platform/<name>/` — one HAL implementation per SoC family;
  `src/platform/hisilicon/common/` is shared by the two HiSilicon SoCs.
- `src/ui/`, `src/app/` — LVGL screens; `main.c` and the UI or headless loop.
- `cmake/platforms/<name>.cmake` — sources, includes, libraries per platform.
- `scripts/` — board bring-up (HiSilicon module staging, Rockchip library build).
- `tests/` — `test_core.c` unit tests, a NETIP camera emulator, a GStreamer
  RTSP server, and the scripts that run `vigild` against them.
- `docs/` — architecture, porting guide, one page per platform.

## Build

```sh
# host (generic platform) + tests
cmake -B build && cmake --build build -j && (cd build && ctest --output-on-failure)

# hi3536dv100, against the SDK (static) or the firmware's headers + shared libs
cmake -B build-hi3536 --toolchain cmake/toolchains/cross.cmake \
      -DCROSS_PREFIX=<toolchain>/bin/arm-openipc-linux-gnueabi- \
      -DVIGIL_PLATFORM=hi3536dv100 -DHI3536DV100_SDK=<sdk root>
#   or -DHI3536DV100_INCLUDE_DIR=<headers> -DHI3536DV100_LIB_DIR=<dir with libmpi.so>

# rockchip, natively on the board, after sudo scripts/rockchip/build-deps.sh
cmake -B build -DVIGIL_PLATFORM=rockchip && cmake --build build -j
```

The per-platform pages under `docs/platforms/` have the full steps, including
loading the HiSilicon kernel modules and the hi3520dv200 musl toolchain.

## Tests

`ctest` in a generic build runs the unit tests and end-to-end runs of `vigild`
against `tests/netip_server.py` and `tests/rtsp_server.py` (skipped without the
GStreamer RTSP server), including misbehaving-server cases; the end-to-end tests
decode the recordings with ffmpeg. A parser or protocol change comes with a test
in `tests/test_core.c` that fails without it.

CI (`.github/workflows/ci.yml`): the generic build + ctest on x86, and a
compile-only Rockchip build on an arm64 runner. The HiSilicon platforms are not
built in CI (the SDK is not redistributable here); cross-build them locally
before pushing a change that touches them.

## Verifying on hardware

Platform changes are verified on a board, and the pull request names the board
and shows the output of that run (`vigild` log, `/proc/umap/vdec` and
`/proc/umap/vpss` counters on HiSilicon, an HDMI capture or a writeback
snapshot). The checklist is in `docs/porting.md`, "Verify".

- **Never write hardware evidence you did not observe.** If a change has not
  run on a board, say so plainly in the pull request.
- The status column in `README.md` and the "Verified" sections of the platform
  pages state only what has been run. Unverified work is tracked as issues.

## Workflow

`main` is protected: changes land through pull requests with the `CI` and
`qodo-gate` checks green. Qodo reviews every pull request; answer each of its
threads (fix it, or decline with a reason) and resolve it, then comment
`/review` for a fresh pass. Pull requests are squash-merged. Keep one pull
request open at a time; multi-step work lands step by step.

Commit messages: an imperative summary line, a body that says why. No
AI-assistant footers in commits or pull request descriptions. Comments in the
code explain *why* (a hardware quirk, a measured limit, a field failure), not
what the next line does.

## Relationship to other OpenIPC repositories

- **OpenIPC/firmware** provides the board side: `load_hisilicon` loads the MPP
  modules, and the SDK's shared libraries ship with
  `hisilicon-osdrv-hi3536dv100`. vigil needs `open_vdec` loaded with
  `VBSource=1`, because it sizes a private picture pool per decoder from the
  SPS; one common pool for every camera does not fit the 128 MiB MMZ.
- **OpenIPC/openhisilicon** builds the hi3536dv100 kernel modules (`open_*.ko`)
  and carries the SDK headers the firmware build compiles vigil against.
  Revision-2 Hi3536DV100 chips need `open_vdec` with the SDK module's
  chip-revision check bypassed; without it, decoding silently never starts.
- **LVGL** is a submodule (`third_party/lvgl`, v9.2.2); clone with
  `--recursive`.
