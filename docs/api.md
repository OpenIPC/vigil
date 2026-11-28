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
| `cameras[].sub_url` | string | `""` | The sub-stream for small tiles; empty: derived for NETIP, else the main stream. |
| `cameras[].events` | `auto`, `netip`, `onvif`, `off` | `auto` | Where the camera's events come from ([Camera events](#camera-events)): `auto` NETIP alarms for a `netip://` camera, else ONVIF events if it has them. Changes without reconnecting the streams. |
| `cameras[].onvif_url` | `http://…`, no user or password | `""` | Its ONVIF device service, for the events (the URL's user and password log in). Empty: found from its address. Set when a camera is added over ONVIF. |
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
| `api.listen` | `"port"` or `"a.b.c.d:port"` | `"0.0.0.0:8080"` | Where the API listens (IPv4). Empty: no API. A change moves it at once ([Ports](#ports)). |
| `api.auth` | boolean | `true` | Require a login. See [Authentication](#authentication). |
| `api.user` | string | `"root"` | The system account: it logs in with every permission (and as `admin` while no user of that name exists). |
| `api.shadow` | absolute path | `"/etc/shadow"` | The shadow file holding that account's password hash. |
| `api.users` | absolute path | `"/etc/vigil.users.yaml"` | vigil's own users and groups ([Accounts and permissions](#accounts-and-permissions)). |
| `web.enabled` | boolean | `true` | The [zero channel](#zero-channel): the screen, mouse and keyboard in a browser. Off: its pages answer `503` and its viewers are disconnected. |
| `web.listen` | `"port"` or `"a.b.c.d:port"` | `"0.0.0.0:80"` | Where its web server listens (IPv4). Empty: none. A change moves it at once ([Ports](#ports)). |
| `web.fps` | integer 1..30 | 15 | Its pictures a second, at most. |
| `rtsp_server.enabled` | boolean | `true` | The cameras' streams to other clients over RTSP ([docs/rtsp.md](rtsp.md)). |
| `rtsp_server.port` | integer 1..65535 | 554 | Its TCP port; it counts in [Ports](#ports). |
| `rtsp_server.auth` | boolean | `true` | Require a Digest login of a user with the `live` permission. |
| `rtsp_server.max_clients` | integer 1..32 | 8 | Clients being sent a stream at once. |
| `discovery.interfaces` | string | `""` | Interfaces to search for cameras on, comma separated; empty: every IPv4 interface ([Discovery](#discovery)). |
| `discovery.targets` | IPv4 addresses, comma separated | `""` | Also searched by unicast: cameras on a routed subnet. |
| `discovery.prefer` | `netip`, `onvif`, `dhip`, comma separated | `"netip,onvif,dhip"` | The protocol a camera that answers several is added with. |
| `qos.type` | `off`, `standard`, `custom` | `"off"` | DSCP marking of vigild's packets ([QoS](#qos-dscp-marking)). `off`: DSCP 0 everywhere; `standard`: the defaults below; `custom`: the four values below. |
| `qos.video` | integer 0..63 | 34 (AF41) | The zero channel's picture (`web.listen`'s WebSocket); the RTSP server's video (its RTP over UDP, an interleaved connection from its `PLAY`); the NETIP server's live streams. |
| `qos.management` | integer 0..63 | 16 (CS2) | The HTTP API and the web server's pages and logins, the ONVIF service and WS-Discovery, the RTSP and NETIP servers' requests (and NETIP's search answers), DDNS, camera searches. |
| `qos.camera` | integer 0..63 | 34 (AF41) | vigild's connections to cameras: RTSP and NETIP control and the ACKs of their video. |
| `qos.alarm` | integer 0..63 | 18 (AF21) | The API's event WebSocket (`/api/v1/ws`), the alarm center's POSTs, mails. |
| `onvif_server.enabled`, `.port` | boolean, integer 1..65535 | `true`, 8899 | vigild as an ONVIF device ([onvif.md](onvif.md)); 80, ONVIF's usual port, is the web server's, 8899 is XM's. Its port counts in [Ports](#ports). |
| `netip_server.enabled`, `.port` | boolean, integer 1..65535 | `true`, 34567 | vigild as an XM NETIP device ([NETIP server](#netip-server)); 34567 is XM's. Its port counts in [Ports](#ports). |

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

Every setting applies while `vigild` runs (`"live"` in the schema). The main thread notices the new configuration within a fraction
of a second and:

- **renames** a camera without touching its stream;
- **restarts** only the cameras whose URL or enabled flag changed; a change
  to `record` restarts every camera (each opens a new segment);
- leaves the others alone: their video and recordings go on.

The result is reported as events (`config applied`, `CAMn added`,
`restarted`, `removed`, `config: N camera(s) failed`) on the TV's event
strip, in the log and on the WebSocket. `status.config` shows the saved
(`generation`) and the running (`applied`) configuration.

## QoS (DSCP marking)

With `qos.type` other than `off`, vigild sets the DSCP of the packets it
sends (`IP_TOS` = DSCP << 2 on IPv4, `IPV6_TCLASS` on IPv6; the ECN bits
stay 0), by traffic class:

| Class | What |
|---|---|
| video | the zero channel's WebSocket on `web.listen` (port 80), from its upgrade on; the [RTSP server](rtsp.md)'s RTP and RTCP over UDP, and each interleaved RTSP connection from its `PLAY` to its `TEARDOWN`; a NETIP connection carrying a live stream, from the stream's start |
| management | the API's, the web server's, the ONVIF service's, the RTSP server's and the NETIP server's listeners, so every connection they accept (pages, logins, REST calls, ONVIF, RTSP and NETIP requests); WS-Discovery; the NETIP server's answers to XM's search; DDNS updates (curl); the camera searches' broadcasts |
| camera | every connection vigild makes to a camera (RTSP, NETIP, the discovery jobs' NETIP logins), from its SYN; RTSP runs over TCP (interleaved), so the video's ACKs carry it too |
| alarm | the API's event WebSocket `/api/v1/ws`, the alarm center's POSTs and the mails (curl's `ip-tos`, which curl has since 8.9: OpenIPC's curl 8.15 does) |

A change applies at once: new sockets get the new values; the listeners,
the camera sessions and the WebSockets open are marked again. An open
HTTP connection to the API or the web pages keeps its mark until it is
closed (they are short). What vigild does not send is not marked: sshd,
ntpd, the firmware's own `httpd` would need iptables' `DSCP` target, and
the OpenIPC kernels built for these boards have no netfilter.

## Ports

`web.listen`, `api.listen`, `rtsp_server.port`, `onvif_server.port` and
`netip_server.port`: no two on one port (an empty listen holds none); a
`PUT` that would put two there is refused (`400`, the later one's path:
`onvif_server.port: port 80 is web.listen's already`). A file that has
them clash is still loaded, with the clash logged: the server that starts
second does not get the port.

A changed `api.listen`, `web.listen` or (while that server is on)
`onvif_server.port`, `rtsp_server.port` or `netip_server.port` is tried before it is saved (bound for a moment, as
the server will): a port another program holds, one below
1024 without root, or an address the recorder does not have is refused
with the reason (`api.listen: port 8081 is in use ...`). Saved, the server
moves at once: it stops (its open connections and WebSockets close) and
listens on the new port. Should that fail all the same (the port was taken
in between), it goes back to the old one, the setting is put back, the
event `api port: ...` (`rtsp port: ...`) says why, and Network -> Ports
shows it. The ONVIF
server follows its port itself (`src/api/onvif.c`), and if the port cannot
be had it tries again every 30 s; the page says so. The NETIP server moves
with the configuration too (its clients reconnect and find it again by
XM's search); if the new port cannot be had it stays on the old one, the
event `netip port: ...` and the page saying so.

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
SHA-512), which check the logins that send the password (the API, the web
page, the screen), and **sealed** beside them (`password_enc`, AES-256-GCM
under `vigil.key`: [security.md](security.md)), from which the RTSP, ONVIF
and NETIP servers work out what their clients prove they know. Neither
leaves `vigild`: the API shows `"password_set": true`. A password set
through the API or the screen works on every server at once.

Permissions: `live` (live view, status, events), `playback`, `ptz`,
`cameras` (the camera list, discovery), `storage` (recording, schedule,
quota, disks, network storage), `network` (network, DDNS, UPnP, mail,
alarm center, QoS), `system` (time, host name, the UI's settings, the whole
configuration), `users` (accounts, and the `api`, `web` and
`rtsp_server` sections, which decide who gets in at all), `remote` (the [zero
channel](#zero-channel): the screen, mouse and keyboard from a browser,
which is the console). Two groups always exist: **admin**, every
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
| `PUT /config` | every permission but `remote` |
| `/config/cameras`, `discovery` | `cameras` |
| `/config/record`, `schedule`, `quota`, `nas` | `storage` |
| `/config/ddns`, `upnp`, `smtp`, `alarm`, `onvif_server`, `qos`, `netip_server` | `network` |
| `/config/time`, `ui`, `display` | `system` |
| `/config/live` | `live` |
| `/config/api`, `web`, `rtsp_server` | `users` |
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
| `PUT /api/v1/users/<system account>` | `{"password"}` → `200`, `{"name", "system": true, "password_set": true}`: the system account's (`api.user`, `root`) password, written into `api.shadow` (its SSH and console login too) and sealed for RTSP, ONVIF and NETIP ([security.md](security.md)). Only the system account itself may (`403` otherwise, even for `admin`'s group); nothing else of it can be set (`400`). |
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

## Zero channel

`http://<recorder>/` (port 80, `web.listen`) shows the recorder's screen
as the monitor has it, the video tiles and the UI over them, live, and
takes the browser's mouse, wheel, touch and keyboard as if they were
plugged into the recorder: a KVM, or what DVRs call the zero channel. It
is a second web server in `vigild` (`src/api/web.c`), apart from the API,
with one page and one WebSocket.

**Who gets in.** A login is always required, whatever `api.auth` says: the
API's accounts and password check (vigil's users, the system account),
whose failures count with the API's (five in a row from one address lock
that address out, for a doubling time up to a minute; every failure is
answered a second late). Only users whose group has the `remote`
permission get in: by default only **admin** (and the system account).
Until such an account has a password, which on OpenIPC means until root
has one, nobody does. The page logs in with a form (`POST /login`) that
sets a session cookie (`HttpOnly`, `SameSite=Strict`, 12 hours at most,
ended by `POST /logout` or when the shadow or users file changes); the
login form and the WebSocket must come from the recorder's own page
(`Origin`), and the pages may not be framed (`X-Frame-Options`, a
`Content-Security-Policy` without inline scripts). Like the API it has no
TLS: the password and then the cookie cross the network in clear, so
anyone who can watch the traffic can take the session. Use it on a
trusted network, or behind a TLS proxy or an SSH tunnel.

**What they may do** is what someone at the monitor may do. With
`ui.login` off (the default) that is everything; with it on, the menus ask
for the UI's own login, as at the monitor, and check that user's
permissions. `remote` is therefore given to whom you would give the keys
to the room the recorder is in.

| Path | |
|---|---|
| `GET /` | The viewer page; without a session `302` to `/login`. |
| `GET /login`, `POST /login` | The login form; `user`, `password` → `303` to `/` with the cookie, or the form again with `401` (wrong), `403` (no `remote` permission, or posted from another site), `429` (locked out, `Retry-After`). |
| `POST /logout` | `303` to `/login`, the cookie cleared. |
| `GET /session` | `{"user": "root"}`, or `401`: the page asks after its WebSocket closed. |
| `GET /kvm.js` | The page's script. |
| WebSocket `/ws` | The pictures and the input. `401` without a session, `403` without `remote` or from another site's page, `503` with two viewers already or `web.enabled` off. |

**The pictures** are composed in software: none of the SoCs reads its
output back (on SigmaStar's i2m, `MI_DISP_GetScreenFrame` is in the SDK's
API but its `mi_disp.ko` answers it with an error without looking, and the
display has no writeback). The UI is LVGL's own pixels, copied as LVGL
flushes what it drew; the video under the UI's holes is each decoder's
newest picture, read from the decoder's output (a user queue of one, kept
only while a viewer is connected) and drawn where the display shows it.
Each viewer gets only the 16x16 blocks that changed since its last
picture, gathered into a few rectangles, each a baseline JPEG
(`src/app/kvm_enc.h`): a moving pointer or a menu opening sends that
corner, a quad view of moving cameras the whole picture. The browser acks
each picture after drawing it and at most two are on the way, so a slow
link or browser lowers the rate instead of queueing; the rate is also at
most `web.fps` and at most what keeps the encoding under half the time
(half a core per viewer), and the viewer threads run below the cameras'.
Nothing is captured while nobody watches. The page offers the screen's
size or half of it, and three qualities.

Messages on `/ws`: from the server, `{"type": "hello", "width": 1280,
"height": 720, "user", "fps"}` first, then binary pictures (`'F'`, flags
(1: whole), width, height, the number of parts, a sequence number, then
each part's x, y, w, h, length and JPEG; little-endian), `{"type":
"stats", "fps", "kbps", "ms"}` every second, `{"type": "pong", "c"}`, and
`{"type": "error", "message"}` before a close. From the browser, as JSON:
`{"t": "ack", "seq"}`, `{"t": "opts", "scale": 1|2, "quality": 10..95}`,
`{"t": "ping", "c"}`, and the input, in screen pixels:
`{"t": "move", "x", "y"}`, `{"t": "btn", "b": 0|1|2, "down"}` (left,
middle, right: right opens the UI's menu), `{"t": "wheel", "n"}` (notches,
positive away from the user), `{"t": "key", "code": "KeyA", "v": 1|0|2}`
(press, release, repeat; `KeyboardEvent.code`, keys by their place on a
US keyboard, as a USB keyboard at the recorder types). The input goes into
the UI as one more absolute pointer and keyboard of the evdev reader
(`src/app/input_evdev.c`), so it is handled exactly as a USB mouse's and
keyboard's, and the monitor shows the remote pointer; a viewer that goes
lets go of whatever it held.

## NETIP server

`vigild` also speaks Xiongmai's NETIP (DVRIP), as an XM recorder does, so
that XM's own clients — CMS / VMS on a PC, XMEye on a phone on the same
LAN (not its cloud P2P) — find it and show its cameras live. It is on by
default: `netip_server.enabled`, on `netip_server.port` (TCP 34567, XM's),
with its own threads (`src/api/netip_srv.c`); a change to either applies
at once ([Ports](#ports)). A build without the API has none, since it logs in as the API's
users.

**Who gets in.** vigil's users and the system account, with their own
passwords. XM's clients never send the password, only XM's 8-character
hash of it (an MD5 folded into 62 letters and digits), which cannot be
checked against the `crypt(3)` hash vigil keeps; it is checked against
the same hash of the user's sealed password ([security.md](security.md)),
so a password works over NETIP as soon as it is set. The system account's
password is in `/etc/shadow`: vigil knows it when it was set through vigil
(`PUT /api/v1/users/root`, the screen's Account → Users), or since that
account last logged in with it to the API, the web page or the UI; after
a `passwd` elsewhere it is not used until then, and such a login is
refused (the log says why). XM's hash is weak (any 8-character string with the same
fold logs in, and the hash is all a client needs), so the LAN is where
this server belongs: five wrong logins from one address lock that address
out for a minute (`Ret 205`). A wrong password or user answers `Ret 203`
and ends the connection.

**What they may do.** Live view of every camera, main stream (`Main`)
and sub-stream (`Extra1`), for users whose group has the `live`
permission (others get `Ret 107` to their claim). Everything else is
read-only: configuration writes, PTZ, time setting, user management,
upgrades get `Ret 107`. Playback, two-way audio, alarms and snapshots are
not there.

| Message | Answer |
|---|---|
| 1000 login | `Ret`, `SessionID`, `AliveInterval` 20, `ChannelNum` (the channels), `"DeviceType "` `DVR` (with the space, as XM devices send it), `ExtraChannel` 0 |
| 1002 logout | `Ret 100`, the connection and its media connections end |
| 1006 keepalive | `Ret 100`; a logged-in connection without one for 60 s ends |
| 1020 `SystemInfo` | `DigChannel` = the channels, `VideoInChannel` 0, `SerialNo` (16 hex digits from the first interface's MAC), `HardWare` `OpenIPC <platform>`, `SoftWareVersion` `V<version>.vigil`; `WorkState` (video loss, motion, recording per channel), `StorageInfo` (empty) |
| 1042 / 1044 config | `ChannelTitle`, `General.General`, `General.Location`, `General`, `NetWork.NetCommon`, `Simplify.Encode` (codec, resolution and frame rate of both streams), `SystemInfo`; any other name `Ret 607` |
| 1048 `ChannelTitle` | the channels' names (the configured name, else the camera's own) |
| 1360 `SystemFunction` | a minimal one: two streams, nothing else; other abilities `Ret 607` |
| 1452 `OPTimeQuery` | the local time, `YYYY-MM-DD hh:mm:ss` |
| 1413 / 1410 `OPMonitor` | live view, below |
| 1040, 1046, 1400, 1450, 1460, 1476-1488, 1500, 1502, 1520, 1532, 1540, 1590 | `Ret 107`: read-only |
| anything else | `Ret 100` and nothing else, so a client does not wait; logged once |

Replies carry the request's msgid + 1 and its sequence number. Each kind
of request it does not serve is logged once (`netip server: ...`).

**Live view.** A client claims a stream (1413, `OPMonitor` `Claim`,
`Channel` from 0, `StreamType` `Main` or `Extra1`) and starts it (1410,
`Start`), as XM devices take it, in either of two ways:

- *XM's clients* (CMS, XMEye): the claim on a **new** connection that never
  logs in, with the session's id in its header, and the start on the
  logged-in one, which answers it (1411); the media go to the claiming
  connection. A start may come before its claim. A `Stop` closes the
  media connection; so does the session's logout or loss. Session ids are
  random (31 bits), and a claim is taken only from the address that
  logged in; any other gets `Ret 105` and the connection ends.
- *python-dvr, vigil's own NETIP source*: claim and start on the logged-in
  connection, the media on it right after the start, which gets no answer
  (a reply among the media would be read as a frame).

The media are 1412 messages of at most 8 KiB, the channel in the
header's byte 12, sequence numbers from 1. Each picture is an XM frame:
`00 00 01 FC` and a 16-byte header for a keyframe (codec 2 H.264 / 3
H.265 in the low nibble of its byte, bits 8-9 of width/8 and height/8 in
its high bits, the frame rate, width/8, height/8, XM's packed local time,
the length), `00 00 01 FD` and the length for the others; the payload is
the camera's access unit as vigil records it (Annex-B, a keyframe with
its parameter sets), never re-encoded. A stream starts at a keyframe.
Each stream has its own queue (1 MiB for a main stream, 256 KiB for a
sub-stream; 16 streams and 6 MiB at most, claims beyond get `Ret 103`): a
client that falls behind loses pictures up to the next keyframe that fits,
and never slows the cameras, the recording or the other clients.

**The LAN search.** On UDP 34569 it answers XM's search (msgid 1530) as XM
devices do, with `NetWork.NetCommon`: the address, mask, gateway and MAC
of the interface the search came in on, `TCPPort`, `HostName`, `SN`,
`ChannelNum` and `Version`, sent to the broadcast address, port 34569,
where XM's tools listen (and back to the asker when it asked from another
port, as vigil's own search does when it asks one address). At most 20
answers a second. vigil's own camera search hears its own answer and
leaves it out (the same serial and port); another vigil lists it as a
NETIP device.

Not verified with XM's own CMS / VMS or XMEye: they are tested only
through what python-dvr and vigil's own NETIP source do (`ctest -R
netip_server_e2e`).

## Camera events

Each camera's events come from one place, `cameras[].events`:

| `events` | `netip://` camera | other camera (`rtsp://`) |
|---|---|---|
| `auto` (default) | its NETIP alarms | its ONVIF events, if it has them |
| `netip` | its NETIP alarms | none |
| `onvif` | its ONVIF events | its ONVIF events |
| `off` | none | none |

A change applies at once, without reconnecting the camera's streams. An
event is held while the camera says it goes on and ends at its stop; one
the camera reports only as a moment (a line crossed) is held 5 s. When the
connection or the subscription is lost, what it said goes on ends.

**NETIP alarms.** The main stream's connection, once logged in and
streaming, sends AlarmSet (msgid 1500) and is then pushed AlarmInfo
(msgid 1504), `{"AlarmInfo": {"Channel": 0, "Event": "VideoMotion",
"StartTime": "...", "Status": "Start"}}` and later `"Stop"`; asked again on
every connection. An NVR's or DVR's other channels' alarms are left out
(`Channel` against the URL's `ch`).

| NETIP `Event` | Event |
|---|---|
| `VideoMotion` | `motion` |
| `HumanDetect`, `appEventHumanDetectAlarm` (OpenIPC majestic) | `human` |
| `FaceDetect` | `face` |
| `CarShapeDetect` | `vehicle` |
| `VideoLoss` | `video_loss` |
| `VideoBlind` | `tamper` |
| `LocalAlarm` | `alarm_input` |
| `VideoAnalyze`, `NetAlarm`, `Storage…`, `NetAbort`, `NetIPConflict`, unknown | `other` (an unknown name is logged once) |

A camera that refuses AlarmSet is logged once and streams on. Some XM
firmwares accept it and push nothing: they send alarms only to the alarm
server of their own settings.

**ONVIF events.** A thread per camera finds the device's Events service
(`GetCapabilities`, else `GetServices`) at `cameras[].onvif_url`, else at
the address discovery heard from it, else `http://host/onvif/device_service`
on port 80, then 8899 (XM's), logged in with the user and password of the
camera's URL (WS-UsernameToken in the camera's clock; HTTP Digest when it
asks). It holds a pull-point subscription (`CreatePullPointSubscription`,
`PullMessages` with a 5 s timeout, `Renew` before its 120 s run out,
`Unsubscribe` at the end) and starts a new one after two failed calls or a
refused `Renew`. With `auto`, a camera without events is logged once and
tried again every 10 minutes; with `onvif`, sooner, each new failure
logged. A camera added over ONVIF gets its device service as `onvif_url`.

| ONVIF topic (any prefixes; vendors' variants by what the name contains) | Event |
|---|---|
| `VideoSource/MotionAlarm`, `RuleEngine/CellMotionDetector/Motion`, `…/MotionRegionDetector/Motion`, Axis VMD | `motion` |
| `RuleEngine/MyRuleDetector/PeopleDetect`, `…/HumanDetect`, any `People`/`Human`/`Person` | `human` |
| `…/FaceDetect` | `face` |
| `…/VehicleDetect`, `Car` | `vehicle` |
| `RuleEngine/LineDetector/Crossed`, `LineCrossing`, `Tripwire` | `line_crossing` |
| `RuleEngine/FieldDetector/ObjectsInside`, `Intrusion` | `intrusion` |
| `VideoSource/SignalLoss` | `video_loss` |
| `VideoSource/GlobalSceneChange/…`, `RuleEngine/TamperDetector/Tamper`, `VideoSource/ImageToo…` | `tamper` |
| `Device/Trigger/DigitalInput`, Axis `IO/Port` | `alarm_input` |
| `…/DogCatDetect`, `Visitor`, `Device/HardwareFailure/…`, an object detector of another class | `other` |
| `Monitoring/…`, `Device/Trigger/Relay`, configuration changes | not an event (logged once with `VIGIL_DEBUG` set) |

The first boolean Data item (`IsMotion`, `State`, `IsInside`, `IsTamper`,
`LogicalState`, …) says start or stop; without one (a line's `ObjectId`)
it is a moment. Each rule (its Source items) is followed on its own: two
field rules hold `intrusion` until both are over. All of a device's
notifications go to its channel; an ONVIF NVR's channels are not told
apart.

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
| `GET /api/v1/config/<section>` | One section (`cameras`, `record`, `schedule`, `quota`, `nas`, `ddns`, `upnp`, `smtp`, `alarm`, `time`, `ui`, `display`, `live`, `api`, `discovery`, `qos`, `rtsp_server`, `onvif_server`, `netip_server`). |
| `PUT /api/v1/config/<section>` | Replace one section; the others stay. |
| `GET /api/v1/schema` | Every setting: type, default, limits, label, help. |
| `GET /api/v1/ws` | WebSocket: pushed status, events and configuration changes. |
| `POST /api/v1/discovery/scan` | Search the LAN for cameras ([Discovery](#discovery)). |
| `GET /api/v1/discovery` | The devices found (so far, while a scan runs), and the current or last discovery job. |
| `POST /api/v1/discovery/stop` | End a running scan now, keeping what it found. |
| `DELETE /api/v1/discovery` | Forget the devices found. |
| `GET /api/v1/discovery/job` | The current or last discovery job. |
| `POST /api/v1/discovery/ip` | Change devices' addresses, or (`dry_run`) show the plan. |
| `POST /api/v1/channels/<n>/assign` | Add a device found to channel n. |
| `POST /api/v1/channels/<n>/events` | Events of channel n's camera, reported from outside ([below](#post-apiv1channelsnevents)). |
| `GET /api/v1/channels/<n>/ptz` | Channel n's PTZ: state, capabilities, presets ([PTZ](#ptz)). |
| `POST /api/v1/channels/<n>/ptz` | Move channel n's camera, stop it, a preset, home, a tour ([PTZ](#ptz)). |
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

`live` is what the screen's live view shows: `layout` (`1x1`, `2x2`, `1+7`,
`3x3`, `4x4`, `8x4`, `8x8`) and `page` (1 the first cameras, 2 the next
...). The on-screen UI saves it itself a few seconds after the user stops
changing it, and when vigild stops, so a restart comes back to it; a `PUT`
shows it at once (`curl -X PUT .../config/live -d '{"layout":"3x3","page":2}'`).
A layout larger than the board's channels shows the largest one that fits,
a page past the last the last one.

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
                "max_channels": 4, "capture_inputs": 0, "max_in_kbps": 64000 },
  "channels": [
    { "camera": 1, "url": "rtsp://admin:***@192.168.1.10/stream=0", "name": "Gate",
      "enabled": true, "host": "192.168.1.10", "running": true, "live": true,
      "state": "live", "status": "live", "title": "CAM01",
      "main": { "status": "live", "state": "" }, "sub": null,
      "events": ["motion"], "decoding": true, "view": "main", "no_decoder": false, "recording": true, "rec_state": "recording",
      "rec_bytes": 73400320,
      "codec": "H.265", "width": 1920, "height": 1080, "fps": 25,
      "main_kbps": 4096, "sub_kbps": 0 }
  ]
}
```

`uptime` is seconds since the API started, `time` the board's clock (Unix
time). One channel per camera slot, in tile order (an empty slot, a camera
whose `url` is `""`, is `status: "empty"`). `running`: the
channel has a source (it is enabled and its URL could be used); `live`:
video arrived in the last 3 s; `state` is `live`, `off`, `CONNECTING`,
`waiting for video` (connected, no frame yet; `status` is still
`connecting`), or the last error (`connect: Connection refused`, `login refused (Ret 203)`, …);
`status` says the same in one word: `empty` (no camera in this slot),
`disabled`, `connecting`, `live`, `no_video` (connected, no video for 3 s),
`offline` (refused, timed out, no route, lost), `auth` (the camera refused
the user or the password: RTSP 401 or 403, NETIP login 203, 205, 206),
`codec` (neither H.264 nor H.265) or `error` (anything else; `state` has
the text). `live`, `state` and `status` are those of the stream on screen
(`view`; the main stream when none is): a small tile showing the
sub-stream is `live` while the sub-stream plays, whatever the main stream
does. `main` and `sub` are each stream's own `status` and `state` (both
streams are connected all the time; `sub` is `null` when the camera has no
sub-stream; both are absent when the channel does not run): a main stream that fails while the sub-stream is shown is said
in `main` (and `rec_state` says nothing is recorded). `title` is what a
NETIP camera calls the channel (its ChannelTitle, asked at each login),
`""` when the camera does not say; with no `name` configured the UI shows
it. `events` are the events the camera reports now: `motion`, `human`,
`face`, `vehicle`, `line_crossing`, `intrusion`, `video_loss`, `tamper`,
`alarm_input`, `other` — from its NETIP alarms or ONVIF events
(`cameras[].events`, [Camera events](#camera-events)) or the `POST` below;
each is held while the camera says it goes on, and 5 s after a momentary
report;
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
until the stream says. `main_kbps` and `sub_kbps` are what each stream
brings in, kbit/s of video averaged over 2 s (0 while it does not run);
the platform's `max_in_kbps` is the chip's rated incoming bandwidth for
network cameras, every stream together (0: not known), which the Channels
page shows their sum against.

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
| `event` | As they happen | `{"seq": 42, "time": 1795700819, "camera": 2, "text": "online"}` — `camera` 0 is the system; `missed` counts events lost by a slow reader. A camera's event also has `event` (its name, as in the status), `state` (`start` or `stop`) and, when the camera names it, `detail`: `{"seq": 43, "time": 1795700820, "camera": 1, "text": "motion start", "event": "motion", "state": "start", "detail": "VideoMotion"}` |
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
| `GET /api/v1/disks/<name>/smart` | A whole disk's SMART, read now (a sleeping disk spins up first): `{"disk", "available", "why", "verdict": "good" \| "warning" \| "failing" \| "unknown", "reason", "transport": "SATA" \| "USB" \| …, "model", "serial", "firmware", "sectors", "sector_size", "physical_sector_size", "capacity", "rotation": rpm \| "ssd" \| null, "sata": {"max", "current"} \| null, "smart_supported", "smart_enabled", "health": "passed" \| "failing" \| null, "temperature", "power_on_hours", "self_test": {"supported", "running", "status", "remaining", "short_minutes", "extended_minutes"}, "attributes": [{"id", "name", "prefailure", "value", "worst", "threshold", "raw", "raw_text", "level": "good" \| "warning" \| "failing", "critical", "note"}], "self_test_log": [{"type", "status", "failed", "hours", "lba"}]}`. A drive that answers no ATA command (a USB stick, a bridge without SAT) is `"available": false` with `why`. A partition is `400`. |
| `POST /api/v1/disks/<name>/selftest` | `{"type": "short" \| "extended" \| "abort"}` → `{"ok": true}`; the drive runs it in the background (`self_test.running`, `remaining`). |
| `GET /api/v1/storage/scan` | The surface scan, running or the last one: `{"state": "idle" \| "running" \| "paused" \| "done" \| "stopped" \| "failed", "disk", "model", "serial", "mode", "read_instead", "start", "end", "block", "sector_size", "pass", "passes", "lba", "blocks_done", "blocks_total", "progress", "mbps", "elapsed", "eta", "started", "finished", "message", "counts": {"lt5", "lt20", "lt50", "lt150", "lt500", "ge500", "error"}, "bad": [{"lba", "count"}], "bad_more", "map"}`. `map` is a digit a cell (up to 2000 cells over the range): `0` not yet, `1`…`7` the classes in `counts`' order, the worst block of the cell. Only `state` while idle. |
| `POST /api/v1/storage/scan` | `{"disk", "mode": "read" \| "verify" \| "write", "start", "end", "block", "confirm"}` → `{"ok": true}`. LBAs in the disk's logical sectors, `end` exclusive, 0 or absent: the end of the disk; `block` in sectors, default 256 (at most 2 MiB for Read and Write). `write` erases the disk: `confirm` must be the disk's name, no partition of it mounted, the recording directory not on it, not the system's disk (`400` naming the field). One scan at a time: `409` while one runs, and a format of its disk (or a mount, during Write) is `409` too. |
| `POST /api/v1/storage/scan/pause`, `/resume`, `/stop` | `{"ok": true}`; `409` when no scan runs. A stop ends after the block under way. |
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

Each answer goes into the device list the moment it is parsed, merged by
MAC (else address) with what other protocols said, so `GET
/api/v1/discovery` shows the scan's results while it runs: poll it (every
few hundred milliseconds) while `running` is true. Its `generation` moves
whenever the list changes (a device heard for the first time or telling
something new, a scan starting, one dropped); `job.found` counts the
devices that have answered this scan, `answered` marks them.

### `POST /api/v1/discovery/stop`

Ends the running scan within a fraction of a second and answers its final
state (`200`; `message` "N devices, stopped"); what it found stays. `409`
if no scan runs: other jobs are not stopped (an address change half done
is worse than one not started).

### `GET /api/v1/discovery`

```json
{ "devices": [
    { "device": "00:12:31:4f:2a:10", "ip": "192.168.1.10", "mac": "00:12:31:4f:2a:10",
      "mask": "255.255.255.0", "gateway": "192.168.1.1", "dhcp": null,
      "protocols": ["netip", "onvif"], "vendor": "Xiongmai", "model": "IPC", "name": "LocalHost",
      "serial": "abcdef0123456789", "version": "V4.02.R11...", "channels": 1,
      "ports": { "netip": 34567, "http": 80 }, "onvif": "http://192.168.1.10:8899/onvif/device_service",
      "interface": "eth0", "age": 4, "answered": true, "channel": null,
      "urls": { "netip": "netip://192.168.1.10:34567/?ch=0&stream=main" } } ],
  "count": 1, "generation": 12, "running": true,
  "job": { "id": 9, "kind": "scan", "running": true, "progress": 40, "found": 1, ... } }
```

`device` is the key the other requests take (the MAC, else the address).
`channel` is the channel whose camera URL points at this address, or
`null`. `urls` are what adding it would use, without credentials (ONVIF's
come from the device when it is added). `age`: seconds since it last
answered; `answered`: it answered the last scan (the running one, while one
runs). `count` is the number of devices listed, `running` the job's.

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
above) and `name` are optional. Without a `name`, a camera added over NETIP
(here, by hand below, or by `autoadd`) is named as it names the channel
itself (it is asked for its ChannelTitle; `""` when it does not say, and
the channel then shows the title it gives when it connects); any other,
the device's name. A channel with a camera is refused unless `replace`; a
device already on a channel is refused.

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

### `POST /api/v1/channels/<n>/events`

`{"events": ["motion", "human"]}` reports events of channel n's camera as
if the camera had (any of the status's `events` names): for an alarm
input vigild does not read itself (a script relaying a camera's or a
sensor's alarm), and for trying the tiles' alarm icons and frame
(`display.alarm_icons`, `display.alert_frame`). Without a `state` each is
held 5 s after its last report, so a source repeats it while it lasts;
`"state": "start"` holds it until the same events come with
`"state": "stop"` (a camera's own NETIP or ONVIF report does not end the
API's, nor the other way round). The status's `events`, the WebSocket's
`event` messages and the live view show it meanwhile. The answer is the
events held now, `{"channel": 1, "events": ["motion", "human"]}`. `400`
for an unknown event or state, `404` past the channels. Needs `cameras`.

```sh
while sleep 2; do
  curl -s -u root:PASSWORD -d '{"events": ["motion"]}' $NVR/api/v1/channels/1/events
done
```

## PTZ

A camera that moves (pan, tilt, zoom; its lens's focus and iris;
presets, tours, a home position) is driven over its own protocol, per
camera by `cameras[].ptz`: `auto` (the default) uses NETIP's
`OPPTZControl` for a `netip://` camera and ONVIF's PTZ service (with
Imaging for focus and iris) for the others when the camera has one;
`netip` or `onvif` forces one (`netip` on an RTSP camera: NETIP on port
34567 of its host); `off`, none. The camera is asked what it can do the
first time anyone looks (the screen's PTZ panel, a `GET` here). Commands
go out from a thread of the channel's own, so a request only queues them
and is answered at once. Both need the `ptz` permission. The camera's
password is the one in its URL; nothing here shows it.

### `GET /api/v1/channels/<n>/ptz`

```json
{ "channel": 1, "state": "ready", "backend": "netip",
  "capabilities": ["pan_tilt", "zoom", "focus", "iris", "presets", "tours"],
  "presets": [ { "id": "1", "name": "Gate" }, { "id": "5", "name": "Road" } ],
  "tours": 8, "moving": false, "error": "" }
```

`state`: `off` (no camera, disabled, `ptz: off`, or a `capture:` input),
`probing` (being asked), `ready`, `none` (the camera has no PTZ: a NETIP
camera answering `Ret 118` or `607`, an ONVIF one without a PTZ service),
`error` (it could not be reached, or refused the login; `error` says why,
and it is asked again a few seconds later). `capabilities`: what of
`pan_tilt`, `zoom`, `focus`, `focus_auto`, `iris`, `presets`, `tours`,
`home` it can do. `error` is also the last command's failure. `?wait=MS`
(up to 10000) waits that long for a camera being asked.

### `POST /api/v1/channels/<n>/ptz`

One of:

| Body | Does |
|---|---|
| `{"move": {"pan": -1..1, "tilt": -1..1, "zoom": -1..1}}` | A continuous move: pan + right, tilt + up, zoom + in; the size is the speed (NETIP: Step 1..8, one direction of eight, then zoom, at a time). |
| `{"stop": true}` | Stop whatever moves. |
| `{"focus": -1..1}`, `{"focus": "auto"}` | Focus + far, - near; or the camera's own focus. |
| `{"iris": -1..1}` | Iris + open, - close. |
| `{"preset": {"goto": ID}}` | Go to a preset. |
| `{"preset": {"set": ID or null, "name": "Gate"}}` | Store the position now as preset ID (null: a new one; NETIP: the lowest free number 1..255). |
| `{"preset": {"rename": ID, "name": "Lane"}}` | A new name (kept by vigild: the camera's position is not touched). |
| `{"preset": {"remove": ID}}` | Delete a preset. |
| `{"home": true}` | The home position. |
| `{"tour": {"start": N}}`, `{"tour": {"stop": N}}` | A tour (patrol) the camera has, 1..`tours`. |

**A move ends on its own**: it lasts `timeout` milliseconds (100..10000,
in `move` or beside it; default 1000) and is stopped then, unless the same
or another move or a stop comes first. A client holding a button repeats
the move (every few hundred milliseconds) and sends a stop on release; a
client that goes away leaves nothing moving. A stop the camera does not
take is sent three times, then the control connection is dropped (majestic
ends a move when its client hangs up); `vigild` stopping, or the camera's
URL changing, stops a move first.

The answer is `202` with the state as `GET` gives it (the command is queued;
a failure shows in `error` afterwards, and on the screen). `400` a bad
body (an unknown member, a value out of range, two of `move`, `stop`,
`focus`, `iris` at once, two preset operations), `404` no such channel,
`409` no PTZ on the channel or not something it can do (`home` and
`focus: "auto"` on NETIP), `503` too many commands waiting.

```sh
# pan left at half speed for 2 s, then the preset named Gate
curl -s -u root:PASSWORD -d '{"move": {"pan": -0.5}, "timeout": 2000}' $NVR/api/v1/channels/1/ptz
curl -s -u root:PASSWORD -d '{"preset": {"goto": 1}}' $NVR/api/v1/channels/1/ptz
```

**Presets** are ids: a NETIP camera's are numbers (`"1"` .. `"255"`), an
ONVIF camera's its own tokens (often `"1"`, `"Preset_1"`). XM cameras keep
no names and list nothing, so vigild keeps the list of what was stored
through it, with the names, in `vigil.ptz.json` beside the configuration
file (`/etc/vigil.ptz.json`), per camera (address, port and channel of an
NVR), with no password; a rename is kept there too for an ONVIF camera,
over the name the camera gives.

**NETIP** (XM cameras and NVRs, majestic with `netip`): message 1400 on a
connection of its own, logged in with the URL's user and password,
`{"Command": "DirectionLeft", "Parameter": {"Channel": 0, "Preset":
65535, "Step": 5, "Tour": 0, ...}}` to start and the same with `"Preset":
-1` to stop; `ZoomTile`/`ZoomWide`, `FocusNear`/`FocusFar`,
`IrisLarge`/`IrisSmall`, `SetPreset`/`GotoPreset`/`ClearPreset`,
`StartTour`/`StopTour`. There is no home position or automatic focus.

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
  `src/api/web.c`: the zero channel's web server, a second CivetWeb
  context with its pages built in (`src/api/web/`, embedded by
  `cmake/embed.cmake`); `src/app/kvm.c`, `kvm_enc.c`, `kvm_input.c`,
  `jpeg_enc.c`: its pictures, their changed parts as JPEGs (an encoder of
  its own: libjpeg is not on the firmware) and its input.
  `src/api/rtsp_server.c`: the [RTSP server](rtsp.md), its own thread,
  fed by `src/core/stream_tap.c`.
  `src/api/onvif.c`: the [ONVIF server](onvif.md#inside), a third
  CivetWeb context and the WS-Discovery responder.
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
