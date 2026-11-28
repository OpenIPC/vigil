# Recording

How `vigild` stores video, finds it again and gives it back: on the
recorder's screen, in a browser, to a VMS, and as files people take away.
This replaces the raw elementary-stream segments of `seg_writer`
(`CAMn_YYYYmmdd_HHMMSS.h264|h265`), which carry no timestamps, no audio
and no events, and so cannot be searched or played by time.

Status: phase 1 is built (the store, recovery and rebuild, the read API,
fMP4 export; "Phase 1 as built" at the end says where it settles what
this page left open or differs from it). The phases say what comes next.

## What it must do

- Record every camera's main stream, and optionally its sub-stream, all
  the time or on events, for months, on a disk that is always nearly full:
  the oldest video goes when room is needed.
- Survive power cuts, which recorders get often: lose seconds, never the
  disk, never the index.
- Answer fast, on a dual Cortex-A7 with ~84 MB of Linux RAM: which days of a
  month have video (calendar), what a day looks like (timeline), which
  events happened, where 14:37:12 starts.
- Play from any point without reading more than the few seconds around it:
  on HDMI, in a browser over a slow link, to a VMS, while recording goes on.
- Export any range (50 minutes of the last 24 hours, with gaps if it was
  event recording) as a file every player opens.
- Work the same on a SATA disk, an SSD, an SD card, a USB disk and a
  network share (NFS, SMB), and leave a disk readable on a PC.

Not goals: re-encoding (the SoCs here have no encoder to spare, the
SSR621Q has none), RAID, a filesystem of our own.

## Why not a vendor-style filesystem

Recorder vendors format disks their own way (Hikvision's pre-allocated
data files with index files, Dahua's DHFS, Xiongmai's WFS). What that buys
them is space split into fixed pieces once, so nothing fragments and the
oldest piece is simply reused, and a built-in index by time. What it costs:
a disk no PC can read without the vendor's or forensic tools, robustness
only as good as one implementation, no other files on the disk, no network
shares (a raw layout needs a block device), and a filesystem and its fsck
to write and keep. vigil gets the same two advantages inside a standard
filesystem: a pool of pre-allocated chunk files used as a ring, and an
index of its own.

## On the disk

Everything lives in one directory on the recording target, `record.path`
(e.g. `/mnt/sda1/rec`, or a directory on a share), so a disk or share
carries all it needs to be played elsewhere:

```
<path>/
  vigil-rec.id            which recorder owns this directory (lock, see below)
  pool/
    c000000.mp4 ...       chunk files, all the same size, pre-allocated
  index/
    chunks.a  chunks.b    the chunk table, two alternating copies
    events.log            the event log (a ring)
    journal               appends since the last chunk-table save
```

### Filesystem

- **ext4** is the default, as now: journaled, readable on any Linux.
  Formatted for few large files (`mkfs.ext4 -T largefile4`), mounted
  `noatime`. On flash, aligned to the card's or drive's erase block.
- **exFAT** and **vfat** keep working for a disk meant for Windows, with a
  warning that a power cut can damage them (no journal). vfat cannot hold
  files over 4 GiB, which chunks never are.
- **NFS and SMB** shares are a directory like any other (below for what
  differs).

### The chunk pool

A chunk is a file of fixed size (`record.chunk_mb`, default 256 MiB;
4..1024, below 64 only for tests and very small media),
created when the pool is set up, until the pool fills the target except
for a reserve (`max(2 %, 1 GiB)`; less on small media: at most a tenth of
it), or up to `record.pool_mb`. Each chunk holds
one stream of one camera (CAM3 main, or CAM3 sub) for as long as it lasts:
at 4 Mbit/s about 8.5 minutes, at 512 kbit/s about an hour.

- **Pre-allocation**: `fallocate()` on ext4, XFS, exFAT and NFS 4.2; where
  the filesystem cannot (NFSv3, some SMB servers, vfat), the file is filled
  with zeros once, in the background at idle priority, and the pool grows
  as files are ready (recording starts with what is there).
