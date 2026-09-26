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

# ---- Codec/demuxer packages ---------------------------------------------
# These are the ONLY plugin targets. Each one builds directly into its
# packages/ subdirectory -- no plugins/ directory is involved.
# The registry is populated at runtime by scanning packages/.

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
