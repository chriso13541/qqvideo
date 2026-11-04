#!/usr/bin/env bash
#
# build-windows.sh -- cross-compile qqvideo for 64-bit Windows from Linux
# and package it as a folder (and .zip) you can copy to a Windows PC.
#
#   tools/build-windows.sh --ffmpeg ~/qqvideo-win-deps/ffmpeg
#
# Needs: a MinGW-w64 cross compiler (x86_64-w64-mingw32-gcc/g++/windres,
# plus the winpthreads library), cmake, pkg-config, curl, and an FFmpeg
# you cross-compiled for Windows (see docs/BUILD-WINDOWS.md). SDL2 is
# downloaded for you (official libsdl.org release, checksum-pinned).
#
# Output: dist/qqvideo-<version>-win64/ and dist/qqvideo-<version>-win64.zip
#
#   qqvideo.exe, SDL2.dll           the player
#   plugins/                        demux_libav + decoder_libav, their
#                                   manifests, and the FFmpeg DLLs
#   licenses/                       third-party license texts
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
Usage: tools/build-windows.sh --ffmpeg DIR [options]

  --ffmpeg DIR     FFmpeg cross-compiled for Windows (the --prefix you gave its
                   configure; has bin/*.dll, lib/pkgconfig/, include/)
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

# ---- checks ---------------------------------------------------------------
CC=""
for c in "$TRIPLE-gcc-posix" "$TRIPLE-gcc"; do command -v "$c" >/dev/null 2>&1 && { CC="$c"; break; }; done
[ -n "$CC" ] || die "no $TRIPLE-gcc found. Install a MinGW-w64 cross compiler (docs/BUILD-WINDOWS.md)."
for t in "$TRIPLE-windres" "$TRIPLE-objdump" cmake pkg-config curl; do
    command -v "$t" >/dev/null 2>&1 || die "missing tool: $t"
done
# qqvideo's decode thread uses pthreads -> the MinGW winpthreads library.
# (Link to a temp file: MinGW's linker turns "-o /dev/null" into /dev/null.exe.)
probe_dir="$(mktemp -d)"; trap 'rm -rf "$probe_dir"' EXIT
printf '#include <pthread.h>\nint main(void){pthread_t t; (void)t; return 0;}\n' > "$probe_dir/t.c"
"$CC" "$probe_dir/t.c" -o "$probe_dir/t.exe" -lpthread 2>/dev/null ||
    die "$CC can't find pthread.h/winpthreads. On Gentoo: USE=libraries for cross-$TRIPLE/mingw64-runtime (docs/BUILD-WINDOWS.md)."

[ -n "$FFMPEG_PREFIX" ] || { usage >&2; die "--ffmpeg DIR is required (without FFmpeg nothing can play)"; }
FFMPEG_PREFIX="$(cd "$FFMPEG_PREFIX" && pwd)" || die "--ffmpeg: no such directory"
[ -f "$FFMPEG_PREFIX/lib/pkgconfig/libavcodec.pc" ] || die "$FFMPEG_PREFIX/lib/pkgconfig/libavcodec.pc not found -- is that FFmpeg's install prefix?"
ls "$FFMPEG_PREFIX"/bin/avcodec-*.dll >/dev/null 2>&1 || die "no avcodec-*.dll in $FFMPEG_PREFIX/bin -- FFmpeg must be built with --enable-shared"

# Which FFmpeg release is this? The library versions don't change between
# point releases (8.1.0 and 8.1.3 are both libavcodec 62), so read the
# release string libavutil embeds: "FFmpeg version <release>"
# (libavutil/version.c, av_util_ffversion).
FFMPEG_RELEASE="$(grep -a -o -m1 'FFmpeg version [^[:space:][:cntrl:]]*' "$FFMPEG_PREFIX"/bin/avutil-*.dll 2>/dev/null |
                  head -1 | sed 's/^FFmpeg version //')"
FFMPEG_RELEASE="${FFMPEG_RELEASE:-unknown}"
AVCODEC_VERSION="$(PKG_CONFIG_LIBDIR="$FFMPEG_PREFIX/lib/pkgconfig" pkg-config --modversion libavcodec)"
say "FFmpeg release: $FFMPEG_RELEASE (libavcodec $AVCODEC_VERSION)"

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

# ---- configure + build --------------------------------------------------------
[ "$CLEAN" = 1 ] && rm -rf "$BUILD_DIR"
# CMake caches what pkg-config found, so pointing at a different FFmpeg
# would quietly keep linking the plugins against the old one (e.g. 7.1's
# avutil-59.dll while 8.1's avutil-60.dll gets bundled). Start over instead.
if [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
    old_ff="$(sed -n 's/^LUMEN_FFMPEG_PREFIX:PATH=//p' "$BUILD_DIR/CMakeCache.txt")"
    if [ "$old_ff" != "$FFMPEG_PREFIX" ]; then
        say "FFmpeg changed since the last build ($old_ff) -- starting a fresh build directory"
        rm -rf "$BUILD_DIR"
    fi
fi
say "Configuring (compiler: $CC, FFmpeg: $FFMPEG_PREFIX)"
cmake -S "$SRC_DIR" -B "$BUILD_DIR" \
      -DCMAKE_TOOLCHAIN_FILE="$SRC_DIR/cmake/toolchain-mingw64.cmake" \
      -DQQ_MINGW_TRIPLE="$TRIPLE" \
      -DQQ_WIN_DEPS="$DEPS_DIR" \
      -DCMAKE_BUILD_TYPE=Release \
      -DLUMEN_SYSTEM_FFMPEG=ON \
      -DLUMEN_FFMPEG_PREFIX="$FFMPEG_PREFIX" \
      -DLUMEN_WIN_CONSOLE="$CONSOLE"
say "Building with $JOBS jobs"
cmake --build "$BUILD_DIR" --parallel "$JOBS"

# ---- package ------------------------------------------------------------------
VERSION="$(git -C "$SRC_DIR" describe --tags --always --dirty 2>/dev/null || echo dev)"
NAME="qqvideo-$VERSION-win64"
OUT="$DIST_ROOT/$NAME"
say "Packaging $OUT"
rm -rf "$OUT"
mkdir -p "$OUT/plugins" "$OUT/licenses"
cp "$BUILD_DIR/bin/qqvideo.exe" "$OUT/"
cp "$SDL2_ROOT/bin/SDL2.dll" "$OUT/"
cp "$BUILD_DIR"/bin/plugins/*.dll "$BUILD_DIR"/bin/plugins/*.manifest.json "$OUT/plugins/"
# FFmpeg's DLLs sit with the plugins that use them: plugins are loaded with
# their own folder on the DLL search path (core/plugin_loader.c).
cp "$FFMPEG_PREFIX"/bin/*.dll "$OUT/plugins/"
cp "$SRC_DIR/third_party/imgui/LICENSE.txt"           "$OUT/licenses/imgui.txt"
cp "$SRC_DIR/third_party/remixicon/LICENSE"           "$OUT/licenses/remixicon.txt"
[ -f "$SRC_DIR/third_party/tinyfiledialogs/NOTICE.md" ] && cp "$SRC_DIR/third_party/tinyfiledialogs/NOTICE.md" "$OUT/licenses/tinyfiledialogs.md"
for f in COPYING.LGPLv2.1 COPYING.LGPLv3 LICENSE.md; do
    for d in "$FFMPEG_PREFIX/share/doc/ffmpeg" "$FFMPEG_PREFIX/share/licenses/ffmpeg"; do
        [ -f "$d/$f" ] && cp "$d/$f" "$OUT/licenses/ffmpeg-$f"
    done
done
[ -f "$SDL2_ROOT/../LICENSE.txt" ] && cp "$SDL2_ROOT/../LICENSE.txt" "$OUT/licenses/SDL2.txt"
[ -f "$SDL2_ROOT/../README-SDL.txt" ] && cp "$SDL2_ROOT/../README-SDL.txt" "$OUT/licenses/README-SDL.txt"

# Every DLL a binary imports must be either shipped or part of Windows.
# Copy MinGW runtime DLLs (e.g. libwinpthread-1.dll) from the toolchain;
# anything else missing is reported, since it would fail on the target PC.
dll_imports() { "$TRIPLE-objdump" -p "$1" | sed -n 's/^[[:space:]]*DLL Name: //p'; }
is_windows_dll() {
    case "${1,,}" in
        kernel32.dll|user32.dll|gdi32.dll|winmm.dll|imm32.dll|ole32.dll|oleaut32.dll|version.dll|\
        advapi32.dll|setupapi.dll|shell32.dll|comdlg32.dll|msvcrt.dll|ws2_32.dll|bcrypt.dll|\
        secur32.dll|ncrypt.dll|crypt32.dll|uuid.dll|dinput8.dll|cfgmgr32.dll|mfplat.dll|mfuuid.dll|\
        ucrtbase.dll|api-ms-win-*) return 0 ;;
    esac
    return 1
}
find_mingw_dll() {
    local p
    p="$("$CC" -print-file-name="$1")"
    [ -f "$p" ] && { echo "$p"; return; }
    find "/usr/$TRIPLE" "/usr/lib/gcc/$TRIPLE" -name "$1" -print -quit 2>/dev/null
}
missing=0
for pass in 1 2; do       # a DLL copied in pass 1 may import another
    while IFS= read -r bin; do
        while IFS= read -r dll; do
            is_windows_dll "$dll" && continue
            [ -f "$OUT/$dll" ] || [ -f "$OUT/plugins/$dll" ] && continue
            src="$(find_mingw_dll "$dll")"
            if [ -n "$src" ]; then
                cp "$src" "$OUT/"; say "  bundled MinGW runtime: $dll"
            elif [ "$pass" = 2 ]; then
                warn "$(basename "$bin") needs $dll, which isn't bundled"; missing=1
            fi
        done < <(dll_imports "$bin")
    done < <(find "$OUT" -name '*.dll' -o -name '*.exe')
done

# What went into this build, for the release notes and for anyone checking
# which FFmpeg they're running (no paths: this file ships to users).
{
    echo "qqvideo        $VERSION"
    echo "built          $(date -u +%Y-%m-%dT%H:%MZ) with $("$CC" -dumpfullversion 2>/dev/null || "$CC" -dumpversion) ($TRIPLE)"
    echo "FFmpeg         $FFMPEG_RELEASE (libavcodec $AVCODEC_VERSION, LGPL, shared)"
    echo "SDL2           $SDL2_VERSION (official MinGW build, sha256 $SDL2_SHA256)"
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
