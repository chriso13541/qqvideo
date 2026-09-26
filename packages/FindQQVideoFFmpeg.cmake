# FindQQVideoFFmpeg.cmake
# ========================
# Finds FFmpeg for qqvideo package builds. Included by per-package CMakeLists.txt.
#
# Search order:
#   1. -DFFMPEG_DIR=/path explicitly passed to cmake
#   2. Auto-detect: walks up the directory tree looking for ffmpeg/install/
#      (so packages/demuxers/mp4/ finds packages/ffmpeg/install/ automatically)
#   3. System FFmpeg via pkg-config (e.g. MSYS2's pacman-installed ffmpeg)
#
# After inclusion, the following variables are set:
#   AV_INCDIRS   - include directories
#   AV_LIBDIRS   - library directories (for system builds)
#   AV_LIBS      - libraries to link (full paths for local builds, names for system)
#   FFMPEG_SOURCE - "local" or "system"

# NOTE: set(... CACHE ...) takes exactly ONE docstring argument. This used
# to pass two, which makes CMake silently treat the whole call as a normal
# list variable (";CACHE;PATH;...") -- non-empty, so auto-detection never
# ran and a -DFFMPEG_DIR= on the command line was shadowed.
set(FFMPEG_DIR "" CACHE PATH
    "Built FFmpeg install prefix (e.g. /c/dev/lumen/packages/ffmpeg/install). Empty = auto-detect packages/ffmpeg/install/, then system pkg-config.")

if (NOT FFMPEG_DIR)
    # Walk up from this package's CMakeLists directory, checking each level for
    # packages/ffmpeg/install/ -- works regardless of how deep the package is nested.
    set(_walk "${CMAKE_CURRENT_SOURCE_DIR}")
    foreach(_level 1 2 3 4 5)
        get_filename_component(_parent "${_walk}/.." ABSOLUTE)
        if (EXISTS "${_parent}/ffmpeg/install/include/libavcodec/avcodec.h")
            set(FFMPEG_DIR "${_parent}/ffmpeg/install")
            message(STATUS "[qqvideo] Auto-detected local FFmpeg build: ${FFMPEG_DIR}")
            break()
        endif()
        set(_walk "${_parent}")
    endforeach()
endif()

if (FFMPEG_DIR)
    # ---- Local FFmpeg build (static libs) --------------------------------
    set(FFMPEG_SOURCE "local")
    set(AV_INCDIRS "${FFMPEG_DIR}/include")
    set(AV_LIBDIRS "")  # not needed, using full paths

    macro(_find_av_lib _var _name)
        find_library(${_var} NAMES "${_name}"
            PATHS "${FFMPEG_DIR}/lib" NO_DEFAULT_PATH)
        if (NOT ${_var})
            message(FATAL_ERROR "[qqvideo] Could not find ${_name} in ${FFMPEG_DIR}/lib.\n"
                "Run packages/ffmpeg/build.sh first, or pass -DFFMPEG_DIR= to cmake.")
        endif()
        message(STATUS "[qqvideo] Found ${_name}: ${${_var}}")
    endmacro()

    _find_av_lib(AV_avformat_LIB    avformat)
    _find_av_lib(AV_avcodec_LIB     avcodec)
    _find_av_lib(AV_avutil_LIB      avutil)
    _find_av_lib(AV_swresample_LIB  swresample)

    set(AV_LIBS
        "${AV_avformat_LIB}"
        "${AV_avcodec_LIB}"
        "${AV_avutil_LIB}"
        "${AV_swresample_LIB}")

    # Static FFmpeg on Windows requires these system libraries at final link time
    if (WIN32)
        list(APPEND AV_LIBS ws2_32 bcrypt)
    endif()

    message(STATUS "[qqvideo] Using local FFmpeg static build: ${FFMPEG_DIR}")

else()
    # ---- System FFmpeg (dynamic, via pkg-config) -------------------------
    set(FFMPEG_SOURCE "system")
    find_package(PkgConfig REQUIRED)

    pkg_check_modules(LIBAVFORMAT   REQUIRED libavformat)
    pkg_check_modules(LIBAVCODEC    REQUIRED libavcodec)
    pkg_check_modules(LIBAVUTIL     REQUIRED libavutil)
    pkg_check_modules(LIBSWRESAMPLE REQUIRED libswresample)

    set(AV_INCDIRS
        ${LIBAVFORMAT_INCLUDE_DIRS}
        ${LIBAVCODEC_INCLUDE_DIRS}
        ${LIBAVUTIL_INCLUDE_DIRS}
        ${LIBSWRESAMPLE_INCLUDE_DIRS})
    set(AV_LIBDIRS
        ${LIBAVFORMAT_LIBRARY_DIRS}
        ${LIBAVCODEC_LIBRARY_DIRS}
        ${LIBAVUTIL_LIBRARY_DIRS}
        ${LIBSWRESAMPLE_LIBRARY_DIRS})
    set(AV_LIBS
        ${LIBAVFORMAT_LIBRARIES}
        ${LIBAVCODEC_LIBRARIES}
        ${LIBAVUTIL_LIBRARIES}
        ${LIBSWRESAMPLE_LIBRARIES})

    message(STATUS "[qqvideo] Using system FFmpeg via pkg-config")
    message(STATUS "  avformat: ${LIBAVFORMAT_VERSION}")
    message(STATUS "  avcodec:  ${LIBAVCODEC_VERSION}")
endif()
