cmake_minimum_required(VERSION 3.16)

set(FR_REWRITE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH
    "Frothy rewrite repository root")
set(FR_REWRITE_BOARD "esp32_devkit_v1" CACHE STRING "Frothy board")
set(FR_REWRITE_PROFILE "esp32_plain" CACHE STRING "Frothy profile")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(frothy)
