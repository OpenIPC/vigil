# Recording

How `vigild` stores video, finds it again and gives it back: on the
recorder's screen, in a browser, to a VMS, and as files people take away.
This replaces the raw elementary-stream segments of `seg_writer`
(`CAMn_YYYYmmdd_HHMMSS.h264|h265`), which carry no timestamps, no audio
and no events, and so cannot be searched or played by time.

Status: design. The phases at the end say what is built in which order.

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

A chunk is a file of fixed size (`record.chunk_mb`, default 256 MiB),
created when the pool is set up, until the pool fills the target except
for a reserve (`max(2 %, 1 GiB)`; less on small media). Each chunk holds
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
| `chunk_mb` | 256 | Chunk size, MiB (64..1024); set when the pool is made |
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
