# LVGL (third_party/lvgl, v9.2.2) as a static library, configured by
# src/ui/lv_conf.h; the colour depth comes from the platform.
file(GLOB_RECURSE LVGL_SOURCES ${CMAKE_SOURCE_DIR}/third_party/lvgl/src/*.c)
if(NOT LVGL_SOURCES)
  message(FATAL_ERROR "third_party/lvgl is empty: git submodule update --init")
endif()
add_library(lvgl STATIC ${LVGL_SOURCES})
target_include_directories(lvgl PUBLIC ${CMAKE_SOURCE_DIR}/third_party/lvgl ${CMAKE_SOURCE_DIR}/src/ui)
target_compile_definitions(lvgl PUBLIC LV_CONF_INCLUDE_SIMPLE LV_COLOR_DEPTH=${VIGIL_UI_COLOR_DEPTH}
                           ${VIGIL_LVGL_DEFS})
target_compile_options(lvgl PRIVATE -w)
