# Building qqvideo for Windows

Two ways, same result:

- **[On Windows with MSYS2](#on-windows-with-msys2)**: the simplest if you're on
  Windows anyway; MSYS2's packages provide SDL2 and (optionally) FFmpeg.
- **[On Linux, cross-compiling](#on-linux-cross-compiling)**: no Windows machine
  needed.

Either way `tools/build-windows.sh` produces `dist/qqvideo-<version>-win64/` (and a
`.zip`) that runs on any Windows PC, with every DLL it needs alongside.

## On Windows with MSYS2

1. Install [MSYS2](https://www.msys2.org) and open the **MSYS2 UCRT64** shell
   (Start menu). Not the plain "MSYS2 MSYS" one, which builds for MSYS2's own
   POSIX layer instead of Windows.
2. Install the build tools and SDL2:

   ```sh
   pacman -S --needed git zip make diffutils \
       mingw-w64-ucrt-x86_64-toolchain mingw-w64-ucrt-x86_64-cmake \
       mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-pkgconf \
       mingw-w64-ucrt-x86_64-SDL2
   ```
3. Pick an FFmpeg (see below), then clone and build:

   ```sh
   git clone https://github.com/chriso13541/qqvideo && cd qqvideo
   tools/build-windows.sh                          # with MSYS2's FFmpeg
   tools/build-windows.sh --ffmpeg ~/ffmpeg-lgpl   # or with your own build
   ```

**Which FFmpeg?**

| | MSYS2's package | Your own build |
|---|---|---|
| Install | `pacman -S mingw-w64-ucrt-x86_64-ffmpeg` | build it once (below) |
| Version | whatever MSYS2 ships (9.0.2 in Nov 2026; has the PixelSmash fix) | any, e.g. 8.1.3 to match your Linux system |
| License | **GPL-3.0**: a release bundling it must be offered under GPL-compatible terms, with source | **LGPL** (the default build) |
| Size | large: it links x264, x265, dav1d, libvpx and dozens more, all bundled | the five FFmpeg DLLs, nothing else |
| Updates | `pacman -Syu`, then rebuild | rebuild FFmpeg yourself |

MSYS2's package is quickest for trying things out. For releases, your own
LGPL build is smaller and keeps qqvideo's license your choice. Building it in
MSYS2 is the same as on Linux, minus the cross-compile options:

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-nasm
git clone --depth 1 --branch n8.1.3 https://github.com/FFmpeg/FFmpeg.git ffmpeg-src && cd ffmpeg-src
./configure --prefix="$HOME/ffmpeg-lgpl" \
    --enable-shared --disable-static --disable-programs --disable-doc --disable-debug \
    --disable-autodetect --enable-w32threads --enable-d3d11va --enable-dxva2
make -j"$(nproc)" && make install
```

(`--disable-autodetect --enable-w32threads` keeps it from pulling in other
MSYS2 libraries while keeping multithreaded decoding, and
`--enable-d3d11va --enable-dxva2` turn hardware decoding back on, which
`--disable-autodetect` also switches off; see the Linux section for the
reasoning.)

**Quick development builds** without packaging work too:

```sh
cmake -S . -B build -G Ninja -DLUMEN_SYSTEM_FFMPEG=ON -DLUMEN_WIN_CONSOLE=ON
cmake --build build
./build/bin/qqvideo.exe --list
```

Run it from the UCRT64 shell: FFmpeg's DLLs are then found through PATH
(`C:\msys64\ucrt64\bin`). qqvideo says so once per plugin: *"its DLLs were found
through PATH"*. Double-clicking that build in Explorer won't find them; the
packaged `dist/` folder has everything next to the `.exe` and runs anywhere.
(The current directory is never searched for plugin DLLs, PATH fallback or
not, so a DLL planted next to a video can't be loaded.)

## On Linux, cross-compiling

You cross-compile three things with MinGW-w64, all from Linux:

1. a **MinGW-w64 cross compiler** (once),
2. **FFmpeg** for Windows (once per FFmpeg version),
3. **qqvideo** itself: `tools/build-windows.sh` builds the `.exe`, downloads
   SDL2, and packages everything into a folder + `.zip`.

The Windows build uses the same two FFmpeg plugins as Linux
(`demux_libav` + `decoder_libav`), with the same codec allowlist and
Playback Rules. (The older per-codec `packages/` flow only has MP4,
H.264 and AAC buildable so far.)

### 1. Cross compiler

**Gentoo** (crossdev):

```sh
emerge --ask sys-devel/crossdev app-eselect/eselect-repository dev-lang/nasm app-arch/zip
eselect repository create crossdev           # crossdev puts its packages in an overlay

# qqvideo's decode thread uses pthreads: that's the winpthreads library,
# built by mingw64-runtime with USE=libraries. Set it before crossdev runs.
echo 'cross-x86_64-w64-mingw32/mingw64-runtime libraries' >> /etc/portage/package.use/crossdev

crossdev --stable --target x86_64-w64-mingw32
```

Check it: `x86_64-w64-mingw32-gcc --version` works, and
`ls /usr/x86_64-w64-mingw32/usr/include/pthread.h` exists. If pthread.h is
missing, the USE flag didn't apply: run
`emerge --oneshot cross-x86_64-w64-mingw32/mingw64-runtime` and check again.
(GCC's "thread model" doesn't matter for qqvideo: only the winpthreads
library does.)

**Debian/Ubuntu**: `sudo apt install mingw-w64 cmake pkg-config nasm zip curl`

### 2. FFmpeg for Windows

Build the **same FFmpeg release your Linux system runs**, so both builds
have the same codec fixes. A Linux `.so` can't be linked into a Windows
`.exe`, so you build that release's source a second time, for Windows.
The examples use 8.1.3: it includes the PixelSmash fix (CVE-2026-8461,
MagicYUV heap overflow), which first shipped in 8.1.2.

```sh
git clone --depth 1 --branch n8.1.3 https://github.com/FFmpeg/FFmpeg.git ffmpeg-win
cd ffmpeg-win
./configure \
    --prefix="$HOME/qqvideo-win-deps/ffmpeg" \
    --enable-cross-compile --target-os=mingw32 --arch=x86_64 \
    --cross-prefix=x86_64-w64-mingw32- \
    --enable-shared --disable-static \
    --disable-programs --disable-doc --disable-debug \
    --disable-autodetect --enable-w32threads --enable-d3d11va --enable-dxva2
make -j"$(nproc)"
make install
```

Why these options:

- `--enable-shared`: qqvideo ships FFmpeg as DLLs next to its plugins, so
  you can update FFmpeg on its own (for a CVE, say) without touching
  qqvideo, and it keeps the LGPL obligations simple.
- `--disable-autodetect`: otherwise configure goes looking for *Linux*
  libraries (via the host pkg-config) and the Windows link fails. This
  also turns off FFmpeg's threading, which is autodetected too, so
  `--enable-w32threads` turns it back on. Without it decoding is
  single-threaded and HD video stutters.
- `--enable-d3d11va --enable-dxva2`: hardware decoding (the graphics card
  decodes; Preferences > Video > Hardware decoding). Also autodetected, so
  also off after `--disable-autodetect`. They only need Windows' own
  headers, which MinGW-w64 has: nothing extra to install. Without them
  qqvideo still works, decoding everything on the CPU.
- No `--enable-gpl`: the default LGPL build has every decoder qqvideo
  uses (H.264, HEVC, AV1, VP9, ...); GPL only adds *encoders* like x264.
- `nasm` must be installed: FFmpeg's hand-written SIMD decoding code
  needs it. Without it configure stops (don't use `--disable-x86asm`
  for a real build; decoding would be several times slower).

**Using Gentoo's exact source instead.** If your Gentoo FFmpeg carries
extra patches on top of the release (a `-r1` revision, say), let Portage
unpack and patch the source (as root), then cross-build that tree:

```sh
ebuild "$(portageq get_repo_path / gentoo)"/media-video/ffmpeg/ffmpeg-8.1.3.ebuild clean prepare
ls /var/tmp/portage/media-video/ffmpeg-8.1.3/work/     # the patched source tree
```

Copy that tree somewhere and run the same `./configure ...; make; make install`
in it. Use the `.ebuild` file name matching your installed version
(`equery list ffmpeg` or `ls /var/db/pkg/media-video/`). Afterwards,
`ebuild ... clean` removes the work directory.

**Checking it.** `tools/build-windows.sh` reads the release string out of
the built `avutil-*.dll`. It prints it, records it in `BUILDINFO.txt`
inside the package, and refuses an 8.1.0/8.1.1 build (pre-PixelSmash)
unless you pass `--allow-unpatched-ffmpeg`.

**Updating FFmpeg later.** Point releases keep the same DLL names
(8.1.x is always `avcodec-62.dll`, `avutil-60.dll`, ...). For a security
fix like 8.1.3 → 8.1.4 you can rebuild FFmpeg and swap the DLLs in an
existing package's `plugins/` folder, without rebuilding qqvideo. For a new
major version (8 → 9) the DLL names change: re-run the script, which
notices the different FFmpeg and rebuilds the plugins from scratch.

### 3. qqvideo

```sh
tools/build-windows.sh --ffmpeg ~/qqvideo-win-deps/ffmpeg
```

Result: `dist/qqvideo-<version>-win64/` and `dist/qqvideo-<version>-win64.zip`:

```
qqvideo.exe            GUI app (no console window)
SDL2.dll               downloaded from the SDL release page, checksum-pinned
libwinpthread-1.dll    MinGW runtime (GCC's own runtimes are linked in)
plugins/               libdemux_libav.dll, libdecoder_libav.dll, manifests,
                       avcodec-62.dll, avutil-60.dll, ... (FFmpeg 8.1)
licenses/              ImGui, Remix Icon, SDL2, FFmpeg, tinyfiledialogs
BUILDINFO.txt          qqvideo commit, compiler, FFmpeg release, SDL2 version
```

Copy the folder (or the zip) to a Windows PC and run `qqvideo.exe`.
DLLs are bundled by following imports. Starting from `qqvideo.exe` and the
plugins, every DLL they import (and everything *those* import) is copied in:
next to the `.exe` if the `.exe` needs it, otherwise into `plugins/`. So
nothing unneeded is shipped (no `avfilter`, `avdevice`), and anything that
can't be found and isn't part of Windows is reported here, not on someone
else's machine.

Options: `--console` builds a console `.exe` instead. It shows the log
output and makes `qqvideo.exe --list` (the available codecs) work. `-j N`
and `--clean` are also available.

Doing it by hand instead: `cmake/toolchain-mingw64.cmake` is a regular
CMake toolchain file (see the comment at its top).

## Notes

- **Testing without Windows:** Wine runs it (`wine qqvideo.exe`), which
  is handy for checking the UI and file browser. Test real playback
  on real Windows before a release.
- **Where things go on Windows:** preferences and the file browser's
  recent folders live in `%APPDATA%\qqvideo\`. Plugins load their FFmpeg
  DLLs from their own folder, the `.exe`'s folder and the system folders
  (plus PATH's absolute folders as a fallback for dev builds), never from
  the current directory (see `core/plugin_loader.c`).
- **Window frame:** on Windows qqvideo draws its own Bluecurve Grape title
  bar and rounded frame instead of the native one (dragging, edge/corner
  resizing, title double-click and the min/max/close buttons all
  work). Tools > Preferences > General > "Bluecurve window frame" switches
  back to the standard Windows frame. Linux keeps your window manager's frame
  unless you turn the option on.
- **Old icon still showing in Explorer?** Explorer caches icons and often
  keeps showing an `.exe`'s previous one after it's rebuilt. Run
  `ie4uinit.exe -show` (Windows 10/11; `ie4uinit.exe -ClearIconCache` on
  older versions), or copy the `.exe` to a new folder, to see the current
  icon. Check the build itself in the file's Properties dialog, which reads
  the `.exe` directly.
- **Known gap:** some Windows code paths still use the "ANSI" file APIs,
  so paths with characters outside the system code page (e.g. a
  Japanese folder name on an English Windows) can fail. The file
  browser itself is Unicode-safe. See the code review notes.
