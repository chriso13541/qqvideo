#!/usr/bin/env bash
#
# install.sh -- build and install qqvideo on Linux in one step.
#
#   ./install.sh                 build + install to /usr/local (asks for sudo/doas)
#   ./install.sh --user          build + install to ~/.local (no root needed)
#   ./install.sh --uninstall     remove what a previous run installed
#
# Works on Gentoo, Debian/Ubuntu/Devuan, Arch and Fedora. It checks for the
# build dependencies first and, if any are missing, prints (and with --deps,
# runs) the right package-manager command for your distro. FFmpeg comes
# from your system: whatever pkg-config finds, or --ffmpeg-prefix DIR.
#
# Run ./install.sh --help for every option.
set -euo pipefail

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SRC_DIR/build"
PREFIX="/usr/local"
USER_INSTALL=0
FFMPEG_PREFIX=""
JOBS="$(nproc 2>/dev/null || echo 2)"
INSTALL_DEPS=0
ASSUME_YES=0
ACTION="install"
CLEAN=0

# FFmpeg 5.1 is the floor (see plugins/common/libav_common.h).
MIN_AVCODEC="59.37.100"
MIN_AVFORMAT="59.27.100"
MIN_AVUTIL="57.28.100"
MIN_SWRESAMPLE="4.7.100"
MIN_SWSCALE="6.7.100"

# ---- output helpers -------------------------------------------------------
if [ -t 1 ]; then B=$'\e[1m'; G=$'\e[32m'; Y=$'\e[33m'; R=$'\e[31m'; N=$'\e[0m'; else B='' G='' Y='' R='' N=''; fi
say()  { printf '%s==>%s %s\n' "$G$B" "$N" "$*"; }
warn() { printf '%s==> WARNING:%s %s\n' "$Y$B" "$N" "$*" >&2; }
die()  { printf '%s==> ERROR:%s %s\n' "$R$B" "$N" "$*" >&2; exit 1; }

usage() {
    cat <<EOF
Usage: ./install.sh [options]

Install:
  --prefix DIR          install prefix (default /usr/local)
  --user                install to ~/.local instead -- no root needed;
                        qqvideo goes in ~/.local/bin (make sure it's on PATH)
  --ffmpeg-prefix DIR   build against an FFmpeg installed in DIR
                        (e.g. /opt/ffmpeg-7); default: whatever pkg-config finds
  --deps                install missing build dependencies with your package
                        manager (emerge / apt / pacman / dnf) before building
  -y, --yes             don't ask before running package-manager or root commands
  -j, --jobs N          parallel build jobs (default: $JOBS)
  --clean               wipe the build directory first (after changing FFmpeg)

Other:
  --uninstall           remove everything the last install put on disk
  --check               only check dependencies, then exit
  -h, --help            this help
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix)         PREFIX="${2:?--prefix needs a directory}"; shift 2 ;;
        --prefix=*)       PREFIX="${1#*=}"; shift ;;
        --user)           USER_INSTALL=1; shift ;;
        --ffmpeg-prefix)  FFMPEG_PREFIX="${2:?--ffmpeg-prefix needs a directory}"; shift 2 ;;
        --ffmpeg-prefix=*) FFMPEG_PREFIX="${1#*=}"; shift ;;
        --deps)           INSTALL_DEPS=1; shift ;;
        -y|--yes)         ASSUME_YES=1; shift ;;
        -j|--jobs)        JOBS="${2:?--jobs needs a number}"; shift 2 ;;
        -j*)              JOBS="${1#-j}"; shift ;;
        --clean)          CLEAN=1; shift ;;
        --uninstall)      ACTION="uninstall"; shift ;;
        --check)          ACTION="check"; shift ;;
        -h|--help)        usage; exit 0 ;;
        *)                usage >&2; die "unknown option: $1" ;;
    esac
done

[ "$(uname -s)" = "Linux" ] || die "this script is for Linux. On Windows, see packages/BUILD.md."
[ "$USER_INSTALL" = 1 ] && PREFIX="$HOME/.local"

