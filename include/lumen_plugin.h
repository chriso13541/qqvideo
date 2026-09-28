/*
 * lumen_plugin.h
 *
 * The stable C ABI every Lumen plugin implements. This is the ONLY header
 * a plugin author needs to include. Keep this file append-only across
 * versions (never remove/reorder existing struct fields) so old plugins
 * keep working against newer cores, and bump LUMEN_ABI_VERSION whenever
 * you DO need a breaking change.
 *
 * v2 change: the core now drives multiple streams (video + audio) per
 * file instead of assuming exactly one video stream. lumen_stream_info_t
 * became lumen_stream_table_t (an array of per-stream descriptors),
 * lumen_frame_t became a tagged union (video pixel planes OR audio
 * samples), decoder open() takes a stream descriptor instead of bare
 * width/height, and the single LUMEN_PLUGIN_OUTPUT kind split into
 * LUMEN_PLUGIN_OUTPUT_VIDEO and LUMEN_PLUGIN_OUTPUT_AUDIO. This is a
 * breaking ABI change -- every plugin in this tree was rebuilt against
 * it together; a v1 plugin binary will be correctly rejected by the
 * ABI version check in plugin_loader.c rather than silently misread.
 */
#ifndef LUMEN_PLUGIN_H
#define LUMEN_PLUGIN_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- export macro: same plugin source compiles for Windows .dll or Linux .so ---- */
#if defined(_WIN32)
  #define LUMEN_EXPORT __declspec(dllexport)
#else
  #define LUMEN_EXPORT __attribute__((visibility("default")))
#endif

#define LUMEN_ABI_VERSION 20

/* v16: was 8. A Blu-ray rip MKV routinely has 1 video + several audio +
 * 10-20 subtitle streams; with 8, every stream past index 7 was silently
 * dropped by the demuxer. */
#define LUMEN_MAX_STREAMS 32
#define LUMEN_MAX_SUB_TRACKS 32
#define LUMEN_PENDING_QUEUE_MAX 16
#define LUMEN_MAX_PLUGIN_INFOS  48
#define LUMEN_MAX_DISABLED_PAIRS 32   /* max codec-per-container blocks */

typedef enum {
    LUMEN_PLUGIN_DEMUXER      = 1,
    LUMEN_PLUGIN_DECODER      = 2,
    LUMEN_PLUGIN_OUTPUT_VIDEO = 3,
    LUMEN_PLUGIN_OUTPUT_AUDIO = 4
} lumen_plugin_kind_t;

typedef enum {
    LUMEN_STREAM_VIDEO = 1,
    LUMEN_STREAM_AUDIO = 2,
    LUMEN_STREAM_SUBTITLE = 3   /* v16 */
} lumen_stream_type_t;

typedef enum {
    LUMEN_PIXFMT_YUV420P = 1,
    LUMEN_PIXFMT_NV12    = 2,
    LUMEN_PIXFMT_RGB24   = 3
} lumen_pixfmt_t;

typedef enum {
    LUMEN_SAMPLEFMT_S16 = 1,  /* signed 16-bit interleaved */
    LUMEN_SAMPLEFMT_FLT = 2   /* 32-bit float interleaved */
} lumen_samplefmt_t;

/* ---------------------------------------------------------------------- */
/* Shared data types passed across the plugin boundary                    */
/* ---------------------------------------------------------------------- */

