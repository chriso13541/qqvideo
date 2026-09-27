# Lumen — a plugin-based, package-managed media player core

This is a working prototype of the architecture discussed: a video player
core that knows nothing about any specific codec or container format.
Every demuxer, decoder, and output is a separate shared library (.so on
Linux, .dll on Windows), discovered via a small manifest file, and loaded
**only when actually needed** to play a specific file.

The property this buys you, concretely demonstrated below: a vulnerability
in a codec (like CVE-2026-8461 / PixelSmash in FFmpeg's MagicYUV decoder)
simply cannot be triggered on a system where that decoder plugin was never
installed -- because the code is never mapped into the process at all,
not merely "disabled."

## Linux: system FFmpeg (default on Linux)

On Linux, qqvideo uses whatever FFmpeg your system already has instead
of per-codec packages you compile yourself. Two plugins do everything:
`demux_libav` (MP4/MOV, MKV/WebM, AVI, TS, Ogg, FLAC, MP3) and
`decoder_libav` (every codec in `plugins/common/libav_common.h`), both
built with the app into `build/bin/plugins/`.

```
sudo apt install build-essential cmake pkg-config libsdl2-dev \
     libavcodec-dev libavformat-dev libavutil-dev libswresample-dev libswscale-dev
mkdir build && cd build && cmake .. && make -j$(nproc)
./bin/qqvideo --list        # which codecs your FFmpeg actually provides
```

To install it system-wide so it shows up in your app launcher (search
"qqvideo") and under "Open With" for video files:

```
sudo cmake --install build                  # to /usr/local by default
sudo cmake --build build --target uninstall # removes exactly what was installed
```

That installs `bin/qqvideo`, the plugins to `lib/qqvideo/plugins/`, a
`.desktop` entry, and the icon. Running `./build/bin/qqvideo` straight
from the build tree keeps working too; the binary checks
`<exe dir>/plugins` first, then the installed location, and
`QQVIDEO_PLUGIN_DIR` overrides both.

Minimum is FFmpeg 5.1 (Debian 12). Nothing else to configure: at startup
qqvideo asks the loaded libavcodec which allowlisted decoders it has, and
logs exactly which FFmpeg it's running
(`libav-decoder 0.3.0 (FFmpeg 6.1.1-3ubuntu5, libavcodec 60.31.102)`).
Codecs your FFmpeg lacks -- e.g. H.264/HEVC on Fedora's `ffmpeg-free` --
show as unavailable in `--list` and Tools > Package Manager.

**Newer FFmpeg than your distro ships:** build it into `/usr/local`, not
`/usr` (that would overwrite dpkg-owned files):

```
git clone https://github.com/FFmpeg/FFmpeg.git && cd FFmpeg && git checkout n7.1
./configure --prefix=/usr/local --enable-shared --disable-static   # needs nasm
make -j$(nproc) && sudo make install && sudo ldconfig
```

then re-run cmake and rebuild qqvideo. For any other prefix (say you
keep several FFmpeg builds side by side), point the build at it:

```
cmake -S . -B build -DLUMEN_FFMPEG_PREFIX=/opt/ffmpeg-9
```

The plugins get an RPATH to that prefix, so both the build-tree and the
installed copy load that FFmpeg with no `LD_LIBRARY_PATH`.

For `/usr/local` none of that is needed: pkg-config searches `/usr/local`
before `/usr` on Debian/Ubuntu, so the build picks it up automatically,
and the runtime linker finds it via `/etc/ld.so.conf.d/libc.conf`. Both
FFmpegs coexist (different sonames, e.g. `libavcodec.so.59` vs `.61`); a
qqvideo binary stays on the one it was built against until rebuilt.
`sudo make uninstall` in the FFmpeg tree reverts to the distro version.

**Subtitles.** The Subtitles menu lists every subtitle track in the file
(labeled from the container's language/title tags) plus **None**, which is
the default for each new file. **Add Subtitle Track...** loads an external
`.srt`, `.ass`, `.ssa` or `.vtt` and selects it; `V` cycles tracks.
Subtitle files next to the movie are listed automatically (never
auto-selected): `Movie.srt`, `Movie.en.srt`, `Movie.eng.forced.srt`, and
anything in a `Subs/` or `Subtitles/` folder, including `Subs/<movie>/`.
If the movie is the only video in its folder, every subtitle file there
is listed; in a folder with several videos (a TV season) only files named
after that episode are, so episodes never pick up each other's subs.
Text formats are supported (SubRip, ASS/SSA, WebVTT, MP4 timed text):
styling tags are stripped and text is drawn outlined at the bottom of the
picture, scaled to the video. Image-based subtitles (Blu-ray PGS, DVD
VobSub, DVB) are listed but grayed out until there's a bitmap renderer.

**What changes about the security model.** A distro `libavcodec.so`
contains every decoder the distro enabled, so "not installed = not
mapped into the process" no longer holds. What holds instead is "not
allowed = not reachable": `decoder_libav` only opens allowlisted codecs,
and `demux_libav` passes the per-file allow-list (installed decoders
minus Playback Rules) to libavformat as `codec_whitelist` +
`format_whitelist`. That matters because `avformat_find_stream_info()`
opens decoders on its own while probing -- verified: without the
whitelist, an MKV with a MagicYUV track runs the MagicYUV decoder during
probing; with it, libavcodec refuses (`Codec (magicyuv) not on
whitelist`) and the stream is skipped. The upside of the trade: CVE fixes
arrive with normal distro updates.

Windows keeps the `packages/` flow (`-DLUMEN_SYSTEM_FFMPEG=OFF`, the
default there); see `packages/BUILD.md`.

## What's here

```
include/lumen_plugin.h        the entire plugin ABI -- the only header plugins need
core/
  registry.{h,c}               scans *.manifest.json, answers "who claims this?"
  plugin_loader.{h,c}           cross-platform dlopen/LoadLibrary wrapper
  player_core.{h,c}             demux -> decode -> present loop, ACROSS EVERY STREAM
  minijson.{h,c}                tiny dependency-free JSON reader for manifests
  main.c                        lumen-play CLI
plugins/
  demux_synthetic/              fake demuxer emitting interleaved fake VIDEO + AUDIO streams
  decoder_passthrough/          fake video decoder, proves the pipeline without a real codec
  decoder_passthrough_audio/    fake audio decoder, proves audio routing through the core
  output_console/                prints video frames instead of rendering (kind: output-video)
  output_console_audio/          prints audio frames instead of playing sound (kind: output-audio)
  decoder_h264/                 a REAL decoder: thin wrapper around libavcodec's H.264 decoder
  demux_rawh264/                a REAL demuxer: splits a raw Annex-B .h264 stream into access
                                 units using libavcodec's own H.264 parser (not a hand-rolled one)
  decoder_aac/                  a REAL decoder: wraps libavcodec's AAC decoder + libswresample
                                 for sample-format conversion (planar float -> interleaved S16)
  demux_rawaac/                 validation-only demuxer: splits a raw ADTS .aac stream into
                                 frames via libavcodec's AAC parser, so decoder_aac could be
                                 tested in isolation before demux_mp4.c exists
  demux_mp4/                    a REAL container demuxer: wraps libavformat's MP4/MOV reader --
                                 the actual milestone the multi-stream core was built for
  output_sdl2/                  a REAL video output: window via SDL2 + real ImGui transport
                                 controls (play/pause, seek bar, volume, fullscreen)
  output_sdl2_audio/             a REAL audio output: queues decoded PCM to an actual SDL2
                                 audio device
tools/lumenctl/lumenctl.sh      install/remove/list plugins, Gentoo-emerge-style
third_party/tinyfiledialogs/    vendored native file-open dialog (see below)
third_party/imgui/              vendored Dear ImGui (v1.91.5) + SDL2/SDLRenderer2 backend
```

## Launching without a terminal: native file-open dialog

`lumen-play` with no file argument now pops a native OS file picker
(Win32 common dialog on Windows; zenity/kdialog/GTK/console fallback on
Linux) instead of requiring a path on the command line -- this is what
lets the compiled app be run by double-clicking it. Implemented via
**tinyfiledialogs**, vendored under `third_party/`.

**Worth knowing about, not just trusting blindly**: tinyfiledialogs has
a real CVE history (CVE-2020-36767, CVE-2023-47104 -- CVSS 9.8 --
both shell-metacharacter command injection via its Linux backend, which
shells out to `zenity`/`kdialog`). The first mirror found while
integrating this was a frozen 2017 snapshot vulnerable to both. The
version actually vendored (v3.18.1, May 2024) was verified directly --
not just trusted by version number -- by confirming `tinyfiledialogs.c`
contains the actual character-rejection logic for quotes/backticks/`$`-
expansions both CVEs were about. Full details and provenance in
`third_party/tinyfiledialogs/NOTICE.md`, including what to check again
if this ever gets updated.

Tested in this sandbox specifically for the no-display/no-TTY case
(headless CI, this sandbox): falls through to console-input mode,
hits EOF on stdin, and exits cleanly with "no file selected" rather
than hanging -- confirmed directly, not assumed. On a real desktop
(Windows or Linux with a display), it pops the actual native dialog
instead of falling back to console mode.

## v2 ABI: multi-stream support

The core now drives every stream a file contains, not just one video
stream. Key changes from the original prototype:

- **Demuxers report a stream TABLE**, not a single video-stream summary
  -- `lumen_stream_table_t` holds N `lumen_stream_desc_t` entries, each
  tagged `LUMEN_STREAM_VIDEO` or `LUMEN_STREAM_AUDIO` with its own
  codec/dimensions/sample-rate.
- **One decoder instance is opened per stream**, looked up independently
  by that stream's fourcc. A file with H.264 video + AAC audio gets two
  separate decoder contexts, each fed only packets matching its own
  `stream_index`.
- **`lumen_frame_t` is a tagged union** (video pixel planes OR audio
  samples), and **output plugins split into two kinds**
  (`output-video` / `output-audio`) with different `open()` signatures
  (width/height/pixfmt vs. sample_rate/channels/sample_fmt).
- **Per-stream graceful degradation**: if no decoder is installed for
  one stream's codec, that stream is skipped with a warning and
  playback continues on the streams that ARE supported -- verified
  directly: removing the synthetic audio decoder's manifest still
  plays all 10 video frames correctly, just without audio, rather than
  failing the whole file.

This is a breaking ABI change (`LUMEN_ABI_VERSION` bumped 1 -> 2); every
plugin in this tree was rebuilt against it together.

## v3/v4 ABI: container codec config and encoder-delay handling

Two further real-world gotchas surfaced building `demux_mp4.c`, each
requiring a small ABI addition (v2->v3->v4), not just a demuxer-side fix:

- **v3 -- `extradata`/`extradata_size` added to `lumen_stream_desc_t`.**
  MP4 stores H.264 as length-prefixed NAL units (AVCC) with SPS/PPS in
  the `avcC` box, NOT Annex-B start-code-prefixed NALs like
  `demux_rawh264` produces. MP4 stores AAC as raw access units with
  audio config in the `esds` box, NOT per-frame ADTS headers like
  `demux_rawaac` produces. Both are solved the same way: the demuxer
  hands the container's codec config through as `extradata`, and
  libavcodec auto-detects the right framing convention once that's
  copied into `AVCodecContext` before `avcodec_open2` -- no branching
  needed in the decoder plugins themselves.
- **v4 -- `skip_samples_start` added to `lumen_packet_t`.** Caught by
  the validation process below, not anticipated in advance: MP4 records
  AAC encoder delay (priming samples needed for lookahead, which must
  be discarded from decoded output) as side data on the first audio
  packet. Without reading it, decoded audio is off by exactly one frame
  versus correct playback -- confirmed directly (see validation below).
  `demux_mp4` reads `AV_PKT_DATA_SKIP_SAMPLES` and sets this field;
  `decoder_aac` trims that many samples off the front of its decoded
  output. `player_core.c` now zero-initializes each packet before every
  `read_packet()` call so demuxers that don't know about this field
  (everything except `demux_mp4`) can simply never touch it rather than
  needing to explicitly zero it themselves.

## v5 ABI: real-time pacing

Software decode is far faster than realtime on modern hardware -- without
deliberate pacing, a movie would decode and "play" in a couple of
seconds instead of at normal speed. Fixing this needed one more ABI
addition: `frame_rate` on `lumen_stream_desc_t` (from the container's
`avg_frame_rate`/`r_frame_rate`, falling back to an assumed 30fps for
sources that don't carry it, like raw Annex-B). `decoder_h264` uses it
to assign `pts` as real milliseconds of playback time instead of a bare
sequential index; `output_sdl2` paces presentation against that,
measured from `SDL_GetTicks64()` at the first frame. Verified directly:
a 2.0s test file took 2.16s of actual wall-clock time to play through
the SDL2 output, not a fraction of a second.

Audio needed no equivalent fix -- a real audio device drains its queue
at the actual sample rate on its own, so `output_sdl2_audio` gets
correct real-time pacing for free just by queuing PCM.

## v6 ABI: real transport controls (play/pause/seek/volume)

Building actual VLC-style controls needed real new capabilities, not
just a UI layer on top of what existed:

- **`lumen_playback_state_t`** -- shared state (paused, seek request,
  position/duration, volume, a `seek_generation` counter) that
  player_core.c owns and hands to output plugins via `bind_state()`.
  Plain shared memory, not a callback table -- everything runs on one
  thread, so there's nothing to synchronize.
- **`pump_ui()`** on both output vtables -- called by player_core.c
  *every loop iteration*, not just when a frame exists. Without this,
  pausing would stop calling `present()` entirely (no new frames to
  show), which means events would never get pumped again and "Play"
  would be unclickable forever, and audio would keep playing whatever
  was already buffered indefinitely. Actual screen redraws are
  rate-limited internally to ~60fps (`pump_ui` gets called once per
  *packet*, far more often than any display needs to redraw); audio's
  `pump_ui` does something different but just as necessary -- see below.
- **`seek()`** on the demuxer vtable, **`flush()`** on the decoder
  vtable -- both optional (NULL is valid; raw elementary-stream demuxers
  correctly have nothing to seek with). `demux_mp4` implements real
  seeking via `av_seek_frame`; `decoder_h264`/`decoder_aac` implement
  flush via `avcodec_flush_buffers`, discarding stale B-frame-reorder
  state so post-seek decode doesn't get corrupted by pre-seek leftovers.
- **`seek_generation`**, not a single pending/cleared flag -- video
  (resyncing its wall-clock pacing reference) and audio (flushing stale
  pre-seek PCM from its queue) both need to react to the same seek
  independently. A single flag has an obvious race over who clears it
  first and whether the other side ever sees it; a monotonic counter
  each side compares against its own last-seen value doesn't.

**A real bug caught before it could ship, not after**: initially audio
pause only stopped *queuing new* PCM -- whatever was already sitting in
the device's buffer (which can be a noticeable fraction of a second)
kept playing after the user clicked Pause. Fixed by having audio's
`pump_ui` call `SDL_PauseAudioDevice` *and* `SDL_ClearQueuedAudio` on
the actual transition into pause, verified directly: a test harness
toggling `state.paused` and checking `SDL_GetAudioDeviceStatus` confirmed
PLAYING -> PAUSED -> PLAYING tracks correctly (note: `SDL_GetQueuedAudioSize`
itself is NOT meaningfully testable under SDL's dummy audio driver --
confirmed it reports 0 unconditionally regardless of how much is queued,
a limitation of that backend, not of the code being tested).

**Seeking verified directly, not just "returns 0"**: a deliberately
multi-keyframe test file (`-g 5 -force_key_frames`, keyframes every
~0.5s) confirmed `demux_mp4`'s `seek(ctx, 1500)` actually lands on the
keyframe at exactly 1.5s (`pts=15360` in this stream's 10240Hz
time_base = precisely 1.500s), not just on whatever was already being
read -- an earlier test against a single-GOP file had looked identical
before/after seeking, which turned out to be the test file having only
one keyframe for AVSEEK_FLAG_BACKWARD to possibly land on, not a broken
seek implementation; worth knowing this trap exists when testing seeking
against your own files later.

**UI**: Dear ImGui (vendored, pinned to tagged release v1.91.5) via its
SDL_Renderer backend -- chosen specifically because `output_sdl2`
already creates an `SDL_Renderer`, not a raw GL context, so this
backend needed zero changes to window/renderer setup. This makes
`output_sdl2.cpp` C++ (ImGui is C++-only); it still exports the same C
ABI entry point everything else does (`extern "C"` on `lumen_get_plugin`
specifically -- internal functions don't need that, only a symbol
looked up by name via `dlsym`/`GetProcAddress` needs unmangled linkage).

## v7 ABI: two real bugs found by actually using the controls

Both reported directly from real usage, not caught by testing in
advance -- exactly the kind of thing that needed a human actually
clicking the UI to surface:

1. **Volume slider looked stuck at 0%/100%.** The slider's range and
   handle position were always correct (0.0-1.0) -- the displayed
   *label* wasn't: `"%.0f%%"` formats whatever raw float it's given, so
   formatting 0.5 directly produces "0%" or "1%" (rounding the RAW
   0.0-1.0 value), never anything like "50%". Fixed by scaling to 0-100
   for display/input and converting back to 0.0-1.0 for the stored
   value -- a display bug, not a control bug.
2. **Seek bar visibly reset to 0 after a successful seek**, even though
   playback itself correctly moved (confirmed: audio and video landed
   in the right place). Root cause: `decoder_h264`'s pts is "frames
   emitted since open/last flush x frame duration" -- resetting that
   counter to 0 on every flush (the original v6 design) made wall-clock
   pacing resync correctly, but the SAME pts also drives
   `state->position_ms` for the seek bar, and resetting to 0 broke that
   the instant the next frame decoded. **Bumped `flush()`'s signature to
   take `resume_at_ms`** -- the decoder reseeds its counter from the
   actual seek target instead of zeroing it, so pts keeps meaning
   "absolute position" for the UI while still correctly restarting
   pacing math. Verified with a real decode test (not just checking the
   arithmetic in isolation): decoded several frames normally (pts
   0,100,200...), seeked+flushed to 2000ms, decoded again --
   post-seek pts came back as 2000,2100,2200, continuing correctly from
   the seek target rather than resetting to 0,100,200.

Also fixed while in there: the seek bar's "use a local value while
dragging" logic was checking `ImGui::IsItemActive()` *before* that
frame's slider widget had even been submitted -- which actually refers
to whatever the *previous* widget was (the time label), not the slider.
Replaced with a flag persisted across frames instead of a same-frame
query ordering that's easy to get subtly wrong with ImGui's
"query-the-last-submitted-item" model.

## v9 ABI: a persistent window, not a one-shot CLI tool

Requested directly: launching the app should give you a real, visible,
empty player -- like VLC's blank startup window -- that you load files
into and return to when one ends, rather than the whole process
existing only for the duration of one file. This needed a real
architecture split, not just a loop around the old per-file logic:

- **`open()` and per-file setup are now separate calls** on both output
  vtables. `open()` creates the window (video) / initializes the audio
  subsystem (audio) ONCE, with no file-specific dimensions at all --
  none exist yet at app startup. The new **`load_stream()`** -- called
  once per file, including the first one, and again every time a
  different file loads into the same still-open window -- is what
  (re)creates the texture sized for that file and resizes the window to
  fit (video), or opens/reopens the actual SDL audio device for that
  file's sample rate/channels (audio, since SDL can't change format on
  an already-open device). `close()` now only tears anything down on
  real app exit, never between files.
- **`player_core.c` gained a session API** (`lumen_session_open` /
  `lumen_session_idle` / `lumen_session_play_file` / `lumen_session_close`)
  replacing the old one-shot `lumen_play_file()`. Outputs open once at
  session start; `lumen_session_idle()` is a real blocking loop that
  pumps UI/events with nothing loaded until the user does something;
  `lumen_session_play_file()` reuses the session's already-open outputs
  via `load_stream()` rather than opening fresh ones, and -- critically
  -- does NOT close them when a file ends. `main.c` is now a small loop
  around this: play a file, then go to idle (file ended naturally) or
  straight back to the file dialog (Open File was clicked) or exit
  (Quit), never the other way around.
- **`has_file`** added to `lumen_playback_state_t` so the control bar
  can render sensibly while idle -- the seek bar is disabled rather
  than sitting there inviting a drag that would set `seek_requested`
  with no active playback loop to ever consume it (confirmed this was
  a real risk: a stale seek request sitting in shared state until the
  next file loads would cause an unwanted seek the instant playback
  actually starts).

**Verified directly, not just by code review**: a stub video output
plugin that triggers quit only after 600ms of wall-clock time confirmed
the full real sequence in actual log output -- file plays, ends
naturally ("done -- 10 video frames"), the process does NOT exit,
`has_file` correctly reads back as `0` during the now-idle period, idle
survives past 600ms, then quits cleanly on request. Separately, loaded
two different-resolution files (320x240 then 160x120) back-to-back into
one session with the REAL `output_sdl2` plugin (not a stub) and
confirmed `load_stream()` correctly recreates the texture and resizes
the window each time, with no crash and no leftover state from the
first file bleeding into the second.

## v8 ABI: a real menu bar, and a light theme

Two requests, one small ABI addition:

- **Light theme**: `ImGui::StyleColorsLight()` instead of
  `StyleColorsDark()` -- a one-line built-in swap, no design work needed.
- **A File menu** (Open File.../Quit) -- deliberately just File for now,
  not a full VLC-style row of mostly-empty menus pretending to have
  features that don't exist yet. More menus (Playback, Audio, Video)
  are the natural place to grow into once there's real functionality
  behind them (track selection, playback speed, a queue/playlist).

"Open File..." needed real plumbing, not just a dialog popup, because a
movie is already playing when you click it: `lumen_playback_state_t`
gained `open_file_requested`, and `lumen_play_stats_t` gained a matching
field so `player_core.c` can report *why* it stopped -- a real quit
(`quit_requested`) vs. "the user wants to switch files"
(`open_file_requested`). `main.c` is now a loop: play a file, check
which one happened, and if it was Open File, prompt for a new path via
the exact same `tinyfiledialogs` call used at startup and play that
instead of exiting. At the time this was written, the window closed
and reopened for the new file (output plugins were scoped to one
`lumen_play_file()` call) -- the persistent-window restructuring below
(v9) is what fixed that, reusing the same window across files instead.

**Verified with a stub output plugin**, not just code review: built a
throwaway plugin that simulates clicking File > Open File on its very
first `pump_ui` call, ran it against a real file, and confirmed the
full chain end-to-end in the actual log output -- playback stops
correctly ("stopped by output"), `main.c`'s loop correctly identifies
it as an Open-File request rather than a quit, and re-prompts via the
same dialog path rather than exiting.

## A third bug from real usage: pause causing A/V desync

Reported directly: pausing for a few seconds, then resuming, made video
visibly rush to "catch up" and fall out of sync with audio. Root cause:
`output_sdl2`'s pacing reference (`start_ticks`, set once at the first
frame) never got adjusted across a pause. Real wall-clock time keeps
advancing through the entire pause -- the OS clock doesn't stop -- but
that reference point stayed frozen. On resume, every soon-to-be-decoded
frame looked like it was already several seconds late, and the
stale-frame-drop threshold (see above) discarded most of the backlog in
a rush instead of resuming cleanly. Audio has no equivalent drifting
reference (it just starts/stops the device directly), which is exactly
why only video visibly scrambled -- a real, concrete explanation for
the desync, not just "timing is hard."

Fixed in `output_sdl2`'s `pump_ui` (the only hook that reliably
observes the pause/resume transition -- `present()` never fires at all
while paused, so it can't): track how long playback was actually
paused, and shift `start_ticks` forward by exactly that amount the
moment it resumes, so the pacing math sees the same relative distance
between "now" and "start" as if the pause had never happened from the
clock's perspective.

**Verified with a real elapsed-time test**, not simulated: presented a
frame, paused for an actual 800ms (`SDL_Delay`), resumed, then presented
10 more frames at normal 33ms spacing as real decode would. Result: 11
rendered, 0 dropped -- without the fix, roughly 24 frames' worth of
artificial lateness (800ms / 33ms) would have blown through the 100ms
drop threshold for most of that backlog.

## Build (Linux, what was tested in this sandbox)

```sh
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

If `libavcodec`/`libavutil` dev packages are found via pkg-config, the real
`decoder_h264` plugin builds too (tested here against Ubuntu's packaged
ffmpeg 6.1.1 -- itself a good illustration of the original problem: distro
ffmpeg is generally a couple of major versions behind upstream).

Manifests live next to their `.so`/`.dll` in the plugins output dir. Install
one with the CLI:

```sh
./tools/lumenctl/lumenctl.sh install packages/h264-decoder --plugins build/bin/plugins
```

Then list what's installed, or play a file:

**Note on output plugins specifically**: don't install both
`output_console`/`output_console_audio` and `output_sdl2`/
`output_sdl2_audio` at the same time yet -- both pairs claim the same
`output-video`/`output-audio` kind with no further claims to
disambiguate, so which one the registry picks is currently undefined
(whichever the directory scan happens to find first). This is exactly
what the priority/fallback registry design in "What's intentionally NOT
here yet" below is for; until that exists, install one pair or the
other, not both.

```sh
./build/bin/lumen-play --plugins build/bin/plugins --list
./build/bin/lumen-play --plugins build/bin/plugins myvideo.mp4
```

## Real codec validation (not just "didn't crash")

Beyond the synthetic pipeline test, `decoder_h264` and `demux_rawh264`
were validated against a real libx264-encoded stream
(`ffmpeg -f lavfi -i testsrc... -c:v libx264 -f h264 test.h264`, 10 frames
with B-frames) by independently decoding the same file with ffmpeg's own
CLI and comparing per-frame luma checksums. This caught two real bugs
that a "did it crash?" smoke test would have missed entirely:

1. **Decoder draining.** Initial result: 8 of 10 frames decoded. B-frames
   mean libavcodec buffers a couple of frames internally for reordering
   and only releases them once told "no more input" via
   `avcodec_send_packet(ctx, NULL)`. The player loop stopped the instant
   the demuxer hit EOF and never sent that signal, silently losing the
   last 2 frames. Fixed by adding an optional `drain()` method to
   `lumen_decoder_vtable_t`, called in a loop after demux EOF in
   `player_core.c`.
2. **PTS mislabeling.** After the drain fix, all 10 frames decoded, but
   checksums landed on the wrong `pts` labels (verified: the *set* of 10
   checksums matched the reference exactly; only the order/labeling was
   wrong). Cause: raw Annex-B elementary streams carry no presentation
   timestamps -- `demux_rawh264` was tagging access units with their
   bitstream/coding order, and libavcodec's internal B-frame reordering
   carried that meaningless tag through to the output instead of a true
   display-order index. Fixed by having `decoder_h264` assign `pts` at
   the point a frame is actually *emitted* (display order, by
   construction) rather than trusting whatever the demuxer attached.

After both fixes: pts 0-9 in order, every luma checksum matching the
ffmpeg reference decode exactly.

`decoder_aac` got the same treatment: a real AAC stream
(`ffmpeg -f lavfi -i sine=... -c:a aac -f adts test.aac`) decoded through
`demux_rawaac` + `decoder_aac`, independently decoded again via
`ffmpeg -i test.aac -f s16le ref.pcm`, and compared frame-by-frame
(byte checksum + first sample). Tested mono (45 frames) and stereo
(23 frames, to actually exercise the channel-layout conversion path
the mono case can't touch) -- **every single frame matched exactly**,
first frame through last, no offset/trimming discrepancies. The
libswresample-based planar-float-to-interleaved-S16 conversion is
correct, not just plausible-looking.

## The actual milestone: a real MP4 with real interleaved video + audio

`demux_mp4` + `decoder_h264` + `decoder_aac`, fed a real MP4
(`ffmpeg ... -c:v libx264 -c:a aac ... test.mp4`, genuinely interleaved
H.264 video and mono AAC audio, not synthetic interleaving), independently
cross-checked against ffmpeg's own reference decode of the exact same
file (`ffmpeg -i test.mp4 -map 0:v -f rawvideo` / `-map 0:a -f s16le`),
frame-by-frame, by checksum:

- **Video: 20/20 frames matched exactly.** This validates the AVCC
  extradata path end to end -- MP4's length-prefixed NALs decode
  correctly once SPS/PPS reaches the decoder via `extradata`.
- **Audio: 87/87 frames matched exactly, after fixing the encoder-delay
  bug above.** Before the fix: 88 decoded frames vs. 87 in the
  reference, and shifting our output by exactly one frame made every
  remaining checksum match perfectly -- conclusive proof of what the bug
  was before writing the fix, not a guess.
- **Graceful degradation confirmed on a real container, not just the
  synthetic one**: removing `decoder_aac`'s manifest and replaying the
  same MP4 still produces all 20 correct video frames, 0 audio frames,
  no crash.

## Real output: a window, real audio, and two bugs caught by testing it

`output_sdl2` / `output_sdl2_audio` replace the console stand-ins with
an actual SDL2 window and audio device. Tested headlessly in this
sandbox with `SDL_VIDEODRIVER=dummy`/`SDL_AUDIODRIVER=dummy` -- enough to
exercise every real API call (window/texture/audio-device creation,
event pump, pacing) without a physical display, and it caught two real
bugs immediately:

1. **No accelerated renderer under the dummy driver** -- `output_sdl2`
   now falls back to `SDL_RENDERER_SOFTWARE` when no accelerated one is
   available, which matters for real users too (RDP sessions, some VMs,
   older GPUs), not just this sandbox.
2. **`player_core.c` never checked output plugins' `open()` return
   value.** When the renderer fallback above didn't exist yet, `open()`
   correctly failed and returned -1 -- but the core used the
   half-initialized (NULL) context anyway on the next frame and
   segfaulted. Real I/O fails for reasons that have nothing to do with
   whether a plugin is installed (no display, no audio device,
   headless/SSH); the core now checks `open()`'s return value for both
   video and audio output and degrades to "no display"/"no audio"
   gracefully instead of crashing -- fixed before it could bite a real
   user with, say, no working audio device.

After both fixes: a 2.0s test file played through SDL2 in 2.16s of real
wall-clock time (pacing confirmed working, not just present in the
code), and a direct unit test confirmed `present()` returns the
stop-request signal the instant an `SDL_QUIT` event (window close) is
pending, which `player_core.c` correctly turns into a clean shutdown --
skips the drain phase, closes every output and decoder, no crash.

## More bugs found by actually using it on Windows

3. **Window sized to the video's exact resolution, not the screen's.**
   A 1920x1080 video on a similarly-sized monitor centers a window whose
   title bar lands at or above the top edge of the screen -- decorations
   technically exist but are inaccessible, reading as "borderless
   fullscreen with no way to minimize" (confirmed directly: this was the
   actual cause of exactly that report on real hardware). Fixed by
   clamping the initial window size to 90% of the display's usable
   bounds (`SDL_GetDisplayUsableBounds`), preserving aspect ratio exactly
   -- verified a 1920x1080 video scales to fit while keeping the precise
   16:9 ratio, and a video already smaller than the screen is left
   untouched. The texture itself stays at full video resolution
   regardless; only the window's *starting* size changed.
4. **No stale-frame dropping -- a backlog renders as a visible
   fast-forward instead of a clean jump.** Window-resize-dragging is an
   OS-modal operation that freezes the whole single-threaded pipeline,
   not just rendering; on resume, every frame whose pts had already
   passed got rendered back-to-back with zero pacing between them,
   which looks like the video briefly speeding up (confirmed directly:
   this is what VLC's own pause/resume glitch is too, just triggered by
   resize-drag instead of pause here). Fixed by adding a
   `LUMEN_SDL2_DROP_THRESHOLD_MS` (100ms) check in `output_sdl2`'s
   `present()`: frames more than that far behind wall-clock time are
   dropped without rendering rather than burned through. Verified with a
   direct test simulating a 500ms stall followed by a burst of
   30fps-spaced catch-up frames -- 12 of 20 frames dropped, 8 rendered to
   catch up, matching the threshold arithmetic exactly (not just "some
   frames got dropped, close enough"). Note: this same OS-modal-freeze
   mechanism applies to window-*move* too, not just resize -- the whole
   pipeline blocks identically either way, the window just shows its
   last-painted frame motionless during the drag (the OS isn't asking
   for a repaint, so there's nothing new to show) and resumes cleanly on
   release thanks to the same drop logic. Eliminating the freeze itself
   (not just the post-freeze glitch) needs decode moved off the main
   thread entirely -- a real, separate piece of future work, not a
   quick fix.
5. **Blocky/aliased scaling, read as "fuzzy" playback.** SDL defaults to
   nearest-neighbor texture scaling; any time the destination size
   doesn't exactly match the video's native resolution -- which the
   window-clamping fix above makes essentially guaranteed for large
   videos -- that looks visibly blocky rather than smooth. Fixed with
   `SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear")`, set before
   `SDL_CreateTexture` (the hint is read once at texture-creation time;
   setting it later has no effect on an already-created texture).
   Verified directly: queried the texture's actual scale mode after
   creation and confirmed it's `1` (linear), not the default `0`
   (nearest). Also added `SDL_WINDOW_ALLOW_HIGHDPI` to the window
   flags -- a DPI-unaware window on a scaled display gets
   compatibility-stretched by Windows itself, producing the identical
   blurry symptom through a completely different layer; costs nothing
   on non-scaled displays.

## Proven in this sandbox (synthetic pipeline)

1. **Full pipeline works end to end** with the synthetic demuxer/decoder/
   output plugins -- `demux -> decode -> present`, ten frames, clean exit.
2. **Removing a plugin's manifest makes its codec genuinely unsupported**,
   not hidden: deleting `decoder_passthrough.manifest.json` and re-running
   produces `no installed decoder claims codec 'SYNT'` and a non-zero exit
   -- `libdecoder_passthrough.so` is never `dlopen`'d, because the registry
   never told the core it exists.
3. **A real codec wrapper compiles and links clean** against actual
   libavcodec/libavutil, proving the "wrap one decoder, ship it as its own
   tiny plugin" pattern is not just theoretical.
4. **`lumenctl install/remove/list`** works as a real CLI against a
   "package" directory containing a manifest + library pair.

## Porting to Windows

The only OS-specific code is already isolated and `#ifdef`'d:

- `core/plugin_loader.c` -- `LoadLibraryA`/`GetProcAddress`/`FreeLibrary`
  branch already written, just needs testing.
- `core/registry.c` -- `FindFirstFileA`/`FindNextFileA` branch already
  written for directory scanning, alongside the POSIX `dirent.h` path.
- `include/lumen_plugin.h` -- `LUMEN_EXPORT` already expands to
  `__declspec(dllexport)` under `_WIN32`.
- `third_party/tinyfiledialogs/tinyfiledialogs.c` -- its native Win32
  file-dialog backend needs `comdlg32`/`ole32`, already wired up via a
  `WIN32`-conditioned `target_link_libraries` in CMakeLists.txt. No
  extra MSYS2 packages needed for this specifically (those libs ship
  with the standard mingw-w64 toolchain).

To actually build on Windows:

1. Install MSYS2 + mingw-w64 (the same environment VideoLAN itself uses
   for official VLC Windows builds), or use MSVC + CMake directly --
   the CMakeLists.txt here doesn't use anything POSIX-specific except the
   `dl` link library, which is conditioned on `UNIX`.
2. For the H.264 plugin (or any other real codec), build a **minimal**
   ffmpeg yourself with `--enable-decoder=h264 --disable-everything-else`
   to keep that one plugin small, the same MSYS2 cross-build approach
   discussed earlier for getting a specific patched ffmpeg version onto
   Windows -- except now you only need to rebuild *this one plugin* when
   a codec-specific CVE drops, not your whole player.
3. UI: real video/audio output already exists (`output_sdl2`/
   `output_sdl2_audio`, SDL2 -- already confirmed working on Windows via
   MSYS2's `mingw-w64-x86_64-SDL2` package). The next UI step is an
   ImGui controls overlay drawn into that same SDL2 window/renderer --
   deliberately not Qt, to avoid the C++ boundary and the Qt5/Qt6
   version churn that prompted this choice in the first place.

## What's intentionally NOT here yet

- **Rounding out the controls.** Play/pause/seek/volume/fullscreen exist
  and are verified working (real seek landing on the right keyframe,
  real pause silencing audio immediately, not just stopping new
  queuing). Deliberately NOT yet done: a separate Stop button (only
  window-close/ESC ends playback for now), Space-bar-toggles-pause
  (skipped to avoid a subtle conflict with ImGui's own Space-activates-
  focused-button behavior -- needs slightly more care, not just wiring
  up another `SDL_SCANCODE`), and seeking after the stream naturally
  reaches EOF (the playback function has already started tearing
  everything down by then -- restarting the file is the current
  workaround).
- **True A/V sync.** Video paces against wall-clock-since-first-frame
  and drops frames more than 100ms stale (see above); audio paces itself
  independently via hardware draining its queue. Both are independently
  correct-ish, not jointly frame-accurate -- the drop mechanism already
  exists, it's just measuring against the wrong clock. The standard fix
  is an audio-driven clock: swap wall-clock-since-start for the audio
  output's actual playback position (queried from the audio device) as
  what `output_sdl2` paces and drops against. Pause/resume and seeking
  now exist and use this same clock/drop machinery (`seek_generation`
  triggers a resync exactly the way a window-resize stall's backlog
  already gets cleanly dropped rather than burned through) -- but they're
  resyncing against the *wrong* clock too, for the same reason.
- **Hardware-accelerated decode, as an installable alternative, not a
  replacement.** Software `decoder_h264` is the right default (modern
  CPUs decode H.264/most codecs far faster than realtime; file size
  barely affects decode cost, resolution/bitrate/codec complexity do).
  A `decoder_h264_hwaccel.c` using ffmpeg's unified hwaccel API
  (`av_hwdevice_ctx_create` + `AVCodecContext.hw_device_ctx` -- one
  plugin, picks D3D11VA/VAAPI/NVDEC per platform at runtime) would claim
  the same `H264` fourcc as the software decoder, letting the user
  choose which is installed/active -- the literal Gentoo-USE-flag pitch
  this whole project started from. Needs a registry upgrade first,
  since `lumen_registry_find_decoder_by_fourcc` currently returns
  whichever plugin the directory scan happens to find first if two
  claim the same fourcc -- undefined, not a real choice. The agreed
  design: a `priority` field in the manifest, the registry returns
  candidates in priority order rather than a single winner, and
  `player_core.c` tries the highest-priority candidate's `open()` first
  and **falls through to the next one if it fails** -- so a hardware
  decoder that can't find a compatible GPU at runtime automatically and
  silently falls back to software, the same `update-alternatives`-style
  pattern as Gentoo's own slot/priority resolution, just applied to
  decoder selection.
- **Decode off the main thread.** Right now demux+decode+present all run
  on one thread, so any OS-modal operation (window move/resize) freezes
  playback entirely for its duration -- confirmed directly as the cause
  of the freeze-while-dragging behavior. The drop-stale-frames fix above
  makes *resuming* clean, but doesn't stop the freeze itself. Fixing
  that for real means a decode thread feeding a frame queue, with the
  main thread's event/render loop continuing to drain already-decoded
  frames even while modally blocked on a drag -- a real architecture
  change (queue, mutex/condvar synchronization), not a quick flag.
- **Per-decoder process sandboxing.** Right now plugins run in-process,
  same as VLC. The next real security hardening step is running each
  decoder plugin in its own child process (job object / AppContainer on
  Windows, seccomp+namespaces on Linux) talking to the core over a shared
  memory ring buffer, so a memory-corruption bug in an *installed* decoder
  is contained rather than full process compromise. Worth doing once the
  plugin surface area is real; not needed to validate the architecture.
- **More container/codec coverage.** `demux_mp4` handles MP4/MOV;
  MKV/WebM (`demux_mkv.c`, wrapping the same libavformat API -- it's
  container-agnostic) is the natural next one. HEVC/AV1/VP9 decoders and
  an Opus decoder follow the exact same wrap-one-codec pattern as
  `decoder_h264.c`/`decoder_aac.c`. `demux_mp4` already reports `UNKN`
  fourccs gracefully for any codec without an installed decoder, so
  adding coverage is purely additive -- no core changes needed.
- **Manifest signing.** `lumenctl` currently trusts whatever's in the
  package dir. Given your Ed25519-signing habits elsewhere, that's the
  natural next addition: sign manifests, verify before `lumen_load_plugin`.
