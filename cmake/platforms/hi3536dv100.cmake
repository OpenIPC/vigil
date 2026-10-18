# Hi3536DV100 NVR: network cameras decoded into HDMI tiles (VDEC -> VPSS ->
# VO), UI on hifb in ARGB8888. Kernel modules:
# scripts/hisilicon/load_mpp_hi3536dv100.sh, or the firmware's load_hisilicon.
#
# Two ways to give it the MPP headers and libraries, both from
# Hi3536DV100_SDK_V2.0.2.0:
#   HI3536DV100_SDK=<sdk root>   mpp/include + the static mpp/lib/*.a: the
#                                binary needs only glibc on the board.
#   HI3536DV100_INCLUDE_DIR + HI3536DV100_LIB_DIR
#                                headers and the shared libmpi.so & co, as
#                                the OpenIPC firmware ships them (its vigil
#                                package uses this).
# -D… wins; otherwise the environment, also on a re-run after a configure
# that failed for want of it (the cache then holds "").
foreach(_v HI3536DV100_SDK HI3536DV100_INCLUDE_DIR HI3536DV100_LIB_DIR)
  set(${_v} "" CACHE PATH "Hi3536DV100 MPP: see cmake/platforms/hi3536dv100.cmake")
  if(NOT ${_v} AND NOT "$ENV{${_v}}" STREQUAL "")
    set(${_v} "$ENV{${_v}}" CACHE PATH "Hi3536DV100 MPP: see cmake/platforms/hi3536dv100.cmake" FORCE)
  endif()
endforeach()

if(HI3536DV100_INCLUDE_DIR OR HI3536DV100_LIB_DIR)
  if(NOT EXISTS "${HI3536DV100_INCLUDE_DIR}/mpi_vdec.h" OR NOT EXISTS "${HI3536DV100_LIB_DIR}/libmpi.so")
    message(FATAL_ERROR "HI3536DV100_INCLUDE_DIR must hold mpi_vdec.h and HI3536DV100_LIB_DIR "
                        "libmpi.so; now '${HI3536DV100_INCLUDE_DIR}' and '${HI3536DV100_LIB_DIR}'")
  endif()
  set(_inc ${HI3536DV100_INCLUDE_DIR})
  # libmpi.so needs the audio and JPEG libraries next to it at run time
  # (the SDK's MPI_LIBS + AUDIO_LIBA); naming them keeps the link complete.
  set(VIGIL_PLATFORM_LIBS -L${HI3536DV100_LIB_DIR} mpi hdmi jpeg VoiceEngine upvqe dnvqe dl m)
else()
  if(NOT EXISTS "${HI3536DV100_SDK}/mpp/include/mpi_vdec.h")
    message(FATAL_ERROR "set HI3536DV100_SDK to the Hi3536DV100_SDK_V2.0.2.0 root "
                        "(the directory holding mpp/include), or HI3536DV100_INCLUDE_DIR "
                        "and HI3536DV100_LIB_DIR; now '${HI3536DV100_SDK}'")
  endif()
  set(_inc ${HI3536DV100_SDK}/mpp/include)
  # libmpi.a references libjpeg and, through its audio adapter, the voice
  # codecs — the SDK's own MPI_LIBS + AUDIO_LIBA.
  set(_lib ${HI3536DV100_SDK}/mpp/lib)
  set(VIGIL_PLATFORM_LIBS
    -Wl,--start-group ${_lib}/libmpi.a ${_lib}/libhdmi.a ${_lib}/libjpeg.a
    ${_lib}/libVoiceEngine.a ${_lib}/libupvqe.a ${_lib}/libdnvqe.a -Wl,--end-group
    dl m)
endif()

set(VIGIL_HAS_UI ON)
set(VIGIL_UI_COLOR_DEPTH 32)        # hifb has no RGB565 mode on this SoC
set(_hi ${CMAKE_SOURCE_DIR}/src/platform/hisilicon)
set(VIGIL_PLATFORM_SOURCES
  ${_hi}/common/hi_display.c
  ${_hi}/common/hal_ui_hifb.c
  ${_hi}/hi3536dv100/hal_hi3536dv100.c)
set(VIGIL_PLATFORM_INCLUDES ${_inc} ${_hi}/common)
set(VIGIL_PLATFORM_DEFS VG_HI3536DV100)

function(vigil_platform_extras)
  add_executable(fbtest ${CMAKE_SOURCE_DIR}/tools/fbtest/fbtest.c ${_hi}/common/hi_display.c)
  target_include_directories(fbtest PRIVATE ${VIGIL_PLATFORM_INCLUDES})
  target_compile_definitions(fbtest PRIVATE ${VIGIL_PLATFORM_DEFS})
  target_link_libraries(fbtest PRIVATE ${VIGIL_PLATFORM_LIBS} Threads::Threads)
endfunction()