- **Ring**: when a stream needs a chunk and none is free, the oldest
  closed chunk on the target is reused, whichever camera it held, subject
  to the quotas (`record.quota`): a camera over its quota gives up its own
  oldest chunk first. `record.on_full: stop` reuses nothing and stops
  recording when the pool is used up.
- **Rewritten in place**: a reused chunk is written from its start over the
  old data; the file is never deleted or truncated, so the filesystem's
  metadata does not change and nothing fragments.
- **Free chunks first**: a new pool, or chunks freed by deleting a camera's
  recordings, are used before any is reused.

### Inside a chunk: fragmented MP4

A chunk is a valid fragmented MP4 (ISO BMFF, the CMAF profile where it
applies) up to the point written so far:

```
[vgch]               vigil's chunk header (a 'uuid' box, ignored by players)
[ftyp][moov]         the stream's initialisation: codec, size, parameter sets
[moof][mdat] ...     fragments, each starting at a keyframe
[vgix]               fragment table and events, written when the chunk closes
[free ...]           the rest of the pre-allocated space
```

- **`vgch`** (fixed 4 KiB, a `uuid` box): magic and version, chunk number,
  pool id, camera, stream (main/sub), codec, start time (UTC, µs), the
  sequence number of this use of the chunk (incremented on every reuse,
  so data left from an earlier use is never mistaken for this one), and a
  CRC. Rewritten in place when the chunk closes, with its end time and the
  offset of `vgix`.
- **`moov`**: one video track (H.264 `avc1` / H.265 `hvc1` with the
  parameter sets in `avcC`/`hvcC`), and later one audio track. Timescale
  90 kHz. A stream that changes size or codec closes the chunk and opens a
  new one: one chunk, one format.
- **Fragments**: one per GOP, cut at the keyframe that starts the next
  one, at most `record.fragment_s` (default 4 s; a longer GOP is cut at
  the first frame past it and the fragment does not start with a keyframe,
  which the fragment table says). Each `moof` has `mfhd` (sequence),
  `tfhd`, `tfdt` (decode time from the chunk start) and `trun` (sizes,
  durations, keyframe flags, composition offsets for B-frames). Sample
  times are the camera's, smoothed over a reconnect, never going back.
- **`vgix`** (a `uuid` box): the fragment table, one entry per fragment
  (time, offset, size, keyframe flag; 16 bytes), and the events inside the
  chunk's time span (the event records below). Written at close; while a
  chunk is open, the same table lives in RAM and in the journal.
- **Unused tail**: a `free` box covers the rest of the file, so the file
  is valid MP4 at every close. An open chunk's tail is old data or zeros;
  readers stop at the last fragment the index or `vgch` vouches for.

A chunk file copied off the disk plays in VLC or ffmpeg as it is, and
`vgix` lets any tool find events and times without the index.

### The index

Small, fixed-size, checksummed records. Everything in it can be rebuilt
from the chunks; it exists to answer queries without reading them.

- **Chunk table** (`index/chunks.a`, `.b`): a header (magic, version, pool
  id, generation, chunk size, number of chunks, CRC) and one 64-byte
  record per chunk: state (free / open / closed / bad), camera, stream,
  codec, use sequence, start and end time (µs), bytes used, number of
  fragments, flags (has events, has gaps), CRC. 32 000 chunks (8 TB of 256
  MiB) are 2 MiB; the whole table is held in RAM.
- **Two copies**: a save writes the whole table to the older copy with the
  next generation, `fsync`s, and only then is the save done. At start the
  newest copy whose CRCs check is used; a torn save leaves the other.
- **Journal** (`index/journal`): what happened since the last save, as
  32-byte records with a CRC each: chunk opened, fragment written (chunk,
  time, offset, size, flags), chunk closed, event. Replayed onto the table
  at start; a torn last record is dropped. The table is saved, and the
  journal emptied, every `record.index_save_s` (default 300 s), at every
  chunk close, and on a clean stop.
- **Event log** (`index/events.log`): a ring of 32-byte records in time
  order: time (µs), duration (ms), camera, type (motion, human, face,
  vehicle, line crossing, intrusion, video loss, tamper, alarm input,
  disk...), detail (zone, rule), the chunk and fragment it falls in (or
  none: an event while not recording), CRC. Sized for about a year of a
  busy recorder (e.g. 4 M events, 128 MiB on a large disk; scaled down on
  small media). A per-day table of offsets into it is kept in RAM.
