# Rockchip RK3588 / RK356x with the vendor kernel (Armbian "vendor", Rockchip
# BSP): MPP hardware decode, RGA composition, DRM/KMS output, LVGL UI on an
# ARGB plane. Libraries from scripts/rockchip/build-deps.sh (pkg-config).
find_package(PkgConfig REQUIRED)
pkg_check_modules(RK REQUIRED IMPORTED_TARGET rockchip_mpp librga libdrm)

set(VIGIL_HAS_UI ON)
set(VIGIL_UI_COLOR_DEPTH 32)
set(_rk ${CMAKE_SOURCE_DIR}/src/platform/rockchip)
set(VIGIL_PLATFORM_SOURCES
  ${_rk}/hal_rockchip.c
  ${_rk}/hal_ui_drm.c
  ${_rk}/rk_display.c)
set(VIGIL_PLATFORM_INCLUDES ${RK_INCLUDE_DIRS} ${_rk})
set(VIGIL_PLATFORM_LIBS PkgConfig::RK stdc++)
