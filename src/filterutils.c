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

#include "filterutils.h"

int odfilter_interp_prefilter_init(struct odfilter *pf, struct aalloc *allocobj, struct fftset *fftset)
{
	float                   *workbuf;
	unsigned i;

	pf->kern_len   = SMPL_INVERSE_FILTER_LEN;
	pf->conv_len   = fftset_recommend_conv_length(SMPL_INVERSE_FILTER_LEN, 4*SMPL_INVERSE_FILTER_LEN) * 2;
	pf->conv       = fftset_create_fft(fftset, FFTSET_MODULATION_FREQ_OFFSET_REAL, pf->conv_len / 2);
	pf->kernel     = aalloc_align_alloc(allocobj, pf->conv_len * sizeof(float), 64);
	aalloc_push(allocobj);
	workbuf = aalloc_align_alloc(allocobj, pf->conv_len * sizeof(float), 64);
	for (i = 0; i < SMPL_INVERSE_FILTER_LEN; i++) {
		workbuf[i] = SMPL_INVERSE_COEFS[i] * (2.0f / pf->conv_len);
	}
	for (; i < pf->conv_len; i++) {
		workbuf[i] = 0.0f;
	}
	fftset_fft_conv_get_kernel(pf->conv, pf->kernel, workbuf);
	aalloc_pop(allocobj);

	return 0;
}

void odfilter_run_inplace
	(const float                *data
	,float                      *output
	,int                         add_to_output
	,unsigned long               susp_start
	,unsigned long               length
	,unsigned                    pre_read
	,int                         is_looped
	,float                      *sc1
	,float                      *sc2
	,float                      *sc3
	,const struct odfilter      *filter
	)
{
	unsigned max_in = filter->conv_len - filter->kern_len + 1;
	float *old_data = malloc(sizeof(float) * length);
	unsigned input_read;
	unsigned input_pos;

	if (old_data == NULL)
		abort();

	/* Copy input buffer to temp buffer. */
	memcpy(old_data, data, sizeof(float) * length);

	/* Zero output buffer. */
	if (!add_to_output) {
		memset(output, 0, sizeof(float) * length);
	}

	/* Run trivial overlap add convolution */
	input_read = 0;
	input_pos = 0;
	while (1) {
		unsigned j = 0;
		int output_pos = (int)input_read-(int)pre_read;

		/* Build input buffer */
		if (is_looped) {
			unsigned op = 0;
			while (op < max_in) {
				unsigned max_read;

				/* How much can we read before we hit the end of the buffer? */
				max_read = length - input_pos;

				/* How much SHOULD we read? */
				if (max_read + op > max_in)
					max_read = max_in - op;

				/* Read it. */
				for (j = 0; j < max_read; j++)
					sc1[j + op] = old_data[j + input_pos];

				/* Increment offsets. */
				input_pos += max_read;
				op        += max_read;

				/* If we read to the end of the buffer, move to the sustain
				 * start. */
				if (input_pos == length)
					input_pos = susp_start;
			}
			for (; op < filter->conv_len; op++)
				sc1[op] = 0.0f;
		} else {
			for (j = 0; j < max_in && input_read+j < length; j++) sc1[j] = old_data[input_read+j];
			for (; j < filter->conv_len;            j++)      sc1[j] = 0.0f;
		}

		/* Convolve! */
		fftset_fft_conv(filter->conv, sc2, sc1, filter->kernel, sc3);

		/* Sc2 contains the convolved buffer. */
		for (j = 0; j < filter->conv_len; j++) {
			int x = output_pos+(int)j;
			if (x < 0)
				continue;
			/* Cast is safe because we check earlier for negative. */
			if ((unsigned)x >= length)
				break;
			output[x] += sc2[j];
		}

		/* added nothing to output buffer! */
		if (j == 0)
			break;

		input_read += max_in;
	};

	free(old_data);
}


