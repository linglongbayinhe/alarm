# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)

project(blufi)

# Keep the Emote pack outside the application image and include it in the
# normal `idf.py flash` target.
set(EMOTE_ASSET_IMAGE "${CMAKE_SOURCE_DIR}/assets/emote/normal.bin")
if(NOT EXISTS "${EMOTE_ASSET_IMAGE}")
    message(FATAL_ERROR "Missing Emote asset pack: ${EMOTE_ASSET_IMAGE}")
endif()
esptool_py_flash_to_partition(flash emote_gen "${EMOTE_ASSET_IMAGE}")

# LVGL is built with LV_CONF_INCLUDE_SIMPLE; pick up lv_conf.h from main/
# Managed package lvgl/lvgl registers as component lvgl__lvgl (see build log "Components:" list).
idf_component_get_property(_lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_include_directories(${_lvgl_lib} PRIVATE "${CMAKE_SOURCE_DIR}/main")

# LVGL and esp_emote_gfx both vendor Nayuki's qrcodegen C implementation.
# Prefix the private esp_emote_gfx copy so the existing LVGL provisioning QR
# implementation can remain enabled without duplicate linker symbols.
idf_component_get_property(_emote_gfx_lib espressif2022__esp_emote_gfx COMPONENT_LIB)
include("${CMAKE_SOURCE_DIR}/cmake/emote_anim_memory_patch.cmake")
blufi_apply_emote_anim_memory_patch(${_emote_gfx_lib})
target_compile_definitions(${_emote_gfx_lib} PRIVATE
    qrcodegen_encodeText=emote_qrcodegen_encodeText
    qrcodegen_encodeBinary=emote_qrcodegen_encodeBinary
    qrcodegen_encodeSegments=emote_qrcodegen_encodeSegments
    qrcodegen_encodeSegmentsAdvanced=emote_qrcodegen_encodeSegmentsAdvanced
    qrcodegen_isAlphanumeric=emote_qrcodegen_isAlphanumeric
    qrcodegen_isNumeric=emote_qrcodegen_isNumeric
    qrcodegen_calcSegmentBufferSize=emote_qrcodegen_calcSegmentBufferSize
    qrcodegen_makeBytes=emote_qrcodegen_makeBytes
    qrcodegen_makeNumeric=emote_qrcodegen_makeNumeric
    qrcodegen_makeAlphanumeric=emote_qrcodegen_makeAlphanumeric
    qrcodegen_makeEci=emote_qrcodegen_makeEci
    qrcodegen_getSize=emote_qrcodegen_getSize
    qrcodegen_getModule=emote_qrcodegen_getModule
)

# The pinned player revision contains one internal error-path variable that is
# intentionally written by ESP_GOTO_ON_* macros but not read afterwards.
idf_component_get_property(_emote_player_lib esp_emote_gen_player COMPONENT_LIB)
target_compile_options(${_emote_player_lib} PRIVATE -Wno-unused-but-set-variable)
