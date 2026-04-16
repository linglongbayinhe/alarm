/**
 * @file lv_conf.h
 * LVGL v9.2.x configuration for this project.
 *
 * Based on lv_conf_template.h from lvgl 9.2; only project-relevant options are enabled.
 * Included via LV_CONF_INCLUDE_SIMPLE; main/ is added to the lvgl component include path
 * in the root CMakeLists.txt.
 */

/* clang-format off */
#if 1 /* Project LVGL configuration enabled */

#ifndef LV_CONF_H
#define LV_CONF_H

/*====================
   COLOR SETTINGS
 *====================*/

/*Color depth: 1 (I1), 8 (L8), 16 (RGB565), 24 (RGB888), 32 (XRGB8888)*/
#define LV_COLOR_DEPTH 16

/*=========================
   STDLIB WRAPPER SETTINGS
 *=========================*/

#define LV_USE_STDLIB_MALLOC  LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_STRING  LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_BUILTIN

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_BUILTIN
    #define LV_MEM_SIZE (48U * 1024U)  /*[bytes]*/
    #define LV_MEM_POOL_EXPAND_SIZE 0
    #define LV_MEM_ADR 0
    #if LV_MEM_ADR == 0
        #undef LV_MEM_POOL_INCLUDE
        #undef LV_MEM_POOL_ALLOC
    #endif
#endif

/*====================
   HAL SETTINGS
 *====================*/

#define LV_DEF_REFR_PERIOD 33      /*[ms]*/
#define LV_DPI_DEF          130    /*[px/inch]*/

/*=================
 * OPERATING SYSTEM
 *=================*/

#define LV_USE_OS LV_OS_NONE

/*========================
 * RENDERING CONFIGURATION
 *========================*/

#define LV_DRAW_BUF_STRIDE_ALIGN   1
#define LV_DRAW_BUF_ALIGN          4
#define LV_DRAW_TRANSFORM_USE_MATRIX 0
#define LV_DRAW_LAYER_SIMPLE_BUF_SIZE (24 * 1024)
#define LV_DRAW_THREAD_STACK_SIZE     (8 * 1024)

#define LV_USE_DRAW_SW 1
#if LV_USE_DRAW_SW == 1
    #define LV_DRAW_SW_SUPPORT_RGB565   1
    #define LV_DRAW_SW_SUPPORT_RGB565A8 1
    #define LV_DRAW_SW_SUPPORT_RGB888   1
    #define LV_DRAW_SW_SUPPORT_XRGB8888 1
    #define LV_DRAW_SW_SUPPORT_ARGB8888 1
    #define LV_DRAW_SW_SUPPORT_L8       1
    #define LV_DRAW_SW_SUPPORT_AL88     1
    #define LV_DRAW_SW_SUPPORT_A8       1
    #define LV_DRAW_SW_SUPPORT_I1       1
    #define LV_DRAW_SW_DRAW_UNIT_CNT    1
    #define LV_USE_DRAW_ARM2D_SYNC      0
    #define LV_USE_NATIVE_HELIUM_ASM    0
    #define LV_DRAW_SW_COMPLEX          1
    #if LV_DRAW_SW_COMPLEX == 1
        #define LV_DRAW_SW_SHADOW_CACHE_SIZE 0
        #define LV_DRAW_SW_CIRCLE_CACHE_SIZE 4
    #endif
    #define LV_USE_DRAW_SW_ASM LV_DRAW_SW_ASM_NONE
    #define LV_USE_DRAW_SW_COMPLEX_GRADIENTS 0
#endif

#define LV_USE_DRAW_VGLITE    0
#define LV_USE_PXP            0
#define LV_USE_DRAW_DAVE2D    0
#define LV_USE_DRAW_SDL       0
#define LV_USE_DRAW_VG_LITE   0

/*=======================
 * FEATURE CONFIGURATION
 *=======================*/

/*--- Logging ---*/
#define LV_USE_LOG 0

/*--- Asserts ---*/
#define LV_USE_ASSERT_NULL          1
#define LV_USE_ASSERT_MALLOC        1
#define LV_USE_ASSERT_STYLE         0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ           0
#define LV_ASSERT_HANDLER_INCLUDE <stdint.h>
#define LV_ASSERT_HANDLER while(1);

/*--- Debug ---*/
#define LV_USE_REFR_DEBUG          0
#define LV_USE_LAYER_DEBUG         0
#define LV_USE_PARALLEL_DRAW_DEBUG 0

/*--- Others ---*/
#define LV_ENABLE_GLOBAL_CUSTOM       0
#define LV_CACHE_DEF_SIZE             0
#define LV_IMAGE_HEADER_CACHE_DEF_CNT 0
#define LV_GRADIENT_MAX_STOPS         2
#define LV_COLOR_MIX_ROUND_OFS        0
#define LV_OBJ_STYLE_CACHE            0
#define LV_USE_OBJ_ID                 0
#define LV_OBJ_ID_AUTO_ASSIGN         LV_USE_OBJ_ID
#define LV_USE_OBJ_ID_BUILTIN         1
#define LV_USE_OBJ_PROPERTY           0
#define LV_USE_OBJ_PROPERTY_NAME      1
#define LV_USE_VG_LITE_THORVG         0

/*==================
 *   FONT USAGE
 *===================*/

#define LV_FONT_MONTSERRAT_14 1

