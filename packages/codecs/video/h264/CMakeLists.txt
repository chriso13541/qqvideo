# qqvideo Package: H.264 / AVC Video Decoder
# ===========================================
# Build independently. Auto-detects packages/ffmpeg/install/ or uses system FFmpeg.
#
# Build:
#   cd packages/codecs/video/h264
#   mkdir build && cd build && cmake .. -G Ninja && ninja

cmake_minimum_required(VERSION 3.10)
project(decoder_h264 C)
set(CMAKE_C_STANDARD 99)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/../../../")  # packages/
include(FindQQVideoFFmpeg)

get_filename_component(QQVIDEO_INCLUDE "${CMAKE_SOURCE_DIR}/../../../include" ABSOLUTE)
if (NOT EXISTS "${QQVIDEO_INCLUDE}/lumen_plugin.h")
    get_filename_component(QQVIDEO_INCLUDE "${CMAKE_SOURCE_DIR}/../../../../include" ABSOLUTE)
endif()

get_filename_component(PLUGIN_SRC
    "${CMAKE_SOURCE_DIR}/../../../plugins/decoder_h264/decoder_h264.c" ABSOLUTE)
if (NOT EXISTS "${PLUGIN_SRC}")
    get_filename_component(PLUGIN_SRC
        "${CMAKE_SOURCE_DIR}/../../../../plugins/decoder_h264/decoder_h264.c" ABSOLUTE)
endif()

if (NOT EXISTS "${PLUGIN_SRC}")
    message(FATAL_ERROR "Could not find decoder_h264.c relative to this CMakeLists.")
endif()

add_library(decoder_h264 SHARED "${PLUGIN_SRC}")

target_include_directories(decoder_h264 PRIVATE "${QQVIDEO_INCLUDE}" ${AV_INCDIRS})
target_link_libraries(decoder_h264 ${AV_LIBS})
if (AV_LIBDIRS)
    target_link_directories(decoder_h264 PRIVATE ${AV_LIBDIRS})
endif()

if (NOT MSVC)
    target_compile_options(decoder_h264 PRIVATE -fvisibility=hidden -Wall)
endif()

set_target_properties(decoder_h264 PROPERTIES
    OUTPUT_NAME "decoder_h264"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}")

get_filename_component(MANIFEST_SRC
    "${CMAKE_SOURCE_DIR}/../../../plugins/decoder_h264/manifest.manifest.json" ABSOLUTE)
if (NOT EXISTS "${MANIFEST_SRC}")
    get_filename_component(MANIFEST_SRC
        "${CMAKE_SOURCE_DIR}/../../../../plugins/decoder_h264/manifest.manifest.json" ABSOLUTE)
endif()
if (EXISTS "${MANIFEST_SRC}")
    configure_file("${MANIFEST_SRC}" "${CMAKE_SOURCE_DIR}/decoder_h264.manifest.json" COPYONLY)
endif()

message(STATUS "")
message(STATUS "  Package: decoder_h264 (H.264 / AVC video decoder)")
message(STATUS "  FFmpeg:  ${FFMPEG_SOURCE}")
message(STATUS "  Output:  ${CMAKE_SOURCE_DIR}/")
message(STATUS "")
