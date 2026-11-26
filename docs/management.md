# Management: network, storage, system, accounts

The main menu's MANAGEMENT row opens the recorder's settings: **Network**,
**Storage**, **System** and **Account** (CAMERA is the camera list,
[ui.md](ui.md)). Most of them change the operating system rather than
vigild: the interfaces, the clock, the host name, the disks. vigild does
that the way OpenIPC firmware does it, so the firmware's init scripts, its
web UI and a person at the shell keep agreeing with what vigild wrote.
The same actions are in the HTTP API ([api.md](api.md#management)).

## What does what

| Page | Item | State | How |
|---|---|---|---|
| Network | Ethernet | works | DHCP or static address, netmask, gateway, two DNS servers, MTU; link, speed, MAC and the addresses in use. A change is undone unless confirmed within 60 s. |
| | Wi-Fi | partial | With a wireless interface: the network to join (SSID, password) and the interface's state. Scanning needs `iw`, which the firmware lacks. Without an adapter the page says so. |
| | LTE | detection | A modem's network interface (RNDIS, ECM, NCM, QMI) and the firmware's modem profile; the firmware brings it up. |
| | DDNS | works | dyndns2 (Dyn and compatibles), No-IP, or a custom URL, every N minutes, through curl. |
| | UPnP | stored | The settings are kept; vigild has no UPnP client yet. |
| | SMTP | works | A test mail; trouble (disk full, recording failed, a camera offline) mailed, at most one mail per 10 minutes, through curl. |
| | Alarm center | works | Every event POSTed as JSON to `http://server:port/path`, and a heartbeat. |
| | P2P | none | vigil has no P2P service; the page says so. |
| Storage | Basic | works | When the disk is full: overwrite the oldest recordings, or stop (event `REC disk full`); segment length in minutes; the recording directory. |
| | Schedule | works | A week per camera in half hours: continuous, motion only, off. Motion only records while the camera reports motion, which no source does yet. |
| | Disk Manager | works | Disks and partitions with their state; format (exFAT or FAT32 here; ext4 where `mkfs.ext4` exists), mount, unmount. |
| | Disk Quota | works | Per camera: the most space (GB) and the most days its recordings may take. |
| | Network Storage | partial | SMB (CIFS) works on this firmware; NFS needs a kernel option the ssr621q kernel lacks (below). Test mounts, writes and reads a file. |
| System | General | works | Device name (host name), language (English for now), date and time, time zone with DST, NTP, date and time format. |
| Account | Users, Groups | works | vigild's own users in groups with permissions, enforced by the API ([api.md](api.md#accounts-and-permissions)). The on-screen UI does not ask for a login yet. |

## Where things are kept

Settings that are vigild's own live in `/etc/vigil.yaml` like every other
setting ([api.md](api.md#the-configuration-file)): `record.on_full`,
`schedule`, `quota`, `nas`, `ddns`, `upnp`, `smtp`, `alarm`, `time.ntp`,
`time.ntp_interval`, `ui.language`, `ui.date_format`, `ui.time_format`.
Users and groups are in `/etc/vigil.users.yaml` (mode 0600). What belongs to
the system is written where the firmware keeps it:

| What | File | Written as |
|---|---|---|
| An interface | `/etc/network/interfaces.d/<if>` | a busybox ifupdown stanza (below) |
| DNS of a static address | the stanza's `pre-up` | `echo -e "nameserver A\nnameserver B" >/tmp/resolv.conf`, as on the OpenIPC wiki; `/etc/resolv.conf` links to `/tmp/resolv.conf` |
| Wi-Fi network | U-Boot environment | `fw_setenv wlanssid` / `wlanpass`; the wlan0 stanza turns them into a wpa_supplicant configuration (`wlan_conf`) |
| Time zone | `/etc/TZ`, `/etc/timezone` | a POSIX TZ string (`CET-1CEST,M3.5.0,M10.5.0/3`) and the zone's name; `/etc/profile`, `rcS` and `S95vigil` export `TZ` from `/etc/TZ` |
| NTP servers | `/etc/ntp.conf` | `server HOST iburst`, read by the firmware's ntpd (`S49ntpd`) |
| Clock | the kernel, the RTC | `settimeofday`; `hwclock -w -u` when `/dev/rtc0` is there; `fake-hwclock save` |
| Host name | `/etc/hostname`, `/etc/hosts` | the name, the `127.0.1.1` line; `/etc/hostname.auto` so `S31hostname` keeps it |
| Disks | `/mnt/<dev>` | mounted where the firmware's automount (`/lib/mdev/automount.sh`) mounts them |
| Network storage | `/mnt/nas` | mounted by vigild while `nas.type` is set |

Every file is replaced atomically (a new file, `fsync`, `rename`). Before an
interface is changed, its old stanza is kept so the change can be undone.

### An interface's stanza

OpenIPC brings eth0 up at boot with `ifup eth0` (`S40network`) from

```
iface eth0 inet dhcp
    hwaddress ether $(get_mac eth0)
```

vigild rewrites only the method and the lines it manages (`address`,
`netmask`, `gateway`, the DNS `pre-up`, `pre-up ip link set dev eth0 mtu N`)
and keeps every other line: `hwaddress`, `metric`, the Wi-Fi `pre-up`s,
comments, other stanzas. Then `ifdown -f eth0; ifup eth0`. A static one:

```
iface eth0 inet static
    address 192.168.1.10
    netmask 255.255.255.0
    gateway 192.168.1.1
    pre-up echo -e "nameserver 1.1.1.1\nnameserver 8.8.8.8" >/tmp/resolv.conf
    hwaddress ether $(get_mac eth0)
```

**Undo.** A change can cut off whoever made it. Made through the API, it is
undone after 60 s unless `POST /api/v1/network/ethernet/confirm` comes
first; on the screen, a question with a countdown asks to keep it. Undoing
puts the last confirmed file back and restarts the interface. If `ifup`
fails outright, the old file is back at once.

## Disks

The Disk Manager lists `/sys/block` (not loop, RAM, zram or flash devices)
with each disk's partitions: model, size, filesystem (from `/proc/mounts`,
or read from the superblock), mount point, free space and a state:
`recording` (the recording directory is on it), `mounted`, `read-only`,
`not mounted`, `unformatted`, `no medium`, `system`.

A device that holds the system (it backs `/`, `/rom` or `/overlay`, is the
kernel's `root=`, or is mounted somewhere other than under `/mnt`) is never
formatted or unmounted, nor is a disk with such a partition.

**Format** asks for the disk's name to be typed. Recording onto the disk is
held first (the channels close their segments and open none until the job
ends), its partitions are unmounted, and then:

- a whole disk gets a new MBR with one partition from sector 2048 (type
  0x07 for exFAT, 0x0c for FAT32, 0x83 for ext4); over 2 TiB, where an MBR
  cannot reach, the filesystem goes on the whole disk, which the firmware's
  automount also mounts;
- the start of the partition is zeroed before `mkfs`, so the firmware's
  automount, which reacts to the new partition, finds nothing to mount;
- `mkfs.exfat` (exfatprogs), `mkfs.vfat -F 32` (busybox) or `mkfs.ext4`,
  whichever the firmware has, labelled VIGIL;
- the result is mounted at `/mnt/<partition>`.

**Unmount** holds recording off the disk and unmounts it; recording then
waits for a disk as it does when one is pulled. **Mount** mounts at
`/mnt/<dev>`, loading the filesystem's module when the kernel asks for it.

## Recording policies

- `record.on_full`: `overwrite` (the default, what vigild always did): below
  the free-space reserve the oldest recording in the directory goes,
  whichever camera made it. `stop`: nothing is deleted; the segment is
  closed and recording stops with the event `REC disk full` and the state
  `disk full`, until there is room again.
- `quota`: per camera, the most gigabytes and the most days. Checked when a
  segment opens, at most once a minute: the camera's oldest files go beyond
  the size, and files older than the days. Other cameras' files are never
  touched.
- `schedule`: per camera, 7 strings of 48 letters, one per half hour from
  00:00 local time, Monday first: `C` continuous, `M` motion only, `-` off.
  Off, the channel closes its segment and keeps its stream; recording
  resumes at the next keyframe in an on slot. A schedule change restarts
  nothing.
- `record.segment` stays in seconds in the file and the API, so existing
  files, `-S` and clients keep working; the UI shows minutes.

## Time

The time zone is one of a list of named zones (tzdata's strings, each one
checked against what the C library makes of it), or one of one's own: an
offset, and optionally daylight saving time with its offset and the
month, week, weekday and time it starts and ends. That becomes a POSIX
string with numeric names, `<+01>-1<+02>,M3.5.0,M10.5.0/3`. vigild uses a
new zone at once; other programs when they start again.

NTP: the servers go to `/etc/ntp.conf` and the firmware's ntpd is restarted
to read them; turning NTP off stops it, at once and whenever vigild
starts. vigild also asks the servers itself (`ntpd -n -q`) every
`time.ntp_interval` minutes and on "Sync now", which shows the last
synchronisation and sets the clock if the daemon is not running. A clock
set by hand is written to the RTC when there is one, and to fake-hwclock.

## Accounts

See [api.md](api.md#accounts-and-permissions): users in groups, each group a
set of permissions (live view, playback, PTZ, cameras, storage, network,
system, users), the built-in groups `admin` and `user`, and the system
account (root) that always logs in as an administrator, so nobody is
locked out.

## On systems vigild does not manage

All of this runs only on OpenIPC firmware (`/etc/os-release` names an
`OPENIPC_VERSION`). On a desktop or a Debian board the pages show what
there is and change nothing; the API answers 409.

## What the firmware needs

Found on the ssr621q image (`ssr621q_lite`) and its kernel:

| Feature | Needs | On ssr621q |
|---|---|---|
| Ethernet | busybox `ifup`/`ifdown`, `udhcpc`, `ip` | there |
| Wi-Fi scan | `iw` (and a Wi-Fi driver: the kernel has cfg80211 but no mac80211 or driver) | missing: `BR2_PACKAGE_IW` and a driver package |
| Wi-Fi join | `wpa_supplicant`, `wpa_passphrase` (used by `wlan_conf`) | missing: `BR2_PACKAGE_WPA_SUPPLICANT` |
| LTE | a modem driver (`rndis_host`, `cdc_ether`, `qmi_wwan`) | `CONFIG_USB_NET_DRIVERS` is off |
| Mail, DDNS, alarm center | `curl` with TLS and SMTP | there (libcurl-openipc, mbedTLS) |
| Format exFAT | `mkfs.exfat` | there (exfatprogs) |
| Format FAT32 | busybox `mkfs.vfat` | there |
| Format ext4 | `mkfs.ext4` (e2fsprogs, or busybox `CONFIG_MKE2FS`) | missing; the kernel has ext4 as a module |
| SMB storage | the `cifs` module | there (`CONFIG_CIFS=m`) |
| NFS storage | NFS v3 in the kernel | missing: `CONFIG_NFS_V3` (only `CONFIG_NFS_V2=m` is set) |
| RTC | `hwclock` and `/dev/rtc0` | `hwclock` there; `CONFIG_MS_RTC=y` |
| NTP | busybox `ntpd` | there |