confirm() {   # confirm "question" -> 0 for yes
    [ "$ASSUME_YES" = 1 ] && return 0
    [ -t 0 ] || return 1
    local ans; read -r -p "$1 [Y/n] " ans
    case "$ans" in ""|y|Y|yes|YES) return 0 ;; *) return 1 ;; esac
}

# Run a command as root if needed: sudo, or doas (common on Gentoo).
ROOT_CMD=()
need_root_for() {   # need_root_for DIR -> sets ROOT_CMD if DIR isn't writable
    local d="$1"
    while [ ! -e "$d" ]; do d="$(dirname "$d")"; done
    if [ -w "$d" ]; then ROOT_CMD=(); return; fi
    if [ "$(id -u)" = 0 ]; then ROOT_CMD=(); return; fi
    if command -v sudo >/dev/null 2>&1; then ROOT_CMD=(sudo)
    elif command -v doas >/dev/null 2>&1; then ROOT_CMD=(doas)
    else die "$1 isn't writable and neither sudo nor doas is installed. Try --user."; fi
}
as_root() {
    if [ "$(id -u)" = 0 ]; then "$@"
    elif command -v sudo >/dev/null 2>&1; then sudo "$@"
    elif command -v doas >/dev/null 2>&1; then doas "$@"
    else die "need root for: $* (no sudo or doas found)"; fi
}

# ---- distro detection -----------------------------------------------------
DISTRO="unknown"
if [ -r /etc/os-release ]; then
    # shellcheck disable=SC1091
    . /etc/os-release
    case " ${ID:-} ${ID_LIKE:-} " in
        *" gentoo "*)                         DISTRO="gentoo" ;;
        *" debian "*|*" ubuntu "*|*" devuan "*) DISTRO="debian" ;;
        *" arch "*)                           DISTRO="arch" ;;
        *" fedora "*|*" rhel "*)              DISTRO="fedora" ;;
    esac
fi

# Package names per distro, by the dependency they provide.
pkg_name() {   # pkg_name <dep>
    case "$DISTRO:$1" in
        gentoo:compiler)   echo "sys-devel/gcc" ;;
        gentoo:cmake)      echo "dev-build/cmake" ;;
        gentoo:pkgconfig)  echo "virtual/pkgconfig" ;;
        gentoo:sdl2)       echo "media-libs/libsdl2" ;;
        gentoo:ffmpeg)     echo "media-video/ffmpeg" ;;
        gentoo:desktop)    echo "dev-util/desktop-file-utils" ;;

        debian:compiler)   echo "build-essential" ;;
        debian:cmake)      echo "cmake" ;;
        debian:pkgconfig)  echo "pkg-config" ;;
        debian:sdl2)       echo "libsdl2-dev" ;;
        debian:ffmpeg)     echo "libavcodec-dev libavformat-dev libavutil-dev libswresample-dev libswscale-dev" ;;
        debian:desktop)    echo "desktop-file-utils" ;;

        arch:compiler)     echo "base-devel" ;;
        arch:cmake)        echo "cmake" ;;
        arch:pkgconfig)    echo "pkgconf" ;;
        arch:sdl2)         echo "sdl2" ;;
        arch:ffmpeg)       echo "ffmpeg" ;;
        arch:desktop)      echo "desktop-file-utils" ;;

        fedora:compiler)   echo "gcc gcc-c++ make" ;;
        fedora:cmake)      echo "cmake" ;;
        fedora:pkgconfig)  echo "pkgconf-pkg-config" ;;
        fedora:sdl2)       echo "SDL2-devel" ;;
        fedora:ffmpeg)     echo "ffmpeg-free-devel" ;;   # or ffmpeg-devel from RPM Fusion for H.264/HEVC
        fedora:desktop)    echo "desktop-file-utils" ;;
        *)                 echo "$1" ;;
    esac
}