typedef struct {
    uint8_t  *data;
    size_t    size;
    int64_t   pts;
    int       stream_index;   /* which stream this packet belongs to --
                                * the core routes it to that stream's
                                * decoder, never to "the" decoder */
    int       keyframe;
    /* Audio-only, usually 0. Some containers (MP4) record that the
     * FIRST decoded frame of an AAC stream contains N "priming" samples
     * the encoder needed for lookahead, which must be discarded from
     * the start of decoded output -- not bytes of this packet, samples
     * of what decoding it produces. Demuxers that read this from
     * container side-data (see demux_mp4.c) set it; decoders that
     * support it (see decoder_aac.c) trim accordingly. Demuxers that
     * don't know about it (everything else in this tree) correctly
     * leave it 0 -- player_core.c zero-initializes every packet before
     * read_packet() fills it in, so this is never garbage. */
    int       skip_samples_start;
    /* v15. The other half of the same side data: samples to discard from
     * the END of this packet's decoded output -- the encoder's trailing
     * padding on the final packet. Matroska/WebM and Ogg Opus files carry
     * this (DiscardPadding); without honoring it an Opus track plays up to
     * a frame of padding past its real end (confirmed: 648 extra samples
     * on a 2.0s test file versus ffmpeg's reference decode). Same
     * zero-default convention as skip_samples_start. */
    int       skip_samples_end;
    /* v16. Presentation time and duration in MILLISECONDS on the same
     * timeline the video decoder's frame pts use (0 = first video frame),
     * or -1 if unknown. `pts` above stays in raw container ticks for
     * backward compatibility; these exist because a decoder never learns
     * the stream's time_base. Currently only consumed for subtitle
     * packets, whose cue timing comes straight from the container. */
    int64_t   pts_ms;
    int64_t   duration_ms;
    /* Demuxer authors: FFmpeg-based decoders need 64 zeroed bytes after
     * `size` (AV_INPUT_BUFFER_PADDING_SIZE). demux_libav provides them and
     * decoder_libav re-pads defensively; don't assume either elsewhere. */
} lumen_packet_t;

/* One entry per elementary stream a demuxer found in the file. A typical
 * MP4 has 2 of these: one VIDEO (H264/AVC1), one AUDIO (AAC). */
typedef struct {
    int                  stream_index;
    lumen_stream_type_t  type;
    char                 codec_fourcc[8];  /* "H264", "AAC ", "OPUS", ... */
    /* video-only fields (ignored for audio streams) */
    int width, height;
    /* Frames per second, from the container (e.g. MP4's stbl box via
     * avg_frame_rate). Needed for real-time presentation pacing --
     * without it, decode (much faster than realtime on modern hardware)
     * would blast through an entire movie in a couple of seconds.
     * 0 means unknown (e.g. a raw elementary stream with no container
     * metadata); decoders/outputs should fall back to a sane assumed
     * default (see decoder_h264.c) rather than not pacing at all. */
    double frame_rate;
    /* audio-only fields (ignored for video streams) */
    int sample_rate;
    int channels;
    /* Out-of-band codec configuration data, when the container carries
     * it separately from the bitstream itself -- MP4's avcC box (H.264
     * SPS/PPS) or esds box (AAC's AudioSpecificConfig), for example.
     * Raw elementary streams (Annex-B H.264, ADTS AAC) carry this
     * in-band instead and should leave this NULL/0; decoder plugins
     * must handle both cases (libavcodec does this automatically once
     * extradata is copied into AVCodecContext before avcodec_open2 --
     * see decoder_h264.c / decoder_aac.c). Pointer is borrowed from the
     * demuxer and valid only until that demuxer's close() is called,
     * which happens after every decoder is already done with it. */
    const uint8_t       *extradata;
    int                   extradata_size;
    /* v16. From container metadata where present (Matroska/MP4 tags),
     * else empty. Used to label tracks in the Subtitles menu. */
    char                  language[8];   /* ISO 639-2, e.g. "eng" */
    char                  title[64];     /* e.g. "English (SDH)" */
    int                   is_default;    /* container's "default track" flag */
    int                   is_forced;     /* "forced" subtitles (foreign dialogue only) */
} lumen_stream_desc_t;

typedef struct {
    int                   stream_count;
    lumen_stream_desc_t   streams[LUMEN_MAX_STREAMS];
    double                duration_sec;
} lumen_stream_table_t;

/* A decoded frame, tagged by stream type. Exactly one arm of the union
 * is valid, selected by `type`. */
