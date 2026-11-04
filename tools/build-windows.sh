#!/usr/bin/env bash
#
# build-windows.sh -- build qqvideo for 64-bit Windows and package it as a
# folder (and .zip) that runs on any Windows PC. Works two ways:
#
#   On Linux, cross-compiling with MinGW-w64:
#       tools/build-windows.sh --ffmpeg ~/qqvideo-win-deps/ffmpeg
#     Needs x86_64-w64-mingw32-gcc/g++/windres (+ winpthreads), cmake,
#     pkg-config, curl, and an FFmpeg cross-compiled for Windows. SDL2 is
#     downloaded (official libsdl.org release, checksum-pinned).
#
#   On Windows, in an MSYS2 UCRT64 (or MINGW64/CLANG64) shell:
#       tools/build-windows.sh                      # MSYS2's own FFmpeg
#       tools/build-windows.sh --ffmpeg ~/ffmpeg-lgpl   # or one you built
#     Needs the MSYS2 packages listed in docs/BUILD-WINDOWS.md (SDL2 and
#     FFmpeg come from pacman unless --ffmpeg says otherwise).
#
# Output: dist/qqvideo-<version>-win64/ and dist/qqvideo-<version>-win64.zip
#
#   qqvideo.exe + the DLLs it needs (SDL2.dll, MinGW runtime)
#   plugins/      demux_libav + decoder_libav, their manifests, and the
#                 DLLs only they need (FFmpeg and anything FFmpeg links to)
#   licenses/     third-party license texts
#   BUILDINFO.txt what went into the build
#
# DLLs are found by following each binary's imports, so nothing needed is
# left out and nothing unneeded is copied.
set -euo pipefail

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$SRC_DIR/build-win64"
DEPS_DIR="$SRC_DIR/build-win64-deps"
DIST_ROOT="$SRC_DIR/dist"
TRIPLE="x86_64-w64-mingw32"
FFMPEG_PREFIX=""
CONSOLE=OFF
JOBS="$(nproc 2>/dev/null || echo 2)"
CLEAN=0
ALLOW_UNPATCHED=0

# SDL2 for MinGW, from the official release page. The checksum pins the
# exact file, so a tampered or swapped download is refused.
SDL2_VERSION="2.30.9"
SDL2_URL="https://github.com/libsdl-org/SDL/releases/download/release-${SDL2_VERSION}/SDL2-devel-${SDL2_VERSION}-mingw.tar.gz"
SDL2_SHA256="b188d165dc4a0372042bff3d0c18e9bb90772581e92a7b8b47b521b47714f75c"

if [ -t 1 ]; then B=$'\e[1m'; G=$'\e[32m'; Y=$'\e[33m'; R=$'\e[31m'; N=$'\e[0m'; else B='' G='' Y='' R='' N=''; fi
say()  { printf '%s==>%s %s\n' "$G$B" "$N" "$*"; }
warn() { printf '%s==> WARNING:%s %s\n' "$Y$B" "$N" "$*" >&2; }
die()  { printf '%s==> ERROR:%s %s\n' "$R$B" "$N" "$*" >&2; exit 1; }

