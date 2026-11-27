# Configuration and HTTP API

`vigild` keeps its settings in one YAML file and serves them, as JSON, together with
its status and live events, over a small HTTP/WebSocket API. A web page, a
phone app or a script can read and change the configuration while video
keeps running; the TV UI and the recorder do not stop.

## The configuration file

Default `/etc/vigil.yaml`, or `vigild -f FILE`. A missing file means the
defaults. It is YAML, like majestic's `/etc/majestic.yaml`;
[vigil.example.yaml](vigil.example.yaml) is a complete one, exactly as
`vigild` writes it; in short:

```yaml
version: 1
cameras:
  - url: "rtsp://admin:12345@192.168.1.10/stream=0"
    name: Gate
    enabled: true
  - url: "netip://admin:@192.168.1.11:34567/?ch=0&stream=main"
    name: Yard
record:
  path: /mnt/sda1/rec
  segment: 60
  keep: 0
api:
  listen: "0.0.0.0:8080"
  auth: true
```

Sections and settings may be left out (they get their defaults), in any
order. Values are typed by the setting they belong to: a boolean is
`true`/`false`, `yes`/`no` or `on`/`off` (any case), an integer is written
in decimal (`060` is refused rather than read as octal), and a string
setting takes the text as it is, so `name: 123` is the name "123". Quote
a value that YAML would otherwise cut short or read differently: a URL
with `?`, `#`, `&` or `: `, a password with special characters, leading or
trailing spaces, or an empty string (`""`). `cameras:` with nothing after
it is no cameras, and a camera may be given as just its URL
(`- rtsp://…`, enabled, no name): the form of the `vigil.yaml` that
OpenIPC firmware shipped before vigild read the file itself, which
therefore still loads. JSON is also valid YAML, so a JSON document loads too.

**Written by vigild.** When the configuration is saved (the API, the
on-screen settings), the file is written anew: two-space indentation,
`key: value`, the cameras as a list, values quoted where needed, and each
section and setting preceded by its label, help text, limits and default
as comments, taken from the schema below. The comments are generated each
time: comments and formatting written by hand are not kept.

