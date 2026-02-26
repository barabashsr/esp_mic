#pragma once

#include <stdio.h>
#include <stdint.h>

// Open a new WAV file and write the header (placeholder sizes).
// Returns file handle or NULL on error.
FILE *wav_open(const char *path, int sample_rate, int bits_per_sample, int channels);

// Append PCM data to an open WAV file. Returns bytes written.
size_t wav_write(FILE *f, const int16_t *samples, size_t num_samples);

// Open a new WAV file with µ-law encoding (audio_format=7, 8-bit).
FILE *wav_open_ulaw(const char *path, int sample_rate, int channels);

// Append µ-law encoded data: converts int16 PCM to 8-bit µ-law and writes.
size_t wav_write_ulaw(FILE *f, const int16_t *samples, size_t num_samples);

// Finalize the WAV file: seek back and fix RIFF/data sizes, then close.
void wav_close(FILE *f);
