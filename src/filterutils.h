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

#ifndef FILTERUTILS_H
#define FILTERUTILS_H

/* Utilitiy functions and objects for Open Diapason filters. */

#include "cop/cop_alloc.h"
#include "fftset/fftset.h"
#include "interpdata.h"

struct odfilter {
	unsigned                 kern_len;
	unsigned                 conv_len;
	const struct fftset_fft *conv;
	float                   *kernel;
};

struct odfilter_temporaries {
	float *tmp1;
	float *tmp2;
	float *tmp3;
};

int   odfilter_init_filter(struct odfilter *pf, struct aalloc *allocobj, struct fftset *fftset, unsigned length);
int   odfilter_init_temporaries(struct odfilter_temporaries *tmps, struct aalloc *allocobj, const struct odfilter *filter);
void  odfilter_build_rect(struct odfilter *pf, struct odfilter_temporaries *tmps, unsigned length, float scale);
float odfilter_build_xcorr(struct odfilter *pf, struct odfilter_temporaries *tmps, unsigned length, const float *buffer, float scale);

int odfilter_interp_prefilter_init(struct odfilter *pf, struct aalloc *allocobj, struct fftset *fftset);

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
	);

void odfilter_run_inplace
	(float                       *data
	,unsigned long                susp_start
	,unsigned long                length
	,unsigned                     pre_read
	,int                          is_looped
	,struct odfilter_temporaries *tmps
	,const struct odfilter       *filter
	);

#endif /* FILTERUTILS_H */
