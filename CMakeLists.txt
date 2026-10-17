# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.16)
project(vigil VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE MinSizeRel CACHE STRING "Build type" FORCE)
endif()

set(VIGIL_PLATFORM generic CACHE STRING "Target platform")
set_property(CACHE VIGIL_PLATFORM PROPERTY STRINGS generic hi3536dv100 hi3520dv200 rockchip)
if(NOT EXISTS ${CMAKE_SOURCE_DIR}/cmake/platforms/${VIGIL_PLATFORM}.cmake)
  message(FATAL_ERROR "Unknown VIGIL_PLATFORM '${VIGIL_PLATFORM}'")
endif()

add_compile_options(-Wall -Wno-unused-function)
find_package(Threads REQUIRED)

# ---- core: hardware-agnostic, builds and tests on any host ------------------
add_library(vigil_core STATIC
  src/core/au_norm.c
  src/core/channel.c
  src/core/events.c
  src/core/md5.c
  src/core/net.c
  src/core/seg_writer.c
  src/core/sps.c
  src/core/source/rtp_depay.c
  src/core/source/rtsp_client.c
  src/core/source/source.c
  src/core/source/source_capture.c
  src/core/source/source_netip.c
  src/core/source/source_rtsp.c)
target_include_directories(vigil_core PUBLIC src/core src/core/source src/hal)
target_link_libraries(vigil_core PUBLIC Threads::Threads)

# ---- platform: sets VIGIL_PLATFORM_SOURCES / _LIBS / _DEFS, VIGIL_HAS_UI ----
set(VIGIL_HAS_UI OFF)
set(VIGIL_UI_COLOR_DEPTH 32)
include(cmake/platforms/${VIGIL_PLATFORM}.cmake)

add_library(vigil_platform OBJECT ${VIGIL_PLATFORM_SOURCES})
target_include_directories(vigil_platform PRIVATE src/core src/core/source src/hal ${VIGIL_PLATFORM_INCLUDES})
target_compile_definitions(vigil_platform PRIVATE ${VIGIL_PLATFORM_DEFS})

set(VIGILD_SOURCES src/app/main.c src/app/frontend_common.c)
if(VIGIL_HAS_UI)
  include(cmake/lvgl.cmake)
  list(APPEND VIGILD_SOURCES src/app/frontend_ui.c src/ui/ui_home.c src/ui/ui_events.c)
  target_link_libraries(vigil_platform PRIVATE lvgl)
else()
  list(APPEND VIGILD_SOURCES src/app/frontend_headless.c)
endif()

add_executable(vigild ${VIGILD_SOURCES} $<TARGET_OBJECTS:vigil_platform>)
target_include_directories(vigild PRIVATE src/app src/ui ${VIGIL_PLATFORM_INCLUDES})
target_compile_definitions(vigild PRIVATE ${VIGIL_PLATFORM_DEFS})
target_link_libraries(vigild PRIVATE vigil_core ${VIGIL_PLATFORM_LIBS})
if(VIGIL_HAS_UI)
  target_link_libraries(vigild PRIVATE lvgl)
endif()
install(TARGETS vigild RUNTIME DESTINATION bin)

if(COMMAND vigil_platform_extras)
  vigil_platform_extras()
endif()

# ---- tests: host builds only ------------------------------------------------
include(CTest)
if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
  add_subdirectory(tests)
endif()