- **RAM**: chunk table plus day offsets, ~2-3 MiB for 8 TB. A fragment
  table is loaded from a chunk's `vgix` only while that chunk is played.

### Recovery

At mount (a disk plugged in, the share mounted, vigild started):

1. Read both chunk-table copies, take the newest valid one, replay the
   journal. Normally done in well under a second.
2. Check the open chunks (the ones the table says were being written):
   read their tail from the last fragment the journal vouches for, keep
   every further fragment that parses and whose `moof` sequence follows,
   close the chunk there. A power cut costs what was not yet flushed.
3. If neither copy is valid (or the directory has chunks and no index):
   **rebuild** in the background from the chunks' `vgch` and `vgix` (for
   an open chunk, by walking its fragments). Recording starts at once
   regardless: it needs only to know which chunks are free or oldest,
   which the `vgch` headers tell, read first. Playback shows what has been
   rebuilt so far, and the UI says a rebuild is running. About 32 000
   small reads for 8 TB: minutes on a hard disk.

Events are in the chunks too (`vgix`), so a rebuild recovers them, except
those logged while nothing was recorded, which live only in the event log
(two-copy safe by its per-record CRCs and ring header copies).

### The owner lock

`vigil-rec.id` holds the pool id, the recorder's id (from the MAC) and its
host name. A recorder writes only to a pool it owns. A pool owned by
another recorder (a disk moved over, a share two recorders point at) is
mounted read-only for playback and export, with the choice in the UI to
take it over (or to set up a pool of its own in another directory).

## Writing

- **One writer thread per target**, fed by the camera sessions through a
  bounded queue of access units (a copy; the session thread never waits on
  a disk). The queue holds ~10 s at the target's bitrate; a disk that
  stalls longer (a share gone, a dying disk) drops to the next keyframe,
  marks a gap in the journal, and raises a disk alarm. Recording never
  stalls a camera, live view or the servers.
- **Writes are a fragment at a time**, aligned to 4 KiB where the
  filesystem allows (O_DIRECT is not needed; large sequential writes go
  through the page cache well).
- **Flush**: `fdatasync` the chunk and append to the journal every
  `record.flush_s`: by default 2 s on a hard disk, 5 s on flash (SD, eMMC,
  SSD: fewer small writes, less wear), 2 s on a share. A power cut loses at
  most that. Index and journal appends are batched with the flush, never
  written one record at a time.
- **Pre-record**: event recording starts `record.pre_s` (default 5 s)
  before the event, from the stream's GOP cache (`src/core/gop_cache.c`,
  which already keeps the current GOP; for pre-record it keeps the last
  `pre_s` seconds of the recorded streams), and goes on `record.post_s`
  (default 10 s) after it ends.
- **Schedule** (`record.schedule`, unchanged): continuous, motion
  (events) only, or off, per half hour per camera.
- **Streams**: the main stream as now; the sub-stream too when
  `record.sub` is on (per camera), into chunks of its own, for remote
  playback over slow links (below).
- **Flash media**: `fstrim` weekly on SSDs; the surface scan's Write mode
  refuses flash; SMART wear (percentage used, wear levelling count) shown
  and alarmed, with an estimate of the remaining life from the measured
  write rate.
- **Shares**: pre-allocation by writing zeros where `fallocate` is not
  supported (NFSv3), at idle priority; a share that goes away is handled
  by the queue above; mounted `hard` with a timeout so a returning server
  resumes, the writer thread being the only one that waits.

## Reading

All queries are answered from RAM or from one small read.

| Question | From | Cost |
|---|---|---|
| Days of a month with video, per camera | chunk table | < 1 ms |
| A day's timeline (recorded ranges, per stream) | chunk table | < 1 ms |
| Events of a day / of a month, by camera and type | day offsets + event log | one read, ms |
| Where 14:37:12 starts | chunk table, then the chunk's fragment table | two reads |

### Sessions