typedef struct {
    lumen_stream_type_t type;
    int64_t              pts;
    union {
        struct {
            uint8_t *planes[4];
            int      stride[4];
            int      width;
            int      height;
            int      pixel_format;
        } video;
        struct {
            uint8_t *data;       /* interleaved samples, all channels */
            size_t   size;
            int      sample_rate;
            int      channels;
            int      sample_fmt; /* lumen_samplefmt_t */
            int      nb_samples; /* samples per channel in this buffer */
        } audio;
        /* v16. One subtitle cue. Plain UTF-8, lines separated by '\n',
         * styling/override tags already stripped by the decoder. */
        struct {
            char    *text;
            int64_t  start_ms;
            int64_t  end_ms;     /* -1 = until the next cue replaces it */
        } subtitle;
    };
} lumen_frame_t;

/* ---------------------------------------------------------------------- */
/* Per-container codec block entry. When present in state->disabled_pairs[],
 * player_core.c refuses to load the named decoder for the named demuxer.
 * Populated by the Package Manager UI (output_sdl2.cpp) when the user
 * toggles "Disable for this container" on a codec entry. */
typedef struct {
    char demuxer_name[64]; /* e.g., "mp4-demuxer" */
    char codec_name[64];   /* e.g., "h264-decoder" */
} lumen_disabled_pair_t;

/* Per-plugin info surfaced to the Package Manager UI. */
/* Populated by main.c after the registry scan; read-only from the output */
/* plugin's perspective. Mirrors lumen_registry_entry_t but sized for     */
/* display rather than full path storage.                                 */
/* ---------------------------------------------------------------------- */
typedef struct {
    char name[64];
    char version[32];
    char kind_str[24];   /* human-readable kind: "Demuxer", "Decoder", "Video Out", etc. */
    char claims[64];     /* comma-separated fourccs or extensions this plugin handles */
} lumen_plugin_info_t;

/* ---------------------------------------------------------------------- */
/* UI entry for one package in the Package Manager.  Populated by main.c from
 * the packages scan result; read by output_sdl2.cpp without ever including
 * packages.h (which is a C-only header that causes MinGW C++ link errors). */
typedef struct {
    char name[64];      /* "H.264 / AVC", "MP4 / M4V / MOV", etc. */
    char detail[64];    /* fourcc for codecs; extensions for demuxers */
    char version[32];
    char dir_path[512]; /* absolute path to package folder (for Install button) */
    char plugin[64];    /* "demux_mp4", "decoder_h264", etc. */
    int  type;          /* LUMEN_PKG_UI_DEMUXER / VIDEO / AUDIO */
    int  installed;     /* 1 = DLL present in dir_path */
} lumen_pkg_ui_entry_t;

