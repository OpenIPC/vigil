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
| | Wi-Fi | works | A USB adapter is recognised by its USB id even before its driver is loaded, with the firmware profile that loads it; joining sets `wlandev`, `wlanssid` and `wlanpass`, loads the driver the firmware's way and brings wlan0 up. Scan with `iw`. Without an adapter the page says so. |
| | LTE | detection | USB modems by their USB id (and the profile of `/etc/wireless/modem` that names them, or by their driver), "Use this profile" sets `wlandev` for the next boot; a modem's network interface and its state once the firmware has brought it up. |
| | Ports | works | The web server's (`web.listen`, 80), the API's (`api.listen`, 8080), ONVIF's (`onvif_server.port`, 8899) and NETIP's (`netip_server.port`, 34567) ports, moved at once when saved, each with what it listens on now (or why not); the RTSP port (554) kept for a server vigild does not have yet, said so on the page. No two on one port; a port that is taken is refused with the reason. |
| | DDNS | works | dyndns2 (Dyn and compatibles), No-IP, or a custom URL, every N minutes, through curl. |
| | UPnP | stored | The settings are kept; vigild has no UPnP client yet. |
| | SMTP | works | A test mail; trouble (disk full, recording failed, a camera offline) mailed, at most one mail per 10 minutes, through curl. A camera event's snapshots mailed as attachments when `snapshot.mail` is on ([Snapshots](#snapshots)); no page sets that yet. |
| | Alarm center | works | Every event POSTed as JSON to `http://server:port/path`, and a heartbeat. |
| | QoS | works | DSCP marking of vigild's own packets (`qos`): off, standard (RFC 4594: video AF41, management CS2, cameras AF41, alarms AF21) or custom values 0..63 per class; applied at once. Other programs (sshd, ntpd) are not marked: the firmware's kernel has no netfilter, so no iptables `DSCP` target. |
| | P2P | none | vigil has no P2P service; the page says so. |
| Storage | Basic | works | When the disk is full: overwrite the oldest recordings, or stop (event `REC disk full`); segment length in minutes; the recording directory. |
| | Schedule | works | A week per camera in half hours: continuous, motion only, off. Motion only records while the camera reports motion, which no source does yet. |
| | Disk Manager | works | Disks and partitions with their state; format (ext4 by default where `mkfs.ext4` exists, else exFAT; FAT32), mount, unmount. |
| | Disk Maintenance | works | SMART of a SATA disk or a USB enclosure that passes ATA commands (verdict, identity, attributes with the important ones highlighted, self-tests and their log); a surface scan (Read, Verify, Write) with a sector map. |
| | Disk Quota | works | Per camera: the most space (GB) and the most days its recordings may take. |
| | Network Storage | works | SMB (CIFS, SMB2/3; SMB1 only where the kernel has nothing else) and NFS v3, mounted at `/mnt/nas`; Test mounts, writes and reads a file. |
| System | General | works | Device name (host name), language (English for now), date and time, time zone with DST, NTP, date and time format. |
| Account | Users, Groups | works | vigild's own users in groups with permissions, enforced by the API ([api.md](api.md#accounts-and-permissions)). The on-screen UI does not ask for a login yet. |

## Where things are kept

