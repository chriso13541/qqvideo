# CMake toolchain file: cross-compile qqvideo for 64-bit Windows from Linux
# with MinGW-w64. Normally used through tools/build-windows.sh, which also
# fetches SDL2 and packages the result; by hand:
#
#   cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake \
#         -DQQ_WIN_DEPS=/path/to/deps -DLUMEN_FFMPEG_PREFIX=/path/to/ffmpeg-win64
#
# QQ_WIN_DEPS     prefix holding the Windows SDL2 (lib/pkgconfig/sdl2.pc ...)
# QQ_MINGW_TRIPLE compiler prefix (default x86_64-w64-mingw32). On Debian/
#                 Ubuntu the "-posix" compiler variants are used if present.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if (NOT QQ_MINGW_TRIPLE)
  set(QQ_MINGW_TRIPLE x86_64-w64-mingw32)
endif()

# Debian/Ubuntu ship win32- and posix-threaded variants of each compiler;
# Gentoo's crossdev has just the plain name.
find_program(_qq_cc  NAMES ${QQ_MINGW_TRIPLE}-gcc-posix ${QQ_MINGW_TRIPLE}-gcc REQUIRED)
find_program(_qq_cxx NAMES ${QQ_MINGW_TRIPLE}-g++-posix ${QQ_MINGW_TRIPLE}-g++ REQUIRED)
find_program(_qq_rc  NAMES ${QQ_MINGW_TRIPLE}-windres REQUIRED)
set(CMAKE_C_COMPILER   "${_qq_cc}")
set(CMAKE_CXX_COMPILER "${_qq_cxx}")
set(CMAKE_RC_COMPILER  "${_qq_rc}")

# Look for libraries/headers in the MinGW sysroot and our deps only, never
# the Linux host's (a host libSDL2.so would "link" and then fail).
# (Anything outside these roots gets re-rooted under them, so the FFmpeg
# prefix has to be listed too or its libraries won't be found.)
set(CMAKE_FIND_ROOT_PATH /usr/${QQ_MINGW_TRIPLE} /usr/${QQ_MINGW_TRIPLE}/usr
    ${QQ_WIN_DEPS} ${LUMEN_FFMPEG_PREFIX})
# try_compile() re-reads this file in a fresh project: hand it our settings.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES QQ_WIN_DEPS QQ_MINGW_TRIPLE LUMEN_FFMPEG_PREFIX)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Same for pkg-config: PKG_CONFIG_LIBDIR *replaces* the default search path,
# so only the Windows .pc files are seen. (LUMEN_FFMPEG_PREFIX adds the
# FFmpeg one via PKG_CONFIG_PATH, which is searched first.)
if (QQ_WIN_DEPS)
  set(ENV{PKG_CONFIG_LIBDIR} "${QQ_WIN_DEPS}/lib/pkgconfig")
else()
  set(ENV{PKG_CONFIG_LIBDIR} "/nonexistent")
endif()
set(ENV{PKG_CONFIG_SYSROOT_DIR} "")
