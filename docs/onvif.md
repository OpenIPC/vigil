# ONVIF server

`vigild` is an ONVIF device: video management software (VMS), another
recorder or a phone app finds the recorder on the network and plays each
of its cameras through it, as it would an ONVIF camera. It covers the part
of Profile S a client needs for that: the device service, media (ver10)
and media2 (ver20) with a profile per camera and stream, and
WS-Discovery. Events, PTZ, imaging, recordings (Profile G) and changing
settings over ONVIF are not there; settings are changed through
[the API](api.md) and the on-screen UI. (As a *client*, vigild does take
its cameras' ONVIF events: [Camera events](api.md#camera-events).)

```yaml
onvif_server:
  enabled: true        # default
  port: 8899           # default
```

Both apply at once: turning it off says goodbye on WS-Discovery (Bye) and
closes the port; a new port is announced (Hello) when it opens.

## The port

The web service is on its own port, `onvif_server.port` (8899, as on
Xiongmai devices), at `http://<recorder>:8899/onvif/device_service`.
Port 80 belongs to the [zero channel](api.md#zero-channel), which runs
only on boards with a screen and has its own logins (session cookies, the
`remote` permission), headers and threads; ONVIF on its own small server
works the same on every board, headless ones too, and can be moved or
turned off without touching the console. Clients rarely need to know the
port: WS-Discovery and `GetCapabilities` / `GetServices` give the full
addresses. When adding the recorder by hand, give the client port 8899.

The video itself comes from the recorder's [RTSP server](rtsp.md):
`GetStreamUri` answers `rtsp://<recorder>:<rtsp_server.port>/ch<N>/main`
and `/ch<N>/sub` (port 554 by default), N from 1, with the host the client
used to reach the ONVIF service. With the RTSP server off
(`rtsp_server.enabled`), `GetStreamUri` answers a fault
(`ter:ActionNotSupported`) rather than an address nothing answers at.

Snapshots come from the [API](api.md#snapshots): `GetSnapshotUri` answers
`http://<recorder>:<api.listen's port>/api/v1/channels/<N>/snapshot.jpg`
(the camera's own JPEG when it gives one, else the recorder's picture of
a channel on the screen), for either stream's profile, with the host the
client used. The client fetches it with the same user, by HTTP Digest
(what ONVIF clients send) or Basic: the API takes both. While the API is
off (`api.listen` empty) the capabilities say `SnapshotUri="false"` and
`GetSnapshotUri` answers `ter:ActionNotSupported`. With `api.listen` on
one address only (`127.0.0.1:8080`), the URI names the host the ONVIF
client reached, where the API does not listen: keep the API on all
addresses for ONVIF clients.

## Logging in

ONVIF clients log in as [vigil's users](api.md#accounts-and-permissions),
with one of:

| Method | Checked against |
|---|---|
| WS-UsernameToken, `PasswordDigest` (what most clients send) | the user's sealed password |
| HTTP Digest (MD5, `qop=auth`; realm `vigil`) | the user's sealed password |
| WS-UsernameToken, `PasswordText`; HTTP Basic | the password hash, like the API |

A digest proves that the client knows the password without sending it:
the server computes the same digest from the password itself, which a
`crypt(3)` hash cannot give. vigil keeps every password it sets sealed
beside its hash ([security.md](security.md)), whether the ONVIF server
is on or not, and works the digest out from it at each login. So:

- A user's password works for ONVIF as soon as it is set (API, on-screen
  settings), and so does the system account's (`api.user`, `root`, and
  `admin` as its other name) when set through vigil. A system account
  password changed elsewhere (`passwd`) is not known to vigil until the
  account logs in once with it to the API, the web page or the screen;
  until then its digest logins are refused (the log says why).
- A user from an older users file that had only hashes of its password
  (no `onvif` key) logs in with digests after its next login with the
  password elsewhere; one with an `onvif` key is converted when the file
  is read ([security.md](security.md#migration)).
- Give a VMS a user of its own all the same (group `user` is enough to
  watch): the system account is also the board's SSH login.

A `PasswordDigest` token is refused when its nonce was seen before (the
last 64 are kept) or its `Created` is more than 15 minutes from the
recorder's clock; with a clock that was never set (before 2020: no RTC,
no NTP yet) only the nonce is checked, so a client is not locked out by
the recorder's wrong time. Clients that follow the ONVIF guide ask
`GetSystemDateAndTime` first and correct for the difference. HTTP Digest
nonces are good for 5 minutes (then `stale=true`); they carry their own
time and a keyed check, so the server keeps no table of the nonces it
gave. It keeps each nonce's last count (`nc`, per `cnonce`; the last 128
nonces used): a request whose count is not above it, a replay of a whole
request, is answered `stale=true` and does nothing. Without `qop` a
nonce is good once.

Failed logins count with the API's and the zero channel's: each is
answered a second late, and an address with 5 failures in a row is locked
out for a doubling time ([Authentication](api.md#authentication)).

### Who may do what

| Operations | Needs |
|---|---|
| `GetSystemDateAndTime`, `GetCapabilities`, `GetServices`, `GetServiceCapabilities` (each service), `GetWsdlUrl`, `GetHostname` | nothing: ONVIF's default access policy lets these be asked before logging in (Core Specification, *Default Access Policy*, `PRE_AUTH`), so a client can find the services and correct its clock first |
| `GetDeviceInformation`, `GetScopes`, `GetDiscoveryMode`, `GetNetworkInterfaces`, `GetNetworkProtocols`; every media and media2 operation | `live` |
| `GetUsers` | `users` |
| `SystemReboot` | `system`, as `POST /api/v1/system/reboot` (the `admin` group has every permission) |

Without credentials an operation that needs a login is answered `401`
with `WWW-Authenticate: Digest ...` and an `ter:NotAuthorized` fault; with
a wrong token, `400` and the same fault; a user whose group lacks the
permission gets `400 ter:NotAuthorized` naming it.

## Operations

Device (`http://www.onvif.org/ver10/device/wsdl`, `/onvif/device_service`):

- `GetDeviceInformation`: manufacturer `OpenIPC`; model the chip from
  `/etc/os-release` (`BUILD_PLATFORM`, e.g. `SSR621Q NVR`; else the
  platform's name); firmware `vigild <version>, OpenIPC <version>`;
  serial number the MAC (eth0, else the first interface); hardware id the
  chip.
- `GetSystemDateAndTime` (NTP or manual from `time.ntp`, the POSIX TZ
  from `/etc/TZ`), `GetCapabilities` (device and media), `GetServices`
  (device, media, media2, with their capabilities when asked),
  `GetServiceCapabilities`, `GetWsdlUrl`, `GetHostname`, `GetScopes`,
  `GetDiscoveryMode` (always discoverable), `GetNetworkInterfaces` (each
  IPv4 interface, DHCP or manual from the network settings),
  `GetNetworkProtocols` (HTTP at the ONVIF port, RTSP at `rtsp_server.port`,
  enabled as `rtsp_server.enabled`).
- `GetUsers`: vigil's users and the system account, read-only (Administrator:
  a group with `system` and `users`; Operator: one with `cameras`,
  `storage`, `network` or `system`; User: the rest).
- `SystemReboot`: as the API's reboot, after a clean stop (recordings
  closed). Refused on a system `vigild` does not manage (a desktop).

Media (`ver10`, `/onvif/media_service`): `GetProfiles`, `GetProfile`,
`GetStreamUri`, `GetSnapshotUri`, `GetVideoSources`, `GetVideoSourceConfigurations`,
`GetVideoSourceConfiguration`, `GetVideoEncoderConfigurations`,
`GetVideoEncoderConfiguration`, `GetServiceCapabilities`.

Media2 (`ver20`, `/onvif/media2_service`): `GetProfiles` (with the
configurations asked for by `Type`), `GetStreamUri`, `GetSnapshotUri`,
`GetVideoSourceConfigurations`, `GetVideoEncoderConfigurations`,
`GetServiceCapabilities`.

An operation is recognised by its namespace, whichever service address it
is posted to (by the address when it declares none). Anything else is
`ter:ActionNotSupported`.

### Profiles

Every camera that is set up and enabled has a video source (`vs<N>`, its
configuration `src<N>`) and a profile per stream: `ch<N>_main` and, when
the camera has a sub-stream, `ch<N>_sub` (encoder configurations
`enc<N>_main`, `enc<N>_sub`), N the channel from 1. All are fixed: they
are the cameras' own streams, passed through without re-encoding, so
their encoder settings cannot be changed here.

Codec, size and frame rate are what the stream last showed (its
parameter sets), whether or not it is on screen; until a camera has
connected once they are a guess (H.264, 1920x1080 main, 640x360 sub, 25
fps), and the stream's own description over RTSP is right regardless.
Bit rate and quality are not known and are reported as 0 and 5.

Media ver10 has no H.265: its encoding is one of JPEG, MPEG4, H264, and a
client that checks the schema fails on any other value. An H.265 stream
is therefore listed as `H264` in media ver10, as other devices do, and as
`H265` in media2, which clients that play H.265 use.

Not there: multicast streaming, audio, OSD, changing a configuration.

## WS-Discovery

`vigild` answers WS-Discovery probes on UDP 3702 (multicast group
239.255.255.250, joined on every IPv4 interface that can take multicast;
also unicast) as a target service of types `dn:NetworkVideoTransmitter`
and `tds:Device`. A probe matches when each type it asks for is one of
those (a prefix it did not declare is taken by its local name) and each
scope is one of ours by the probe's rule: `rfc3986` (the default:
scheme and host in any case, then whole path segments) or `strcmp0`.
The answer, sent back to the asker, gives the device service at the
address the probe came in on. At most 20 probes a second are answered.

Its scopes: `onvif://www.onvif.org/type/NetworkVideoTransmitter`,
`.../type/video_encoder`, `.../Profile/Streaming`, `.../name/<host
name>`, `.../hardware/<model>`, `.../manufacturer/OpenIPC`,
`.../MAC/<mac>`. The endpoint reference is a UUID made from the MAC, the
same across restarts.

A `Hello` goes out on each interface when the service starts and when the
interfaces' addresses change, a `Bye` when it stops or is turned off.

The recorder's own camera search (`POST /api/v1/discovery/scan`) sends
ONVIF probes too, and hears this answer: it leaves out the device with
the recorder's own endpoint reference, so the recorder never offers itself
as a camera.

## Tested with

- `onvif-zeep-async` (Home Assistant's ONVIF library): `update_xaddrs`,
  device information, capabilities, services, network interfaces, scopes,
  media profiles, stream addresses, with WS-UsernameToken digests.
- `zeep` with ONVIF's media2 WSDL and strict schema checks: profiles with
  their configurations (H.265 as `H265`), stream addresses.
- `WSDiscovery` (python-ws-discovery): probes by type, by scope, a scope
  that does not match.
- `tests/onvif_test.py` (`ctest -R onvif`): discovery, the operations
  allowed before a login, both media services, HTTP Digest, replays and
  wrong passwords, permissions, the system account.
- `onvif-zeep` (`ONVIFCamera`, media `GetServiceCapabilities`,
  `GetProfiles`, `GetSnapshotUri`) and `requests`' `HTTPDigestAuth` on the
  URIs, against the SSR621Q in emulation; `tests/snapshot_test.py`
  (`ctest -R snapshot_e2e`): `GetSnapshotUri` on both media services and
  the URI fetched with Digest.

Not yet tried with ONVIF Device Manager, Milestone XProtect, Blue Iris,
Hikvision iVMS or Synology Surveillance Station.

## Inside

- `src/core/onvif_proto.{h,c}`: the wire formats as pure functions over
  untrusted bytes: the SOAP request's operation (the Body's first
  element, its namespace looked up in the xmlns declarations) and its
  UsernameToken; the UsernameToken digest; HTTP Digest (the header, the
  expected response, self-checking nonces); WS-Discovery probe parsing,
  type and scope matching, and the ProbeMatches, Hello and Bye messages.
  The XML is `disc_proto.h`'s tolerant scanner, the one camera discovery
  uses: elements by local name, five entities, no DTD. Tested in
  `tests/test_onvif.c`, each request also cut at every length.
- `src/api/onvif.c`: a supervisor thread that follows `onvif_server`,
  starts and stops a CivetWeb server (4 workers) for `/onvif/…`, and owns
  the WS-Discovery socket; the operations as templates written into one
  growing buffer per answer. Its workers only read snapshots
  (`config_get`, `channels_status`), like the API's.
- The sealed passwords: `src/api/users.c` (`users_login_with`), the
  failure counting: `api_login_with` in `src/api/api.c`.
- For tests, `VIGIL_ONVIF_WSD_PORT` moves WS-Discovery off 3702, so test
  runs side by side do not answer each other's probes.
