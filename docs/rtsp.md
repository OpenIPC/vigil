# RTSP server

`vigild` passes its cameras on to other clients over RTSP: a VMS, VLC or
ffmpeg, another recorder, a phone app. What a client gets is what the camera
sent, H.264 or H.265 as recorded, without re-encoding and without a second
connection to the camera: the session that records and shows a stream hands
each access unit to the server too (`src/core/stream_tap.c`).

## Addresses

`rtsp://<recorder>:554/ch<N>/main` and `/ch<N>/sub`, N from 1 (the
camera's tile, CAM1 is `ch1`); `/ch<N>` alone is the main stream. A `sub`
exists when the camera has a sub-stream (`sub_url`, or derived for NETIP).
For clients that only know other recorders' paths, these name the same
streams:

| Path | Stream |
|------|--------|
| `/ch1/main`, `/ch1/sub`, `/ch1` | vigil's own |
| `/Streaming/Channels/101`, `/Streaming/Channels/102`, also under `/ISAPI` | Hikvision style: channel × 100 + 1 main, 2 sub |
| `/h264/ch1/main/av_stream`, `/h264/ch1/sub/av_stream` | older Hikvision (whatever the codec) |
| `/cam/realmonitor?channel=1&subtype=0` (`1`: sub) | Dahua style |

A channel without a camera, or without that stream, answers `404`; one
whose camera has not sent its first picture yet (connecting, offline)
answers `503` with `Retry-After`.

## What it serves

- **Video only.** The cameras' audio is not taken from them, so there is
  none to pass on; the SDP has one video track.
- **H.264** (RFC 6184, packetization mode 1: single NAL units and FU-A)
  and **H.265** (RFC 7798: single NAL units and FUs), payload type 96,
  payloads of at most 1400 bytes. The SDP carries the parameter sets of the
  stream's last keyframe (`sprop-parameter-sets`, or `sprop-vps`,
  `sprop-sps`, `sprop-pps`) and `profile-level-id`; every keyframe carries
  them in-band as well.
- **RTP timestamps** at 90 kHz from the access units' times, continuing
  smoothly over a camera's reconnect. One SSRC and one sequence per stream;
  RTCP sender reports (with a CNAME) every 5 s.
- **Transports**: RTP over the RTSP connection (`RTP/AVP/TCP;interleaved`)
  and RTP over UDP unicast (`RTP/AVP;unicast;client_port=`), the default of
  most VMSs. UDP goes out of one pair of ports, the first free from 6970
  (`server_port` in the reply); RTCP from clients is read there and keeps a
  session alive. No multicast, no RTSP over HTTP.
- **Methods**: `OPTIONS`, `DESCRIBE`, `SETUP`, `PLAY`, `PAUSE`, `TEARDOWN`,
  and `GET_PARAMETER` / `SET_PARAMETER` as keepalives. One session, one
  track, per connection: closing the connection ends it. A UDP session
  without a request or RTCP for 60 s ends (`Session: …;timeout=60`); a TCP
  client that takes no data for 20 s is dropped.

## The picture at once

A decoder can only start at a keyframe, which cameras send every 1 to 4 s.
The channel keeps each stream's GOP so far (the keyframe and every picture
since), and a stream nobody was watching starts with it: the client gets
the keyframe at once and the GOP up to the present. Each stream being
served keeps its own last GOP in its buffer, so a second client of it
starts at that keyframe, with no wait at all. Measured on the host
(`tests/rtsp_server_test.py`): the first packet after `PLAY` is the
keyframe, within one frame time of the camera (about 40 ms at 25 fps) for
a stream not yet served, under a millisecond for one already served.

While the server runs, every stream's GOP is kept as long as the GOP caches
hold less than half their 24 MiB budget, after what the decoders need: on a
recorder with many large GOPs the first client of a stream that is not on
screen may wait for its keyframe; the next ones do not.

## Logins

`rtsp_server.auth` (on by default): **Digest** (RFC 2617, MD5, realm
`vigil`) against [vigil's users](api.md#accounts-and-permissions); the user's
group needs the **`live`** permission (live view: the same right as watching
a camera on the screen or through the API), so `admin` and the built-in
`user` group can watch, a group without `live` gets `403`. Basic is not
offered: the password would cross the network in clear.

Digest needs the password, or `md5(user:vigil:password)` (HA1), and
vigil's users file holds `crypt(3)` hashes, from which it cannot be had. So
the users file keeps each user's HA1 beside its hash (`digest`): written
whenever a password is set, through the API or the screen, and, for users
made before the RTSP server, at their next login there (the screen, the
web page or the API); until then such a user is refused and the log says
why. The system account (`root`, `admin` while no user has that name)
has its password only in `/etc/shadow`: its HA1 is learnt when it logs in
to the screen, the web page or the API, kept in memory only, and forgotten
when the shadow file changes. **Give a VMS a user of its own** in a
group with just `live`.

An HA1 is as good as the password for logging in to this server (not
elsewhere: it is bound to the realm and the user name); the users file is
mode 0600, as before.

Five failed logins in a row from one address make that address wait 2, 4,
8, … up to 300 s, without its credentials being looked at; other addresses
are not affected. `rtsp_server.auth: false` lets anyone who reaches the port
watch every camera (logged as `WITHOUT authentication`).

## Settings

| Key | Type | Default | Meaning |
|-----|------|---------|---------|
| `rtsp_server.enabled` | boolean | `true` | Serve the cameras over RTSP. |
| `rtsp_server.port` | 1..65535 | `554` | The RTSP port (TCP). |
| `rtsp_server.auth` | boolean | `true` | Require a Digest login with the `live` permission. |
| `rtsp_server.max_clients` | 1..32 | `8` | Sessions being sent a stream at once; one more gets `453 Not Enough Bandwidth`. |

All of them apply while `vigild` runs: a new port or `enabled` restarts the
server (its clients reconnect), `auth` and `max_clients` count for the next
request.

## Inside

`src/api/rtsp_server.c`, one thread with `poll()` for every client.

- A stream being served has a **tap** (`stream_tap.h`) and a **ring** of
  RTP packets. The camera's session thread packetizes each access unit
  into the ring once, however many clients watch it, and wakes the server
  thread; it never waits for a client.
- A **client is a position in the ring**. The server thread copies the
  packets from there into the client's output (64 KiB, framed for
  interleaved TCP) as fast as its socket takes them, or sends them as UDP
  datagrams (at most 32 KiB a turn, turns 4 ms apart while it is behind:
  UDP has no flow control, and a GOP sent at once would overflow buffers
  on the way).
- A **client that falls behind** (it stopped reading, or its link is slower
  than the camera) is overtaken by the camera: the packets it missed are
  gone, so it goes on at the next keyframe still in the ring, or the next
  to come; its RTP sequence numbers show the gap. Its own socket and
  output are all it holds up. The log says how often (`skipped to a
  keyframe N times`).
- A ring starts at 2 MiB (main stream) or 512 KiB (sub-stream) and doubles,
  up to 4 MiB or 1 MiB, at a keyframe when the GOP before it did not fit.
  It is freed 10 s after the stream's last client leaves (at once while
  more streams are open than `max_clients`).
- No allocation per packet or per access unit: per client, its 64 KiB
  output and a few hundred bytes; per stream, its ring.
