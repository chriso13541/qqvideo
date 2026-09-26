# qqvideo Package: MP4 / M4V / MOV Demuxer
# ==========================================
# Build this independently from the main qqvideo build.
#
# Build:
#   cd packages/demuxers/mp4
#   mkdir build && cd build
#   cmake .. -G Ninja          # auto-finds packages/ffmpeg/install/ if present
#   ninja
#
# Or to use system FFmpeg (MSYS2: pacman -S mingw-w64-x86_64-ffmpeg):
#   cmake .. -G Ninja          # falls back to pkg-config automatically
#
# Or to point at a specific FFmpeg build:
#   cmake .. -G Ninja -DFFMPEG_DIR=/path/to/ffmpeg/install

cmake_minimum_required(VERSION 3.10)
project(demux_mp4 C)
set(CMAKE_C_STANDARD 99)

# Locate FFmpeg (local build, custom path, or system pkg-config)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/../../")  # packages/
include(FindQQVideoFFmpeg)

# Resolve qqvideo include/ relative to this package (2 levels up from demuxers/mp4/)
get_filename_component(QQVIDEO_INCLUDE "${CMAKE_SOURCE_DIR}/../../include" ABSOLUTE)
if (NOT EXISTS "${QQVIDEO_INCLUDE}/lumen_plugin.h")
    # Try the root if run from outside the normal tree
    get_filename_component(QQVIDEO_INCLUDE "${CMAKE_SOURCE_DIR}/../../../include" ABSOLUTE)
endif()

# The actual plugin source lives in qqvideo's plugins/ tree
get_filename_component(PLUGIN_SRC
    "${CMAKE_SOURCE_DIR}/../../plugins/demux_mp4/demux_mp4.c" ABSOLUTE)
if (NOT EXISTS "${PLUGIN_SRC}")
    get_filename_component(PLUGIN_SRC
        "${CMAKE_SOURCE_DIR}/../../../plugins/demux_mp4/demux_mp4.c" ABSOLUTE)
endif()

if (NOT EXISTS "${PLUGIN_SRC}")
    message(FATAL_ERROR
        "Could not find demux_mp4.c. Expected at:\n  ${PLUGIN_SRC}\n"
        "This CMakeLists.txt must be run from inside the qqvideo source tree.")
endif()

add_library(demux_mp4 SHARED "${PLUGIN_SRC}")

target_include_directories(demux_mp4 PRIVATE
    "${QQVIDEO_INCLUDE}"
    ${AV_INCDIRS})

target_link_libraries(demux_mp4 ${AV_LIBS})
if (AV_LIBDIRS)
    target_link_directories(demux_mp4 PRIVATE ${AV_LIBDIRS})
endif()

if (NOT MSVC)
    target_compile_options(demux_mp4 PRIVATE -fvisibility=hidden -Wall)
endif()

# Output DLL and manifest go directly into the package folder (next to package.json)
set_target_properties(demux_mp4 PROPERTIES
    OUTPUT_NAME "demux_mp4"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}")

# Copy the manifest alongside the DLL
get_filename_component(MANIFEST_SRC
    "${CMAKE_SOURCE_DIR}/../../plugins/demux_mp4/manifest.manifest.json" ABSOLUTE)
if (NOT EXISTS "${MANIFEST_SRC}")
    get_filename_component(MANIFEST_SRC
        "${CMAKE_SOURCE_DIR}/../../../plugins/demux_mp4/manifest.manifest.json" ABSOLUTE)
endif()
if (EXISTS "${MANIFEST_SRC}")
    configure_file("${MANIFEST_SRC}" "${CMAKE_SOURCE_DIR}/demux_mp4.manifest.json" COPYONLY)
endif()

message(STATUS "")
message(STATUS "  Package: demux_mp4 (MP4 / M4V / MOV demuxer)")
message(STATUS "  FFmpeg:  ${FFMPEG_SOURCE}")
message(STATUS "  Output:  ${CMAKE_SOURCE_DIR}/")
message(STATUS "")
