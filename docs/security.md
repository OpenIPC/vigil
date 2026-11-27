# Credentials

Every password vigil keeps is in one of three files, and every one that
vigil has to give back to someone, or work from later, is **sealed**:
encrypted with AES-256-GCM under one key that lives in a file of its own.

| What | File | Kept as |
|---|---|---|
| The cameras' passwords (in `cameras[].url` and `sub_url`) | `/etc/vigil.yaml` | sealed, inside the URL: `rtsp://admin:enc:v1:…@10.0.0.2/stream` |
| `smtp.password`, `ddns.password`, `nas.password` | `/etc/vigil.yaml` | sealed: `password: "enc:v1:…"` |
| vigil's users' passwords | `/etc/vigil.users.yaml` (`api.users`) | a `crypt(3)` hash (`password`) **and** sealed (`password_enc`) |
| The system account's (`root`) password | `/etc/shadow` (`api.shadow`), and `/etc/vigil.users.yaml` | a `crypt(3)` hash in the shadow file; sealed in the users file's `system` section |
| The key | `/etc/vigil.key` (beside the configuration file: `dirname(-f)/vigil.key`) | 64 hex digits, mode 0600 |

All three vigil files are mode 0600, written atomically (a new file, synced,
renamed over the old one). The Wi-Fi passphrase and the LTE settings are not
in vigil's files: they are written where the firmware keeps them
([management.md](management.md)), as the firmware's own tools would.

## The key

`vigil.key` is 32 random bytes from `/dev/urandom`, written the first time
vigild starts (or seals a password before that: `vigild -w`),
mode 0600, never replaced. One found readable by others (copied back
with `cp`, restored from an archive) is made 0600 again when vigild owns
it, and logged otherwise. A key file that is there but unusable (not 64
hex digits) is left alone and logged; nothing new can be sealed until it is
fixed or removed.

A sealed value is `enc:v1:` and the base64 of a fresh 12-byte nonce, the
ciphertext and GCM's 16-byte tag. The field it belongs to is the cipher's
additional data — `config:cameras[0].url`, `config:smtp.password`,
`users:anna.password`, `users:system:root.password` — so a value copied
into another field, another user or another camera's place does not open
there. The cipher is mbedTLS's (`libmbedcrypto`, already on OpenIPC
images); `src/core/secret.c`.

## What it protects, and what it does not

**Protected:** a copy of the configuration or the users file on its own.
Backups, configuration exports, files attached to a support request or
pasted into a forum, a file left on a USB stick, the `.bad` copy of an
invalid file: without `vigil.key` none of them gives a password away.
**Keep the key out of backups** — copy `vigil.yaml` and
`vigil.users.yaml`, not `vigil.key` — or the backup carries everything.
The HTTP API never returns a password, a hash or a sealed value
(`"***"` for a set password, `"password_set": true` for users); passwords
never appear in vigild's log.

**Not protected:** anyone with root on the board, or a dump of its flash.
The SoCs vigil runs on have no secure key storage, so the key sits on the
same filesystem as what it seals; whoever can read `/etc/vigil.key` can
open every sealed value, and whoever is root can also read vigild's memory,
where the passwords are in the clear while it runs (the cameras need
them). The sealing keeps passwords out of the copies, not away from the
board's owner.

Two weaker spots remain by design: an XM client logs in with an
8-character hash of the password that is as good as the password on this
board's NETIP server, and HTTP Basic (the API, ONVIF's `PasswordText`) and
the zero channel's login form send the password itself; all of them cross
the network unencrypted (vigild's web servers have no TLS yet). Use them on
a trusted network.

## One password, every protocol

The clients that never send the password each prove they know it in their
own way, and vigil works out the same proof from the sealed password when
they log in:

| Server | The client sends | vigil compares with |
|---|---|---|
| RTSP, ONVIF's HTTP Digest | Digest, `md5(md5(user:vigil:password):nonce:…)` | the same, from the opened password |
| ONVIF, WS-UsernameToken `PasswordDigest` | `base64(sha1(nonce + created + password))` | the same |
| NETIP (CMS, XMEye) | XM's 8-character hash of the password | `md5_sofia` of the opened password |
| The API, the web page, the screen, ONVIF `PasswordText` / Basic | the password | the `crypt(3)` hash (nothing opened) |

So a password set through the API or the on-screen settings works on every
server at once, and after any number of restarts.

**The system account.** `root`'s password is the board's console and SSH
login, in `/etc/shadow`. Set through vigil — `PUT /api/v1/users/root
{"password": "…"}` by `root` itself, or Account → Users → *System account
password* on the screen, which asks for the current one — vigild writes
the new SHA-512 hash into the shadow file (atomically, its mode and owner
kept) and seals the password into the users file's `system` section,
together with which shadow entry it belongs to (an MD5 of the hash there).
A password changed elsewhere (`passwd` over SSH) no longer matches that
entry: the sealed copy is not used, and RTSP, ONVIF and NETIP refuse
`root` (the log says why) until `root` logs in once with the new password
to the API, the web page or the screen, which seals it again.

## Migration

Nothing is to be done by hand:

- **vigil.yaml** from an older vigild, or with a password typed in by
  hand: the passwords in the clear are sealed as vigild loads the file,
  which it rewrites at once (logged: "N password(s) found in the clear,
  sealed").
- **Users with an ONVIF key** (`onvif:`, the password base64-encoded, from
  vigild versions that kept it while the ONVIF server was on): sealed into
  `password_enc` as the file is read; `onvif`, `digest` and
  `netip_password` are dropped from the file then.
- **Users with only the hashes** (`digest`, `netip_password`): those keep
  working for RTSP and NETIP; ONVIF's digests need the password itself.
  The user's next login with the password (API, web page, screen) seals
  it, and the old fields go.
- **The system account's old hashes** (`system.digest`, `admin_digest`,
  `netip_password`): the same, until `root`'s next login.

## Without the key

If `vigil.key` is lost (deleted, the configuration copied to another
board, a restore that left it out), vigild starts all the same and logs
which values it cannot open: those cameras connect without a password
(and fail to log in), those mail, DDNS and storage passwords are empty,
and those users cannot log in over RTSP, ONVIF or NETIP ("the stored
password of 'anna' cannot be read"). A new key is made for what is sealed
next. The values that did not open stay in the files as they were, sealed,
through any number of saves of other settings, so putting the old key back
(in place of the new one) brings them back; a password set again replaces
its old value. Set the cameras' and services' passwords again; users and `root`
need only log in once to the API, the web page or the screen (their
`crypt(3)` hashes are not sealed), or have their passwords set again.
Moving a recorder's whole configuration to a new board means moving
`vigil.key` with it.
