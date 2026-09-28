cmake_minimum_required(VERSION 3.5)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# config/lv_assert_abort.h, LVGL's assert handler (see there), is included by
# name from LVGL's sources, so its directory goes on every include path.
idf_build_set_property(COMPILE_OPTIONS "-I${CMAKE_CURRENT_LIST_DIR}/config" APPEND)

project(multi_display)