**What the parser takes.** One document of maps, lists and scalars, up to
256 KiB, 8 levels deep (vigild's own file needs 3) and 4096 values. Aliases (`*name`)
are refused, so a small file cannot expand into a huge one; anchors and
tags are ignored; a key given twice is an error. Text is UTF-8: values
sent through the API must be. A `.json` file written by an earlier
version may hold other bytes; it still loads and is saved as JSON, but is
not converted to YAML (`-w` names the setting to fix).

**JSON files.** A file whose name ends in `.json` is read and written as
JSON, the format of earlier versions (`/etc/vigil.json`), so a
configuration in that format keeps working where its path is given with
`-f`. To convert one: `vigild -f /etc/vigil.json -w /etc/vigil.yaml`.
`-w OUT` writes the configuration (the file, with any command-line options
applied) to OUT, mode 0600, and exits without starting anything; `-w -`
prints it as YAML. It refuses to write when the file failed to load, which
would write the defaults instead. `vigild -f /dev/null -w -` prints the
defaults, commented.

| Setting | Type | Default | Meaning |
|---|---|---|---|
| `cameras[]` | array, up to 16 | `[]` | In tile order: the first is CAM1. |
| `cameras[].url` | string, required | | `rtsp://…`, `netip://…` or `capture:N` (formats in [architecture.md](architecture.md#sources-camera-protocols)). Checked with the source drivers' own URL parsers. |
| `cameras[].name` | string, ≤ 31 | `""` | Shown in the status and on the tile instead of the host. |
| `cameras[].enabled` | boolean | `true` | A disabled camera keeps its tile (and its CAMn number), empty. |
| `record.path` | absolute path, no `.` or `..` | `""` | Where streams are recorded. Empty: no recording. Used only on a disk: see [Where recordings may go](#where-recordings-may-go). |
| `record.segment` | integer 10..3600 | 60 | Segment length in seconds. |
| `record.keep` | integer 0..64 | 3 | Segments kept per camera; 0 keeps all (the free-space reserve still applies). 64 is what the segment writer can track. |
| `record.allow_rootfs` | boolean | `false` | Count the root filesystem as a disk. For a host whose root filesystem is one; never on a board, where it is the flash. |
| `record.on_full` | `overwrite` or `stop` | `overwrite` | A full disk: the oldest recordings make room, or recording stops (`REC disk full`) until there is room. |
| `schedule[]` | array, up to 16 | `[]` | A week per camera ([management.md](management.md#recording-policies)); a camera without one records all the time. |
| `schedule[].camera` | integer 1..16, required | | The camera's channel. |
| `schedule[].mon` … `sun` | 48 letters of `C`, `M`, `-` | all `C` | One per half hour from 00:00 local time: continuous, motion only, off. |
| `quota[]` | array, up to 16 | `[]` | Per camera limits. |
| `quota[].camera` | integer 1..16, required | | The camera's channel. |
| `quota[].max_gb`, `max_days` | integer | 0 | The most gigabytes / days of its recordings; 0: no limit. |
| `nas.type` | `none`, `cifs`, `nfs` | `none` | Network storage mounted at `/mnt/nas`. |
| `nas.server`, `share`, `user`, `password`, `options` | strings | `""` | The share; the password is shown as `***`. |
| `ddns.enabled`, `provider`, `host`, `user`, `password`, `url`, `interval` | | off, `dyndns`, 10 min | Dynamic DNS: `dyndns`, `noip` or `custom` (`url` with `{host}`, `{user}`, `{password}`). |
| `upnp.enabled`, `http_port`, `rtsp_port` | | off | Kept; no UPnP client yet. |
| `smtp.enabled`, `server`, `port`, `security`, `user`, `password`, `from`, `to` | | off, 587, `starttls` | Mail: trouble events, and a test mail. `to` is a comma-separated list. |
| `alarm.enabled`, `protocol`, `host`, `port`, `path`, `interval` | | off, `http`, 80, `/`, 60 s | Every event POSTed as JSON, and a heartbeat. |
| `time.ntp`, `time.ntp_interval` | boolean, minutes 10..1440 | `true`, 60 | NTP on (the firmware's ntpd, with the servers of `/etc/ntp.conf`), and how often vigild checks itself. |
| `ui.language`, `date_format`, `time_format` | enums | `en`, `YYYY-MM-DD`, `24h` | The on-screen UI. |
| `api.listen` | `"port"` or `"a.b.c.d:port"` | `"0.0.0.0:8080"` | Where the API listens (IPv4). Empty: no API. Read at start only. |
| `api.auth` | boolean | `true` | Require a login. See [Authentication](#authentication). |
| `api.user` | string | `"root"` | The system account: it logs in with every permission (and as `admin` while no user of that name exists). |
| `api.shadow` | absolute path | `"/etc/shadow"` | The shadow file holding that account's password hash. |
| `api.users` | absolute path | `"/etc/vigil.users.yaml"` | vigil's own users and groups ([Accounts and permissions](#accounts-and-permissions)). |
| `discovery.interfaces` | string | `""` | Interfaces to search for cameras on, comma separated; empty: every IPv4 interface ([Discovery](#discovery)). |
| `discovery.targets` | IPv4 addresses, comma separated | `""` | Also searched by unicast: cameras on a routed subnet. |
| `discovery.prefer` | `netip`, `onvif`, `dhip`, comma separated | `"netip,onvif,dhip"` | The protocol a camera that answers several is added with. |

`GET /api/v1/schema` returns this table as data (below), so it is never out
of date for a client.

**Loading.** Every value is checked against the schema. A file that is not
valid YAML (`/etc/vigil.yaml: not valid YAML: ... (line 7, column 3)`), or
holds an invalid value, is logged with each error and its path
(`/etc/vigil.yaml: record.segment: 5 is out of range 10..3600`), and
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

## Where recordings may go

`vigild` creates the recording directory itself. With the disk it is meant
for not mounted, that would create it on the root filesystem underneath the
mount point and record there: on a board, onto the flash, until it wears out
or fills up. So a directory is recorded to only when it is, or would be
created, on a disk. The deepest part of `record.path` that exists decides:

- it must not be on the root filesystem (its `st_dev` is not that of `/`),
  unless `record.allow_rootfs` is on;
- the filesystem it is on (the longest mount point in
  `/proc/self/mountinfo` that holds it, after links are resolved) must not be
  one without a disk — `tmpfs`, `ramfs`, `proc`, `sysfs` and the like — or
  flash — `jffs2`, `ubifs`, `squashfs`, `cramfs`, `romfs`, `yaffs`; any other
  type (`vfat`, `exfat`, `ext4`, `xfs`, `btrfs`, `ntfs`, `nfs`, `cifs`, FUSE, …)
  is a disk. RAM and flash are refused whatever `allow_rootfs` says, a root
  filesystem of that kind included. Where mountinfo cannot be read, a
  directory off the root filesystem cannot be told apart from RAM, and is
  refused (`error`);
- it must not be mounted read-only.

Only the part below that directory is created, so nothing is ever made
above the mount point. Otherwise the channel does not record. It reports why
(`rec_state`), and it looks again every 10 s, at a keyframe, so recording
starts by itself once the disk is mounted. While recording, the directory is
looked at again before every new segment and after a failed write, so a disk
that goes away stops recording before a segment could be opened on the
mount point left behind; the channel then waits for it the same way. The
configuration is not changed: `record.path` stays what was set.

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

HTTP Basic, checked against vigil's own users ([Accounts and
permissions](#accounts-and-permissions)) or the system account: the hash
of `api.user` in `api.shadow` is verified with `crypt(3)` (whatever
methods the C library supports: `$1$`, `$5$`, `$6$`, …), and compared in
constant time. Without credentials, or with wrong ones, the answer is
`401` with `WWW-Authenticate: Basic realm="vigil"`; a user who logged in
but lacks the permission an endpoint needs gets `403`
`{"error": "permission denied", "needs": "storage"}`.

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
  password, four logins at a time), so a polling client does not run
  `crypt()` every time; a change to the shadow file or the users file ends
  it, so a disabled user or a new password counts at once.
- Basic authentication sends the password in clear on every request. The
  API has no TLS: use it on a trusted network, or put a TLS proxy or an SSH
  tunnel in front.
- `"auth": false` turns all of this off, permissions included: **anyone who
  can reach the port can change the configuration** (and point a camera URL anywhere). `vigild`
  logs `listening … WITHOUT authentication`. Only for a closed network or
  with `listen` on `127.0.0.1`.

Browsers send cached credentials with a WebSocket upgrade, so the WebSocket
refuses an `Origin` that is not the host it was asked for: a page elsewhere
cannot read the NVR through the user's login. Plain HTTP calls from another
origin are stopped by the browser itself (`PUT` needs a CORS preflight, which
gets a `401`).

## Accounts and permissions

Besides the system account, vigil has users of its own, each in a group
that says what its users may do. They are kept in `api.users`
(`/etc/vigil.users.yaml`, mode 0600), commented YAML written by `vigild`
like `vigil.yaml`; passwords are stored as `crypt(3)` hashes (SHA-512,
`$6$`, with a random salt; SHA-256 or MD5 where the C library has no
SHA-512) and never leave `vigild`: the API shows `"password_set": true`.

Permissions: `live` (live view, status, events), `playback`, `ptz`,
`cameras` (the camera list, discovery), `storage` (recording, schedule,
quota, disks, network storage), `network` (network, DDNS, UPnP, mail,
alarm center), `system` (time, host name, the UI's settings, the whole
configuration), `users` (accounts, and the `api` section, which decides
who gets in at all). Two groups always exist: **admin**, every
permission, which cannot be changed or deleted, and **user**, live view
and playback to start with, which can be changed but not deleted. At most
16 groups and 32 users; user and group names are 1 to 31 of `a-z 0-9 _ . -`.

**Logging in.** A user of the file, enabled, with its password, gets its
group's permissions. The system account (`api.user`, `root`) always logs
in, with every permission, whatever the file says, so a broken or lost
users file never locks the owner out; `admin` is that account too, with
its password, until a user named `admin` is created. A user named `root`
cannot be created.

**Migration.** Nothing changes for an existing installation: with no users
file, only `root` (and `admin`, as its other name) can log in, with every
permission, as before. Users added through the API or the on-screen
settings create the file. If it cannot be loaded (invalid YAML, a bad
name), its users cannot log in (logged), `root` still can, and nothing is
saved over it until it is fixed or removed.

| Endpoint | Needs |
|---|---|
| `GET /status`, `/ws` | `live` |
| `GET /schema`, `/me`, `/jobs/…` | a login |
| `GET /config` | `system` |
| `PUT /config` | every permission |
| `/config/cameras`, `discovery` | `cameras` |
| `/config/record`, `schedule`, `quota`, `nas` | `storage` |
| `/config/ddns`, `upnp`, `smtp`, `alarm` | `network` |
| `/config/time`, `ui`, `display` | `system` |
| `/config/api` | `users` |
| `/network/…` | `network` |
| `/system/…` | `system` |
| `/disks…`, `/storage/…` | `storage` |
| `/discovery…`, `/channels/…` | `cameras` |
| `/users…`, `/groups…` | `users` |
| anything else | `system` |

The table is one function, `api_perm_needed` (`src/api/api_perm.c`),
read before any request is dispatched: an endpoint not listed there needs
`system`, never nothing.

| Method and path | |
|---|---|
| `GET /api/v1/me` | `{"user": "anna", "permissions": ["live", "playback"]}` (`user` is `""` with `api.auth` off). |
| `GET /api/v1/users` | `[{"name", "group", "enabled", "password_set"}]` |
| `POST /api/v1/users` | `{"name", "password", "group": "user", "enabled": true}` → `201` and the user. The password: 4 to 128 characters. |
| `GET`, `PUT`, `DELETE /api/v1/users/<name>` | `PUT` takes any of `group`, `enabled`, `password` (a user is not renamed). |
| `GET /api/v1/groups` | `[{"name", "builtin", "editable", "members", "permissions": {"live": true, …}}]` |
| `POST /api/v1/groups` | `{"name", "permissions": {"live": true}}` (missing permissions are off) → `201`. |
| `GET`, `PUT`, `DELETE /api/v1/groups/<name>` | `PUT {"permissions": {…}}` changes the ones it names. |

Invalid values are `400` with the usual `errors` list (`user.password`:
"at least 4 characters", `group.permissions.fly`: "unknown permission");
an unknown name `404`; a name taken, a group that still has users, or a
change to a built-in group that is not allowed `409`. Every change is
logged, without passwords.

```sh
curl -u root:PASSWORD -X POST $NVR/api/v1/groups -d '{"name": "viewer", "permissions": {"live": true}}'
curl -u root:PASSWORD -X POST $NVR/api/v1/users -d '{"name": "anna", "group": "viewer", "password": "s3cret"}'
curl -u anna:s3cret $NVR/api/v1/me
```

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
| `GET /api/v1/config/<section>` | One section (`cameras`, `record`, `schedule`, `quota`, `nas`, `ddns`, `upnp`, `smtp`, `alarm`, `time`, `ui`, `display`, `api`, `discovery`). |
| `PUT /api/v1/config/<section>` | Replace one section; the others stay. |
| `GET /api/v1/schema` | Every setting: type, default, limits, label, help. |
| `GET /api/v1/ws` | WebSocket: pushed status, events and configuration changes. |
| `POST /api/v1/discovery/scan` | Search the LAN for cameras ([Discovery](#discovery)). |
| `GET /api/v1/discovery` | The devices found, and the current or last discovery job. |
| `DELETE /api/v1/discovery` | Forget the devices found. |
| `GET /api/v1/discovery/job` | The current or last discovery job. |
| `POST /api/v1/discovery/ip` | Change devices' addresses, or (`dry_run`) show the plan. |
| `POST /api/v1/channels/<n>/assign` | Add a device found to channel n. |
| `POST /api/v1/discovery/autoadd` | Add every new device to the empty channels. |
| `POST /api/v1/devices/test` | Connect to a camera described by hand: codec and size, or why not. |

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
      "state": "live", "status": "live", "events": ["motion"], "decoding": true, "view": "main", "no_decoder": false, "recording": true, "rec_state": "recording",
      "rec_bytes": 73400320,
      "codec": "H.265", "width": 1920, "height": 1080, "fps": 25 }
  ]
}
```

`uptime` is seconds since the API started, `time` the board's clock (Unix
time). One channel per camera slot, in tile order (an empty slot, a camera
whose `url` is `""`, is `status: "empty"`). `running`: the
channel has a source (it is enabled and its URL could be used); `live`:
video arrived in the last 3 s; `state` is `live`, `off`, `CONNECTING` or the
last error (`connect: Connection refused`, `login refused (Ret 203)`, …);
`status` says the same in one word: `empty` (no camera in this slot),
`disabled`, `connecting`, `live`, `no_video` (connected, no video for 3 s),
`offline` (refused, timed out, no route, lost), `auth` (the camera refused
the user or the password: RTSP 401 or 403, NETIP login 203, 205, 206),
`codec` (neither H.264 nor H.265) or `error` (anything else; `state` has
the text); `events` are the events the camera reports now, each held 5 s
after the last report: `motion`, `human`, `face`, `vehicle` (no source
reports them yet: camera alarms over NETIP and ONVIF events are to come);
`recording`: a segment is open; `rec_state` is `off` (no `record.path`);
when recording is set up, `recording`, `ready` (no segment open yet: waiting
for a keyframe or for the camera) or `failed` (the last write failed); when it
is not, why: `waiting for disk` (the path leads onto the root filesystem:
nothing is mounted there), `not a disk` (RAM or flash), `read-only`, `error`
(not a directory, no permission, mountinfo unreadable; the log says which);
`rec_bytes` counts what was written since the channel started; `decoding`: a
decoder shows the channel; `no_decoder`: the tile wants a picture but the
platform could not give a decoder (media memory, decoder channels), asked
for again; `view`: the stream on screen, or about to be
(`main`, `sub` for the camera's sub-stream in a small tile, `""` when the
channel is not on screen or nothing decodes it); `codec`, `width`,
`height` and `fps` (averaged over 2 s) are that stream's; `codec` is `null`
until the stream says.

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
`down`, `no decoder`, `REC failed`, `REC on`, `REC waiting for disk`,
`REC not a disk`, `REC read-only`, `REC error`, `added`, `restarted`, `removed`,
`bad URL`, `config applied`, `config: N camera(s) failed`. A client that
wants to know when its change runs waits for a `config` message whose
`applied` reached the generation its `PUT` produced (`status.config` after
the `PUT`).

```sh
# with websocat
websocat -H "Authorization: Basic $(printf root:PASSWORD | base64)" ws://192.168.1.20:8080/api/v1/ws
```

## Management

The management pages ([management.md](management.md)) over HTTP. They change
the system, not only `vigil.yaml`, and only on OpenIPC firmware: elsewhere
every change answers `409` ("vigild does not manage this system") and the
`GET`s show what there is. Changes that take a while answer `202` with a job:

```json
{ "job": 7 }
```

and `GET /api/v1/jobs/7` says how it goes:
`{"id": 7, "kind": "format", "target": "sda", "state": "running", "message": "", "started": 1795700819, "finished": null}`
(`state`: `queued`, `running`, `done`, `failed`; the last 16 jobs are kept).

| Method and path | |
|---|---|
| `GET /api/v1/network/ethernet` | `{"managed", "config": {"dhcp", "address", "netmask", "gateway", "dns1", "dns2", "mtu"}, "status": {"interface", "present", "up", "link", "speed", "duplex", "mac", "mtu", "address", "netmask", "gateway", "dns": [...]}, "pending": {"interface", "seconds"} \| null}` |
| `PUT /api/v1/network/ethernet` | Any of the `config` keys (the others stay) → `202` `{"job", "confirm_within": 60}`. Undone after 60 s unless confirmed; `"confirm": false` skips that. `400` with the field on a bad address, netmask, gateway, DNS or MTU. |
| `POST /api/v1/network/ethernet/confirm` | Keep the change; `409` when none waits. |
| `POST /api/v1/network/ethernet/revert` | Undo it now. |
| `GET /api/v1/network/wifi` | `{"present", "interface", "profile" (wlandev), "adapters": [{"usb_id", "name", "driver", "profile", "bound"}]}`, and with an adapter `ssid`, `password_set`, `can_scan`, and the interface's `status`. |
| `PUT /api/v1/network/wifi` | `{"ssid", "password", "profile"}` → `202`; `profile` (default: the adapter's, when `wlandev` is unset) becomes `wlandev`, and with no interface yet its driver is loaded the firmware's way. `409` without an adapter. |
| `GET /api/v1/network/wifi/scan` | `{"networks": [{"ssid", "signal", "secured"}]}`; `501` without `iw`. |
| `GET /api/v1/network/lte` | `{"present", "interface", "driver", "profile", "modems": [{"usb_id", "name", "driver", "profile", "bound"}], "status"}` |
| `PUT /api/v1/network/lte` | `{"profile"}`: `wlandev` = that modem profile, for the next boot. |
| `GET /api/v1/network/services` | The last DDNS update, mail and alarm post (`{"enabled", "last", "ok", "result"}`), `upnp` (`"active": false`), `p2p` (`"available": false`), and whether `curl` is there. |
| `POST /api/v1/network/ddns/update`, `/network/smtp/test` | `202`: an update now, a test mail with the settings in force. |
| `GET /api/v1/system/time` | `{"time", "local", "zone", "tz", "rule": {"std_offset", "dst", "dst_offset", "start": {"month", "week", "wday", "minute"}, "end": {…}}, "rtc", "ntp": {"enabled", "interval", "servers", "last_try", "last_ok", "result"}}`; offsets in minutes east of UTC. |
| `PUT /api/v1/system/time` | Any of `"zone"` (a name from `/system/zones`), `"tz"` (a POSIX string) or `"rule"` (as above: built into a POSIX string), `"time"` (Unix time: set the clock), `"ntp": {"enabled", "servers", "interval"}`. |
| `POST /api/v1/system/time/sync` | `202`: synchronise with the NTP servers now. |
| `GET /api/v1/system/zones` | `[{"name": "Europe/Berlin", "tz": "CET-1CEST,M3.5.0,M10.5.0/3"}, …]` |
| `GET`/`PUT /api/v1/system/hostname` | `{"hostname"}`; also in `GET /api/v1/status`. |
| `POST /api/v1/system/reboot`, `/system/shutdown` | `{"ok": true}`, then vigild stops as on SIGTERM (the channels close their recordings), syncs the disks and runs busybox's `reboot` or `poweroff`. Needs the `system` permission; `409` where vigild does not manage the system. The UI's Exit does the same. |
| `GET /api/v1/disks` | `{"managed", "disks": [{"name", "parent", "partition", "size", "model", "removable", "read_only", "fs", "mount", "total", "free", "system", "state"}], "formats": ["exfat", "vfat"]}` |
| `POST /api/v1/disks/<name>/format` | `{"fs": "exfat", "confirm": "<name>"}` → `202`. A system device, or a wrong `confirm`, is `400`. |
| `POST /api/v1/disks/<name>/mount`, `/unmount` | `202`. |
| `GET /api/v1/storage/nas` | `{"support": {"cifs": {"ok", "why"}, "nfs": {…}}, "mount": "/mnt/nas", "mounted", "status"}` |
| `POST /api/v1/storage/nas/test` | `202`: mount the share in force at a test directory, write and read a file, unmount. |
| `GET /api/v1/jobs/<id>` | A job, as above. |

## Discovery

vigild finds cameras on its network, changes their addresses and adds them
to channels, over three protocols:

| | NETIP (Xiongmai, OpenIPC majestic) | ONVIF | DHIP (Dahua and its OEMs) |
|---|---|---|---|
| Found by | search (msgid 1530) to UDP 34569, by broadcast; the answer comes by broadcast to 34569 | WS-Discovery Probe for `NetworkVideoTransmitter` to 239.255.255.250:3702; the answer comes back to the asker | `DHDiscover.search` to 239.255.255.251:37810 and by broadcast; the answer comes back to the asker |
| Address changed by | the broadcast set (msgid 1532, the MAC and the sofia hash of the password; no connection, so a device on another subnet can be moved into this one); if it does not answer, a login on TCP 34567 and `ConfigSet` of `NetWork.NetCommon` | `SetNetworkDefaultGateway` and `SetNetworkInterfaces` (static address), `SystemReboot` when the device asks for it; WS-Security digest, HTTP Digest on a 401 | RPC2 over HTTP: `global.login` (Default encryption), `configManager.setConfig` of `Network` |
| Camera URL | `netip://user:pass@ip:34567/?ch=0&stream=main`; the sub-stream is derived | `GetProfiles`, `GetStreamUri` of the first two profiles (main, sub) | `rtsp://user:pass@ip:554/cam/realmonitor?channel=1&subtype=0`, `sub_url` `subtype=1` |

ONVIF and DHIP changes need a TCP connection to the device, so the device
has to be reachable (on this subnet, or routed); NETIP's broadcast set does
not. A device that answers more than one protocol is one entry (merged by
MAC, else by address). When it can be added more than one way, the URL
comes from NETIP first (both streams without guessing a path), then ONVIF
(the device names its own streams), then DHIP (Dahua's fixed RTSP path);
`discovery.prefer` changes the order.

Everything that touches the network is a **job** on discovery's own
thread, one at a time (another request gets `409`): `scan` returns at once
(`202`) with the running job; `ip`, `assign` and `autoadd` wait up to 8 s
and answer `200` with the outcome, or `202` with the job still running.
`GET /api/v1/discovery/job` (or the `job` of `GET /api/v1/discovery`)
follows it:

```json
{ "id": 7, "kind": "ip", "running": false, "dry_run": false, "progress": 100,
  "message": "1 changed, 0 failed",
  "results": [ { "device": "00:12:31:4f:2a:10", "ip": "192.168.1.10", "new_ip": "192.168.0.31",
                 "reprobed": true, "protocol": "netip", "done": true, "ok": true, "message": "changed" } ] }
```

A failed result has `"ok": false`, `error` (`auth`: the device refused the
credentials; `unsupported`; `timeout`; `unreachable`; `protocol`: an answer
that could not be used; `invalid`) and a `message` saying what happened.

**Credentials** (`user`, default `admin`; `password`, default empty) come
in the request, are used for that job and wiped; they are kept only in the
camera URL of a device added to a channel. Answers and the log never show
them (URLs are redacted as everywhere else). Each refusal by a device is
counted: after 10 in a minute, requests with credentials get `429` with
`Retry-After` until the oldest is a minute old, and at most 30 such
requests a minute are taken in any case, so the API cannot be used to guess
a camera's password at speed or to lock its account.

### `POST /api/v1/discovery/scan`

```json
{ "protocols": ["netip", "onvif", "dhip"], "seconds": 3,
  "interfaces": ["eth0"], "targets": ["10.1.2.3"], "broadcast": true,
  "ports": { "netip": 34569, "onvif": 3702, "dhip": 37810 } }
```

All optional: `protocols` (or `"all"`, the default), `seconds` 1..15
(default 3; the searches go out three times, a second apart),
`interfaces` (default `discovery.interfaces`, or every IPv4 interface that
is up, not loopback), `targets`: addresses also asked by unicast (cameras
on a routed subnet; `discovery.targets` are asked too),
`broadcast: false` asks only those, and `ports` the UDP ports asked (devices
behind port forwarding, test responders). Devices are kept 10 minutes after
they last answered, 64 at most.

### `GET /api/v1/discovery`

```json
{ "devices": [
    { "device": "00:12:31:4f:2a:10", "ip": "192.168.1.10", "mac": "00:12:31:4f:2a:10",
      "mask": "255.255.255.0", "gateway": "192.168.1.1", "dhcp": null,
      "protocols": ["netip", "onvif"], "vendor": "Xiongmai", "model": "IPC", "name": "LocalHost",
      "serial": "abcdef0123456789", "version": "V4.02.R11...", "channels": 1,
      "ports": { "netip": 34567, "http": 80 }, "onvif": "http://192.168.1.10:8899/onvif/device_service",
      "interface": "eth0", "age": 4, "channel": null,
      "urls": { "netip": "netip://192.168.1.10:34567/?ch=0&stream=main" } } ],
  "job": { ... } }
```

`device` is the key the other requests take (the MAC, else the address).
`channel` is the channel whose camera URL points at this address, or
`null`. `urls` are what adding it would use, without credentials (ONVIF's
come from the device when it is added). `age`: seconds since it last
answered.

### `POST /api/v1/discovery/ip`

One device, `{"device": "00:12:31:4f:2a:10", "ip": "192.168.0.31", "mask":
"255.255.255.0", "gateway": "192.168.0.1", "user": "admin", "password":
"..."}`, or many: `"devices": ["mac1", {"device": "mac2", "ip": "..."}]`
with `"start": "192.168.0.31"` for those without an address: they get the
next free ones from `start` within its subnet, skipping the addresses of
known devices and cameras, this board's own, and any that answer ARP or
ping. `"dry_run": true` only plans (no credentials needed, nothing is sent
to the devices). Each device is changed over the first protocol it answered
in the order above; a refused password is not tried on another protocol.
After a change the device is looked for at its new address for up to 10 s
(`reprobed`).

### `POST /api/v1/channels/<n>/assign`

`{"device": "...", "user": "admin", "password": "...", "protocol": "onvif",
"name": "Gate", "replace": false}` puts a device found on channel n (the
channels before it, if empty, stay empty). `protocol` (default: the order
above) and `name` (default: the device's) are optional. A channel with a
camera is refused unless `replace`; a device already on a channel is
refused.

A camera that discovery does not find (another subnet, a host name, a
plain RTSP camera) is described by hand instead of `device`:

```json
{ "manual": { "protocol": "netip", "address": "192.168.5.20", "port": 34567,
              "user": "admin", "password": "...", "channel": 1 },
  "name": "Gate", "replace": false }
```

| `manual.` | |
|---|---|
| `protocol` | `netip` (Xiongmai XMeye), `onvif`, `dahua` (or `dhip`), `rtsp` |
| `address` | IPv4 address or host name |
| `port` | default 34567, 80, 554 (Dahua: the RTSP port, not 37777), 554 |
| `user`, `password` | default `admin` (plain RTSP: none) and empty |
| `channel` | the device's video channel, 1-based: an NVR's or DVR's; default 1 |
| `main`, `sub` | `rtsp` only: the main and the (optional) sub-stream, each a path (`/stream1`) or a whole `rtsp://` URL |

NETIP gives `netip://user:pass@host:port/?ch=N-1&stream=main` (sub-stream
derived), Dahua `rtsp://…/cam/realmonitor?channel=N&subtype=0` and
`subtype=1`, ONVIF the URIs the device names for its channel's two
profiles (GetProfiles, GetStreamUri, at the time of the request), RTSP the
paths given. The results show the URLs, redacted (`url`, `sub_url`). A
manual device is not refused for being on another channel already (an NVR's
channels share an address).

### `POST /api/v1/devices/test`

The same description as `manual` above, as the body. The job connects as
a channel would (NETIP login and stream, RTSP DESCRIBE/SETUP/PLAY; ONVIF
first asks for the stream URIs) and waits up to 6 s for the first
keyframe. Its one result:

```json
{ "device": "192.168.5.20", "ip": "192.168.5.20", "protocol": "netip",
  "url": "netip://admin:***@192.168.5.20:34567/?ch=0&stream=main",
  "codec": "H.264", "width": 1920, "height": 1080, "done": true, "ok": true,
  "message": "H.264 1920x1080" }
```

or `"ok": false` with `error`: `auth` (the device refused the user or the
password), `unreachable` (nothing answers: offline), `codec` (neither
H.264 nor H.265), `unsupported` (for ONVIF, "not an ONVIF device": a web
server, but no SOAP), `timeout` (connected, no video), `invalid` (an
address or credentials a URL cannot carry), `protocol`; `message` has the
device's words. The classification is the tiles' (`status` in
`GET /api/v1/status`). A test is a job like the others, and its refused
passwords count against the limit.

### `POST /api/v1/discovery/autoadd`

`{"user": "admin", "password": "..."}`: every device found that is not on
a channel goes to the next empty channel, in address order, with these
credentials (one URL per device: for an NVR or DVR found this way, its
first channel). The configuration is saved once, at the end; the results
say which device went where.

## Inside

- `src/core/schema.{h,c}`: the generic engine (defaults, JSON in and out,
  checks, describing itself). `src/core/schema_yaml.{h,c}`: the file's YAML,
  read with libyaml into the same JSON document and written with the
  schema's help as comments. `src/core/config_schema.c`: vigild's table.
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
  `src/api/auth.c`: Basic credentials, the shadow file, `crypt(3)`;
  `src/api/users.c`: the accounts file and logging in; `src/api/api_perm.c`:
  which permission each endpoint needs; `src/api/api_users.c`: the
  accounts' endpoints.
  `src/api/auth.c`: Basic credentials, the shadow file, `crypt(3)`.
  `src/api/api_disc.c`: the discovery endpoints, over
  `src/core/discovery.{h,c}` (the device table and the jobs),
  `src/core/disc_proto.{h,c}` (the searches and their answers, as pure
  parsers of untrusted input) and `src/core/disc_ops.{h,c}` (ONVIF, Dahua
  RPC2 and NETIP calls on one device). `tests/fake_discovery.py` plays
  devices of each kind for `tests/discovery_test.py`.
- JSON is parsed by cJSON ([third_party/cJSON](../third_party/cJSON), MIT),
  bodies capped at 64 KiB; YAML by [libyaml](https://github.com/yaml/libyaml)
  (MIT, the system's).
- `-DVIGIL_API=OFF` builds `vigild` without CivetWeb and `crypt`
  (`src/api/api_none.c`); the configuration file still works.
- If the port cannot be bound, `vigild` logs it and runs on without the API.
