# qqvideo Package Development Guide

qqvideo ships with zero precompiled codec or demuxer packages.
You build exactly what you need, from whatever source you trust.

---

## Quickstart: build all packages from FFmpeg source

```
packages/
├── ffmpeg/        ← clone FFmpeg here, then run build.sh
├── demuxers/mp4/  ← cmake auto-links to ffmpeg/install/
├── codecs/video/h264/
└── codecs/audio/aac/
```

### Step 1 — Clone FFmpeg (if you haven't already)

```bash
cd /c/dev/lumen/packages
git clone https://github.com/FFmpeg/FFmpeg ffmpeg
```

### Step 2 — Build FFmpeg static libraries

Open an **MSYS2 MinGW64** terminal (not PowerShell, not cmd.exe):

```bash
cd /c/dev/lumen/packages/ffmpeg
bash build.sh
```

Prerequisites:
```bash
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-make nasm
```

This produces static libraries at `packages/ffmpeg/install/`.

### Step 3 — Build the packages

Each package's CMakeLists.txt auto-detects `packages/ffmpeg/install/` — no flags needed:

```bash
cd /c/dev/lumen/packages/demuxers/mp4
mkdir build && cd build && cmake .. -G Ninja && ninja

cd /c/dev/lumen/packages/codecs/video/h264
mkdir build && cd build && cmake .. -G Ninja && ninja

cd /c/dev/lumen/packages/codecs/audio/aac
mkdir build && cd build && cmake .. -G Ninja && ninja
```

The DLL and manifest land next to the package.json. Restart qqvideo.

---

## Alternative: MSYS2 pre-built FFmpeg

```bash
pacman -S mingw-w64-x86_64-ffmpeg
```

Then build packages the same way — cmake falls back to pkg-config automatically
when packages/ffmpeg/install/ doesn't exist.

---

## Alternative: explicit FFmpeg path

```bash
cmake .. -G Ninja -DFFMPEG_DIR=/path/to/your/ffmpeg/install
```

---

## How the auto-detection works

`FindQQVideoFFmpeg.cmake` (in packages/):
1. Check for `-DFFMPEG_DIR=...` explicit
2. Walk up the directory tree looking for `ffmpeg/install/include/libavcodec/avcodec.h`
3. Fall back to system pkg-config

---

## Package format

```
packages/<type>/<id>/
    package.json               ← metadata (always present)
    lib<plugin>.dll            ← compiled plugin (present = installed)
    <plugin>.manifest.json     ← ABI descriptor
```

"Installed" = DLL is present. Delete it to uninstall. Restart qqvideo after changes.

---

## Writing a plugin from scratch

```c
#include "lumen_plugin.h"   // only qqvideo header you need

LUMEN_EXPORT const lumen_plugin_descriptor_t *lumen_get_plugin(void);
```

See plugins/decoder_h264/decoder_h264.c for a complete ~200-line example.

```bash
gcc -shared -fvisibility=hidden -o libmyplugin.dll myplugin.c \
    -I/c/dev/lumen/include -lavcodec -lavutil
```

## manifest.json format

```json
{
    "name": "my-decoder", "version": "1.0.0", "kind": "decoder",
    "library": "mydecoder",
    "claims": { "codec_fourccs": ["H264"] }
}
```

`library` is the base name — qqvideo loads `lib<library>.dll` on Windows.