install_cmd() {   # install_cmd <packages...> -> the command, as an array in PKG_CMD
    case "$DISTRO" in
        gentoo) PKG_CMD=(emerge --ask=n --noreplace --verbose "$@") ;;
        debian) PKG_CMD=(apt-get install -y "$@") ;;
        arch)   PKG_CMD=(pacman -S --needed --noconfirm "$@") ;;
        fedora) PKG_CMD=(dnf install -y "$@") ;;
        *)      PKG_CMD=() ;;
    esac
}

# ---- dependency check -----------------------------------------------------
if [ -n "$FFMPEG_PREFIX" ]; then
    [ -d "$FFMPEG_PREFIX" ] || die "--ffmpeg-prefix $FFMPEG_PREFIX doesn't exist"
    export PKG_CONFIG_PATH="$FFMPEG_PREFIX/lib/pkgconfig:$FFMPEG_PREFIX/lib64/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
fi

MISSING=()        # required deps (keys for pkg_name)
MISSING_OPT=()    # nice-to-have
FFMPEG_TOO_OLD=""

check_deps() {
    MISSING=(); MISSING_OPT=(); FFMPEG_TOO_OLD=""
    { command -v cc >/dev/null 2>&1 || command -v gcc >/dev/null 2>&1 || command -v clang >/dev/null 2>&1; } &&
    { command -v c++ >/dev/null 2>&1 || command -v g++ >/dev/null 2>&1 || command -v clang++ >/dev/null 2>&1; } ||
        MISSING+=(compiler)
    command -v cmake >/dev/null 2>&1 || MISSING+=(cmake)
    if ! command -v pkg-config >/dev/null 2>&1 && ! command -v pkgconf >/dev/null 2>&1; then
        MISSING+=(pkgconfig)
        # Without pkg-config we can't check the libraries; list them all.
        MISSING+=(sdl2 ffmpeg)
    else
        pkg-config --exists sdl2 || MISSING+=(sdl2)
        local ff_ok=1 m v
        for m in libavcodec:$MIN_AVCODEC libavformat:$MIN_AVFORMAT libavutil:$MIN_AVUTIL \
                 libswresample:$MIN_SWRESAMPLE libswscale:$MIN_SWSCALE; do
            if ! pkg-config --exists "${m%%:*}"; then ff_ok=0
            elif ! pkg-config --atleast-version="${m#*:}" "${m%%:*}"; then
                v="$(pkg-config --modversion "${m%%:*}")"
                FFMPEG_TOO_OLD+="  ${m%%:*} $v (need >= ${m#*:})"$'\n'
            fi
        done
        [ "$ff_ok" = 1 ] || MISSING+=(ffmpeg)
    fi
    command -v update-desktop-database >/dev/null 2>&1 || MISSING_OPT+=(desktop)
}

