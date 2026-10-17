#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# stage_mpp_hi3536dv100.sh root@NVR — copy the MPP kernel modules vigil
# needs to a Hi3536DV100 board running the OpenIPC lite image (which ships
# none) and load them. They live in /tmp, so run this after every reboot;
# nothing is loaded when /dev/fb0 already exists.
#
# Sources (all outside this repository; override by environment):
#   KMOD_DIR      openhisilicon kernel/ build for hi3536dv100 (open_*.ko:
#                 OSAL from source, MPP modules relinked from SDK V2.0.2.0)
#   VFMW_KO       the SDK's hi3536dv100_vfmw.ko
#   VDEC_KO       hi3536dv100_vdec.ko with the chip-revision check bypassed:
#                 the stock module silently refuses frames on revision-2
#                 chips (reads 0x12060024 > 1)
#   CROSS_PREFIX  toolchain prefix, for strip
set -eu
BOARD=${1:?usage: $0 root@board}
HERE=$(cd "$(dirname "$0")" && pwd)
KMOD_DIR=${KMOD_DIR:-$HOME/git/openhisilicon/kernel}
SDK=${HI3536DV100_SDK:-$HOME/projects/cameras/sdk/Hi3536DV100_SDK_V2.0.2.0}
VFMW_KO=${VFMW_KO:-$SDK/mpp/ko/hi3536dv100_vfmw.ko}
VDEC_KO=${VDEC_KO:-$HOME/projects/cameras/Hi3536DV100-vdec/hi3536dv100_vdec.ko}
CROSS_PREFIX=${CROSS_PREFIX:-$HOME/git/firmware/output-hi3536dv100/host/bin/arm-openipc-linux-gnueabi-}

if ssh "$BOARD" test -e /dev/fb0; then
    echo "MPP already loaded on $BOARD"
    exit 0
fi
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
for m in osal base sys jpegd tde region vgs vpss vou hifb hdmi; do
    "${CROSS_PREFIX}strip" --strip-unneeded -R .comment -R .note \
        -o "$STAGE/open_$m.ko" "$KMOD_DIR/open_$m.ko"
done
"${CROSS_PREFIX}strip" --strip-unneeded -R .comment -R .note -o "$STAGE/hi3536dv100_vfmw.ko" "$VFMW_KO"
"${CROSS_PREFIX}strip" --strip-unneeded -R .comment -R .note -o "$STAGE/hi3536dv100_vdec.ko" "$VDEC_KO"
cp "$HERE/load_mpp_hi3536dv100.sh" "$STAGE/"
ssh "$BOARD" 'mkdir -p /tmp/mpp/kmod'
scp -O -q "$STAGE"/* "$BOARD":/tmp/mpp/kmod/
ssh "$BOARD" 'sh /tmp/mpp/kmod/load_mpp_hi3536dv100.sh'
