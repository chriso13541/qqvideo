cmake_minimum_required(VERSION 3.10)
project(qqvideo C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
# No CMAKE_LIBRARY_OUTPUT_DIRECTORY for plugins/ -- packages go to their
# own subdirectories in bin/packages/; plugins/ is no longer used.

if (NOT MSVC)
  add_compile_options(-Wall -Wextra -fvisibility=hidden)
endif()

# ---- Helper: deploy a package (DLL + manifest) into its packages/ tree ----
# deploy_package(<target> <pkg_subdir> <manifest_src_dir>)
#   target           CMake target name
#   pkg_subdir       path relative to bin/packages/ (e.g. "mp4/video/h264")
#   manifest_src_dir source dir under plugins/ containing manifest.manifest.json
macro(deploy_package target pkg_subdir manifest_src_dir)
    set(_dest "${CMAKE_BINARY_DIR}/bin/packages/${pkg_subdir}")
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_dest}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:${target}>"
            "${_dest}/$<TARGET_FILE_NAME:${target}>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/plugins/${manifest_src_dir}/manifest.manifest.json"
            "${_dest}/${target}.manifest.json"
        COMMENT "Deploying ${target} to packages/${pkg_subdir}/")
endmacro()

# ---- FFmpeg source -------------------------------------------------------
# ON (default on Linux): link the system FFmpeg found by pkg-config and
#   build decoder_libav + demux_libav right here, into bin/plugins/.
#   "System FFmpeg" = apt/dnf/pacman's, or a source build installed to
#   /usr/local -- pkg-config searches /usr/local first on Debian/Ubuntu,
#   so the newer one wins automatically. To force a specific install:
#     PKG_CONFIG_PATH=/opt/ffmpeg/lib/pkgconfig cmake ..
# OFF (default on Windows): the per-codec packages/ flow (packages/BUILD.md).
if (WIN32)
  set(_lumen_sys_default OFF)
else()
  set(_lumen_sys_default ON)
endif()
option(LUMEN_SYSTEM_FFMPEG "Use the system FFmpeg via pkg-config" ${_lumen_sys_default})

# Optional: use an FFmpeg installed somewhere pkg-config doesn't look by
# default (e.g. a source build in /opt/ffmpeg-9 or ~/ffmpeg-install).
# Not needed for /usr/local -- that's searched automatically.
#   cmake -S . -B build -DLUMEN_FFMPEG_PREFIX=/opt/ffmpeg-9
set(LUMEN_FFMPEG_PREFIX "" CACHE PATH "FFmpeg install prefix to build against (empty = system default)")
if (LUMEN_FFMPEG_PREFIX)
  set(ENV{PKG_CONFIG_PATH}
      "${LUMEN_FFMPEG_PREFIX}/lib/pkgconfig:${LUMEN_FFMPEG_PREFIX}/lib64/pkgconfig:$ENV{PKG_CONFIG_PATH}")
endif()

if (UNIX)
  include(GNUInstallDirs)
  # Installed plugins keep an RPATH to any non-system library dir they
  # linked against, so a custom-prefix FFmpeg is still found at runtime
  # after `cmake --install` -- no LD_LIBRARY_PATH needed. System dirs
  # (/usr/lib/...) are never added.
  set(CMAKE_INSTALL_RPATH_USE_LINK_PATH ON)
endif()

# ---- Detect optional build dependencies ---------------------------------
find_package(PkgConfig QUIET)
if (PkgConfig_FOUND)
  pkg_check_modules(SDL2          sdl2)
  pkg_check_modules(LIBAVCODEC    libavcodec)
  pkg_check_modules(LIBAVUTIL     libavutil)
  pkg_check_modules(LIBSWRESAMPLE libswresample)
  pkg_check_modules(LIBAVFORMAT   libavformat)
endif()

# ---- Core executable ----------------------------------------------------
# output_sdl2 and output_sdl2_audio are compiled DIRECTLY into lumen-play.
# They are NOT plugins, packages, or loadable DLLs.  The player window is
# always available regardless of which codec packages are installed.

