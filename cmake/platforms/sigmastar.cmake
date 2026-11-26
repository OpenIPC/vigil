# SigmaStar infinity2m NVRs (SSR621Q): network cameras decoded into HDMI/VGA
# tiles (MI_VDEC -> MI_DISP), UI on the GOP framebuffer. Kernel modules,
# /config and the libraries: OpenIPC's sigmastar-osdrv-infinity2m
# (load_sigmastar) and sigmastar-opensdk (the HDMI transmitter).
#
#   SIGMASTAR_INCLUDE_DIR  the MI headers and sstarFb.h matching the
#                          libraries: OpenIPC/opensigmastar include/infinity2m
#   SIGMASTAR_LIB_DIR      libmi_sys.so, libmi_vdec.so and libmi_disp.so
#                          (i2m NVR SDK, glibc, hard float)
# Each takes -D… first, otherwise the environment.
set(_doc "SigmaStar MI: see cmake/platforms/sigmastar.cmake")
foreach(_v SIGMASTAR_INCLUDE_DIR SIGMASTAR_LIB_DIR)
  set(${_v} "" CACHE PATH "${_doc}")
  if(NOT ${_v} AND NOT "$ENV{${_v}}" STREQUAL "")
    set(${_v} "$ENV{${_v}}" CACHE PATH "${_doc}" FORCE)
  endif()
endforeach()
if(NOT EXISTS "${SIGMASTAR_INCLUDE_DIR}/mi_vdec.h" OR NOT EXISTS "${SIGMASTAR_INCLUDE_DIR}/sstarFb.h"
   OR NOT EXISTS "${SIGMASTAR_LIB_DIR}/libmi_vdec.so")
  message(FATAL_ERROR "SIGMASTAR_INCLUDE_DIR must hold mi_vdec.h and sstarFb.h, and "
                      "SIGMASTAR_LIB_DIR libmi_vdec.so; now '${SIGMASTAR_INCLUDE_DIR}' "
                      "and '${SIGMASTAR_LIB_DIR}'")
endif()

set(VIGIL_HAS_UI ON)
set(VIGIL_UI_COLOR_DEPTH 32)        # the GOP takes ARGB8888
set(_ss ${CMAKE_SOURCE_DIR}/src/platform/sigmastar)
set(VIGIL_PLATFORM_SOURCES ${_ss}/hal_sigmastar.c ${_ss}/hal_ui_fbdev.c)
set(VIGIL_PLATFORM_INCLUDES ${SIGMASTAR_INCLUDE_DIR})
set(VIGIL_PLATFORM_LIBS -L${SIGMASTAR_LIB_DIR} mi_vdec mi_disp mi_sys)