#define LV_FONT_MONTSERRAT_28_COMPRESSED 0
#define LV_FONT_DEJAVU_16_PERSIAN_HEBREW 0
#define LV_FONT_SIMSUN_14_CJK           0
#define LV_FONT_SIMSUN_16_CJK           0
#define LV_FONT_UNSCII_8                0
#define LV_FONT_UNSCII_16               0

#define LV_FONT_CUSTOM_DECLARE LV_FONT_DECLARE(SourceHanSans_Normal_16)

#define LV_FONT_DEFAULT &lv_font_montserrat_14

#define LV_FONT_FMT_TXT_LARGE 0
#define LV_USE_FONT_PLACEHOLDER 1

/*=================
 *  TEXT SETTINGS
 *=================*/

#define LV_TXT_ENC LV_TXT_ENC_UTF8
#define LV_TXT_BREAK_CHARS " ,.;:-_)]}\/"
#define LV_TXT_LINE_BREAK_LONG_LEN 0
#define LV_TXT_LINE_BREAK_LONG_PRE_MIN_LEN  3
#define LV_TXT_LINE_BREAK_LONG_POST_MIN_LEN 3

/*==================
 *  WIDGETS
 *==================*/

#define LV_WIDGETS_HAS_DEFAULT_VALUE 1

#define LV_USE_ANIMIMAGE  1
#define LV_USE_ARC        1
#define LV_USE_BAR        1
#define LV_USE_BUTTON     1
#define LV_USE_BUTTONMATRIX 1
#define LV_USE_CALENDAR   1
#define LV_USE_CANVAS     1
#define LV_USE_CHART      1
#define LV_USE_CHECKBOX   1
#define LV_USE_DROPDOWN   1
#define LV_USE_IMAGE      1
#define LV_USE_IMAGEBUTTON 1
#define LV_USE_KEYBOARD   1
#define LV_USE_LABEL      1
#if LV_USE_LABEL
    #define LV_LABEL_TEXT_SELECTION 1
    #define LV_LABEL_LONG_TXT_HINT 1
    #define LV_LABEL_WAIT_CHAR_COUNT 3
#endif
#define LV_USE_LED        1
#define LV_USE_LINE       1
#define LV_USE_LIST       1
#define LV_USE_MENU       1
#define LV_USE_MSGBOX     1
#define LV_USE_ROLLER     1
#define LV_USE_SCALE      1
#define LV_USE_SLIDER     1
#define LV_USE_SPAN       1
#if LV_USE_SPAN
    #define LV_SPAN_SNIPPET_STACK_SIZE 64
#endif
#define LV_USE_SPINBOX    1
#define LV_USE_SPINNER    1
#define LV_USE_SWITCH     1
#define LV_USE_TABLE      1
#define LV_USE_TABVIEW    1
#define LV_USE_TEXTAREA   1
#define LV_USE_TILEVIEW   1
#define LV_USE_WIN        1

/*==================
 * THEMES
 *==================*/

#define LV_USE_THEME_DEFAULT 1
#if LV_USE_THEME_DEFAULT
    #define LV_THEME_DEFAULT_DARK 0
    #define LV_THEME_DEFAULT_GROW 1
    #define LV_THEME_DEFAULT_TRANSITION_TIME 80
#endif

#define LV_USE_THEME_SIMPLE 1
#define LV_USE_THEME_MONO   1

/*==================
 * LAYOUTS
 *==================*/

#define LV_USE_FLEX 1
#define LV_USE_GRID 1

/*==================
 * 3RD PARTY LIBS
 *==================*/

#define LV_USE_FS_STDIO  0
#define LV_USE_FS_POSIX  0
#define LV_USE_FS_WIN32  0
#define LV_USE_FS_FATFS  0
#define LV_USE_FS_LITTLEFS 0
#define LV_USE_FS_MEMFS  0
#define LV_USE_LODEPNG   0
#define LV_USE_LIBPNG    0
#define LV_USE_BMP       0
#define LV_USE_RLE       0
#define LV_USE_TJPGD     0
#define LV_USE_LIBJPEG_TURBO 0
#define LV_USE_GIF       0
#define LV_USE_QRCODE    0
#define LV_USE_BARCODE   0
#define LV_USE_FREETYPE  0
#define LV_USE_TINY_TTF  0
#define LV_USE_RLOTTIE   0
#define LV_USE_FFMPEG    0
#define LV_USE_THORVG_INTERNAL 0
#define LV_USE_THORVG_EXTERNAL 0
#define LV_USE_LZ4_INTERNAL    0
#define LV_USE_LZ4_EXTERNAL    0
#define LV_USE_SNAPSHOT         1
#define LV_USE_SYSMON           0
#define LV_USE_PROFILER         0
#define LV_USE_MONKEY           0
#define LV_USE_GRIDNAV          0
#define LV_USE_FRAGMENT          0
#define LV_USE_IMGFONT           0
#define LV_USE_OBSERVER          1
#define LV_USE_IME_PINYIN        0

/*==================
 * DEMOS
 *==================*/

#define LV_USE_DEMO_WIDGETS        0
#define LV_USE_DEMO_KEYPAD_AND_ENCODER 0
#define LV_USE_DEMO_BENCHMARK      0
#define LV_USE_DEMO_STRESS         0
#define LV_USE_DEMO_MUSIC          0
#define LV_USE_DEMO_RENDER         0
#define LV_USE_DEMO_SCROLL         0
#define LV_USE_DEMO_VECTOR_GRAPHIC 0
#define LV_USE_DEMO_TRANSFORM      0

#endif /* LV_CONF_H */
#endif /* Content enable */