A playback session reads fragments, never whole chunks: from the fragment
that holds the start time (the keyframe at or before it), then forward,
keeping a few seconds to ~30 s ahead of what is shown. Readers run at
lower I/O priority than the writer, a few at a time (`record.readers`,
default 4 remote sessions plus the screen); export is rate-limited so that
recording never waits.

A range that spans chunks is joined fragment by fragment: decode times are
re-based to one continuous timeline, `moof` sequences renumbered. A range
with gaps (event recording, a camera offline) is reported as separate
recorded spans; players skip the gaps.

### On the screen (HDMI)

Playback uses the same decoders as live view: on the SSR621Q four
pictures at once (`mi_vdec` MaxChnNum 4), so a four-camera synchronous
playback replaces the live view while it runs. Speeds: 1/4x to 4x by
pacing; faster, keyframes only (8x to 64x), backwards keyframe by
keyframe. The timeline bar shows the recorded spans and the events.

### In a browser

Fragments go to the browser as they are stored, for Media Source
Extensions (`video/mp4; codecs="avc1..."` / `"hvc1..."`): an init segment
from the chunk's `moov`, then `moof`+`mdat` fragments, over HTTP (one
request per few fragments, or HLS with fMP4 segments) or the API's
WebSocket. No conversion on the recorder.

- **Slow links**: the recorder cannot lower a bitrate (no encoder). The
  answers are the sub-stream's recordings (`record.sub`), and keyframes
  only (about one picture per GOP, a fraction of the bitrate; also how
  fast-forward works).
- **H.265**: browsers decode it only with hardware support (Safari; Chrome
  and Edge on capable machines; Firefox mostly not). The player checks
  `MediaSource.isTypeSupported` and offers the H.264 sub-stream where
  there is one, or says why it cannot play.

### To other clients

The RTSP server plays recordings too: `rtsp://NVR/ch1/main?start=...`
(with `Range: clock=` and `Scale:` for speed), and ONVIF Profile G
(recording search and replay) builds on the same queries. Later phases.

### Export

A range of one camera's stream becomes one MP4, made while it is sent,
without re-encoding:

- **fMP4** (default): the `ftyp`+`moov` of the first chunk, then the
  range's fragments re-based. Starts at the keyframe at or before the start
  time; an edit list (`elst`) tells players to begin showing at the exact
  start time.
- **Classic MP4** (`faststart`, for players that want an index at the
  front): the `moov` with full sample tables is built from the fragments'
  `trun`s (no video is read for it), then the samples follow. Same cost.
