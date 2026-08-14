cmake_minimum_required(VERSION 3.22)

set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(blip-led-transport-qualification)
