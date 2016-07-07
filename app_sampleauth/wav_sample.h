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

#define WAV_SAMPLE_MAX_MARKERS             (64)
#define WAV_SAMPLE_MAX_UNSUPPORTED_CHUNKS  (32)

struct wav_marker {
	/* id, in_cue and in_smpl are used while the markers are being loaded.
	 * They are not used by the serialisation code and are free to be read
	 * from and written to by the calling code for other purposes. */
	uint_fast32_t         id;
	int                   in_cue;
	int                   in_smpl;

	/* From labl */
	char                 *name;

	/* From note */
	char                 *desc;

	/* From ltxt or smpl. */
	uint_fast32_t         length;
	int                   has_length;

	/* Sample offset this marker applies at. */
	uint_fast32_t         position;
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

#define RIFF_ID(c1, c2, c3, c4) \
	(   ((uint_fast32_t)(c1)) \
	|   (((uint_fast32_t)(c2)) << 8) \
	|   (((uint_fast32_t)(c3)) << 16) \
	|   (((uint_fast32_t)(c4)) << 24) \
	)

static COP_ATTR_UNUSED uint_fast32_t SUPPORTED_INFO_TAGS[] =
{	RIFF_ID('I', 'A', 'R', 'L')
,	RIFF_ID('I', 'A', 'R', 'T')
,	RIFF_ID('I', 'C', 'M', 'S')
,	RIFF_ID('I', 'C', 'M', 'T')
,	RIFF_ID('I', 'C', 'O', 'P')
,	RIFF_ID('I', 'C', 'R', 'D')
,	RIFF_ID('I', 'C', 'R', 'P')
,	RIFF_ID('I', 'D', 'I', 'M')
,	RIFF_ID('I', 'D', 'P', 'I')
,	RIFF_ID('I', 'E', 'N', 'G')
,	RIFF_ID('I', 'G', 'N', 'R')
,	RIFF_ID('I', 'K', 'E', 'Y')
,	RIFF_ID('I', 'L', 'G', 'T')
,	RIFF_ID('I', 'M', 'E', 'D')
,	RIFF_ID('I', 'N', 'A', 'M')
,	RIFF_ID('I', 'P', 'L', 'T')
,	RIFF_ID('I', 'P', 'R', 'D')
,	RIFF_ID('I', 'S', 'B', 'J')
,	RIFF_ID('I', 'S', 'F', 'T')
,	RIFF_ID('I', 'S', 'H', 'P')
,	RIFF_ID('I', 'S', 'R', 'C')
,	RIFF_ID('I', 'S', 'R', 'F')
,	RIFF_ID('I', 'T', 'C', 'H')
};

#define NB_SUPPORTED_INFO_TAGS (sizeof(SUPPORTED_INFO_TAGS) / sizeof(SUPPORTED_INFO_TAGS[0]))

struct wav_chunk {
	uint_fast32_t     id;
	uint_fast32_t     size;
	unsigned char    *data;
	struct wav_chunk *next;
};

struct wav_sample {
	/* String metadata found in the info chunk. */
	char                      *info[NB_SUPPORTED_INFO_TAGS];

	/* If there was a smpl chunk, this will always be non-zero and pitch-info
	 * will be set to the midi pitch information. */
	int                        has_pitch_info;
	uint_fast64_t              pitch_info;

	/* Positional based metadata loaded from the waveform. */
	unsigned                   nb_marker;
	struct wav_marker          markers[WAV_SAMPLE_MAX_MARKERS            ];

	/* The data format of the wave file. */
	struct wav_sample_format   format;

	/* The number of samples in the wave file and the pointer to its data. */
	uint_fast32_t              data_frames;
	void                      *data;

	/* Chunks which were found in the wave file which cannot be handled by
	 * this implementation. This is anything other than: INFO, fmt, data, cue,
	 * smpl, adtl and fact. */
	unsigned                   nb_unsupported;
	struct wav_chunk           unsupported[WAV_SAMPLE_MAX_UNSUPPORTED_CHUNKS];
};

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
