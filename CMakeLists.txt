# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)
project(blufi)

# LVGL is built with LV_CONF_INCLUDE_SIMPLE; pick up lv_conf.h from main/
# Managed package lvgl/lvgl registers as component lvgl__lvgl (see build log "Components:" list).
idf_component_get_property(_lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_include_directories(${_lvgl_lib} PRIVATE "${CMAKE_SOURCE_DIR}/main")
