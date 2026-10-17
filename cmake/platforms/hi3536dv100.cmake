# Hi3536DV100 NVR: network cameras decoded into HDMI tiles (VDEC -> VPSS ->
# VO), UI on hifb in ARGB8888. Vendor SDK: Hi3536DV100_SDK_V2.0.2.0, linked
# statically, so the binary needs only glibc on the board. Kernel modules:
# scripts/hisilicon/load_mpp_hi3536dv100.sh.
set(HI3536DV100_SDK "$ENV{HI3536DV100_SDK}"
    CACHE PATH "Hi3536DV100 SDK root (mpp/include, mpp/lib)")
if(NOT EXISTS "${HI3536DV100_SDK}/mpp/include/mpi_vdec.h")
  message(FATAL_ERROR "set HI3536DV100_SDK to the Hi3536DV100_SDK_V2.0.2.0 root "
                      "(the directory holding mpp/include); now '${HI3536DV100_SDK}'")
endif()

set(VIGIL_HAS_UI ON)
set(VIGIL_UI_COLOR_DEPTH 32)        # hifb has no RGB565 mode on this SoC
set(_hi ${CMAKE_SOURCE_DIR}/src/platform/hisilicon)
set(VIGIL_PLATFORM_SOURCES
  ${_hi}/common/hi_display.c
  ${_hi}/common/hal_ui_hifb.c
  ${_hi}/hi3536dv100/hal_hi3536dv100.c)
set(VIGIL_PLATFORM_INCLUDES ${HI3536DV100_SDK}/mpp/include ${_hi}/common)
set(VIGIL_PLATFORM_DEFS VG_HI3536DV100)
# libmpi.a references libjpeg and, through its audio adapter, the voice
# codecs — the SDK's own MPI_LIBS + AUDIO_LIBA.
set(_lib ${HI3536DV100_SDK}/mpp/lib)
set(VIGIL_PLATFORM_LIBS
  -Wl,--start-group ${_lib}/libmpi.a ${_lib}/libhdmi.a ${_lib}/libjpeg.a
  ${_lib}/libVoiceEngine.a ${_lib}/libupvqe.a ${_lib}/libdnvqe.a -Wl,--end-group
  dl m)

function(vigil_platform_extras)
  add_executable(fbtest ${CMAKE_SOURCE_DIR}/tools/fbtest/fbtest.c ${_hi}/common/hi_display.c)
  target_include_directories(fbtest PRIVATE ${VIGIL_PLATFORM_INCLUDES})
  target_compile_definitions(fbtest PRIVATE ${VIGIL_PLATFORM_DEFS})
  target_link_libraries(fbtest PRIVATE ${VIGIL_PLATFORM_LIBS} Threads::Threads)
endfunction()
