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

#ifndef FASTCONV_H
#define FASTCONV_H

#include "cop/cop_alloc.h"

/* How to use this API:
 *
 *   1) Initialize an fftset object. This will hold all the memory for the
 *      different DFT coefficients.
 *   2) Create an fft pass object. This will describe a particular DFT (or
 *      variety on a DFT) matrix depending on the constructor used. Currently
 *      there is only one constructor:
 *        - fastconv_get_real_conv() which creates a real input, complex
 *          output modulation. The frequency vector is shifted by 0.5.
 *          Convolution still works on the output.
 *   3) Depending on whether you are using this DFT for convolution or ordered
 *      analysis:
 *        - Convolution: use fastconv_execute_fwd() to create the frequency
 *          domain filter kernel. Then use calls to fastconv_execute_conv() to
 *          perform convolution operations.
 *        - Analysis (output bins required to be in a particular order): use
 *          fastconv_execute_fwd_reord() and fastconv_execute_rev_reord() to
 *          perform the forward/inverse modulations respectively. */

/* Holds the memory for a variety of different DFTs. */
struct fastconv_fftset;

/* Descrives the execution of a particular DFT. */
struct fastconv_pass;

/* Creates an fftset which will be used by convolution kernels. This provides
 * a way to reuse twiddles and memory so that each time a kernel is
 * constructed, there is only a minimal amount of work that needs to be done.
 */
void fastconv_fftset_init(struct fastconv_fftset *fc);

/* Destroys an fftset. Once this function has been called, all kernel objects
 * which were created using the given fftset must not be executed. The kernel
 * objects may be destroyed via fastconv_kernel_real_destroy() after this
 * function is called. */
void fastconv_fftset_destroy(struct fastconv_fftset *fc);

/* Given a particular kernel length and a maximum usage block size, give a
 * reasonably optimal length to use for the convolution FFT. The result is
 * guaranteed to be useable by fastconv_get_real_conv() and will be
 * greater than kernel_length + max_block_size - 1.
 *
 * The actual maximum block size which can be used can be found by the result
 * of this function plus one minus the kernel length. */
unsigned fastconv_recommend_length(unsigned kernel_length, unsigned max_block_size);

/* Get a convolution pass that takes real input with the specified length.
 * The result is useable until fastconv_fftset_destroy() is called. */
const struct fastconv_pass *fastconv_get_real_conv(struct fastconv_fftset *fc, unsigned real_length);

void
fastconv_execute_fwd
	(const struct fastconv_pass *first_pass
	,const float                *input_buf
	,float                      *output_buf
	);

void
fastconv_execute_fwd_reord
	(const struct fastconv_pass *first_pass
	,const float                *input_buf
	,float                      *output_buf
	,float                      *work_buf
	);

void
fastconv_execute_rev_reord
	(const struct fastconv_pass *first_pass
	,const float                *input_buf
	,float                      *output_buf
	,float                      *work_buf
	);

void
fastconv_execute_conv
	(const struct fastconv_pass *first_pass
	,const float                *input_buf
	,const float                *kernel_buf
	,float                      *output_buf
	,float                      *work_buf
	);

/* Only defined so you can shove it on the stack. Don't access directly. */
struct fastconv_fftset {
	/* Sorted list of all available inner vector passes. */
	struct fastconv_pass *first_inner;
	/* Sorted list of all available outer passes. */
	struct fastconv_pass *first_outer;
	/* Memory for everything! */
	struct aalloc         memory;
};

#endif
