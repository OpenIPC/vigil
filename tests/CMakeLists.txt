# SPDX-License-Identifier: GPL-3.0-or-later
add_executable(test_core test_core.c)
target_link_libraries(test_core PRIVATE vigil_core)
add_test(NAME core COMMAND test_core ${CMAKE_CURRENT_SOURCE_DIR}/data)
add_executable(test_discovery test_discovery.c)
target_link_libraries(test_discovery PRIVATE vigil_core)
add_test(NAME discovery COMMAND test_discovery ${CMAKE_CURRENT_SOURCE_DIR}/data)
add_executable(test_onvif test_onvif.c)
target_link_libraries(test_onvif PRIVATE vigil_core)
add_test(NAME onvif COMMAND test_onvif)

# src/sys on a fake root: fake /etc, /sys, /proc and /dev files, and a
# recorder in place of ifup, mkfs, mount, date, ntpd, hostname, curl.
add_executable(test_sys test_sys.c sys_fake.c test_sys_net.c test_sys_time.c test_sys_curl.c
               test_sys_disk.c test_sys_mgmt.c test_sys_maint.c)
target_include_directories(test_sys PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_sys PRIVATE vigil_sys)
add_test(NAME sys COMMAND test_sys)

if(TARGET vigil_api)
  add_executable(test_api test_api.c)
  target_link_libraries(test_api PRIVATE vigil_api)
  add_test(NAME api_auth COMMAND test_api)
endif()

if(TARGET vigil_ui)
  # The on-screen UI on an in-memory display, with the real evdev reader,
  # the bridge and the configuration store, and fake channels.
  add_executable(test_ui test_ui.c ${CMAKE_SOURCE_DIR}/src/app/ui_bridge.c
                 ${CMAKE_SOURCE_DIR}/src/app/ui_mgmt.c ${CMAKE_SOURCE_DIR}/src/app/input_evdev.c)
  target_include_directories(test_ui PRIVATE ${CMAKE_SOURCE_DIR}/src/app)
  target_compile_definitions(test_ui PRIVATE VIGIL_VERSION="${PROJECT_VERSION}")
  target_link_libraries(test_ui PRIVATE vigil_ui vigil_sys vigil_core)
  if(TARGET vigil_api)
    target_link_libraries(test_ui PRIVATE vigil_api)
    target_compile_definitions(test_ui PRIVATE VIGIL_HAVE_USERS)
  endif()
  # Every screen as PNGs into build/screenshots, for looking at (not a test):
  # cmake --build build --target screenshots
  add_custom_target(screenshots
                    COMMAND ${CMAKE_SOURCE_DIR}/scripts/screenshots.sh ${CMAKE_BINARY_DIR}
                    DEPENDS test_ui USES_TERMINAL)
  # ... and that it still walks through every screen.
  add_test(NAME ui_screenshots COMMAND test_ui --shots ${CMAKE_CURRENT_BINARY_DIR}/shots)

  # The zero channel: its encoders and input mapping, and src/app/kvm.c on
  # an in-memory display with the evdev reader; with --serve, the web
  # server around it for web_test.py.
  if(TARGET vigil_api)
    add_executable(test_kvm test_kvm.c ${CMAKE_SOURCE_DIR}/src/app/input_evdev.c
                   ${CMAKE_SOURCE_DIR}/src/app/kvm.c ${CMAKE_SOURCE_DIR}/src/app/kvm_enc.c
                   ${CMAKE_SOURCE_DIR}/src/app/kvm_input.c ${CMAKE_SOURCE_DIR}/src/app/jpeg_enc.c)
    target_include_directories(test_kvm PRIVATE ${CMAKE_SOURCE_DIR}/src/app ${CMAKE_SOURCE_DIR}/src/api)
    target_link_libraries(test_kvm PRIVATE vigil_api vigil_ui vigil_core)
    add_test(NAME kvm COMMAND test_kvm)
  endif()
endif()

add_test(NAME cli_too_long COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_cli.sh $<TARGET_FILE:vigild>)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  # src/ui/theme_tokens.h and web/theme.css are generated from src/ui/theme.json
  # and committed: they must match it.
  add_test(NAME theme_tokens COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/gen-theme.py --check)
  # src/ui/icons.[ch] are rendered from src/ui/icons/*.svg and committed.
  add_test(NAME ui_icons COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/gen-icons.py --check)
  if(TARGET test_ui)
    # Screenshots of the main screens against tests/ui/*.png
    # (UPDATE=1 ctest -R ui_screens rewrites them).
    add_test(NAME ui_screens
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/ui_compare.py $<TARGET_FILE:test_ui>
                     ${CMAKE_CURRENT_SOURCE_DIR}/ui ${CMAKE_CURRENT_BINARY_DIR}/ui)
  endif()
  foreach(clip testsrc_640x360.h264 majestic_1080p.h265)
    add_test(NAME netip_${clip}
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_netip.sh $<TARGET_FILE:vigild>
                     ${CMAKE_CURRENT_SOURCE_DIR}/data/${clip})
  endforeach()
  add_test(NAME netip_bad_password
           COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_netip.sh $<TARGET_FILE:vigild>
                   ${CMAKE_CURRENT_SOURCE_DIR}/data/testsrc_640x360.h264 badpass)
  # An XM 4 MP main stream (size bits in the codec byte), the channel's
  # name from the camera, and a camera that does not answer for it.
  add_test(NAME netip_wide
           COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_netip.sh $<TARGET_FILE:vigild>
                   ${CMAKE_CURRENT_SOURCE_DIR}/data/majestic_1080p.h265 wide)
  foreach(case title notitle)
    add_test(NAME netip_${case}
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_netip.sh $<TARGET_FILE:vigild>
                     ${CMAKE_CURRENT_SOURCE_DIR}/data/testsrc_640x360.h264 ${case})
  endforeach()
  foreach(case chatty stall audioonly shutdown)
    add_test(NAME fault_${case}
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_faults.sh $<TARGET_FILE:vigild>
                     ${CMAKE_CURRENT_SOURCE_DIR}/data/testsrc_640x360.h264 ${case})
  endforeach()
  if(TARGET vigil_api)
    # Discovery against fake NETIP, ONVIF and Dahua devices on 127.0.0.x.
    add_test(NAME discovery_e2e
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/discovery_test.py $<TARGET_FILE:vigild>
                     ${CMAKE_CURRENT_SOURCE_DIR}/data/testsrc_640x360.h264)
    if(TARGET test_kvm)
      # The zero channel's web server end to end: logins, the WebSocket,
      # pictures (decoded with PIL when it is there) and input.
      add_test(NAME web_e2e COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/web_test.py
                                    $<TARGET_FILE:test_kvm>)
      set_tests_properties(web_e2e PROPERTIES SKIP_RETURN_CODE 77)
    endif()
    # The ONVIF server as a client sees it: discovery, logins, profiles.
    add_test(NAME onvif_e2e
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/onvif_test.py $<TARGET_FILE:vigild>
                     ${CMAKE_CURRENT_SOURCE_DIR}/data/testsrc_640x360.h264)
    add_test(NAME api_e2e
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/api_test.py $<TARGET_FILE:vigild>
                     ${CMAKE_CURRENT_SOURCE_DIR}/data/testsrc_640x360.h264)
  endif()
  foreach(case h264:none h264:digest h264:basic h265:none h264:wrong)
    string(REPLACE ":" "_" name ${case})
    add_test(NAME rtsp_${name}
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_rtsp.sh $<TARGET_FILE:vigild> ${case})
    set_tests_properties(rtsp_${name} PROPERTIES SKIP_RETURN_CODE 77)
  endforeach()
endif()