Settings that are vigild's own live in `/etc/vigil.yaml` like every other
setting ([api.md](api.md#the-configuration-file)): `record.on_full`,
`snapshot`, `schedule`, `quota`, `nas`, `ddns`, `upnp`, `smtp`, `alarm`, `time.ntp`,
`time.ntp_interval`, `ui.language`, `ui.date_format`, `ui.time_format`.
Users and groups are in `/etc/vigil.users.yaml` (mode 0600). Passwords in
either file are sealed with the key in `/etc/vigil.key` ([security.md](security.md)). What belongs to
the system is written where the firmware keeps it:

| What | File | Written as |
|---|---|---|
| An interface | `/etc/network/interfaces.d/<if>` | a busybox ifupdown stanza (below) |
| DNS of a static address | the stanza's `pre-up` | `echo -e "nameserver A\nnameserver B" >/tmp/resolv.conf`, as on the OpenIPC wiki; `/etc/resolv.conf` links to `/tmp/resolv.conf` |
| Wi-Fi network | U-Boot environment | `fw_setenv wlanssid` / `wlanpass`; the wlan0 stanza turns them into a wpa_supplicant configuration (`wlan_conf`) |
| Wi-Fi adapter, modem | U-Boot environment | `fw_setenv wlandev <profile>`: `S40network` runs `/etc/wireless/usb` or `/etc/wireless/modem` with it at boot, which loads the driver |
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
- `mkfs.ext4 -F -L vigil` (e2fsprogs; the default where it is there: a
  journal, so a power cut costs the open segment, not the filesystem),
  `mkfs.exfat -L VIGIL` (exfatprogs; the default otherwise, readable by any
  computer) or `mkfs.vfat -F 32` (busybox). e2fsprogs 1.47 turns on
  `orphan_file` and `metadata_csum_seed`; the ssr621q's 4.9 kernel mounted
  such a filesystem in QEMU;
- the result is mounted at `/mnt/<partition>`.

**Unmount** holds recording off the disk and unmounts it; recording then
waits for a disk as it does when one is pulled. **Mount** mounts at
`/mnt/<dev>`, loading the filesystem's module when the kernel asks for it.

## Disk maintenance

**SMART** is read by vigild itself: the firmware has no room for
smartmontools, and five ATA commands are all it takes. They go to the
drive as ATA PASS-THROUGH (SAT: the 16-byte command, the 12-byte one
where a USB bridge knows only that) through the `SG_IO` ioctl on
`/dev/sdX`: libata translates for a SATA disk, a USB enclosure's bridge
in its firmware. IDENTIFY DEVICE gives the model, serial, firmware,
capacity, sector sizes, rotation rate (or solid state) and the SATA link
speeds; SMART READ DATA and READ THRESHOLDS the attribute table and the
self-test state; SMART RETURN STATUS the drive's own verdict; READ LOG
06h the self-test log. A USB stick, a memory card or a bridge without SAT
answers none of it: "SMART not available", and why. NVMe is not handled
(the recorders have none).

The verdict:

| | When |
|---|---|
| Failing | the drive says so (RETURN STATUS), an attribute is at or below its threshold, or the last self-test failed |
| Warning | a raw count that only grows on a sick disk is not zero: reallocated sectors (5), reallocation events (196), pending sectors (197), offline uncorrectable (198), reported uncorrectable (187), command timeouts (188), spin retries (10), end-to-end errors (184); interface CRC errors (199), which are usually the cable or the connection, not the disk; an attribute that was at its threshold once (worst); hotter than 55 °C (an SSD: 70 °C); an SSD with 10 % of its life left or less |
| Good | none of that |
| Unknown | no SMART, or SMART off |

Raw values are decoded where their meaning is common: temperatures with
the minimum and maximum some drives keep, power-on hours (the low 32
bits), spin-up time in ms, Seagate's three command-timeout counters.
Self-tests (short, extended) run inside the drive, which keeps working.

**The surface scan**, like Victoria or MHDD, reads a range of LBAs block
by block on a thread of vigild and times each block:

| Mode | How |
|---|---|
| Read | `pread` with `O_DIRECT`: no page cache, the disk's own time |
| Verify | ATA READ VERIFY SECTORS EXT through the same pass-through: the drive reads the block into nothing, no data crosses the cable. Where nothing passes it (a USB stick, some bridges), the scan reads and says so |
| Write | destructive: each sector gets a pattern (its LBA, the scan's nonce, a fixed byte), then a second pass reads every block back and compares. The disk is opened `O_EXCL`, which the kernel refuses while a partition of it is mounted; afterwards the kernel re-reads the (now gone) partition table |

Each block falls in a class by its time: under 5, 20, 50, 150, 500 ms,
500 ms or more, or an error (a failed read, a drive error, a block that
reads back different); the classes are counted and the errors' LBA
ranges listed. The map is up to 2000 cells over the range, each the worst
class of its blocks. The default block is 256 sectors (128 KiB);
Victoria's thresholds assume that size.

Rules: one scan at a time; whole disks only; Write only on a disk with
nothing mounted, not the recording directory's (`record.path` under
`/mnt/<one of its devices>`), not the system's, and with its name typed
(the screen names its model, serial and size). Read and Verify are
allowed on a disk in use; on a mounted one the scan's thread runs at the
idle I/O priority so recording goes first (the CFQ scheduler honours it;
Verify's ATA commands bypass the scheduler), and recording may still slow
down. A format of the scanned disk waits for the scan, and so does a
mount during Write. The scan goes on when the page is left; stopping
vigild stops it after the block under way (Write leaves the disk partly
written). Events: `disk sdb: surface scan (read) started`, `... done, 0
bad ranges`. Errors are counted per block read or written, so Write's
read-back can count a block its write already failed; the bad ranges list
each LBA range once.

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

## Snapshots

Still pictures ([api.md](api.md#snapshots)) are kept in one directory of
JPEG files on the recording disk: `snapshot.path`, else next to the
recordings (`record.path` `/mnt/sda1/rec`: `/mnt/sda1/snapshots`), or a
USB stick by giving its path. Like a recording directory it is used only
on a disk: never the root filesystem (the flash) unless
`record.allow_rootfs`, never a RAM or flash filesystem, never read-only;
with no disk there a picture is refused, not written to the flash. The
files' names say the channel, the time and why (`manual`, or the camera
event: `motion`, `human`, ...). After each picture the oldest go while the
directory's snapshots take more than `snapshot.max_mb` (1 GiB by default;
nothing but snapshots' files is touched). While a disk is being unmounted
or formatted (Disk Manager), no picture is written to it. An event's
pictures (`cameras[].snapshot_on_event`, `snapshot.event_count`,
`snapshot.event_interval`) are mailed together through the SMTP settings
when `snapshot.mail` is on: a `multipart/mixed` message with the JPEGs as
base64 attachments, through curl like the other mails. The settings have no
page of their own yet (the Alarm page will hold the per-event actions);
they are in `vigil.yaml` and the API's `snapshot` section.

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

## USB Wi-Fi adapters and modems

OpenIPC loads a Wi-Fi or modem driver only when the U-Boot environment's
`wlandev` names a profile: one of `/etc/wireless/usb` (`mt7601u-generic`,
`rt2800usb-generic`, ...) or `/etc/wireless/modem` (`rndis-generic`,
`rndis-e3372h`, `ec25`, ...). `S40network` runs the profile at boot (it
loads the module, maybe switches the modem's USB mode) and then brings the
interface up. A plugged adapter without its profile has no driver and no
interface, so vigild looks at the USB devices themselves: each
interface's `modalias` (`/sys/bus/usb/devices/*/modalias`) is matched
against the firmware's `modules.alias`, which names only drivers this
firmware has. A Wi-Fi adapter's profile is the `-generic` one that loads
its module (the others are cameras' own radios, powered through their
GPIOs); a modem's is one that names its USB id (`usb_modeswitch -v -p`,
`usbserial vendor= product=`, `new_id`), else one that loads its driver.
Joining a network with a new adapter sets `wlandev` with `wlanssid` and
`wlanpass`, runs `/etc/wireless/usb <profile>` and waits for the
interface; from then on the firmware starts it at every boot. Wi-Fi and
modem share `wlandev`. A QMI modem (`qmi_wwan`) is recognised, but no
profile of this firmware names one.

## What the firmware needs

Found on the ssr621q image (`ssr621q_lite`, OpenIPC/firmware branch
`vigil-yaml` at 932f160) and its kernel:

| Feature | Needs | On ssr621q |
|---|---|---|
| Ethernet | busybox `ifup`/`ifdown`, `udhcpc`, `ip` | there |
| Wi-Fi | mac80211, a driver, `iw`, `wpa_supplicant`, `wpa_passphrase` (used by `wlan_conf`) | there: mac80211, `mt7601u` and `rt2800usb` as modules, `iw`, `wpa_supplicant`, `wpa_passphrase` |
| LTE | modem drivers (`rndis_host`, `cdc_ether`, `cdc_ncm`, `qmi_wwan`, `option`, `usbserial`) | there, as modules |
| Mail, DDNS, alarm center | `curl` with TLS and SMTP | there (libcurl-openipc, mbedTLS) |
| Format ext4 | `mkfs.ext4` (e2fsprogs) | there, with `e2fsck` and `tune2fs`; ext4 is a module |
| Format exFAT | `mkfs.exfat` | there (exfatprogs) |
| Format FAT32 | busybox `mkfs.vfat` | there |
| SATA disks | the SATA host module | loaded at boot |
| SMB storage | the `cifs` module with SMB2/3 | there, as a module |
| NFS storage | NFS v3 (`nfsv3`, loaded by vigild; mounted with `addr=`, `nolock`, `vers=3`) | there, as a module |
| RTC | `hwclock` and `/dev/rtc0` | `hwclock` there; `CONFIG_MS_RTC=y` |
| NTP | busybox `ntpd` | there |

Older images (before those firmware changes) lack Wi-Fi, the modem
drivers, `mkfs.ext4`, NFS v3 and SMB2/3; the pages then say what is
missing: a kernel without `CONFIG_CIFS_SMB2` answers SMB2's `vers=2.1`
with "Unknown vers=" in its log, and vigild then mounts with SMB1 and
says so; without `nfsv3`, the NFS line names `CONFIG_NFS_V3`.