/* Shared playback state for transport controls (play/pause/seek/volume)  */
/*                                                                        */
/* player_core.c owns one instance per playback session and hands a       */
/* pointer to it to the video output via bind_state() (see                */
/* lumen_video_output_vtable_t below). The output plugin draws controls   */
/* that read/write these fields directly (e.g. a Pause button just sets   */
/* paused=1); player_core.c's main loop reads them every iteration to     */
/* decide whether to keep decoding, perform a seek, etc. Plain shared     */
/* memory rather than a callback table -- both sides run on the same      */
/* thread (no locking needed), and this is simpler to reason about than   */
/* a function-pointer indirection for what's fundamentally just state.    */
/* ---------------------------------------------------------------------- */
typedef struct {
    int      paused;
    int      quit_requested;     /* user closed the window / pressed a quit control */

    /* True only while a file is actually loaded/playing. False during
     * the idle state (window open, nothing loaded) introduced for the
     * persistent-window architecture -- lets output plugins render
     * sensibly when idle (e.g. skip drawing a seek bar that has
     * nothing to seek within) instead of showing controls that would
     * silently do nothing or, worse, queue a stale request that fires
     * the moment a file actually loads. */
    int      has_file;

    /* Set by the Stop button: stops current playback, clears the queue,
     * and returns to the idle blank-player state. Different from
     * quit_requested (closes the whole app) and open_file_requested
     * (prompts for a new file). Propagated via lumen_play_stats_t
     * so main.c can call queue_clear() and go idle. */
    int      stop_requested;

    /* Mute toggle: independent of volume so the slider position is
     * preserved while muted. Audio pump_ui() clears the device queue
     * immediately on the mute→unmute transition for instant silence;
     * audio present() skips queuing new frames while muted. */
    int      muted;

    /* Set by the File menu's "Open File..." item. When triggered via the
     * threaded dialog (output_sdl2.cpp), the selected path is also stored
     * in open_file_pending_path so main.c doesn't need to pop a second
     * blocking dialog -- it just reads the path directly. Empty = cancelled
     * or not yet used. */
    int  open_file_requested;
    char open_file_pending_path[512];

    /* Seeking: the output plugin sets seek_requested=1 and
     * seek_target_ms to the desired position; player_core.c performs
     * the actual seek (demuxer seek + decoder flush + clock resync),
     * then clears seek_requested back to 0. The output plugin should
     * treat seek_requested!=0 as "a seek is pending, don't issue
     * another one yet". */
    int      seek_requested;
    int64_t  seek_target_ms;

    /* Position/duration, kept current by player_core.c so the UI can
     * draw a seek bar and time labels without re-deriving this itself. */
    int64_t  position_ms;
    int64_t  duration_ms;

    /* 0.0 - 1.0. Audio output plugins read this and scale PCM samples
     * before queuing them -- see output_sdl2_audio.c. */
    float    volume;

    /* ---- Queue state ---- */

    /* Queue display -- arrays owned by main.c, kept current before
     * every idle/play call. Output plugins treat these as read-only;
     * main.c is the sole writer. */
    const char * const *queue_names;   /* display names (basename only) for each item */
    int                 queue_count;
    int                 queue_position; /* index of currently playing item; -1 while idle */

    /* Output plugin sets show_queue to toggle the queue panel. Persists
     * across files (it's a UI preference, not a per-file thing). */
    int  show_queue;

    /* Output plugin sets these when the user double-clicks a queue item.
     * player_core.c's main loop checks them and stops playback with
     * queue_jump_index reported in lumen_play_stats_t so main.c can
     * jump to that position. Set to 0 by player_core after consuming. */
    int  queue_jump_requested;
    int  queue_jump_index;

    /* Pending additions from "Add to Queue" (output plugin writes paths
     * here after showing the file dialog, main.c drains them between
     * files). Lets the user queue up future files WITHOUT interrupting
     * the currently-playing one -- the paths just accumulate here
     * silently and get appended to the queue the next time main.c
     * regains control (natural file end, seek, etc). */
    char pending_add_paths[LUMEN_PENDING_QUEUE_MAX][512];
    int  pending_add_count;

    /* Monotonic per-session counter for seek clock resync (v6/v7). Output
     * plugins each keep their OWN local copy of "the last generation I
     * reacted to" and compare against this each tick -- e.g. video
     * resyncs its wall-clock pacing reference, audio clears whatever
     * stale PCM is still sitting in its queue from before the seek.
     * A monotonic counter (rather than a single pending/cleared flag)
     * means two independent consumers can't race over who gets to see
     * and clear it -- each just remembers its own last-seen value. */
    int64_t  seek_generation;

    /* ---- Package Manager ---- */
    int show_package_manager;

    /* Codec-per-container block list. Populated by the Package Manager UI;
     * read by player_core.c before loading any decoder. When a (demuxer_name,
     * codec_name) pair is present here, the decoder is treated as if it were
     * not installed for that container only. Takes effect on the next file open. */
    lumen_disabled_pair_t disabled_pairs[LUMEN_MAX_DISABLED_PAIRS];
    int                   disabled_pair_count;

    /* Registry snapshot populated by main.c after lumen_registry_scan(). */
    lumen_plugin_info_t installed_plugins[LUMEN_MAX_PLUGIN_INFOS];
    int                 installed_plugin_count;

    /* Error message set by player_core.c when a required codec is missing
     * or blocked. The output plugin displays this as an overlay and clears
     * it when the user interacts. Empty string = no error. */
    char error_msg[256];

    /* Path to the packages/ directory (set by main.c after lumen_packages_scan).
     * The Package Manager window re-scans this directory when it opens so it
     * always reflects the real filesystem state, not a cached snapshot. */
    char packages_dir[512];

    /* ---- Package Manager UI data ---- */
    /* Flat list of every known package (installed or not) populated by main.c
     * after lumen_packages_scan(). The Package Manager in output_sdl2.cpp reads
     * ONLY this array -- it never includes packages.h or calls C-only scanner
     * functions directly, which avoids C/C++ name-mangling conflicts under MinGW. */
#define LUMEN_PKG_UI_MAX       32
#define LUMEN_PKG_UI_DEMUXER    0   /* container reader  */
#define LUMEN_PKG_UI_VIDEO      1   /* video decoder     */
#define LUMEN_PKG_UI_AUDIO      2   /* audio decoder     */
#define LUMEN_PKG_UI_SUBTITLE   3   /* subtitle decoder (v16) */
    lumen_pkg_ui_entry_t pkg_entries[LUMEN_PKG_UI_MAX];
    int                  pkg_entry_count;
    /* Set by Package Manager after Install -- main.c rescans and refreshes pkg_entries */
    int                  rescan_packages_requested;

    /* ---- Subtitles (v16) ----
     * player_core.c owns the cues. It publishes the track list and the
     * text to show RIGHT NOW (for the selected track at position_ms), so
     * an output plugin only ever reads subtitle_text and draws it. */
    struct {
        char label[96];   /* "English (SDH) [SRT]", "movie.en.srt" */
        int  available;   /* 0 = listed but can't be shown (image-based) */
        int  external;    /* 1 = separate file (found next to the movie, or added) */
        char file[128];   /* external: the file's name, shown as a tooltip */
    } subtitle_tracks[LUMEN_MAX_SUB_TRACKS];
    int  subtitle_track_count;
    int  subtitle_selected;          /* index into subtitle_tracks, -1 = None. UI writes. */
    char subtitle_text[1024];        /* current on-screen text, "" = nothing. Core writes. */
    /* UI sets these after the "Add Subtitle Track..." dialog; the core
     * loads the file, adds + selects the track, then clears the flag. */
    char subtitle_add_path[512];
    int  subtitle_add_requested;

    /* v17. Set by player_core.c before each video present(): 1 = the core
     * already waited for this frame's moment on the audio clock, so show it
     * immediately; 0 = no audio clock (silent file / audio not started yet),
     * pace against the wall clock as before. */
    int  video_sync_external;

    /* v19. Direction for the next seek (LUMEN_SEEK_FORWARD / _BACKWARD),
     * set by the UI together with seek_requested. Arrow-key jumps use the
     * direction of travel; the seek bar uses BACKWARD (the default). */
    int  seek_flags;

    /* v20. 1 = precise seeking (default): land on the exact target time by
     * decoding from the keyframe before it and discarding what comes first.
     * 0 = keyframe seeking: instant, but lands on the nearest keyframe --
     * worth it for 4K HEVC on a slow CPU, where catching up from a keyframe
     * several seconds back takes a while. Toggled in Tools. */
    int  precise_seek;
} lumen_playback_state_t;

