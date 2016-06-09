/* Copyright (c) 2016 Nick Appleton
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE. */

#ifndef WAV_DUMPER_H
#define WAV_DUMPER_H

#include <stdio.h>
#include <stdint.h>

#define WAV_DUMPER_WRITE_BUFFER_SIZE (4096)

struct wav_dumper {
	FILE          *f;
	uint_fast64_t  riff_size;
	unsigned       channels;
	unsigned       bits_per_sample;
	unsigned       block_align;
	unsigned       buffer_size;
	unsigned char  buffer[WAV_DUMPER_WRITE_BUFFER_SIZE];
};

/* Starts a wave dumper with the given format configuration.
 * Return value is zero if the wave file was opened and initialised
 * successfully. */
int wav_dumper_begin(struct wav_dumper *dump, const char *filename, size_t channels, unsigned bits_per_sample, uint_fast32_t rate);

unsigned wav_dumper_write_from_floats(struct wav_dumper *dump, const float *data, unsigned num_samples, unsigned sample_stride, unsigned channel_stride);

/* Close the wave dumper.
 * Undefined to close a dumper that is not opened.
 * Returns zero if the header was updated successfully. */
int wav_dumper_end(struct wav_dumper *dump);

#endif /* WAV_DUMPER_H */
