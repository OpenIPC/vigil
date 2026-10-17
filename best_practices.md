# vigil review standards

These are the judgement-based rules a reviewer (human or bot) applies to a vigil
pull request. The binary gates are in `pr_compliance_checklist.yaml`.

vigil records cameras on cheap DVR/NVR boards that run unattended for months.
The failures that matter are the quiet ones: a recording that does not play, a
channel that stopped reconnecting, a disk that filled up last week. Review for
those first.

## 1. The media path

### 1.1 Access units stay whole and in order
Every source produces Annex-B access units; `au_norm` makes them uniform for the
decoder and the recorder. A change to a source or to `au_norm` must keep:
- one emitted unit = one picture, with the units that precede it (parameter
  sets, SEI) carried into it rather than emitted on their own;
- parameter sets in front of every keyframe that lacks them, so each recorded
  segment and each decoder start is self-contained;
- nothing emitted after a discontinuity until the next keyframe.

### 1.2 Loss is detected, not guessed
RTP sequence gaps, truncated NETIP frames and short reads are discontinuities.
Code that "recovers" by resyncing on a byte pattern must say why the data cannot
be read in order instead — the RTSP partial-header bug (a `$` frame whose
4-byte header arrived in pieces, dropped by a resync) is the example.

### 1.3 Recordings play on their own
`seg_writer` starts a segment only at a keyframe. Any change to segmenting,
retention or the free-space reserve needs a test in `tests/test_core.c` and a
statement of what a viewer sees when the disk is full.

## 2. Threads

- Each channel has one thread; shared status is read under the channel's lock.
- LVGL is called only from the UI thread. Other threads post through
  `events.c` or a status snapshot.
- Platform callbacks (capture) arrive on platform threads and must not block
  them for long; queue and return.
- Every loop that waits checks the stop flag at least once a second.

## 3. Bounds

Everything that grows with input has a cap: access-unit buffers, queues,
reassembly buffers, the number of segment files. A cap that is hit drops data
and flags a discontinuity — it does not grow, and it does not crash.

## 4. The hardware boundary

### 4.1 The core does not know the board
`src/core`, `src/ui` and `src/app` (except `frontend_ui.c`) include no vendor
header and test no platform macro. What a board can do is asked through
`hal_caps()`; how it is done is behind `hal_*.h`.

### 4.2 A new platform is one directory and one CMake file
Adding SigmaStar, a new HiSilicon generation or a cloud backend means
`src/platform/<vendor>/...` plus `cmake/platforms/<name>.cmake`. If it needs a
change in the core, the change is a HAL interface every platform can implement
(or stub), argued in its own PR.

### 4.3 A new protocol is one source
A camera protocol is a `source_<name>.c` with a `source_ops_t`, registered in
`source.c`, emitting access units. It does not touch the channel, the decoder
or the recorder.

## 5. Provenance

- The repository is GPL-3.0-or-later. Vendor SDKs, libraries and kernel modules
  are referenced from outside the tree through CMake cache variables.
- Code taken from another project keeps its licence notice and must be
  GPL-compatible. Vendor sample code with a proprietary notice is rewritten,
  not copied.
- Protocol knowledge from reverse-engineered or documented sources (for example
  NETIP from python-dvr) is cited in the file header.

## 6. Evidence

- **Core changes**: `ctest` passes; parser and protocol fixes add the failing
  case to `tests/`.
- **Platform changes**: name the board and paste its output — the vigild log,
  `/proc/umap/*` counters on HiSilicon, MPP/DRM logs on Rockchip, a captured
  frame where the display is involved (the lab loops the NVR's HDMI into an
  HDMI-RX capture port for exactly this).
- A test plan whose steps were not run is worse than none. Say what was not
  verified.

## 7. Scope

A pull request does what its title says. Refactors that move code are kept
separate from changes in behaviour, so the review can tell which is which.
