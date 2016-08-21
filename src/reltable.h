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

#ifndef RELTABLE_H
#define RELTABLE_H

#include <stddef.h>

#define RELTABLE_MAX_ENTRIES (128)

struct reltable {
	unsigned nb_entry;
	struct {
		int      rel_id;
		unsigned last_sample;
		double   m;
		double   b;
		float    gain;
		float    avgerr;
	} entry[RELTABLE_MAX_ENTRIES];
};

double
reltable_find
	(const struct reltable *reltable
	,double                 sample
	,float                 *gain
	,float                 *avgerr
	,unsigned              *rel_id
	);

/* Creates a release alignment table for aligning a release with an
 * attack/sustain segment.
 *
 * - envelope_buf is the power envelope of the input signal channels over a
 *   particular number of samples: N. i.e.
 *
 *   for (i = 0; i < M; i++) {
 *     envelope_buf[i] = 0.0;
 *     for (ch = 0; ch < num_channels; ch++) {
 *       for (n = 0; n < N; n++) {
 *         envelope_buf[i] += attack_sustain[ch][i + n] * attack_sustain[ch][i + n];
 *       }
 *     }
 *   }
 *
 * - correlation_buf is the sum of the correlations (one for each channel) of
 *   N samples of the release segment with the input signal. i.e.
 *
 *   for (i = 0; i < M; i++) {
 *     correlation_buf[i] = 0.0;
 *     for (ch = 0; ch < num_channels; ch++) {
 *       for (n = 0; n < N; n++) {
 *         correlation_buf[i] += attack_sustain[ch][i + n] * release[ch][i + n];
 *       }
 *     }
 *   }
 *
 * - rel_power is the sum of the powers of the release over N samples (over all
 *   channels). i.e.
 *
 *   rel_power = 0.0;
 *   for (ch = 0; ch < num_channels; ch++) {
 *     for (n = 0; n < N; n++) {
 *       rel_power += release[ch][n] * release[ch][n];
 *     }
 *   }
 *
 * - buf_len is the number of valid data points in envelope_buf and
 *   correlation_buf.
 *
 * - period is the period in samples of the audio data.
 *
 * - debug_prefix is a file-name or path which will be used as a prefix to
 *   debug dump files. They will all begin with "reltable". If this is NULL,
 *   no debug files will be dumped. */
void
reltable_build
	(struct reltable *reltable
	,const float     *envelope_buf
	,const float     *correlation_bufs
	,const float     *rel_powers
	,unsigned         nb_rels
	,size_t           rel_stride
	,unsigned         buf_len
	,float            period
	,const char      *debug_prefix
	);

#endif
