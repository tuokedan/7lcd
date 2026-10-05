cmake_minimum_required(VERSION 3.5)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/components/esp32-camera"
    "${CMAKE_CURRENT_LIST_DIR}/components/screen"
    "${CMAKE_CURRENT_LIST_DIR}/components/bus"
    "${CMAKE_CURRENT_LIST_DIR}/components/decoder_ijg"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

add_compile_options(-fdiagnostics-color=always)

project(esp32s3-camera)