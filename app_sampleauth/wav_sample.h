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

#ifndef WAV_SAMPLE_H
#define WAV_SAMPLE_H

#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include "cop/cop_attributes.h"

#define MAX_MARKERS (64)
#define MAX_CHUNKS  (32)

#define MAX_INFO    (64)

struct wav_marker {
	/* Cue ID */
	uint_fast32_t         id;

	/* From labl */
	char                 *name;

	/* From note */
	char                 *desc;

	/* From ltxt or smpl. */
	uint_fast32_t         length;
	int                   has_length;

	/* Is this marker in smpl. */
	int                   in_cue;
	int                   in_smpl;
	uint_fast32_t         position;
};

struct wav_chunk {
	uint_fast32_t     id;
	uint_fast32_t     size;
	unsigned char    *data;
	struct wav_chunk *next;
};

struct wav_info {
	uint_fast32_t     id;
	unsigned char    *str;
};

#define WAV_SAMPLE_PCM16   (0)
#define WAV_SAMPLE_PCM24   (1)
#define WAV_SAMPLE_PCM32   (2)
#define WAV_SAMPLE_FLOAT32 (3)

struct wav_sample_format {
	int           format;
	uint_fast32_t sample_rate;
	uint_fast16_t channels;
	uint_fast16_t bits_per_sample;
};

struct wav_sample {
	int                       has_pitch_info;
	uint_fast64_t             pitch_info;

	unsigned                  nb_marker;
	struct wav_marker         markers[MAX_MARKERS];

	struct wav_sample_format  format;

	uint_fast32_t             data_frames;
	void                     *data;

	struct wav_chunk         *unsupported;
};

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

#define RIFF_ID(c1, c2, c3, c4) \
	(   ((uint_fast32_t)(c1)) \
	|   (((uint_fast32_t)(c2)) << 8) \
	|   (((uint_fast32_t)(c3)) << 16) \
	|   (((uint_fast32_t)(c4)) << 24) \
	)

static COP_ATTR_UNUSED uint_fast16_t get_container_size(int format)
{
	switch (format) {
		case WAV_SAMPLE_PCM16:
			return 2;
		case WAV_SAMPLE_PCM24:
			return 3;
		default:
			assert(format == WAV_SAMPLE_PCM32 || format == WAV_SAMPLE_FLOAT32);
			return 4;
	}
}

#endif /* WAV_SAMPLE_H */
