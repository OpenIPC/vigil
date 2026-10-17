# vigil

Open-source DVR/NVR application for cheap recorder hardware running
[OpenIPC](https://openipc.org), built so that one code base runs on any
board — HiSilicon, Rockchip, and in time SigmaStar and plain Linux/cloud
hosts.

Status: early. Documentation lands in `docs/`.

```sh
git clone --recursive <repo> vigil && cd vigil
cmake -B build -DVIGIL_PLATFORM=generic && cmake --build build && (cd build && ctest)
./build/vigild -c rtsp://user:pass@camera/stream -c netip://admin:@dvr:34567/ -o /tmp/rec
```

License: GPL-3.0-or-later.