/* ---------------------------------------------------------------------- */
/* Demuxer plugin vtable                                                  */
/* ---------------------------------------------------------------------- */

typedef struct lumen_demuxer_ctx lumen_demuxer_ctx_t; /* opaque, plugin-owned */

typedef struct {
    int (*probe)(const uint8_t *header_bytes, size_t header_len, const char *file_ext);

    /* Open the source and report EVERY stream found, video and audio. */
    int (*open)(lumen_demuxer_ctx_t **out_ctx, const char *path, lumen_stream_table_t *out_table);

    /* Fill *out_pkt with the next packet from ANY stream -- check
     * out_pkt->stream_index to know which one. Returns 0 on success,
     * 1 on EOF, <0 on error. */
    int (*read_packet)(lumen_demuxer_ctx_t *ctx, lumen_packet_t *out_pkt);

    void (*packet_free)(lumen_packet_t *pkt);

    /* Optional (NULL is valid -- e.g. raw elementary stream demuxers
     * have no container-level index to seek with, and correctly don't
     * support this; player_core.c checks for NULL and ignores seek
     * requests gracefully rather than crashing). Seeks to the nearest
     * keyframe at or before target_ms. Returns 0 on success. */
    int (*seek)(lumen_demuxer_ctx_t *ctx, int64_t target_ms);

    void (*close)(lumen_demuxer_ctx_t *ctx);

    /* v15, optional (NULL is valid; the core falls back to open()).
     * Same as open(), but the core also passes the codec fourccs that are
     * actually allowed for this file: every fourcc an installed decoder
     * claims, minus anything the user disabled for this container.
     *
     * Why this exists: libavformat's avformat_find_stream_info() opens
     * DECODERS internally to probe stream parameters. With a system-wide
     * libavcodec (every decoder the distro enabled lives in one .so), that
     * means a file could get fed to a decoder no Lumen plugin claims --
     * e.g. an MKV with a MagicYUV track reaching the MagicYUV decoder
     * during probing, without the registry ever being consulted.
     * libav-backed demuxers must turn this list into libavformat's
     * codec_whitelist so only allowed decoders can ever be opened. */
    int (*open_ex)(lumen_demuxer_ctx_t **out_ctx, const char *path,
                   lumen_stream_table_t *out_table,
                   const char *const *allowed_fourccs, int allowed_count);

    /* v19, optional (NULL = the core uses seek()). Seek with a direction:
     *   LUMEN_SEEK_FORWARD:  land on the first keyframe AT OR AFTER target
     *   LUMEN_SEEK_BACKWARD: land on the last keyframe AT OR BEFORE target
     *                        (what plain seek() does)
     * Relative jumps need this: with keyframes every ~10 s, "position +
     * 10 s, then back to the keyframe before it" can land on the keyframe
     * you started from, and pressing Right does nothing. Returns -1 if
     * there's no keyframe in that direction (e.g. forward near the end). */
    int (*seek_ex)(lumen_demuxer_ctx_t *ctx, int64_t target_ms, int flags);
} lumen_demuxer_vtable_t;

