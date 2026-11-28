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

# libyaml (MIT): the configuration file is YAML. OpenIPC images carry it
# (yaml-cli); on a host, libyaml-dev. pkg-config first; when cross compiling
# only when PKG_CONFIG_LIBDIR or PKG_CONFIG_SYSROOT_DIR point it at the
# target, since otherwise it describes the host's library. Otherwise
# a search, which cross.cmake points at the toolchain's sysroot (Buildroot's
# toolchain file at staging, with CMAKE_SYSROOT); or give
# -DLIBYAML_INCLUDE_DIR=… -DLIBYAML_LIBRARY=….
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND AND (NOT CMAKE_CROSSCOMPILING OR DEFINED ENV{PKG_CONFIG_LIBDIR}
                         OR DEFINED ENV{PKG_CONFIG_SYSROOT_DIR}))
  pkg_check_modules(LIBYAML QUIET IMPORTED_TARGET yaml-0.1)
endif()
add_library(vigil_yaml INTERFACE)
if(TARGET PkgConfig::LIBYAML)
  target_link_libraries(vigil_yaml INTERFACE PkgConfig::LIBYAML)
else()
  find_path(LIBYAML_INCLUDE_DIR yaml.h)
  find_library(LIBYAML_LIBRARY NAMES yaml)
  if(NOT LIBYAML_INCLUDE_DIR OR NOT LIBYAML_LIBRARY)
    message(FATAL_ERROR "libyaml not found (yaml.h, libyaml): apt install libyaml-dev; when "
                        "cross compiling, the target's libyaml in the toolchain's sysroot, "
                        "-DCMAKE_FIND_ROOT_PATH=<staging>, or -DLIBYAML_INCLUDE_DIR=… -DLIBYAML_LIBRARY=…")
  endif()
  target_include_directories(vigil_yaml INTERFACE ${LIBYAML_INCLUDE_DIR})
  target_link_libraries(vigil_yaml INTERFACE ${LIBYAML_LIBRARY})
endif()

# libmbedcrypto (mbedTLS, Apache-2.0): AES-256-GCM for the passwords vigil
# keeps in its files (src/core/secret.c, docs/security.md). OpenIPC images
# carry it (mbedtls-openipc); on a host, libmbedtls-dev. Found as libyaml is.
if(PKG_CONFIG_FOUND AND (NOT CMAKE_CROSSCOMPILING OR DEFINED ENV{PKG_CONFIG_LIBDIR}
                         OR DEFINED ENV{PKG_CONFIG_SYSROOT_DIR}))
  pkg_check_modules(MBEDCRYPTO QUIET IMPORTED_TARGET mbedcrypto)
endif()
add_library(vigil_mbedcrypto INTERFACE)
if(TARGET PkgConfig::MBEDCRYPTO)
  target_link_libraries(vigil_mbedcrypto INTERFACE PkgConfig::MBEDCRYPTO)
else()
  find_path(MBEDCRYPTO_INCLUDE_DIR mbedtls/gcm.h)
  find_library(MBEDCRYPTO_LIBRARY NAMES mbedcrypto)
  if(NOT MBEDCRYPTO_INCLUDE_DIR OR NOT MBEDCRYPTO_LIBRARY)
    message(FATAL_ERROR "libmbedcrypto not found (mbedtls/gcm.h, libmbedcrypto): apt install libmbedtls-dev; "
                        "when cross compiling, the target's mbedTLS in the toolchain's sysroot, "
                        "-DCMAKE_FIND_ROOT_PATH=<staging>, or -DMBEDCRYPTO_INCLUDE_DIR=… -DMBEDCRYPTO_LIBRARY=…")
  endif()
  target_include_directories(vigil_mbedcrypto INTERFACE ${MBEDCRYPTO_INCLUDE_DIR})
  target_link_libraries(vigil_mbedcrypto INTERFACE ${MBEDCRYPTO_LIBRARY})
endif()

