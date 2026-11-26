# Configuration and HTTP API

`vigild` keeps its settings in one JSON file and serves them, together with
its status and live events, over a small HTTP/WebSocket API. A web page, a
phone app or a script can read and change the configuration while video
keeps running; the TV UI and the recorder do not stop.

## The configuration file

Default `/etc/vigil.json`, or `vigild -f FILE`. A missing file means the
defaults. [vigil.example.json](vigil.example.json) shows every setting:

```json
{
  "version": 1,
  "cameras": [
    { "url": "rtsp://admin:12345@192.168.1.10/stream=0", "name": "Gate", "enabled": true },
    { "url": "netip://admin:@192.168.1.11:34567/?ch=0&stream=main", "name": "Yard", "enabled": true }
  ],
  "record": { "path": "/mnt/sda1/rec", "segment": 60, "keep": 0 },
  "api":    { "listen": "0.0.0.0:8080", "auth": true, "user": "root", "shadow": "/etc/shadow" }
}
```

| Setting | Type | Default | Meaning |
|---|---|---|---|
| `cameras[]` | array, up to 16 | `[]` | In tile order: the first is CAM1. |
| `cameras[].url` | string, required | | `rtsp://…`, `netip://…` or `capture:N` (formats in [architecture.md](architecture.md#sources-camera-protocols)). Checked with the source drivers' own URL parsers. |
| `cameras[].name` | string, ≤ 31 | `""` | Shown in the status and on the tile instead of the host. |
| `cameras[].enabled` | boolean | `true` | A disabled camera keeps its tile (and its CAMn number), empty. |
| `record.path` | absolute path | `""` | Where streams are recorded. Empty: no recording. A directory that cannot be created yet (a card not mounted) is tried again every 10 s. |
| `record.segment` | integer 10..3600 | 60 | Segment length in seconds. |
| `record.keep` | integer 0..64 | 3 | Segments kept per camera; 0 keeps all (the free-space reserve still applies). 64 is what the segment writer can track. |
| `api.listen` | `"port"` or `"a.b.c.d:port"` | `"0.0.0.0:8080"` | Where the API listens (IPv4). Empty: no API. Read at start only. |
| `api.auth` | boolean | `true` | Require the system account's password. See [Authentication](#authentication). |
| `api.user` | string | `"root"` | The account whose password the API asks for. |
| `api.shadow` | absolute path | `"/etc/shadow"` | The shadow file holding that account's password hash. |

`GET /api/v1/schema` returns this table as data (below), so it is never out
of date for a client.

**Loading.** Every value is checked against the schema. A file that is not
valid JSON, or holds an invalid value, is logged with each error and its
path (`/etc/vigil.json: record.segment: 5 is out of range 10..3600`), and
`vigild` runs with the defaults; the file is left as it is, and kept as
`FILE.bad` (a hard link, or a copy where the filesystem has no links) when
a new configuration is saved over it. If no copy can be made, nothing is
saved over it. Keys `vigild` does
not know are skipped, so a configuration written by a newer `vigild` still
loads in an older one.

**Saving.** The API writes a new, exclusively created `FILE.XXXXXX` (mode
0600: it holds camera passwords; a fixed name could already be someone's
file or symlink), `fsync`s it, renames it over `FILE` and `fsync`s the
directory, so a power cut leaves either the old or the new file, never a
torn one. A new configuration takes effect only once it is saved. Paths
that do not fit vigild's buffers (`-f` 255 bytes, `-c` 511, `-o` 191) are
refused at startup rather than shortened.

**Command-line options** override the file for that run. Precedence:
schema defaults < the file < options. `-c` replaces the file's camera list,
`-o` sets `record.path` (`none` = `""`), `-S` and `-K` set `record.segment`
and `record.keep` (out-of-range values are brought into range, with a log
line). Options are not written back by themselves; but the API's view, and
what a `PUT` saves, is the configuration in force, options included.

## What applies when

Every setting except `api.listen` applies while `vigild` runs (`"live"` in
the schema). The main thread notices the new configuration within a fraction
of a second and:

- **renames** a camera without touching its stream;
- **restarts** only the cameras whose URL or enabled flag changed; a change
  to `record` restarts every camera (each opens a new segment);
- leaves the others alone: their video and recordings go on.

The result is reported as events (`config applied`, `CAMn added`,
`restarted`, `removed`, `config: N camera(s) failed`) on the TV's event
strip, in the log and on the WebSocket. `status.config` shows the saved
(`generation`) and the running (`applied`) configuration.

## Authentication

HTTP Basic, checked against the system account: the hash of `api.user`
in `api.shadow` is verified with `crypt(3)` (whatever methods the C library
supports: `$1$`, `$5$`, `$6$`, …), and compared in constant time. Without
credentials, or with wrong ones, the answer is `401` with
`WWW-Authenticate: Basic realm="vigil"`.

- A failed login is answered after one second. From the fifth failure in a
  row **from one client address**, every login from that address (even a
  right one) gets `429` with `Retry-After` for 2, 4, 8, … up to 60 seconds,
  without the password being looked at; a success from it resets its count.
  Other addresses are not affected, so a stranger on the network cannot lock
  the owner out (32 addresses are tracked; the one that failed least
  recently makes room). The trade-off: an attacker with many addresses
  gets five tries per address before its lockout; what bounds guessing
  overall is the one-second answer on each of the 8 workers (at most 8
  guesses a second, whatever the addresses) and `crypt()` running one at a
  time. While such a flood lasts, the API answers slowly for everyone; it
  does not lock anyone out afterwards. Clients behind one NAT address share
  a count.
- An account without a password (an empty hash field, as OpenIPC ships
  `root`) or a locked one (`!`, `*`) cannot log in: set a password with
  `passwd`. The reason is logged once.
- A good login is remembered for five minutes (as a keyed digest, not the
  password), so a polling client does not run `crypt()` every time; a change
  to the shadow file ends it.
- Basic authentication sends the password in clear on every request. The
  API has no TLS: use it on a trusted network, or put a TLS proxy or an SSH
  tunnel in front.
- `"auth": false` turns all of this off: **anyone who can reach the port can
  change the configuration** (and point a camera URL anywhere). `vigild`
  logs `listening … WITHOUT authentication`. Only for a closed network or
  with `listen` on `127.0.0.1`.

Browsers send cached credentials with a WebSocket upgrade, so the WebSocket
refuses an `Origin` that is not the host it was asked for: a page elsewhere
cannot read the NVR through the user's login. Plain HTTP calls from another
origin are stopped by the browser itself (`PUT` needs a CORS preflight, which
gets a `401`).

## Passwords in camera URLs

Camera URLs carry their passwords (`rtsp://admin:12345@…`). The API never
shows them: everywhere it returns a URL (the configuration, the status) the
password is replaced with `***`:

```
rtsp://admin:***@192.168.1.10/stream=0
```

On `PUT`, a URL that is exactly the redacted form of a stored camera's URL
means "keep that camera's stored password". A client can therefore `GET`
the configuration, change something and `PUT` it back without knowing the
passwords; cameras may be reordered, as the match is on the whole URL
(scheme, user, host, port, path). To change a password, send the URL with
the new one. A `***` that matches no stored camera is an error. (A real
password `***` cannot be set through the API.) URLs without a password, or
with an empty one (`netip://admin:@…`), are shown as they are.

There is no way to read a password back through the API. The log never
shows them either; the file itself holds them, readable by root only.

## Endpoints

All under `/api/v1`, JSON in and out (`Content-Type: application/json`).
Errors are `{"error": "…"}`; validation errors add a list:

```json
{ "error": "invalid configuration",
  "errors": [ { "path": "record.segment", "message": "5 is out of range 10..3600" },
              { "path": "cameras[0].url", "message": "not a camera URL (schemes: rtsp, netip, capture)" },
              { "path": "bogus", "message": "unknown section" } ] }
```

| Method and path | |
|---|---|
| `GET /api/v1/status` | Platform, uptime, configuration generation, one entry per camera. |
| `GET /api/v1/config` | The configuration in force, passwords redacted. |
| `PUT /api/v1/config` | Replace the whole configuration. Sections and keys it omits get their defaults. |
| `GET /api/v1/config/<section>` | One section: `cameras`, `record` or `api`. |
| `PUT /api/v1/config/<section>` | Replace one section; the others stay. |
| `GET /api/v1/schema` | Every setting: type, default, limits, label, help. |
| `GET /api/v1/ws` | WebSocket: pushed status, events and configuration changes. |

A `PUT` is validated as a whole (every error is reported, at most 16 listed,
`more_errors` counts the rest); unknown keys are errors here, to catch
typos. On success it is saved, then applied, and the answer is the stored
document (or section), redacted. Status codes: `200`; `400` invalid JSON or
values; `401`/`429` authentication; `404` unknown path or section; `405`
other methods (with `Allow`); `413` a body over 64 KiB; `500` the file could
not be saved (the configuration in force is unchanged), or — "the
configuration is in force, but saving it is not confirmed" — the new file
replaced the old one but its directory could not be synced, so it may not
survive a power cut.

### Examples

```sh
NVR=http://192.168.1.20:8080
curl -u root:PASSWORD $NVR/api/v1/status
curl -u root:PASSWORD $NVR/api/v1/config

# add / replace the cameras
curl -u root:PASSWORD -X PUT $NVR/api/v1/config/cameras -d '[
  { "url": "rtsp://admin:12345@192.168.1.10/stream=0", "name": "Gate" },
  { "url": "netip://admin:@192.168.1.11/?stream=extra", "name": "Yard" }
]'

# start recording, 5-minute segments, keep everything
curl -u root:PASSWORD -X PUT $NVR/api/v1/config/record \
     -d '{ "path": "/mnt/sda1/rec", "segment": 300, "keep": 0 }'

# rename CAM1, keeping its password: edit what GET returned and send it back
curl -su root:PASSWORD $NVR/api/v1/config/cameras |
  jq '.[0].name = "Front gate"' |
  curl -u root:PASSWORD -X PUT $NVR/api/v1/config/cameras -d @-
```

### `GET /api/v1/status`

For example:

```json
{
  "version": "0.1.0",
  "uptime": 3605,
  "time": 1795700819,
  "config": { "generation": 7, "applied": 7 },
  "platform": { "name": "sigmastar", "display": true, "decoder": true,
                "decoder_max_width": 2592, "decoder_max_height": 2592,
                "max_channels": 4, "capture_inputs": 0 },
  "channels": [
    { "camera": 1, "url": "rtsp://admin:***@192.168.1.10/stream=0", "name": "Gate",
      "enabled": true, "host": "192.168.1.10", "running": true, "live": true,
      "state": "live", "decoding": true, "recording": true, "rec_bytes": 73400320,
      "codec": "H.265", "width": 1920, "height": 1080, "fps": 25 }
  ]
}
```

`uptime` is seconds since the API started, `time` the board's clock (Unix
time). One channel per configured camera, in tile order. `running`: the
channel has a source (it is enabled and its URL could be used); `live`:
video arrived in the last 3 s; `state` is `live`, `off`, `CONNECTING` or the
last error (`connect: Connection refused`, `login refused (Ret 203)`, …);
`rec_bytes` counts what was written since the channel started; `fps` is
averaged over 2 s; `codec` is `null` until the stream says.

### `GET /api/v1/schema`

```json
{ "version": 1,
  "sections": [
    { "key": "record", "type": "object", "label": "Recording", "help": "",
      "fields": [
        { "key": "segment", "path": "record.segment", "type": "integer",
          "label": "Segment length (s)", "help": "A new file starts at …",
          "live": true, "required": false, "default": 60, "min": 10, "max": 3600 } ] },
    { "key": "cameras", "type": "array", "max_items": 16, "fields": [ … ] } ] }
```

Field types: `string` (`max_length`; `secret: "url-password"` marks the
redacted URL), `integer` (`min`, `max`), `boolean`, `enum` (`values`).
`path` uses `[]` for array items (`cameras[].url`); validation errors use the
index (`cameras[2].url`). `live: false` means the setting takes effect when
`vigild` starts again.

### WebSocket `/api/v1/ws`

Same authentication as the rest (the `Authorization` header on the upgrade
request; browsers send it after a login on the same origin). At most four
clients at once. The server sends text frames, each one JSON object
`{"type": …, "data": …}`; what a client sends is ignored, except close and
ping. The server pings an idle client every 10 s and drops one that stops
answering.

| `type` | When | `data` |
|---|---|---|
| `status` | On connect, every second, and at once when a camera goes live, offline or changes state | as `GET /api/v1/status` |
| `event` | As they happen | `{"seq": 42, "time": 1795700819, "camera": 2, "text": "online"}` — `camera` 0 is the system; `missed` counts events lost by a slow reader |
| `config` | On connect, and when a configuration is saved or applied | `{"generation": 8, "applied": 7}` |

Events are the same ones the TV's event strip shows: `online`, `offline`,
`down`, `no decoder`, `REC failed`, `REC on`, `added`, `restarted`, `removed`,
`bad URL`, `config applied`, `config: N camera(s) failed`. A client that
wants to know when its change runs waits for a `config` message whose
`applied` reached the generation its `PUT` produced (`status.config` after
the `PUT`).

```sh
# with websocat
websocat -H "Authorization: Basic $(printf root:PASSWORD | base64)" ws://192.168.1.20:8080/api/v1/ws
```

## Inside

- `src/core/schema.{h,c}`: the generic engine (defaults, JSON in and out,
  checks, describing itself). `src/core/config_schema.c`: vigild's table.
  `src/core/config.{h,c}`: the file and the thread-safe store with its
  generation counter.
- `src/api/api.c`: CivetWeb ([third_party/civetweb](../third_party/civetweb),
  MIT) built with `NO_SSL NO_CGI NO_FILES NO_CACHING USE_WEBSOCKET`, on its
  own threads (a master thread and up to 8 workers, started on demand; a
  WebSocket holds a worker for as long as it is open), one thread that
  gathers status, events and configuration changes, and a writer thread per
  WebSocket client with its own mailbox: a client that stops reading holds
  up only its own writer (each frame times out after 10 s, then the client
  is dropped), never the other clients or shutdown. A slow client loses the
  oldest of its 32 queued events and configuration messages, and only its
  latest status is kept.
  `src/api/auth.c`: Basic credentials, the shadow file, `crypt(3)`.
- JSON is parsed by cJSON ([third_party/cJSON](../third_party/cJSON), MIT),
  bodies capped at 64 KiB.
- `-DVIGIL_API=OFF` builds `vigild` without CivetWeb and `crypt`
  (`src/api/api_none.c`); the configuration file still works.
- If the port cannot be bound, `vigild` logs it and runs on without the API.