/* ---------------------------------------------------------------------- */
/* Decoder plugin vtable                                                  */
/* One decoder instance is opened PER STREAM -- a file with 1 video + 1   */
/* audio stream gets two separate decoder contexts, even if (in a never  */
/* realistic case) the same plugin somehow claimed both fourccs.         */
/* ---------------------------------------------------------------------- */

typedef struct lumen_decoder_ctx lumen_decoder_ctx_t; /* opaque, plugin-owned */

#define LUMEN_PROBE_VIDEO 1
#define LUMEN_PROBE_AUDIO 2
#define LUMEN_SEEK_BACKWARD 0   /* seek_ex flags (v19) */
#define LUMEN_SEEK_FORWARD  1
#define LUMEN_PROBE_SUBTITLE 3

typedef struct {
    /* Returns 0 if this decoder cannot handle the fourcc. Nonzero means
     * it can; decoders MAY return LUMEN_PROBE_VIDEO / LUMEN_PROBE_AUDIO
     * to also say which kind (decoder_libav does, so the core can list
     * what the system FFmpeg actually provides without linking libav). */
    int (*probe)(const char *codec_fourcc);

    /* `stream` describes the specific stream this decoder instance will
     * handle -- width/height for video, sample_rate/channels for audio. */
    int (*open)(lumen_decoder_ctx_t **out_ctx, const lumen_stream_desc_t *stream);

    /* 0 = a frame is ready in out_frame, 1 = need more packets, <0 = error */
    int (*decode)(lumen_decoder_ctx_t *ctx, const lumen_packet_t *pkt, lumen_frame_t *out_frame);

    void (*frame_free)(lumen_frame_t *frame);

    /* Optional. See player_core.c -- call after demux EOF, in a loop,
     * until it returns 1, to retrieve frames still buffered internally
     * for reordering (e.g. B-frames). NULL is valid if the codec never
     * buffers (nothing to drain). */
    int (*drain)(lumen_decoder_ctx_t *ctx, lumen_frame_t *out_frame);

    /* Optional (NULL is valid). Discards any internal buffering (B-frame
     * reorder state, etc.) WITHOUT closing the decoder -- needed after a
     * seek, where packets before the seek target are no longer
     * relevant and must not bleed into frames decoded after it.
     *
     * `resume_at_ms` tells the decoder what absolute position playback
     * is resuming at. This matters specifically because decoder_h264.c
     * assigns pts as "frames emitted since open/last flush * frame
     * duration" -- resetting that counter to 0 on every flush (as an
     * earlier version of this did) makes pacing work but silently
     * breaks position tracking: the very next frame's pts would read
     * ~0 instead of continuing from the seek target, which is exactly
     * what made the seek bar visibly reset to the start after a
     * successful seek even though playback itself had correctly moved
     * (confirmed directly as the cause, not a guess). Passing the
     * target back in lets the decoder reseed its counter so pts keeps
     * meaning "absolute position", not "time since last flush". */
    void (*flush)(lumen_decoder_ctx_t *ctx, int64_t resume_at_ms);

    void (*close)(lumen_decoder_ctx_t *ctx);

    /* v18, optional. Returns another frame produced by input already given
     * to decode(), without new input: 0 = frame in *out, 1 = none left.
     * The core calls it after every decode() until it returns 1, so a
     * packet that yields several frames never loses any. (decode() alone
     * can only hand back one frame per packet.) */
    int  (*receive)(lumen_decoder_ctx_t *ctx, lumen_frame_t *out);

    /* v18, optional. Speed/quality trade-off the core applies when decoding
     * can't keep up with playback:
     *   0 = decode everything (default)
     *   1 = skip frames no other frame depends on (non-reference/B-frames):
     *       lower frame rate, no quality loss on the frames that remain
     *   2 = also skip the in-loop deblocking filter: much faster, blockier
     * Keeping up matters more than every frame: falling behind stalls the
     * audio too, since one loop decodes both. */
    void (*set_skip)(lumen_decoder_ctx_t *ctx, int level);

} lumen_decoder_vtable_t;

