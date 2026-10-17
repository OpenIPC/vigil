#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# build-deps.sh — build the Rockchip media libraries vigil's rockchip
# platform links against, from source, natively on the board, into
# PREFIX (default /usr/local). Both are pinned:
#
#   rockchip-linux/mpp       (Apache-2.0)  hardware video decoder (VPU)
#   librga, as maintained in nyanmisaka/rk-mirrors "jellyfin-rga"
#                            (Apache-2.0)  RGA 2D scaler/blitter; the
#                            source Jellyfin's ffmpeg builds from
#
# Debian/Ubuntu packages for these are not in the stock repositories, and
# the airockchip/librga repository ships binaries only.
set -eu
PREFIX=${PREFIX:-/usr/local}
WORK=${WORK:-/tmp/vigil-deps}
MPP_REPO=https://github.com/rockchip-linux/mpp
MPP_REV=20959ec3dabb313b8c33ca0b7b09c06ffd2a9caa
RGA_REPO=https://github.com/nyanmisaka/rk-mirrors
RGA_REV=1d330cc28551943bed3380261a5a9c6fbd58ff53
JOBS=$(nproc)

fetch() {   # repo rev dir
    [ -d "$3/.git" ] || git init -q "$3"
    git -C "$3" fetch -q --depth 1 "$1" "$2"
    git -C "$3" checkout -q FETCH_HEAD
}

mkdir -p "$WORK"
fetch $MPP_REPO $MPP_REV "$WORK/mpp"
cmake -S "$WORK/mpp" -B "$WORK/mpp/build" -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="$PREFIX" -DRKPLATFORM=ON -DHAVE_DRM=ON -DBUILD_TEST=OFF
cmake --build "$WORK/mpp/build" -j "$JOBS"
cmake --install "$WORK/mpp/build"

fetch $RGA_REPO $RGA_REV "$WORK/rga"
meson setup "$WORK/rga/build" "$WORK/rga" --prefix="$PREFIX" --libdir=lib \
      --buildtype=release -Dcpp_args=-fpermissive -Dlibdrm=false -Dlibrga_demo=false
ninja -C "$WORK/rga/build"
ninja -C "$WORK/rga/build" install

ldconfig
echo "installed: rockchip_mpp $(pkg-config --modversion rockchip_mpp), librga $(pkg-config --modversion librga)"