usage() {
    cat <<EOF
Usage: tools/build-windows.sh [--ffmpeg DIR] [options]

  --ffmpeg DIR     FFmpeg built for Windows (the --prefix you gave its configure;
                   has bin/*.dll, lib/pkgconfig/, include/). Required when
                   cross-compiling; in MSYS2 the default is MSYS2's own FFmpeg
  --console        build a console .exe (log output visible; --list works)
  --triple T       compiler prefix (default $TRIPLE)
  -j N             parallel jobs (default $JOBS)
  --clean          start from an empty build directory
  --allow-unpatched-ffmpeg
                   build even with an FFmpeg 8.1 older than 8.1.2 (PixelSmash)
  -h, --help       this help
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --ffmpeg)    FFMPEG_PREFIX="${2:?--ffmpeg needs a directory}"; shift 2 ;;
        --ffmpeg=*)  FFMPEG_PREFIX="${1#*=}"; shift ;;
        --console)   CONSOLE=ON; shift ;;
        --triple)    TRIPLE="${2:?}"; shift 2 ;;
        -j)          JOBS="${2:?}"; shift 2 ;;
        -j*)         JOBS="${1#-j}"; shift ;;
        --clean)     CLEAN=1; shift ;;
        --allow-unpatched-ffmpeg) ALLOW_UNPATCHED=1; shift ;;
        -h|--help)   usage; exit 0 ;;
        *)           usage >&2; die "unknown option: $1" ;;
    esac
done

# ---- where are we? -----------------------------------------------------------
# MSYS2 sets MSYSTEM (UCRT64, MINGW64, CLANG64, ... or plain MSYS) and,
# in the MinGW shells, MINGW_PREFIX (/ucrt64, /mingw64, ...).
NATIVE=0
if [ -n "${MSYSTEM:-}" ]; then
    [ -n "${MINGW_PREFIX:-}" ] && [ "$MSYSTEM" != MSYS ] ||
        die "this is the plain MSYS shell -- open the \"MSYS2 UCRT64\" shell instead (Start menu)"
    NATIVE=1
fi
# CMake and pkg-config in MSYS2 are native Windows programs: hand them
# C:/msys64/... paths rather than /c/... or /ucrt64/... ones.
winpath() { if [ "$NATIVE" = 1 ]; then cygpath -m "$1"; else printf '%s\n' "$1"; fi; }

# ---- checks ---------------------------------------------------------------
CC=""
if [ "$NATIVE" = 1 ]; then
    command -v gcc >/dev/null 2>&1 && CC=gcc
    command -v clang >/dev/null 2>&1 && [ -z "$CC" ] && CC=clang
    [ -n "$CC" ] || die "no compiler -- pacman -S --needed ${MINGW_PACKAGE_PREFIX}-toolchain (docs/BUILD-WINDOWS.md)"
    OBJDUMP=objdump
    command -v objdump >/dev/null 2>&1 || OBJDUMP=llvm-objdump
    for t in "$OBJDUMP" cmake pkg-config cygpath; do
        command -v "$t" >/dev/null 2>&1 || die "missing tool: $t (see docs/BUILD-WINDOWS.md for the pacman line)"
    done
else
    for c in "$TRIPLE-gcc-posix" "$TRIPLE-gcc"; do command -v "$c" >/dev/null 2>&1 && { CC="$c"; break; }; done
    [ -n "$CC" ] || die "no $TRIPLE-gcc found. Install a MinGW-w64 cross compiler (docs/BUILD-WINDOWS.md)."
    OBJDUMP="$TRIPLE-objdump"
    for t in "$TRIPLE-windres" "$OBJDUMP" cmake pkg-config curl; do
        command -v "$t" >/dev/null 2>&1 || die "missing tool: $t"
    done
fi
# qqvideo's decode thread uses pthreads -> the MinGW winpthreads library.
# (Link to a temp file: MinGW's linker turns "-o /dev/null" into /dev/null.exe.)
probe_dir="$(mktemp -d)"; trap 'rm -rf "$probe_dir"' EXIT
printf '#include <pthread.h>\nint main(void){pthread_t t; (void)t; return 0;}\n' > "$probe_dir/t.c"
"$CC" "$probe_dir/t.c" -o "$probe_dir/t.exe" -lpthread 2>/dev/null ||
    die "$CC can't find pthread.h/winpthreads. On Gentoo: USE=libraries for cross-$TRIPLE/mingw64-runtime (docs/BUILD-WINDOWS.md)."

if [ -z "$FFMPEG_PREFIX" ]; then
    [ "$NATIVE" = 1 ] || { usage >&2; die "--ffmpeg DIR is required when cross-compiling (without FFmpeg nothing can play)"; }
    FFMPEG_PREFIX="$MINGW_PREFIX"            # MSYS2's own FFmpeg (pacman)
    FFMPEG_IS_SYSTEM=1
    [ -f "$FFMPEG_PREFIX/lib/pkgconfig/libavcodec.pc" ] ||
        die "MSYS2's FFmpeg isn't installed -- pacman -S ${MINGW_PACKAGE_PREFIX}-ffmpeg, or pass --ffmpeg DIR"
else
    FFMPEG_IS_SYSTEM=0
fi
FFMPEG_PREFIX="$(cd "$FFMPEG_PREFIX" && pwd)" || die "--ffmpeg: no such directory"
[ -f "$FFMPEG_PREFIX/lib/pkgconfig/libavcodec.pc" ] || die "$FFMPEG_PREFIX/lib/pkgconfig/libavcodec.pc not found -- is that FFmpeg's install prefix?"
ls "$FFMPEG_PREFIX"/bin/avcodec-*.dll >/dev/null 2>&1 || die "no avcodec-*.dll in $FFMPEG_PREFIX/bin -- FFmpeg must be built with --enable-shared"

# Which FFmpeg release is this? The library versions don't change between
# point releases (8.1.0 and 8.1.3 are both libavcodec 62), so read the
# release string libavutil embeds: "FFmpeg version <release>"
# (libavutil/version.c, av_util_ffversion).
# (|| true: no match isn't an error here -- it falls back to "unknown".)
FFMPEG_RELEASE="$(grep -a -o -m1 'FFmpeg version [^[:space:][:cntrl:]]*' "$FFMPEG_PREFIX"/bin/avutil-*.dll 2>/dev/null |
                  head -1 | sed 's/^FFmpeg version //' || true)"
FFMPEG_RELEASE="${FFMPEG_RELEASE:-unknown}"
AVCODEC_VERSION="$(PKG_CONFIG_LIBDIR="$(winpath "$FFMPEG_PREFIX/lib/pkgconfig")" pkg-config --modversion libavcodec)"
# Its license, as compiled in (libavutil's avutil_license(): "LGPL version
# 2.1 or later", "GPL version 3 or later", or "nonfree and unredistributable").
FFMPEG_LICENSE="$(grep -a -o -m1 -E 'nonfree and unredistributable|(L?GPL) version [0-9.]+ or later' "$FFMPEG_PREFIX"/bin/avutil-*.dll 2>/dev/null | head -1 || true)"
FFMPEG_LICENSE="${FFMPEG_LICENSE:-unknown license}"
say "FFmpeg release: $FFMPEG_RELEASE (libavcodec $AVCODEC_VERSION, $FFMPEG_LICENSE)"
case "$FFMPEG_LICENSE" in
    nonfree*) die "this FFmpeg was built with --enable-nonfree: it may not be redistributed at all. Use an LGPL or GPL build." ;;
    GPL*)     warn "this FFmpeg is GPL-licensed (MSYS2's package is): a release that bundles it must be offered under GPL-compatible terms, with source. An LGPL FFmpeg (docs/BUILD-WINDOWS.md) avoids that." ;;
esac

# PixelSmash (CVE-2026-8461, MagicYUV heap overflow) was fixed in FFmpeg
# 8.1.2. 8.1.0/8.1.1 are known vulnerable: refuse them unless overridden.
# Other branches may have backported the fix in their own point releases,
# so for those this only warns.
if [[ "$FFMPEG_RELEASE" =~ ^n?([0-9]+)\.([0-9]+)(\.([0-9]+))? ]]; then
    ff_maj=${BASH_REMATCH[1]} ff_min=${BASH_REMATCH[2]} ff_pt=${BASH_REMATCH[4]:-0}
    if [ "$ff_maj" -eq 8 ] && [ "$ff_min" -eq 1 ] && [ "$ff_pt" -lt 2 ]; then
        [ "$ALLOW_UNPATCHED" = 1 ] || die "FFmpeg $FFMPEG_RELEASE predates the PixelSmash fix (CVE-2026-8461, fixed in 8.1.2). Build 8.1.2 or newer, or pass --allow-unpatched-ffmpeg."
        warn "FFmpeg $FFMPEG_RELEASE predates the PixelSmash fix (8.1.2) -- building anyway (--allow-unpatched-ffmpeg)"
    elif [ "$ff_maj" -lt 8 ] || { [ "$ff_maj" -eq 8 ] && [ "$ff_min" -lt 1 ]; }; then
        warn "FFmpeg $FFMPEG_RELEASE is older than the 8.1 branch; the PixelSmash fix (CVE-2026-8461) shipped in 8.1.2 -- make sure your branch has it"
    fi
elif [ "$FFMPEG_RELEASE" = unknown ]; then
    warn "couldn't read the FFmpeg release from avutil-*.dll -- check by hand that it has the PixelSmash fix (8.1.2+)"
else
    say "  (a git snapshot -- make sure it's newer than the PixelSmash fix, June 2026)"
fi

# ---- SDL2 -------------------------------------------------------------------
if [ "$NATIVE" = 1 ]; then
    pkg-config --exists sdl2 || die "SDL2 isn't installed -- pacman -S ${MINGW_PACKAGE_PREFIX}-SDL2"
    SDL2_VERSION="$(pkg-config --modversion sdl2)"
    SDL2_ROOT="$MINGW_PREFIX"
    SDL2_NOTE="MSYS2 package"
else
SDL2_NOTE="official MinGW build, sha256 $SDL2_SHA256"
SDL2_ROOT="$DEPS_DIR/SDL2-$SDL2_VERSION/$TRIPLE"
if [ ! -f "$SDL2_ROOT/lib/pkgconfig/sdl2.pc" ]; then
    say "Downloading SDL2 $SDL2_VERSION (MinGW development package)"
    mkdir -p "$DEPS_DIR"
    TARBALL="$DEPS_DIR/SDL2-devel-$SDL2_VERSION-mingw.tar.gz"
    curl -fL --proto '=https' -o "$TARBALL" "$SDL2_URL"
    echo "$SDL2_SHA256  $TARBALL" | sha256sum -c --quiet - || die "SDL2 download checksum mismatch -- refusing to use it"
    tar xzf "$TARBALL" -C "$DEPS_DIR"
    rm -f "$TARBALL"
fi
# The .pc file names the prefix SDL was built in; point it at ours.
mkdir -p "$DEPS_DIR/lib/pkgconfig"
sed "s|^prefix=.*|prefix=$SDL2_ROOT|" "$SDL2_ROOT/lib/pkgconfig/sdl2.pc" > "$DEPS_DIR/lib/pkgconfig/sdl2.pc"
fi

# ---- configure + build --------------------------------------------------------
[ "$CLEAN" = 1 ] && rm -rf "$BUILD_DIR"
# CMake caches what pkg-config found, so pointing at a different FFmpeg
# would quietly keep linking the plugins against the old one (e.g. 7.1's
# avutil-59.dll while 8.1's avutil-60.dll gets bundled). Start over instead.
if [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
    old_ff="$(sed -n 's/^LUMEN_FFMPEG_PREFIX:PATH=//p' "$BUILD_DIR/CMakeCache.txt")"
    if [ "$old_ff" != "$(winpath "$FFMPEG_PREFIX")" ]; then
        say "FFmpeg changed since the last build ($old_ff) -- starting a fresh build directory"
        rm -rf "$BUILD_DIR"
    fi
fi
if [ "$NATIVE" = 1 ]; then
    say "Configuring natively in MSYS2 $MSYSTEM (compiler: $CC, FFmpeg: $FFMPEG_PREFIX)"
    GEN=()
    command -v ninja >/dev/null 2>&1 && [ ! -f "$BUILD_DIR/CMakeCache.txt" ] && GEN=(-G Ninja)
    cmake -S "$(winpath "$SRC_DIR")" -B "$(winpath "$BUILD_DIR")" "${GEN[@]+"${GEN[@]}"}" \
          -DCMAKE_BUILD_TYPE=Release \
          -DLUMEN_SYSTEM_FFMPEG=ON \
          -DLUMEN_FFMPEG_PREFIX="$(winpath "$FFMPEG_PREFIX")" \
          -DLUMEN_WIN_CONSOLE="$CONSOLE"
else
    say "Configuring (compiler: $CC, FFmpeg: $FFMPEG_PREFIX)"
    cmake -S "$SRC_DIR" -B "$BUILD_DIR" \
          -DCMAKE_TOOLCHAIN_FILE="$SRC_DIR/cmake/toolchain-mingw64.cmake" \
          -DQQ_MINGW_TRIPLE="$TRIPLE" \
          -DQQ_WIN_DEPS="$DEPS_DIR" \
          -DCMAKE_BUILD_TYPE=Release \
          -DLUMEN_SYSTEM_FFMPEG=ON \
          -DLUMEN_FFMPEG_PREFIX="$FFMPEG_PREFIX" \
          -DLUMEN_WIN_CONSOLE="$CONSOLE"
fi
say "Building with $JOBS jobs"
cmake --build "$(winpath "$BUILD_DIR")" --parallel "$JOBS"

# ---- package ------------------------------------------------------------------
VERSION="$(git -C "$SRC_DIR" describe --tags --always --dirty 2>/dev/null || echo dev)"
NAME="qqvideo-$VERSION-win64"
OUT="$DIST_ROOT/$NAME"
say "Packaging $OUT"
rm -rf "$OUT"
mkdir -p "$OUT/plugins" "$OUT/licenses"
cp "$BUILD_DIR/bin/qqvideo.exe" "$OUT/"
cp "$BUILD_DIR"/bin/plugins/*.dll "$BUILD_DIR"/bin/plugins/*.manifest.json "$OUT/plugins/"
# Every other DLL is added below by following imports.
cp "$SRC_DIR/third_party/imgui/LICENSE.txt"           "$OUT/licenses/imgui.txt"
cp "$SRC_DIR/third_party/remixicon/LICENSE"           "$OUT/licenses/remixicon.txt"
[ -f "$SRC_DIR/third_party/tinyfiledialogs/NOTICE.md" ] && cp "$SRC_DIR/third_party/tinyfiledialogs/NOTICE.md" "$OUT/licenses/tinyfiledialogs.md"
for f in COPYING.LGPLv2.1 COPYING.LGPLv3 COPYING.GPLv2 COPYING.GPLv3 LICENSE.md LICENSE; do
    for d in "$FFMPEG_PREFIX/share/doc/ffmpeg" "$FFMPEG_PREFIX/share/licenses/ffmpeg"; do
        [ -f "$d/$f" ] && cp "$d/$f" "$OUT/licenses/ffmpeg-$f"
    done
done
if [ "$NATIVE" = 1 ]; then
    [ -f "$MINGW_PREFIX/share/licenses/SDL2/LICENSE.txt" ] && cp "$MINGW_PREFIX/share/licenses/SDL2/LICENSE.txt" "$OUT/licenses/SDL2.txt"
else
    [ -f "$SDL2_ROOT/../LICENSE.txt" ] && cp "$SDL2_ROOT/../LICENSE.txt" "$OUT/licenses/SDL2.txt"
    [ -f "$SDL2_ROOT/../README-SDL.txt" ] && cp "$SDL2_ROOT/../README-SDL.txt" "$OUT/licenses/README-SDL.txt"
fi

# ---- DLLs: follow the imports -------------------------------------------------
# Every DLL a binary imports must be either shipped or part of Windows.
# Starting from qqvideo.exe and the plugins, each import is looked up in
# the build's DLL folders and copied -- next to qqvideo.exe if the .exe
# (or something it loads) needs it, else into plugins/ (plugins resolve
# their DLLs from their own folder and the .exe's, core/plugin_loader.c)
# -- and then its own imports are followed the same way.
DLL_DIRS=()
[ -d "$FFMPEG_PREFIX/bin" ] && DLL_DIRS+=("$FFMPEG_PREFIX/bin")
if [ "$NATIVE" = 1 ]; then
    DLL_DIRS+=("$MINGW_PREFIX/bin")
    SYS32="$(cygpath -u "$(cygpath -W)")/System32"
else
    [ -d "$SDL2_ROOT/bin" ] && DLL_DIRS+=("$SDL2_ROOT/bin")
    SYS32=""
fi
dll_imports() { "$OBJDUMP" -p "$1" | sed -n 's/^[[:space:]]*DLL Name: //p' | tr -d '\r'; }
is_windows_dll() {
    local l="${1,,}"
    [ -n "$SYS32" ] && [ -f "$SYS32/$1" ] && return 0       # MSYS2: ask Windows itself
    case "$l" in
        api-ms-win-*|ext-ms-win-*) return 0 ;;
        kernel32.dll|kernelbase.dll|ntdll.dll|user32.dll|gdi32.dll|winmm.dll|imm32.dll|ole32.dll|\
        oleaut32.dll|version.dll|advapi32.dll|setupapi.dll|shell32.dll|shlwapi.dll|comdlg32.dll|\
        comctl32.dll|msvcrt.dll|ucrtbase.dll|ws2_32.dll|bcrypt.dll|secur32.dll|ncrypt.dll|crypt32.dll|\
        uuid.dll|dinput8.dll|cfgmgr32.dll|mfplat.dll|mfuuid.dll|mfreadwrite.dll|evr.dll|psapi.dll|\
        d3d9.dll|d3d11.dll|d3d12.dll|dxgi.dll|dxva2.dll|opengl32.dll|glu32.dll|hid.dll|avrt.dll|\
        propsys.dll|dwmapi.dll|uxtheme.dll|usp10.dll|msimg32.dll|iphlpapi.dll|dnsapi.dll|userenv.dll|\
        winhttp.dll|wininet.dll|normaliz.dll|wldap32.dll|rpcrt4.dll|powrprof.dll|dbghelp.dll|\
        winspool.drv|mpr.dll|netapi32.dll|wtsapi32.dll|xinput1_4.dll|mscms.dll|strmiids.dll) return 0 ;;
    esac
    return 1
}
find_dll() {
    local d f
    for d in "${DLL_DIRS[@]+"${DLL_DIRS[@]}"}"; do
        [ -f "$d/$1" ] && { echo "$d/$1"; return; }
        f="$(find "$d" -maxdepth 1 -iname "$1" -print -quit 2>/dev/null)"   # import names' case can differ
        [ -n "$f" ] && { echo "$f"; return; }
    done
    if [ "$NATIVE" = 0 ]; then                                # MinGW runtime from the cross toolchain
        f="$("$CC" -print-file-name="$1")"
        [ -f "$f" ] && { echo "$f"; return; }
        find "/usr/$TRIPLE" "/usr/lib/gcc/$TRIPLE" -name "$1" -print -quit 2>/dev/null
    fi
}
have_dll() { [ -n "$(find "$OUT" "$OUT/plugins" -maxdepth 1 -iname "$1" -print -quit 2>/dev/null)" ]; }
MISSING=()
bundle_deps() {          # bundle_deps <binary> <destination dir>
    local dll src
    while IFS= read -r dll; do
        [ -n "$dll" ] || continue
        have_dll "$dll" && continue
        src="$(find_dll "$dll")"
        if [ -n "$src" ]; then
            cp "$src" "$2/"
            say "  bundled $(basename "$src")$([ "$2" = "$OUT/plugins" ] && echo ' (plugins/)')"
            bundle_deps "$2/$(basename "$src")" "$2"
        elif ! is_windows_dll "$dll"; then
            MISSING+=("$(basename "$1") needs $dll")
        fi
    done < <(dll_imports "$1")
}
bundle_deps "$OUT/qqvideo.exe" "$OUT"
for p in "$OUT"/plugins/*.dll; do bundle_deps "$p" "$OUT/plugins"; done
# sdl2-compat (SDL2's API on top of SDL3) loads SDL3.dll at run time rather
# than importing it, so imports alone wouldn't find it.
if [ -f "$OUT/SDL2.dll" ] && grep -a -q -i 'SDL3\.dll' "$OUT/SDL2.dll" && ! have_dll SDL3.dll; then
    src="$(find_dll SDL3.dll)"
    if [ -n "$src" ]; then cp "$src" "$OUT/"; bundle_deps "$OUT/SDL3.dll" "$OUT"; say "  bundled SDL3.dll (SDL2.dll is sdl2-compat)"
    else MISSING+=("SDL2.dll (sdl2-compat) needs SDL3.dll"); fi
fi
missing=0
if [ ${#MISSING[@]} -gt 0 ]; then
    for m in "${MISSING[@]}"; do warn "$m, which wasn't found"; done
    missing=1
fi
ndll=$(find "$OUT" -name '*.dll' | wc -l); size=$(du -sh "$OUT" | cut -f1)
say "  $ndll DLLs, $size in total"

# What went into this build, for the release notes and for anyone checking
# which FFmpeg they're running (no paths: this file ships to users).
{
    echo "qqvideo        $VERSION"
    echo "built          $(date -u +%Y-%m-%dT%H:%MZ) with $CC $("$CC" -dumpfullversion 2>/dev/null || "$CC" -dumpversion) ($([ "$NATIVE" = 1 ] && echo "MSYS2 $MSYSTEM" || echo "cross, $TRIPLE"))"
    echo "FFmpeg         $FFMPEG_RELEASE (libavcodec $AVCODEC_VERSION, $FFMPEG_LICENSE, shared$([ "$FFMPEG_IS_SYSTEM" = 1 ] && echo ', MSYS2 package'))"
    echo "SDL2           $SDL2_VERSION ($SDL2_NOTE)"
} > "$OUT/BUILDINFO.txt"

if command -v zip >/dev/null 2>&1; then
    (cd "$DIST_ROOT" && rm -f "$NAME.zip" && zip -qr "$NAME.zip" "$NAME")
    say "Done: $DIST_ROOT/$NAME.zip"
else
    say "Done: $OUT  (install 'zip' to also get a .zip)"
fi
[ "$missing" = 0 ] || warn "some DLLs are missing (above) -- the .exe would fail to start on Windows"
echo "    Copy the folder to Windows and run qqvideo.exe. Codecs available depend"
echo "    on how FFmpeg was configured; build with --console and run"
echo "    'qqvideo.exe --list' to see them."