/* ---------------------------------------------------------------------- */
/* Output plugin vtables -- separate shapes for video vs audio            */
/* ---------------------------------------------------------------------- */

typedef struct lumen_output_ctx lumen_output_ctx_t; /* opaque, plugin-owned */

typedef struct {
    /* Creates the window/renderer ONCE, before any file is loaded --
     * this is what lets the app launch into a real, visible "blank
     * player" (like VLC's idle window) rather than only existing for
     * the duration of one file. No width/height here on purpose: a
     * persistent window can't know any file's dimensions yet. */
    int  (*open)(lumen_output_ctx_t **out_ctx);

    /* Called once per file (including the FIRST one, and again every
     * time a different file is loaded into the same still-open window)
     * to (re)configure for that file's actual video parameters --
     * recreating the texture, resizing the window to fit, resetting
     * pacing state. NOT optional: every video output plugin needs a
     * way to receive per-file dimensions now that open() no longer
     * carries them. Returns 0 on success. */
    int  (*load_stream)(lumen_output_ctx_t *ctx, int width, int height, int pixel_format);

    /* Optional. Called once after a successful open() -- gives the
     * output plugin a pointer to the shared playback state it can draw
     * controls against (play/pause button toggles state->paused,
     * dragging a seek bar sets state->seek_requested, etc). NULL is
     * valid for an output plugin with no UI (e.g. output_console.c). */
    void (*bind_state)(lumen_output_ctx_t *ctx, lumen_playback_state_t *state);

    /* Optional. Called by player_core.c on EVERY loop iteration,
     * including while paused, between frames, AND while idle with no
     * file loaded at all -- NOT just when a new frame exists
     * (present() only fires when there's actually a decoded frame to
     * show). This is what keeps the window responsive and the UI
     * redrawn/interactive at every point in the app's lifecycle, not
     * just during active playback. Returns nonzero to request stop,
     * same convention as present(). */
    int  (*pump_ui)(lumen_output_ctx_t *ctx, lumen_playback_state_t *state);

    /* Return 0 normally. Return nonzero to ask the core to stop playback
     * entirely -- e.g. the user closed the SDL window or pressed ESC.
     * Either output plugin (video or audio) requesting stop ends both. */
    int  (*present)(lumen_output_ctx_t *ctx, const lumen_frame_t *frame);

    /* Tears down the window/renderer for real -- only called when the
     * whole app is exiting, NOT between files (load_stream handles
     * switching files within the same still-open window). */
    void (*close)(lumen_output_ctx_t *ctx);
} lumen_video_output_vtable_t;