if (SDL2_FOUND)
  set(BUILTIN_OUTPUT_SOURCES
      plugins/output_sdl2/output_sdl2.cpp
      plugins/output_sdl2_audio/output_sdl2_audio.c
      third_party/imgui/imgui.cpp
      third_party/imgui/imgui_draw.cpp
      third_party/imgui/imgui_tables.cpp
      third_party/imgui/imgui_widgets.cpp
      third_party/imgui/imgui_impl_sdl2.cpp
      third_party/imgui/imgui_impl_sdlrenderer2.cpp
      third_party/fonts/lumen_font.cpp
  )
else()
  set(BUILTIN_OUTPUT_SOURCES "")
  message(STATUS "SDL2 NOT found -- player window will be unavailable")
endif()

if (WIN32)
  # Windows resource file embeds the ICO so Explorer and the taskbar show
  # the qqvideo icon rather than the generic application icon.
  set(WIN_RC resources/qqvideo.rc)
else()
  set(WIN_RC "")
endif()

add_executable(qqvideo
    core/main.c
    core/player_core.c
    core/registry.c
    core/packages.c
    core/plugin_loader.c
    core/minijson.c
    core/builtin_output_fallback.c
    third_party/tinyfiledialogs/tinyfiledialogs.c
    ${BUILTIN_OUTPUT_SOURCES}
    ${WIN_RC}
)
target_include_directories(qqvideo PRIVATE
    include core
    third_party/tinyfiledialogs
    third_party/imgui
    third_party/fonts
    third_party
    resources
    ${SDL2_INCLUDE_DIRS})
target_compile_definitions(qqvideo PRIVATE LUMEN_BUILTIN)
target_link_libraries(qqvideo ${SDL2_LIBRARIES})
target_link_directories(qqvideo PRIVATE ${SDL2_LIBRARY_DIRS})
if (UNIX)
  target_link_libraries(qqvideo dl)
endif()
if (WIN32)
  target_link_libraries(qqvideo comdlg32 ole32)
endif()