report_deps() {
    local k pkgs=() opt=()
    for k in "${MISSING[@]+"${MISSING[@]}"}"; do read -ra _p <<<"$(pkg_name "$k")"; pkgs+=("${_p[@]}"); done
    for k in "${MISSING_OPT[@]+"${MISSING_OPT[@]}"}"; do read -ra _p <<<"$(pkg_name "$k")"; opt+=("${_p[@]}"); done

    if [ -n "$FFMPEG_TOO_OLD" ]; then
        warn "your FFmpeg is older than 5.1, which qqvideo needs:"
        printf '%s' "$FFMPEG_TOO_OLD" >&2
        echo "    Build a newer one into /usr/local (see README.md, 'Newer FFmpeg than your distro ships')," >&2
        echo "    or point this script at one with --ffmpeg-prefix DIR." >&2
    fi
    if [ ${#MISSING_OPT[@]} -gt 0 ]; then
        for k in "${MISSING_OPT[@]}"; do
            case "$k" in
                desktop) warn "update-desktop-database not found: 'Open With' may not list qqvideo until next login ($(pkg_name desktop))." ;;
            esac
        done
    fi
    if [ ${#pkgs[@]} -gt 0 ]; then
        echo
        echo "${B}Missing build dependencies:${N} ${MISSING[*]}"
        install_cmd "${pkgs[@]}"
        if [ ${#PKG_CMD[@]} -gt 0 ]; then
            echo "Install them with:"
            echo "    ${PKG_CMD[*]}"
            [ "$DISTRO" = gentoo ] && echo "    (Gentoo: ffmpeg's default USE flags are fine; libsdl2 needs X or wayland, plus alsa/pulseaudio/pipewire for sound.)"
        else
            echo "Unrecognized distro -- install a C/C++ compiler, cmake, pkg-config, SDL2 and the FFmpeg development libraries."
        fi
        [ ${#opt[@]} -gt 0 ] && [ ${#PKG_CMD[@]} -gt 0 ] && echo "Optional extras: ${opt[*]}"
        echo
    fi
}

# ---- uninstall ------------------------------------------------------------
# install.sh saves a copy of CMake's install manifest under the prefix, so
# uninstall works even after the build directory is deleted.
find_manifest() {
    local c
    for c in "$PREFIX/share/qqvideo/install_manifest.txt" \
             "$HOME/.local/share/qqvideo/install_manifest.txt" \
             "/usr/local/share/qqvideo/install_manifest.txt" \
             "/usr/share/qqvideo/install_manifest.txt" \
             "$BUILD_DIR/install_manifest.txt"; do
        [ -f "$c" ] && { echo "$c"; return 0; }
    done
    return 1
}

if [ "$ACTION" = "uninstall" ]; then
    MANIFEST="$(find_manifest)" || die "no qqvideo install found (looked for install_manifest.txt under $PREFIX, ~/.local, /usr/local)"
    say "Uninstalling using $MANIFEST"
    need_root_for "$(dirname "$MANIFEST")"
    # Copy the manifest out first: it lists itself, so it gets removed mid-run.
    TMP_MANIFEST="$(mktemp)"; trap 'rm -f "$TMP_MANIFEST"' EXIT
    cp "$MANIFEST" "$TMP_MANIFEST"
    command -v cmake >/dev/null 2>&1 || die "cmake is needed to uninstall"
    "${ROOT_CMD[@]+"${ROOT_CMD[@]}"}" cmake -DMANIFEST="$TMP_MANIFEST" -P "$SRC_DIR/cmake/uninstall.cmake"
    "${ROOT_CMD[@]+"${ROOT_CMD[@]}"}" rmdir "$(dirname "$MANIFEST")" 2>/dev/null || true
    say "qqvideo removed. (Your settings in ~/.config/qqvideo were left alone.)"
    exit 0
fi

# ---- install --------------------------------------------------------------
say "qqvideo installer -- distro: $DISTRO, prefix: $PREFIX"
check_deps
report_deps

if [ "$ACTION" = "check" ]; then
    if [ ${#MISSING[@]} -eq 0 ] && [ -z "$FFMPEG_TOO_OLD" ]; then
        say "all build dependencies found"
        pkg-config --exists libavcodec && echo "    FFmpeg: libavcodec $(pkg-config --modversion libavcodec) ($(pkg-config --variable=prefix libavcodec))"
        pkg-config --exists sdl2 && echo "    SDL2:   $(pkg-config --modversion sdl2)"
        exit 0
    fi
    exit 1
fi

if [ ${#MISSING[@]} -gt 0 ]; then
    if [ "$INSTALL_DEPS" = 1 ] && [ ${#PKG_CMD[@]} -gt 0 ]; then
        if confirm "Run '${PKG_CMD[*]}' as root now?"; then
            [ "$DISTRO" = debian ] && as_root apt-get update -q
            as_root "${PKG_CMD[@]}"
            hash -r
            check_deps
            [ ${#MISSING[@]} -eq 0 ] || die "still missing after install: ${MISSING[*]}"
        else
            die "missing dependencies -- install them and re-run"
        fi
    else
        die "missing dependencies -- install them (or re-run with --deps) and re-run"
    fi
fi
[ -z "$FFMPEG_TOO_OLD" ] || die "FFmpeg too old (see above)"

if [ "$CLEAN" = 1 ] && [ -d "$BUILD_DIR" ]; then
    say "Removing old build directory"
    rm -rf "$BUILD_DIR"
fi

# A build dir configured for a different prefix or FFmpeg would silently
# keep the old settings' cached paths; reconfigure from scratch instead.
if [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
    old_prefix="$(sed -n 's/^CMAKE_INSTALL_PREFIX:PATH=//p' "$BUILD_DIR/CMakeCache.txt")"
    old_ff="$(sed -n 's/^LUMEN_FFMPEG_PREFIX:PATH=//p' "$BUILD_DIR/CMakeCache.txt")"
    if [ "$old_prefix" != "$PREFIX" ] || [ "$old_ff" != "$FFMPEG_PREFIX" ]; then
        say "Build settings changed since last time -- starting a fresh build directory"
        rm -rf "$BUILD_DIR"
    fi
fi

say "Configuring"
GEN=()
command -v ninja >/dev/null 2>&1 && [ ! -f "$BUILD_DIR/CMakeCache.txt" ] && GEN=(-G Ninja)
cmake -S "$SRC_DIR" -B "$BUILD_DIR" "${GEN[@]+"${GEN[@]}"}" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="$PREFIX" \
      -DLUMEN_SYSTEM_FFMPEG=ON \
      -DLUMEN_FFMPEG_PREFIX="$FFMPEG_PREFIX"

say "Building with $JOBS jobs"
cmake --build "$BUILD_DIR" --parallel "$JOBS"

say "Checking the build"
"$BUILD_DIR/bin/qqvideo" --list >/dev/null 2>&1 || warn "'qqvideo --list' failed -- the build may be broken; run $BUILD_DIR/bin/qqvideo --list to see why"

say "Installing to $PREFIX"
need_root_for "$PREFIX"
[ ${#ROOT_CMD[@]} -gt 0 ] && echo "    (${ROOT_CMD[0]} needed to write to $PREFIX)"
"${ROOT_CMD[@]+"${ROOT_CMD[@]}"}" cmake --install "$BUILD_DIR"

# Keep a copy of the manifest under the prefix (listing itself) so
# --uninstall still works after the source/build tree is gone.
MDIR="$PREFIX/share/qqvideo"
TMP_MANIFEST="$(mktemp)"; trap 'rm -f "$TMP_MANIFEST"' EXIT
cp "$BUILD_DIR/install_manifest.txt" "$TMP_MANIFEST"
# CMake writes the manifest without a final newline.
[ -n "$(tail -c1 "$TMP_MANIFEST")" ] && echo >> "$TMP_MANIFEST"
echo "$MDIR/install_manifest.txt" >> "$TMP_MANIFEST"
"${ROOT_CMD[@]+"${ROOT_CMD[@]}"}" mkdir -p "$MDIR"
"${ROOT_CMD[@]+"${ROOT_CMD[@]}"}" install -m 644 "$TMP_MANIFEST" "$MDIR/install_manifest.txt"

echo
say "Done. qqvideo is installed:"
echo "    binary:  $PREFIX/bin/qqvideo"
echo "    plugins: $(sed -n 's|^\(.*/qqvideo/plugins\)/.*|\1|p' "$BUILD_DIR/install_manifest.txt" | head -1)"
echo "    FFmpeg:  libavcodec $(pkg-config --modversion libavcodec) from $(pkg-config --variable=libdir libavcodec)"
if ! command -v qqvideo >/dev/null 2>&1 || [ "$(command -v qqvideo)" != "$PREFIX/bin/qqvideo" ]; then
    case ":$PATH:" in
        *":$PREFIX/bin:"*) ;;
        *) warn "$PREFIX/bin isn't on your PATH -- add it, e.g. in ~/.bashrc:  export PATH=\"$PREFIX/bin:\$PATH\"" ;;
    esac
fi
echo "    Run 'qqvideo --list' to see which codecs your FFmpeg provides."
if [ "$USER_INSTALL" = 1 ]; then echo "    To remove it later: ./install.sh --uninstall --user"
else echo "    To remove it later: ./install.sh --uninstall"; fi
