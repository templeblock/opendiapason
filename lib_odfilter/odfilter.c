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

#include "opendiapason/odfilter.h"


int odfilter_init_filter(struct odfilter *pf, struct aalloc *allocobj, struct fftset *fftset, unsigned length)
{
	pf->kern_len = length;
	pf->conv_len = fftset_recommend_conv_length(length, 512) * 2;
	pf->conv     = fftset_create_fft(fftset, FFTSET_MODULATION_FREQ_OFFSET_REAL, pf->conv_len / 2);
	pf->kernel   = aalloc_align_alloc(allocobj, sizeof(float) * pf->conv_len, 64);
	return 0;
}

int odfilter_init_temporaries(struct odfilter_temporaries *tmps, struct aalloc *allocobj, const struct odfilter *filter)
{
	tmps->tmp1 = aalloc_align_alloc(allocobj, sizeof(float) * filter->conv_len, 64);
	tmps->tmp2 = aalloc_align_alloc(allocobj, sizeof(float) * filter->conv_len, 64);
	tmps->tmp3 = aalloc_align_alloc(allocobj, sizeof(float) * filter->conv_len, 64);
	return 0;
}

void odfilter_build_rect(struct odfilter *pf, struct odfilter_temporaries *tmps, unsigned length, float scale)
{
	unsigned i;
	assert(length < pf->conv_len);
	scale *= 2.0f / pf->conv_len;
	for (i = 0; i < length;  i++)      tmps->tmp1[i] = scale;
	for (     ; i < pf->conv_len; i++) tmps->tmp1[i] = 0.0f;
	fftset_fft_conv_get_kernel(pf->conv, pf->kernel, tmps->tmp1);
}

float odfilter_build_xcorr(struct odfilter *pf, struct odfilter_temporaries *tmps, unsigned length, const float *buffer, float scale)
{
	unsigned i;
	float psum = 0.0f;
	assert(length < pf->conv_len);
	scale *= 2.0f / pf->conv_len;
	for (i = 0; i < length; i++) {
		float s = buffer[length - 1 - i];
		tmps->tmp1[i] = s * scale;
		psum += s * s;
	}
	for (; i < pf->conv_len; i++) tmps->tmp1[i] = 0.0f;
	fftset_fft_conv_get_kernel(pf->conv, pf->kernel, tmps->tmp1);
	return psum;
}

void odfilter_build_conv(struct odfilter *pf, struct odfilter_temporaries *tmps, unsigned length, const float *buffer, float scale)
{
	unsigned i;
	assert(length < pf->conv_len);
	scale *= 2.0f / pf->conv_len;
	for (i = 0; i < length;  i++)      tmps->tmp1[i] = buffer[i] * scale;
	for (     ; i < pf->conv_len; i++) tmps->tmp1[i] = 0.0f;
	fftset_fft_conv_get_kernel(pf->conv, pf->kernel, tmps->tmp1);
}

void odfilter_run
	(const float                 *input
	,float                       *output
	,int                          add_to_output
	,unsigned long                susp_start
	,unsigned long                length
	,unsigned                     pre_read
	,int                          is_looped
	,struct odfilter_temporaries *tmps
	,const struct odfilter       *filter
	)
{
	const unsigned max_in = filter->conv_len - filter->kern_len + 1;
	unsigned input_read;
	unsigned input_pos;
	float *sc1 = tmps->tmp1;
	float *sc2 = tmps->tmp2;
	float *sc3 = tmps->tmp3;

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
				unsigned desire_read;
				/* How much can we read before we hit the end of the buffer? */
				max_read    = length - input_pos;
				desire_read = max_in - op;

				/* How much SHOULD we read? */
				if (desire_read > max_read)
					desire_read = max_read;

				/* Read it. */
				for (j = 0; j < desire_read; j++)
					sc1[j + op] = input[j + input_pos];

				/* Increment offsets. */
				input_pos += desire_read;
				op        += desire_read;

				/* If we read to the end of the buffer, move to the sustain
				 * start. */
				if (input_pos == length)
					input_pos = susp_start;
			}
			for (; op < filter->conv_len; op++)
				sc1[op] = 0.0f;
		} else {
			for (j = 0; j < max_in && input_read+j < length; j++) sc1[j] = input[input_read+j];
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
	}
}

void odfilter_run_inplace
	(float                       *data
	,unsigned long                susp_start
	,unsigned long                length
	,unsigned                     pre_read
	,int                          is_looped
	,struct odfilter_temporaries *tmps
	,const struct odfilter       *filter
	)
{
	float *old_data = malloc(sizeof(float) * length);

	if (old_data == NULL)
		abort();

	/* Copy input buffer to temp buffer. */
	memcpy(old_data, data, sizeof(float) * length);

	odfilter_run
		(old_data
		,data
		,0
		,susp_start
		,length
		,pre_read
		,is_looped
		,tmps
		,filter
		);

	free(old_data);
}