# ---- System-FFmpeg plugins ------------------------------------------------
if (LUMEN_SYSTEM_FFMPEG)
  find_package(PkgConfig REQUIRED)
  # Floor = FFmpeg 5.1 (Debian 12). See plugins/common/libav_common.h for why.
  pkg_check_modules(LIBAV REQUIRED IMPORTED_TARGET
      libavcodec>=59.37.100
      libavformat>=59.27.100
      libavutil>=57.28.100
      libswresample>=4.7.100
      libswscale>=6.7.100)

  target_compile_definitions(qqvideo PRIVATE LUMEN_SYSTEM_FFMPEG)

  set(_lumen_plugin_dir "${CMAKE_BINARY_DIR}/bin/plugins")
  foreach(_p decoder_libav demux_libav)
    add_library(${_p} MODULE plugins/${_p}/${_p}.c)
    target_include_directories(${_p} PRIVATE include)
    target_link_libraries(${_p} PRIVATE PkgConfig::LIBAV)
    set_target_properties(${_p} PROPERTIES
        PREFIX "lib"
        LIBRARY_OUTPUT_DIRECTORY "${_lumen_plugin_dir}")
    add_custom_command(TARGET ${_p} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/plugins/${_p}/manifest.manifest.json"
            "${_lumen_plugin_dir}/${_p}.manifest.json")
    add_dependencies(qqvideo ${_p})
  endforeach()

  # ---- Install (Linux): sudo cmake --install build ----------------------
  #   <prefix>/bin/qqvideo
  #   <prefix>/<libdir>/qqvideo/plugins/   decoder_libav, demux_libav + manifests
  #   <prefix>/share/applications/qqvideo.desktop
  #   <prefix>/share/icons/hicolor/64x64/apps/qqvideo.png
  # Default prefix is /usr/local, which app launchers already search.
  if (UNIX)
    set(_lumen_plugin_install_dir "${CMAKE_INSTALL_LIBDIR}/qqvideo/plugins")
    # Path from bin/ to the plugin dir, baked into main.c so an installed
    # binary finds its plugins (and still works under `--prefix X`).
    file(RELATIVE_PATH _lumen_plugin_reldir
         "${CMAKE_INSTALL_FULL_BINDIR}"
         "${CMAKE_INSTALL_FULL_LIBDIR}/qqvideo/plugins")
    target_compile_definitions(qqvideo PRIVATE "LUMEN_PLUGIN_RELDIR=\"${_lumen_plugin_reldir}\"")

    install(TARGETS qqvideo RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
    install(TARGETS decoder_libav demux_libav LIBRARY DESTINATION "${_lumen_plugin_install_dir}")
    foreach(_p decoder_libav demux_libav)
      install(FILES "plugins/${_p}/manifest.manifest.json"
              DESTINATION "${_lumen_plugin_install_dir}"
              RENAME "${_p}.manifest.json")
    endforeach()
    install(FILES resources/qqvideo.desktop
            DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
    install(FILES resources/qqvideo_64.png
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/64x64/apps"
            RENAME qqvideo.png)

    # Refresh launcher + icon caches so qqvideo shows up immediately.
    # Skipped for staged installs (DESTDIR), e.g. when building a .deb.
    install(CODE "set(_lumen_datadir \"${CMAKE_INSTALL_DATADIR}\")")
    install(CODE [[
      if ("$ENV{DESTDIR}" STREQUAL "")
        find_program(_lumen_udd update-desktop-database)
        if (_lumen_udd)
          execute_process(COMMAND "${_lumen_udd}" -q "${CMAKE_INSTALL_PREFIX}/${_lumen_datadir}/applications")
        endif()
        find_program(_lumen_gic gtk-update-icon-cache)
        if (_lumen_gic)
          execute_process(COMMAND "${_lumen_gic}" -q -t -f "${CMAKE_INSTALL_PREFIX}/${_lumen_datadir}/icons/hicolor"
                          ERROR_QUIET)
        endif()
      endif()
    ]])

    add_custom_target(uninstall
        COMMAND ${CMAKE_COMMAND} -DMANIFEST=${CMAKE_BINARY_DIR}/install_manifest.txt
                -P ${CMAKE_SOURCE_DIR}/cmake/uninstall.cmake
        COMMENT "Uninstalling qqvideo")
  endif()

  message(STATUS "")
  message(STATUS "  FFmpeg: system (pkg-config)")
  message(STATUS "    libavcodec  ${LIBAV_libavcodec_VERSION}  (${LIBAV_libavcodec_PREFIX})")
  message(STATUS "    libavformat ${LIBAV_libavformat_VERSION}")
  if (UNIX)
    message(STATUS "  Install: ${CMAKE_INSTALL_PREFIX}  (plugins -> ${_lumen_plugin_install_dir})")
  endif()
  message(STATUS "")
endif()

# ---- Codec/demuxer packages ---------------------------------------------
# These are the ONLY plugin targets. Each one builds directly into its
# packages/ subdirectory -- no plugins/ directory is involved.
# The registry is populated at runtime by scanning packages/.

if (NOT LUMEN_SYSTEM_FFMPEG)
if (LIBAVCODEC_FOUND AND LIBAVUTIL_FOUND)
  message(STATUS "libavcodec found (available for building packages separately)")
else()
  message(STATUS "libavcodec NOT found -- install FFmpeg dev libs to build codec packages")
endif()
if (LIBAVFORMAT_FOUND)
  message(STATUS "libavformat found (available for building packages separately)")
endif()

message(STATUS "")
message(STATUS "  No codec/demuxer packages are built by this CMake configuration.")
message(STATUS "  Packages are built independently -- see packages/BUILD.md")
message(STATUS "")

# ---- packages/ directory: copy source tree files to build output -----------
# Copies package.json metadata, per-package CMakeLists.txt build files, and
# BUILD.md so that a distributed qqvideo install includes everything needed to
# build new packages without requiring access to the qqvideo source tree.
file(GLOB_RECURSE PKG_COPY_FILES
    "${CMAKE_SOURCE_DIR}/packages/*/package.json"
    "${CMAKE_SOURCE_DIR}/packages/*/CMakeLists.txt"
    "${CMAKE_SOURCE_DIR}/packages/BUILD.md"
    "${CMAKE_SOURCE_DIR}/packages/FindQQVideoFFmpeg.cmake"
    "${CMAKE_SOURCE_DIR}/packages/ffmpeg/build.sh")
foreach(src_file ${PKG_COPY_FILES})
    file(RELATIVE_PATH rel "${CMAKE_SOURCE_DIR}/packages" "${src_file}")
    configure_file("${src_file}" "${CMAKE_BINARY_DIR}/bin/packages/${rel}" COPYONLY)
endforeach()
endif() # NOT LUMEN_SYSTEM_FFMPEG