typedef struct {
    /* Initializes the audio subsystem ONCE, but does NOT open an actual
     * device yet -- no file's sample rate/channels are known until
     * load_stream(). There's no real cost to "no device open" while
     * idle (no file loaded), unlike video where a window still needs
     * to exist to look at. */
    int  (*open)(lumen_output_ctx_t **out_ctx);

    /* Called once per file to open (or, if switching from a previous
     * file with different parameters, close and reopen) the actual
     * audio device -- SDL doesn't support changing format on an
     * already-open device, so a format change means a real close+
     * reopen under the hood, but the plugin object/ctx itself stays
     * alive across that, same as video's load_stream. Returns 0 on
     * success. */
    int  (*load_stream)(lumen_output_ctx_t *ctx, int sample_rate, int channels, int sample_fmt);

    /* Gives the audio output a pointer to read state->volume from when
     * scaling PCM before queuing -- see output_sdl2_audio.c. Optional. */
    void (*bind_state)(lumen_output_ctx_t *ctx, lumen_playback_state_t *state);
    /* Optional. Same role as the video output's pump_ui: called every
     * loop iteration regardless of whether a new frame exists, AND
     * while idle with no file loaded. Audio needs this specifically to
     * react to state->paused -- present() alone never fires while
     * paused, so without this, pressing pause would stop QUEUING new
     * audio but whatever's already buffered in the device would keep
     * playing for as long as that buffer lasts (confirmed: this is a
     * real, audible bug without it -- pause doesn't actually silence
     * sound immediately). Also where stale post-seek audio gets
     * flushed -- see seek_generation. */
    int  (*pump_ui)(lumen_output_ctx_t *ctx, lumen_playback_state_t *state);
    int  (*present)(lumen_output_ctx_t *ctx, const lumen_frame_t *frame); /* same stop convention as video */

    /* Tears down for real -- only on app exit, not between files. */
    void (*close)(lumen_output_ctx_t *ctx);

    /* v17, optional. The AUDIO CLOCK: the presentation time (ms, same
     * timeline as frame pts) of the sound coming out of the speakers right
     * now, plus how much audio is queued but not yet heard. Returns 1 if
     * the clock is valid, 0 if not (nothing queued since load/seek).
     *
     * player_core.c times video frames against this instead of the wall
     * clock ("audio master clock", as every real player does): video waits
     * for audio, never the other way round. That keeps lip sync through
     * pauses, stalls and sound-card clock drift, and it's how audio-only
     * files get paced at all -- without it nothing slowed the loop down
     * and a whole album decoded into RAM in seconds. */
    int  (*get_clock)(lumen_output_ctx_t *ctx, int64_t *clock_ms, int *buffered_ms);

} lumen_audio_output_vtable_t;

/* ---------------------------------------------------------------------- */
/* The single descriptor every plugin returns from its entry point        */
/* ---------------------------------------------------------------------- */

typedef struct {
    uint32_t            abi_version;
    lumen_plugin_kind_t  kind;
    const char          *name;
    const char          *version;
    union {
        const lumen_demuxer_vtable_t       *demuxer;
        const lumen_decoder_vtable_t       *decoder;
        const lumen_video_output_vtable_t  *video_output;
        const lumen_audio_output_vtable_t  *audio_output;
    } vtable;
} lumen_plugin_descriptor_t;

typedef const lumen_plugin_descriptor_t *(*lumen_get_plugin_fn)(void);
#define LUMEN_PLUGIN_ENTRYPOINT_NAME "lumen_get_plugin"

#ifdef __cplusplus
}
#endif

#endif /* LUMEN_PLUGIN_H */
