#pragma once

#include <stdio.h>
#include <stdint.h>

// Open a new WAV file and write the header (placeholder sizes).
// Returns file handle or NULL on error.
FILE *wav_open(const char *path, int sample_rate, int bits_per_sample, int channels);

// Append PCM data to an open WAV file. Returns bytes written.
size_t wav_write(FILE *f, const int16_t *samples, size_t num_samples);

// Finalize the WAV file: seek back and fix RIFF/data sizes, then close.
void wav_close(FILE *f);
