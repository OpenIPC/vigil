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
set_property(CACHE VIGIL_PLATFORM PROPERTY STRINGS generic hi3536dv100 hi3520dv200 rockchip sigmastar)
if(NOT EXISTS ${CMAKE_SOURCE_DIR}/cmake/platforms/${VIGIL_PLATFORM}.cmake)
  message(FATAL_ERROR "Unknown VIGIL_PLATFORM '${VIGIL_PLATFORM}'")
endif()

option(VIGIL_API "HTTP/WebSocket API (CivetWeb, docs/api.md)" ON)
include(CTest)                      # BUILD_TESTING

add_compile_options(-Wall -Wno-unused-function)
find_package(Threads REQUIRED)

# cJSON (third_party/cJSON, MIT): the configuration file and the API's JSON.
if(NOT EXISTS ${CMAKE_SOURCE_DIR}/third_party/cJSON/cJSON.c)
  message(FATAL_ERROR "third_party/cJSON is empty: git submodule update --init")
endif()
add_library(cjson STATIC third_party/cJSON/cJSON.c)
target_include_directories(cjson PUBLIC third_party/cJSON)
target_link_libraries(cjson PUBLIC m)

# ---- core: hardware-agnostic, builds and tests on any host ------------------
add_library(vigil_core STATIC
  src/core/au_norm.c
  src/core/channel.c
  src/core/config.c
  src/core/config_schema.c
  src/core/events.c
  src/core/md5.c
  src/core/net.c
  src/core/schema.c
  src/core/rec_place.c
  src/core/seg_writer.c
  src/core/sps.c
  src/core/source/rtp_depay.c
  src/core/source/rtsp_client.c
  src/core/source/source.c
  src/core/source/source_capture.c
  src/core/source/source_netip.c
  src/core/source/source_rtsp.c)
target_include_directories(vigil_core PUBLIC src/core src/core/source src/hal)
target_link_libraries(vigil_core PUBLIC cjson Threads::Threads)

# ---- platform: sets VIGIL_PLATFORM_SOURCES / _LIBS / _DEFS, VIGIL_HAS_UI ----
set(VIGIL_HAS_UI OFF)
set(VIGIL_UI_COLOR_DEPTH 32)
include(cmake/platforms/${VIGIL_PLATFORM}.cmake)

add_library(vigil_platform OBJECT ${VIGIL_PLATFORM_SOURCES})
target_include_directories(vigil_platform PRIVATE src/core src/core/source src/hal ${VIGIL_PLATFORM_INCLUDES})
target_compile_definitions(vigil_platform PRIVATE ${VIGIL_PLATFORM_DEFS})

set(VIGILD_SOURCES src/app/main.c src/app/frontend_common.c)
# The UI (src/ui: LVGL and hal_ui.h only) as a library: vigild links it on
# platforms with a display, the host build for its screenshot tests.
set(VIGIL_UI_SOURCES src/ui/ui_app.c src/ui/ui_cursor.c src/ui/ui_events.c src/ui/ui_form.c
    src/ui/ui_layout.c src/ui/ui_live.c src/ui/ui_menu.c src/ui/ui_system.c src/ui/ui_theme.c)
if(VIGIL_HAS_UI OR (BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING))
  include(cmake/lvgl.cmake)
  add_library(vigil_ui STATIC ${VIGIL_UI_SOURCES})
  target_include_directories(vigil_ui PUBLIC src/ui src/hal)
  target_link_libraries(vigil_ui PUBLIC lvgl)
endif()
if(VIGIL_HAS_UI)
  list(APPEND VIGILD_SOURCES src/app/frontend_ui.c src/app/ui_bridge.c src/app/input_evdev.c)
  target_link_libraries(vigil_platform PRIVATE lvgl)
else()
  list(APPEND VIGILD_SOURCES src/app/frontend_headless.c src/ui/ui_layout.c)
endif()

# ---- API: CivetWeb + crypt(3); -DVIGIL_API=OFF drops both --------------------
if(VIGIL_API)
  include(cmake/api.cmake)
else()
  list(APPEND VIGILD_SOURCES src/api/api_none.c)
endif()

add_executable(vigild ${VIGILD_SOURCES} $<TARGET_OBJECTS:vigil_platform>)
target_include_directories(vigild PRIVATE src/app src/api src/ui ${VIGIL_PLATFORM_INCLUDES})
target_compile_definitions(vigild PRIVATE ${VIGIL_PLATFORM_DEFS} VIGIL_VERSION="${PROJECT_VERSION}")
target_link_libraries(vigild PRIVATE vigil_core ${VIGIL_PLATFORM_LIBS})
if(VIGIL_API)
  target_link_libraries(vigild PRIVATE vigil_api)
endif()
if(VIGIL_HAS_UI)
  target_link_libraries(vigild PRIVATE vigil_ui)
endif()
install(TARGETS vigild RUNTIME DESTINATION bin)

if(COMMAND vigil_platform_extras)
  vigil_platform_extras()
endif()

# ---- tests: host builds only ------------------------------------------------
if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
  add_subdirectory(tests)
endif()
