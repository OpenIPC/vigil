/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * lv_conf.h — minimal LVGL v9.2 config for vigild's quad view.
 *
 * Target: 1280x720 on the platform's UI layer (hal_ui.h). The colour depth
 * comes from the platform's CMake file (VIGIL_UI_COLOR_DEPTH, passed as
 * LV_COLOR_DEPTH): 16 (RGB565) on hi3520dv200, 32 (ARGB8888) elsewhere.
 * No input devices (signage mode). The tick comes from frontend_ui.c
 * (lv_tick_set_cb on CLOCK_MONOTONIC).
 */
#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/* ----- Colour ---------------------------------------------------------- */
#ifndef LV_COLOR_DEPTH
#define LV_COLOR_DEPTH                  16          /* RGB565 — matches hifb */
#endif
#define LV_COLOR_16_SWAP                0

/* ----- Memory ---------------------------------------------------------- */
#define LV_USE_STDLIB_MALLOC            LV_STDLIB_BUILTIN
/* 512 KiB heap = headroom for two 150 KiB partial-render buffers + widgets.
 * LVGL's lv_linux_fbdev_set_file() lv_malloc's the draw buffers (LV_LINUX_FBDEV_BUFFER_SIZE
 * each, 2 of them). 96 KiB was too tight and triggered a buf1==NULL assert. */
#define LV_MEM_SIZE                     (512U * 1024U)
#define LV_MEM_POOL_EXPAND_SIZE         0
#define LV_USE_STDLIB_STRING            LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_SPRINTF           LV_STDLIB_BUILTIN

/* ----- HAL ------------------------------------------------------------- */
#define LV_DEF_REFR_PERIOD              33          /* ms — ~30 fps */
#define LV_DPI_DEF                      96

/* ----- Drivers --------------------------------------------------------- */
#define LV_USE_LINUX_FBDEV              1
#define LV_LINUX_FBDEV_BSD              0
#define LV_LINUX_FBDEV_NUTTX            0
#define LV_LINUX_FBDEV_RENDER_MODE      LV_DISPLAY_RENDER_MODE_PARTIAL
#define LV_LINUX_FBDEV_BUFFER_COUNT     2
/* Number of LINES per partial-render buffer (LVGL multiplies by hor_res * bpp/8
 * internally). 60 lines × 1280 × 2 bytes RGB565 = 150 KiB per buffer × 2 = 300 KiB
 * from LV_MEM_SIZE; at 32 bpp, 30 lines keeps the same 300 KiB.
 * Mis-spec'd this as a byte count before and it asked for 375 MB. */
#define LV_LINUX_FBDEV_BUFFER_SIZE      (LV_COLOR_DEPTH == 32 ? 30 : 60)

#define LV_USE_LINUX_DRM                0
#define LV_USE_X11                      0
#define LV_USE_NUTTX                    0
#define LV_USE_WAYLAND                  0
#define LV_USE_SDL                      0
#define LV_USE_LIBINPUT                 0
#define LV_USE_EVDEV                    0

/* ----- File system / image decoders (off) ----------------------------- */
#define LV_USE_FS_STDIO                 0
#define LV_USE_FS_POSIX                 0
#define LV_USE_FS_WIN32                 0
#define LV_USE_FS_FATFS                 0
#define LV_USE_FS_MEMFS                 0
#define LV_USE_FS_LITTLEFS              0
#define LV_USE_LZ4_INTERNAL             0
#define LV_USE_LZ4_EXTERNAL             0
#define LV_USE_GIF                      0
#define LV_USE_BIN_DECODER              1
#define LV_USE_BMP                      0
#define LV_USE_TJPGD                    0
#define LV_USE_LIBJPEG_TURBO            0
#define LV_USE_LIBPNG                   0
#define LV_USE_LODEPNG                  0
#define LV_USE_SVG                      0
#define LV_USE_QRCODE                   0
#define LV_USE_BARCODE                  0
#define LV_USE_FREETYPE                 0
#define LV_USE_FONT_COMPRESSED          0

/* ----- Log ------------------------------------------------------------- */
#define LV_USE_LOG                      1
#define LV_LOG_LEVEL                    LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF                   1
#define LV_USE_ASSERT_NULL              1
#define LV_USE_ASSERT_MALLOC            1
#define LV_USE_ASSERT_OBJ               0

/* ----- Drawing --------------------------------------------------------- */
#define LV_USE_DRAW_SW                  1
#define LV_DRAW_SW_DRAW_UNIT_CNT        1
#define LV_DRAW_SW_SUPPORT_RGB565       1
#define LV_DRAW_SW_SUPPORT_RGB888       (LV_COLOR_DEPTH == 32)  /* 32 bpp transform/rotate paths reference these */
#define LV_DRAW_SW_SUPPORT_XRGB8888     (LV_COLOR_DEPTH == 32)
#define LV_DRAW_SW_SUPPORT_ARGB8888     (LV_COLOR_DEPTH == 32)
#define LV_DRAW_SW_SUPPORT_L8           0
#define LV_DRAW_SW_SUPPORT_AL88         0
#define LV_DRAW_SW_SUPPORT_A8           0
#define LV_DRAW_SW_SUPPORT_I1           0
#define LV_DRAW_SW_COMPLEX              1
#define LV_DRAW_SW_SHADOW_CACHE_SIZE    0
#define LV_DRAW_SW_CIRCLE_CACHE_SIZE    4
#define LV_DRAW_LAYER_SIMPLE_BUF_SIZE   (24U * 1024U)
#define LV_DRAW_THREAD_STACK_SIZE       (8U * 1024U)

