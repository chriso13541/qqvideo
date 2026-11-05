/* tempo.h -- change the speed of audio without changing its pitch.
 *
 * WSOLA (waveform-similarity overlap-add), the same family of method as
 * FFmpeg's atempo filter: the output is built from 30 ms Hann-windowed
 * slices of the input, overlapped by half. Slices are taken from the input
 * `speed` times faster (or slower) than they're laid down, and each one is
 * shifted by up to +-10 ms to where its waveform best continues the previous
 * slice (cross-correlation), so the seams don't click or warble. Pitch stays
 * put; only the timing changes. Works for 0.25x to 4x and any channel count.
 *
 * Streaming: feed decoded frames in, take what it produced out. It holds
 * ~40 ms of input back (the window plus the search range). */
#pragma once
#include <stdint.h>

typedef struct tempo tempo_t;

tempo_t *tempo_new(int channels, int sample_rate);
void     tempo_free(tempo_t *t);

/* Forget everything (after a seek, or when the speed goes back to 1.0). */
void     tempo_reset(tempo_t *t);

/* Feed `frames` interleaved S16 sample frames and process at `speed`.
 * Output (float, interleaved, roughly -32768..32767) is appended to an
 * internal buffer; take it with tempo_take(). Returns 0, or -1 when out
 * of memory. */
int      tempo_feed(tempo_t *t, const int16_t *in, int frames, double speed);

/* The output produced so far: *frames sample frames at the returned
 * pointer, valid until the next call. The buffer is emptied. */
const float *tempo_take(tempo_t *t, int *frames);

/* Position in the INPUT (sample frames since the last reset) that the end
 * of the output produced so far corresponds to -- for the audio clock. */
double   tempo_input_pos(const tempo_t *t);
