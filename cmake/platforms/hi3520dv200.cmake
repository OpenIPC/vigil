# Hi3520DV200 DVR: four analog cameras (NVP6114 -> VI) shown in the HDMI
# tiles and encoded to H.264 for recording. UI on hifb in RGB565.
# Toolchain + MPP: the OpenIPC hi3520dv200 SDK toolchain (musl), whose
# sdk/include and sdk/lib carry MPP V2.0.5.1 (openipc/firmware#2108, #2120)
# and libglibc-compat-static.a (#2110/#2119).
# -DHI3520DV200_TOOLCHAIN_SDK=… wins; otherwise the environment, also on a re-run after a
# configure that failed for want of it (the cache then holds "").
set(HI3520DV200_TOOLCHAIN_SDK "" CACHE PATH "OpenIPC hi3520dv200 toolchain root (sdk/include, sdk/lib)")
if(NOT HI3520DV200_TOOLCHAIN_SDK AND NOT "$ENV{HI3520DV200_TOOLCHAIN_SDK}" STREQUAL "")
  set(HI3520DV200_TOOLCHAIN_SDK "$ENV{HI3520DV200_TOOLCHAIN_SDK}" CACHE PATH "OpenIPC hi3520dv200 toolchain root (sdk/include, sdk/lib)" FORCE)
endif()
if(NOT EXISTS "${HI3520DV200_TOOLCHAIN_SDK}/sdk/include/mpi_vi.h")
  message(FATAL_ERROR "set HI3520DV200_TOOLCHAIN_SDK to the OpenIPC hi3520dv200 toolchain root "
                      "(the directory holding sdk/include); now '${HI3520DV200_TOOLCHAIN_SDK}'")
endif()

set(VIGIL_HAS_UI ON)
set(VIGIL_UI_COLOR_DEPTH 16)
set(_hi ${CMAKE_SOURCE_DIR}/src/platform/hisilicon)
set(VIGIL_PLATFORM_SOURCES
  ${_hi}/common/hi_display.c
  ${_hi}/common/hal_ui_hifb.c
  ${_hi}/hi3520dv200/hal_hi3520dv200.c
  ${_hi}/hi3520dv200/hi_video_pipe.c
  ${_hi}/hi3520dv200/venc_reader.c)
set(VIGIL_PLATFORM_INCLUDES ${HI3520DV200_TOOLCHAIN_SDK}/sdk/include ${_hi}/common ${_hi}/hi3520dv200)
set(VIGIL_PLATFORM_DEFS VG_HI3520DV200)
# libmpi.so on this SoC is glibc-built: the static shim provides the mmap
# entry points it needs from a musl process, exported so the dynamic linker
# binds libmpi to them.
set(_sdk ${HI3520DV200_TOOLCHAIN_SDK}/sdk/lib)
set(VIGIL_PLATFORM_LIBS
  -L${_sdk} -lmpi -lhdmi -Wl,--unresolved-symbols=ignore-in-shared-libs
  -Wl,-u,mmap -Wl,-u,mmap64 -lglibc-compat-static
  -Wl,--export-dynamic-symbol=mmap -Wl,--export-dynamic-symbol=mmap64 m)
# EXPERIMENTAL: see src/platform/hisilicon/hi3520dv200/hal_hi3520dv200.c.

function(vigil_platform_extras)
  add_executable(fbtest ${CMAKE_SOURCE_DIR}/tools/fbtest/fbtest.c ${_hi}/common/hi_display.c)
  target_include_directories(fbtest PRIVATE ${VIGIL_PLATFORM_INCLUDES})
  target_compile_definitions(fbtest PRIVATE ${VIGIL_PLATFORM_DEFS})
  target_link_libraries(fbtest PRIVATE ${VIGIL_PLATFORM_LIBS})
endfunction()
