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

#ifndef WAV_SAMPLE_READ_H
#define WAV_SAMPLE_READ_H

#include "wav_sample.h"

struct wav {
	struct wav_sample     sample;

	unsigned              nb_chunks;
	struct wav_chunk      chunks[MAX_CHUNKS];

	struct wav_chunk     *info;
	struct wav_chunk     *adtl;
	struct wav_chunk     *cue;
	struct wav_chunk     *smpl;
	struct wav_chunk     *fact;
	struct wav_chunk     *data;

	struct wav_chunk     *fmt;
};

#define FLAG_RESET                (1)
#define FLAG_PRESERVE_UNKNOWN     (2)
#define FLAG_PREFER_SMPL_LOOPS    (4)
#define FLAG_PREFER_CUE_LOOPS     (8)

int load_wave_sample(struct wav *wav, unsigned char *buf, size_t bufsz, const char *filename, unsigned flags);

void sort_and_reassign_ids(struct wav_sample *wav);

#endif /* WAV_SAMPLE_READ_H */
