# SPDX-License-Identifier: GPL-3.0-or-later
add_executable(test_core test_core.c)
target_link_libraries(test_core PRIVATE vigil_core)
add_test(NAME core COMMAND test_core ${CMAKE_CURRENT_SOURCE_DIR}/data)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  foreach(clip testsrc_640x360.h264 majestic_1080p.h265)
    add_test(NAME netip_${clip}
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_netip.sh $<TARGET_FILE:vigild>
                     ${CMAKE_CURRENT_SOURCE_DIR}/data/${clip})
  endforeach()
  add_test(NAME netip_bad_password
           COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_netip.sh $<TARGET_FILE:vigild>
                   ${CMAKE_CURRENT_SOURCE_DIR}/data/testsrc_640x360.h264 badpass)
  foreach(case h264:none h264:digest h264:basic h265:none h264:wrong)
    string(REPLACE ":" "_" name ${case})
    add_test(NAME rtsp_${name}
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_rtsp.sh $<TARGET_FILE:vigild> ${case})
    set_tests_properties(rtsp_${name} PROPERTIES SKIP_RETURN_CODE 77)
  endforeach()
endif()