- **Gaps**: kept on the clock (one file, timestamps jump; the default),
  removed (continuous playback, the real times in a subtitle track and in
  the file's metadata), or one file per recorded span (event clips).
- **Format change inside the range** (resolution, codec): the export is
  split there; players handle two formats in one MP4 badly.
- **Size**: a range over 4 GiB is split when the destination is vfat.
- Destinations: the browser (download), a USB stick on the recorder, a
  share. A watermark-free file: the recorder signs nothing yet.

## The API

Under `/api/v1/record/` (permission `playback`):

- `GET days?camera=&stream=&month=YYYY-MM` → days with video.
- `GET spans?camera=&stream=&from=&to=` → recorded spans, with the events
  in them (types and times).
- `GET events?camera=&type=&from=&to=&limit=` → events, newest first or
  oldest first.
- `GET init?camera=&stream=&at=` and `GET fragments?camera=&stream=&from=&count=`
  → the MSE pieces (init segment, then fragments re-based from `from`).
- `GET export.mp4?camera=&stream=&from=&to=&gaps=keep|remove|split&layout=frag|faststart`
  → the file, streamed.
- `GET status` → targets, pools, sizes, oldest video per camera, rebuild
  progress, write rate, estimated days kept.
- `DELETE recordings?camera=&from=&to=` → frees the chunks wholly inside the
  range (permission `storage`).

## Configuration

New keys under `record` (the existing `path`, `on_full`, `schedule`,
`quota` keep their meaning; `segment` and `keep` belong to the old format
and go):

| Key | Default | Meaning |
|---|---|---|
| `chunk_mb` | 256 | Chunk size, MiB (4..1024); set when the pool is made |
| `pool_mb` | 0 | The most the pool takes; 0: the target less the reserve. Grows, never shrinks |
| `fragment_s` | 4 | Longest fragment, seconds |
| `flush_s` | auto | 2 on disks and shares, 5 on flash |
| `pre_s`, `post_s` | 5, 10 | Event recording before and after |
| `sub` | per camera, false | Record the sub-stream too |
| `index_save_s` | 300 | Chunk table save interval |

## Old recordings

The `CAMn_*.h264|h265` segments of earlier builds stay where they are and
are not part of the pool; Storage shows them, and they can be deleted
there. Setting up a pool on a disk that has them keeps them unless the
user formats. (Wrapping them into chunks is possible, their times coming
from the file names and the frame count, but not planned.)

`seg_writer` stays for the DVR boards that record their own encoder
(`hi3520dv200`) until they move to the pool too.

## Phases

1. **Store**: pool, chunk format, writer thread, journal, index, recovery
   and rebuild, quotas and the ring, the owner lock; the `record/` read
   API (days, spans, events, init, fragments, status) and fMP4 export;
   continuous recording of the main stream replacing `seg_writer` on
   SigmaStar. Tests: format round-trip, power cut at every write point
   (truncated chunk, torn journal and table), rebuild equals the index,
   ring and quota order, a 24 h range export across chunks with gaps.
2. **Playback**: on the screen (timeline, calendar, speeds, four cameras)
   and in the browser (MSE player in the web UI, sub-stream and keyframe
   modes), classic MP4 export, export to USB and shares.
3. **Events**: camera events (NETIP alarms, ONVIF events) into the log and
   the timeline, event recording with pre/post, search by type.
4. **More**: sub-stream recording, RTSP playback and ONVIF Profile G,
   audio, several targets (disk + share), flash wear estimates.

## Phase 1 as built

Where the code settles what the design leaves open, or differs from it:

- **Configuration**: `chunk_mb` (4..1024), `pool_mb` (new: a pool's
  largest size; a pool grows to a larger value, never shrinks),
  `fragment_s`, `flush_s`, `index_save_s`. `pre_s`, `post_s` and `sub` are
  not there yet (phases 3 and 4). `segment` and `keep` stay, for the local
  inputs only: a board's own encoder (`capture:N`, hi3520dv200) is still
  recorded by `seg_writer`; every camera goes into the pool. The
  environment variable `VIGIL_REC_POOL_MB_MAX` caps every pool's size
  (the tests set it, so a `record.path` in a build tree never fills the
  build machine's disk).
- **Writing a fragment**: not a fragment at a time. A fragment's samples
  are written as they arrive (no GOP is held in RAM: sixteen cameras'
  would not fit in the SSR621Q's), into room kept before them for the
  moof (enough for 1.5 times the most samples a fragment of the stream
  has had, 32..1024); at the next keyframe (or `fragment_s`, or that room
  full) the moof and the mdat header are written into it, and a `free`
  box pads what the moof did not need. The pad begins `vgfr`, the chunk's
  use and the fragment's key/gap flags, so a tail scan knows a fragment
  of this use and whether it follows a gap. Writes are not aligned.
- **vgch**: its 4 KiB hold the record twice, copy A (written when the
  chunk opens) at 64 and copy B (written when it closes) at 2048, in
  another sector: a torn rewrite of B leaves A. B counts only when it is
  of A's use.
- **Fragment entries** (16 B): start and duration in ms from the chunk's
  start, the moof's offset, the size of moof and mdat with two flag bits
  (starts with a keyframe, follows a gap). Exact times are in the moofs.
- **Journal**: records are 32 B with the CRC seeded by the pool id and the
  table's generation; the journal is rewritten from its start at each
  save, never truncated, and a record of another generation never
  checks. Before a chunk is reused, its "open" record is appended and
  synced (write-ahead): a power cut never leaves the table saying a chunk
  holds what was already overwritten. Fragment records follow an
  `fdatasync` of the chunk, so the journal vouches only for data on the
  disk.
- **Recovery**: an open chunk is walked from the init segment; fragments
  past what the journal vouched for are kept only when their samples
  parse as length-prefixed NAL units (a moof can reach the disk before
  its data). A chunk whose header for this use never got written is free
  again; one that closed but whose close missed the journal is taken
  from its vgch. Recovered chunks are closed (flag "recovered"). A power
  cut at every one of the test scenario's 3 728 writes, torn, with and
  without the writes not yet synced, loses nothing vouched for
  (`tests/test_rec.c`, `REC_CUT_ALL=1`).
- **Rebuild**: while it runs, only chunks already read back are used or
  reused, and the table is not saved (a table saved half-way would hide
  the rest). A lost event log is refilled from the chunks' vgix.
- **Ring**: the oldest by start time; a stream that finds no chunk while
  the pool is still being made or rebuilt waits, it is not "disk full".
  A camera's size quota counts its chunks at their full size.
- **Pre-allocation**: `posix_fallocate`; where it is not supported the
  file is filled with zeros 1 MiB at a time, resting three times as long
  as each write took (the core is POSIX: no I/O priorities).
- **Flush default**: from the mount (`nfs`, `cifs`, `smb3`... a share: 2 s)
  and the device's `/sys/class/block/*/queue/rotational` (`mmcblk*` and 0:
  flash, 5 s; else 2 s).
- **Queue**: 10 s of what came in over the last 10 s, at least 4 and at
  most 24 MiB.
- **Times**: sample times are the camera's, anchored to the wall clock
  and anchored again when they are more than 2 s off it. Arrival-time
  timestamps (NETIP) jitter with the network; without B-frames they are
  smoothed (each frame an average interval after the one before,
  corrected towards the camera's time by at most an eighth of an
  interval), so players see even frame durations. A chunk starts on a
  multiple of 100 us (9 ticks), so times convert exactly.
- **Events**: from the camera event hub (`cam_events.h`) as they begin
  and end: a record at the start, its duration at the stop; events going
  on when a pool is mounted begin then. The log holds 256 events per
  chunk (4 096 .. 4 M) and is searched by time with a binary search, not
  a per-day table: records go in at their start, so the log is in time
  order.
- **Owner lock**: the recorder's id is the MAC address of its first
  network interface by name (else its host name). Another recorder's
  pool is mounted read-only; `POST /api/v1/record/takeover` makes it ours.
- **API** (docs/api.md, "Recordings"): times are Unix seconds with
  decimals. `fragments` re-bases to `base` (default: local midnight of
  `from`) and says where to go on (`X-Vigil-Next`, then `cont=1`).
  `export.mp4` knows its size ahead (`Content-Length`) and carries a
  `sidx` after its init segment and the duration in `mvhd`/`tkhd`/`mehd`
  (without them Chrome took the first fragment's length for the file's); `gaps=split`
  answers a list of one URL per span; `gaps=remove` has no subtitle track
  with the real times yet; a change of format ends the file there
  (`X-Vigil-Format-Change`); `layout=faststart` is phase 2 (501).
  `DELETE recordings` is there already.
- **Status**: a channel is "recording" while data goes into an open chunk
  (written within the last 3 s), not merely while one is open.
- **Old recordings**: the `CAMn_*` files are left alone; showing them in
  Storage and deleting them there is for phase 2.
- **Measured** (QEMU's `ssr621q` machine, not a board; a rough proxy):
  four NETIP cameras at 720p 15 fps (~2.6 Mbit/s each) into an ext4 disk,
  32 MiB chunks, a 384 MiB pool that went round in ten minutes: vigild at
  12 % of one core (decoding included) and 8.5-9 MiB resident, nothing
  dropped. The guest killed (a power cut) and booted again: the open
  chunks were recovered, 0.1 to 2.1 s lost before the cut (flush 2 s), a
  155 s export across chunks read 2 340 frames without an error. The
  writer alone (`test_rec --bench`) wrote 16 streams of 4 Mbit/s there at
  11 times real time, 9 % of a core at real time. On the build host the
  calendar and a day's timeline on a 32 000-chunk table take 0.1-0.3 ms.
