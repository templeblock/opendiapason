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

#ifndef WAVELDR_H
#define WAVELDR_H

#include "decode_types.h"
#include "reltable.h"
#include "cop/cop_alloc.h"
#include "opendiapason/fastconv.h"

struct memory_wave {
	float         **data;
	unsigned long   length;
	unsigned        channels;
	unsigned        nloop;
	unsigned long  *loops;
	unsigned long   release_pos; /* 0 for no marker */
	unsigned long   rate;
	unsigned        native_bits;
	float           frequency;   /* < 0.0 for unknown */
};

struct pipe_v1 {
	struct dec_smpl attack;
	struct dec_smpl release;
	struct reltable reltable;
	double          frequency;
	unsigned long   sample_rate;
};


const char *
load_smpl_f
	(struct pipe_v1             *pipe
	,const char                 *filename
	,struct aalloc              *allocator
	,struct fastconv_fftset     *fftset
	,const float                *prefilt_kern
	,unsigned                    prefilt_kern_len
	,unsigned                    prefilt_real_fft_len
	,const struct fastconv_pass *prefilt_fft
	);

const char *load_smpl_mem(struct memory_wave *smpl, unsigned char *buf, unsigned long fsz);

const char *load_smpl(struct memory_wave *smpl, const char *fname, double *frequency);



#endif /* WAVELDR_H */
