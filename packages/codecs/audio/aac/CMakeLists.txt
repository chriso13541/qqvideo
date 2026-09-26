# qqvideo Package: AAC Audio Decoder
# ====================================

cmake_minimum_required(VERSION 3.10)
project(decoder_aac C)
set(CMAKE_C_STANDARD 99)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/../../../")
include(FindQQVideoFFmpeg)

get_filename_component(QQVIDEO_INCLUDE "${CMAKE_SOURCE_DIR}/../../../include" ABSOLUTE)
if (NOT EXISTS "${QQVIDEO_INCLUDE}/lumen_plugin.h")
    get_filename_component(QQVIDEO_INCLUDE "${CMAKE_SOURCE_DIR}/../../../../include" ABSOLUTE)
endif()

get_filename_component(PLUGIN_SRC
    "${CMAKE_SOURCE_DIR}/../../../plugins/decoder_aac/decoder_aac.c" ABSOLUTE)
if (NOT EXISTS "${PLUGIN_SRC}")
    get_filename_component(PLUGIN_SRC
        "${CMAKE_SOURCE_DIR}/../../../../plugins/decoder_aac/decoder_aac.c" ABSOLUTE)
endif()

if (NOT EXISTS "${PLUGIN_SRC}")
    message(FATAL_ERROR "Could not find decoder_aac.c relative to this CMakeLists.")
endif()

add_library(decoder_aac SHARED "${PLUGIN_SRC}")

target_include_directories(decoder_aac PRIVATE "${QQVIDEO_INCLUDE}" ${AV_INCDIRS})
target_link_libraries(decoder_aac ${AV_LIBS})
if (AV_LIBDIRS)
    target_link_directories(decoder_aac PRIVATE ${AV_LIBDIRS})
endif()

if (NOT MSVC)
    target_compile_options(decoder_aac PRIVATE -fvisibility=hidden -Wall)
endif()

set_target_properties(decoder_aac PROPERTIES
    OUTPUT_NAME "decoder_aac"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}")

get_filename_component(MANIFEST_SRC
    "${CMAKE_SOURCE_DIR}/../../../plugins/decoder_aac/manifest.manifest.json" ABSOLUTE)
if (NOT EXISTS "${MANIFEST_SRC}")
    get_filename_component(MANIFEST_SRC
        "${CMAKE_SOURCE_DIR}/../../../../plugins/decoder_aac/manifest.manifest.json" ABSOLUTE)
endif()
if (EXISTS "${MANIFEST_SRC}")
    configure_file("${MANIFEST_SRC}" "${CMAKE_SOURCE_DIR}/decoder_aac.manifest.json" COPYONLY)
endif()

message(STATUS "")
message(STATUS "  Package: decoder_aac (AAC audio decoder)")
message(STATUS "  FFmpeg:  ${FFMPEG_SOURCE}")
message(STATUS "  Output:  ${CMAKE_SOURCE_DIR}/")
message(STATUS "")