/* ----- Widgets we actually use ---------------------------------------- */
#define LV_USE_OBJ                      1
#define LV_USE_LABEL                    1
#define LV_LABEL_TEXT_SELECTION         0
#define LV_LABEL_LONG_TXT_HINT          1
#define LV_USE_LED                      1
#define LV_USE_LINE                     1
#define LV_USE_LIST                     0
#define LV_USE_BAR                      0
#define LV_USE_BUTTON                   0
#define LV_USE_IMAGE                    1
#define LV_USE_ARC                      0
#define LV_USE_ANIMIMG                  0
#define LV_USE_CALENDAR                 0
#define LV_USE_CANVAS                   0
#define LV_USE_CHART                    0
#define LV_USE_CHECKBOX                 0
#define LV_USE_DROPDOWN                 0
#define LV_USE_KEYBOARD                 0
#define LV_USE_MENU                     0
#define LV_USE_MSGBOX                   0
#define LV_USE_ROLLER                   0
#define LV_USE_SCALE                    0
#define LV_USE_SLIDER                   0
#define LV_USE_SPAN                     0
#define LV_USE_SPINBOX                  0
#define LV_USE_SPINNER                  0
#define LV_USE_SWITCH                   0
#define LV_USE_TABLE                    0
#define LV_USE_TABVIEW                  0
#define LV_USE_TEXTAREA                 0
#define LV_USE_TILEVIEW                 0
#define LV_USE_WIN                      0
#define LV_USE_OBSERVER                 0
#define LV_USE_BUTTONMATRIX             0

#define LV_USE_FLEX                     1
#define LV_USE_GRID                     1

/* ----- Themes ---------------------------------------------------------- */
#define LV_USE_THEME_DEFAULT            1
#define LV_THEME_DEFAULT_DARK           1
#define LV_THEME_DEFAULT_GROW           0
#define LV_THEME_DEFAULT_TRANSITION_TIME 80
#define LV_USE_THEME_SIMPLE             1
#define LV_USE_THEME_MONO               0

/* ----- Fonts ----------------------------------------------------------- */
#define LV_FONT_DEFAULT                 &lv_font_montserrat_14
#define LV_FONT_MONTSERRAT_12           0
#define LV_FONT_MONTSERRAT_14           1
#define LV_FONT_MONTSERRAT_16           1
#define LV_FONT_MONTSERRAT_18           0
#define LV_FONT_MONTSERRAT_20           0
#define LV_FONT_MONTSERRAT_22           0
#define LV_FONT_MONTSERRAT_24           1
#define LV_FONT_MONTSERRAT_26           0
#define LV_FONT_MONTSERRAT_28           0
#define LV_FONT_MONTSERRAT_30           0
#define LV_FONT_MONTSERRAT_32           0
#define LV_FONT_MONTSERRAT_36           0
#define LV_FONT_MONTSERRAT_40           0
#define LV_FONT_MONTSERRAT_42           0
#define LV_FONT_MONTSERRAT_44           0
#define LV_FONT_MONTSERRAT_46           0
#define LV_FONT_MONTSERRAT_48           0
#define LV_FONT_UNSCII_8                1
#define LV_FONT_UNSCII_16               0
#define LV_FONT_SOURCE_HAN_SANS_SC_14_CJK    0
#define LV_FONT_SUBPX_BGR               0
#define LV_FONT_FMT_TXT_LARGE           0
#define LV_USE_FONT_PLACEHOLDER         1

/* ----- Misc ------------------------------------------------------------ */
#define LV_USE_PERF_MONITOR             0
#define LV_USE_MEM_MONITOR              0
#define LV_USE_REFR_DEBUG               0
#define LV_USE_OS                       LV_OS_NONE
#define LV_USE_SYSMON                   0
#define LV_USE_PROFILER                 0
#define LV_USE_SNAPSHOT                 0
#define LV_USE_MONKEY                   0
#define LV_USE_GRIDNAV                  0
#define LV_USE_FRAGMENT                 0
#define LV_USE_IMGFONT                  0
#define LV_USE_MSG                      0

/* ----- Compilation hints ---------------------------------------------- */
#define LV_BIG_ENDIAN_SYSTEM            0
#define LV_USE_NATIVE_HELIUM_ASM        0
#define LV_USE_OBJ_PROPERTY             0
#define LV_USE_OBJ_PROPERTY_NAME        0
#define LV_USE_OBJ_ID                   0
#define LV_USE_VG_LITE_THORVG           0

/* ----- Examples / demos / tests off ----------------------------------- */
#define LV_BUILD_EXAMPLES               0
#define LV_USE_DEMO_WIDGETS             0
#define LV_USE_DEMO_KEYPAD_AND_ENCODER  0
#define LV_USE_DEMO_BENCHMARK           0
#define LV_USE_DEMO_RENDER              0
#define LV_USE_DEMO_STRESS              0
#define LV_USE_DEMO_MUSIC               0
#define LV_USE_DEMO_FLEX_LAYOUT         0
#define LV_USE_DEMO_MULTILANG           0
#define LV_USE_DEMO_TRANSFORM           0
#define LV_USE_DEMO_SCROLL              0
#define LV_USE_DEMO_VECTOR_GRAPHIC      0

#endif /* LV_CONF_H */
