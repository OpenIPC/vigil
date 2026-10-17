#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Load the MPP kernel modules vigild / fbtest need on a hi3536dv100 NVR
# running the OpenIPC lite image (which ships none). Runs on the board from
# the directory holding the modules; stage_mpp_hi3536dv100.sh copies both
# there and runs it.
#
# Decode + display: SYS, VB/MMZ, VDEC/VFMW/JPEGD, TDE, region, VGS, VPSS,
# VO, hifb, HDMI. Order and parameters follow OpenIPC's load_hisilicon and
# the SDK's load3536dv100, except that the MMZ is one 'anonymous' zone
# covering all RAM above the kernel: the extra 'vou' zone there overlaps it
# and OSAL rejects it ("Conflict MMZ").
#
# hi3536dv100_vdec.ko / hi3536dv100_vfmw.ko are the SDK's (the vdec with
# its chip-revision check bypassed, see stage_mpp_hi3536dv100.sh); the rest
# are openhisilicon's open_*.ko.
set -e
cd "$(dirname "$0")"

if [ -e /dev/fb0 ]; then
	echo "MPP already loaded"
	exit 0
fi

# set -e must not end the script when an env variable is missing: the
# defaults below apply then.
mem_total=$(fw_printenv -n totalmem 2>/dev/null | tr -d M || true)
os_mem=$(awk 'BEGIN{RS=" "} /^mem=[0-9]+M/{gsub(/^mem=|M.*$/,""); print; exit}' /proc/cmdline)
: "${mem_total:=256}" "${os_mem:=128}"
mmz_start=$(printf '0x%x' $((0x80000000 + os_mem * 1024 * 1024)))
mmz_size=$((mem_total - os_mem))M
echo "MMZ anonymous $mmz_start $mmz_size"

insmod open_osal.ko mmz=anonymous,0,$mmz_start,$mmz_size anony=1
insmod open_base.ko
insmod open_sys.ko mem_total=$mem_total
# VBSource=1: each VDEC channel has a private picture pool, sized by
# dvr_home from the camera's SPS (HI_MPI_VDEC_SetChnVBCnt).
insmod hi3536dv100_vdec.ko VBSource=1 u32ProtocolSwitch=0 StreamCompaMode=1 MiniBufMode=0
insmod hi3536dv100_vfmw.ko
insmod open_jpegd.ko
insmod open_tde.ko
insmod open_region.ko
insmod open_vgs.ko
insmod open_vpss.ko vpss_vb_source=0 vpss_en_ratio=0
insmod open_vou.ko
insmod open_hifb.ko video="hifb:vram0_size:8100,vram1_size:128" softcursor="off"
insmod open_hdmi.ko
echo "MPP loaded"