# ---- core: hardware-agnostic, builds and tests on any host ------------------
add_library(vigil_core STATIC
  src/core/aspect.c
  src/core/cam_events.c
  src/core/au_norm.c
  src/core/channel.c
  src/core/config.c
  src/core/config_schema.c
  src/core/disc_ops.c
  src/core/disc_proto.c
  src/core/discovery.c
  src/core/events.c
  src/core/gop_cache.c
  src/core/md5.c
  src/core/net.c
  src/core/netip_proto.c
  src/core/overscan.c
  src/core/picture.c
  src/core/onvif_events.c
  src/core/ptz.c
  src/core/ptz_netip.c
  src/core/ptz_onvif.c
  src/core/onvif_proto.c
  src/core/qos.c
  src/core/schema.c
  src/core/schema_yaml.c
  src/core/secret.c
  src/core/sha1.c
  src/core/rec_place.c
  src/core/rec_sched.c
  src/core/seg_writer.c
  src/core/stream_tap.c
  src/core/tar.c
  src/core/sps.c
  src/core/source/rtp_depay.c
  src/core/source/rtsp_client.c
  src/core/source/source.c
  src/core/source/source_capture.c
  src/core/source/source_netip.c
  src/core/source/source_rtsp.c)
target_include_directories(vigil_core PUBLIC src/core src/core/source src/hal)
target_link_libraries(vigil_core PUBLIC cjson vigil_yaml vigil_mbedcrypto Threads::Threads m)

# ---- sys: looking after the system vigild runs on (src/sys, Linux) ------------
# Network, time, host name, disks, network storage, mail: OS work the
# management pages do, kept out of the POSIX-only core. Every file and
# command goes through src/sys/sys.h, so the tests run it on a fake root.
add_library(vigil_sys STATIC
  src/sys/mgmt.c
  src/sys/sys.c
  src/sys/sys_ata.c
  src/sys/sys_net.c
  src/sys/sys_curl.c
  src/sys/sys_disk.c
  src/sys/sys_nas.c
  src/sys/sys_scan.c
  src/sys/sys_smart.c
  src/sys/sys_time.c)
target_include_directories(vigil_sys PUBLIC src/sys)
target_link_libraries(vigil_sys PUBLIC vigil_core)
target_compile_definitions(vigil_sys PRIVATE VIGIL_VERSION="${PROJECT_VERSION}")

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
set(VIGIL_UI_SOURCES src/ui/ui_app.c src/ui/ui_channels.c src/ui/icons.c src/ui/ui_cursor.c src/ui/ui_form.c src/ui/ui_i18n.c src/ui/ui_settings.c src/ui/ui_set_network.c src/ui/ui_set_storage.c src/ui/ui_set_maint.c
    src/ui/ui_set_system.c src/ui/ui_set_account.c src/ui/ui_set_display.c src/ui/ui_set_backup.c
    src/ui/ui_layout.c src/ui/ui_live.c src/ui/ui_menu.c src/ui/ui_osk.c src/ui/ui_ptz.c src/ui/ui_session.c src/ui/ui_system.c src/ui/ui_theme.c)
if(VIGIL_HAS_UI OR (BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING))
  include(cmake/lvgl.cmake)
  add_library(vigil_ui STATIC ${VIGIL_UI_SOURCES})
  target_include_directories(vigil_ui PUBLIC src/ui src/hal)
  target_link_libraries(vigil_ui PUBLIC lvgl)
endif()
# The zero channel (src/app/kvm.c): its pixel loops and JPEG encoder are
# the only hot code vigild runs on the CPU, so they are built for speed.
set(VIGIL_KVM_SOURCES src/app/kvm.c src/app/kvm_enc.c src/app/kvm_input.c src/app/jpeg_enc.c)
set_source_files_properties(src/app/kvm.c src/app/kvm_enc.c src/app/jpeg_enc.c
                            ${CMAKE_SOURCE_DIR}/src/app/kvm.c ${CMAKE_SOURCE_DIR}/src/app/kvm_enc.c
                            ${CMAKE_SOURCE_DIR}/src/app/jpeg_enc.c PROPERTIES COMPILE_OPTIONS "-O2")
if(VIGIL_HAS_UI)
  list(APPEND VIGILD_SOURCES src/app/frontend_ui.c src/app/ui_bridge.c src/app/ui_mgmt.c
       src/app/input_evdev.c ${VIGIL_KVM_SOURCES})
  target_link_libraries(vigil_platform PRIVATE lvgl)
else()
  list(APPEND VIGILD_SOURCES src/app/frontend_headless.c src/ui/ui_layout.c src/app/kvm_none.c)
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
target_link_libraries(vigild PRIVATE vigil_sys vigil_core ${VIGIL_PLATFORM_LIBS})
if(VIGIL_API)
  target_link_libraries(vigild PRIVATE vigil_api)
  target_compile_definitions(vigild PRIVATE VIGIL_HAVE_USERS)
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
