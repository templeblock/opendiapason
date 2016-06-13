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
