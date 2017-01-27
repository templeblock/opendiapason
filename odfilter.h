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

/* Utilitiy library for performing real-valued convolusions in particular ways
 * which are applicable to organ samples. */

#ifndef ODFILTER_H
#define ODFILTER_H

#include "cop/cop_alloc.h"
#include "fftset/fftset.h"

/* An odfilter structure defines a convolution kernel. You are permitted to
 * fill in this structure yourself, but there are functions provided to
 * assist which might be easier.
 *
 * All convolutions which are performed by this library use the
 * FFTSET_MODULATION_FREQ_OFFSET_REAL modulator which is important if you wish
 * to initialise the kernel data buffer yourself. */
struct odfilter {
	/* The length of the real-valued kernel and the length of the real-valued
	 * modulator. conv_len should always be substantially greater than
	 * kern_len and will determine how frequently the modulators will be
	 * called. i.e. conv_len-kern_len+1=max_input_block_length */
	unsigned                 kern_len;
	unsigned                 conv_len;

	/* A FFTSET_MODULATION_FREQ_OFFSET_REAL modulator of length conv_len (i.e.
	 * was initialised with complex_len=conv_len/2). */
	const struct fftset_fft *conv;

	/* The properly aligned kernel buffer. This must contain conv_len elements
	 * which were obtained using fftset_fft_conv_get_kernel() with the
	 * modulation given by conv. */
	float                   *kernel;
};

/* This structure containes junk memory buffers required for the convolution
 * to be performed without any extra allocations. The rationale as to why
 * these are not incorporated into odfilter is to enable one odfilter kernel
 * to be shared between multiple threads. If this is occuring, each thread
 * must have its own odfilter_temporaries structure. You can set this
 * structure up yourself by setting tmp1, tmp2 and tmp3 to all be properly
 * aligned pointers to conv_len elements for the filter which this will be
 * used with - or you can use odfilter_init_temporaries() to set up the
 * structure appropriately with buffers big enough for the given filter. */
struct odfilter_temporaries {
	float *tmp1;
	float *tmp2;
	float *tmp3;
};

/* Initialise a filter which is designed for a kernel of the given length. The
 * function will pick a suitable value for conv_len based on the length of the
 * kernel. The kernel data will be allocated but not configured, you must do
 * this either manually (see documentation for odfilter structure) or use one
 * of the odfilter_build_*() functions which have been provided for
 * convenience. */
int odfilter_init_filter(struct odfilter *pf, struct cop_alloc_iface *allocobj, struct fftset *fftset, unsigned length);

/* Allocate the temporary pointers required for performing a convolution using
 * the given allocation object based on the supplied filter. The filter kernel
 * is not required to have been allocated nor configured at this point. */
int odfilter_init_temporaries(struct odfilter_temporaries *tmps, struct cop_alloc_iface *allocobj, const struct odfilter *filter);

/* Builds a kernel that is a rectangle. This filter will have the effect of
 * summing length elements together. length must be less than or equal to
 * the kernel length of the supplied filter object. scale is applied to each
 * value in the filter. i.e. setting scale to 1.0/length would cause the
 * filter to average length values. */
void odfilter_build_rect(struct odfilter *pf, struct odfilter_temporaries *tmps, unsigned length, float scale);

/* Builds a kernel that is a reversed version of the supplied buffer where
 * each element is pre-multiplied by scale. length must be less than or equal
 * to the kernel length of the supplied filter object. The return value is the
 * sum of the squares of the input buffer (scale is NOT applied to the values
 * before nor after squaring). */
float odfilter_build_xcorr(struct odfilter *pf, struct odfilter_temporaries *tmps, unsigned length, const float *buffer, float scale);

/* Builds a kernel with samples taken from the supplied buffer where each
 * element is pre-multiplied by scale. length must be less than or equal to
 * the kernel length of the supplied filter object. */
void odfilter_build_conv(struct odfilter *pf, struct odfilter_temporaries *tmps, unsigned length, const float *buffer, float scale);

/* Perform the filtering operation on the supplied input buffer placing the
 * results into output. If add_to_output is non-zero, the filtered output will
 * be summed into output rather than set. length specifies the length of the
 * input buffer and also the number of samples which will be written into
 * output. If is_looped is non-zero, the input data will be treated as
 * continuing on past length as if it started again at susp_start - if
 * is_looped is zero, susp_start has no effect on the results and it is
 * assumed that the input signal immediately goes to zero after length.
 * pre_read is used to centre the output of the convolution. For example, if
 * pre_read is zero and the filter kernel is symmetric, there will be a some
 * pre-ringing in the output and there will be an overall delay between the
 * input and output buffers. pre_read will discard some of the pre-ringing
 * and can be used to re-align the output to the input for symmetric
 * filters. */
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

#endif /* ODFILTER_H */
