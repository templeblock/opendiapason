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
#include "fftset/fftset.h"

#define SMPL_COMP_LOADFLAG_AUTO (0)
#define SMPL_COMP_LOADFLAG_AS   (1)
#define SMPL_COMP_LOADFLAG_R    (2)

struct smpl_comp {
	const char *filename;

	/* Either SMPL_COMP_LOADFLAG_AUTO or a set of
	 * SMPL_COMP_LOADFLAG_* flags or'ed together. */
	unsigned    load_flags;

	int         load_format;
};

struct rel_data {
	float           *data;
	size_t           chan_stride;
	uint_fast32_t    length;

	float            period;   /* in samples. 0.0 = unknown. */

	int              load_format;

	uint_fast32_t    position;

	struct rel_data *next;
};

struct as_data {
	/* Attack and sustain data. May be null. */
	float           *data;
	size_t           chan_stride;
	uint_fast32_t    length;

	float            period;   /* in samples. 0.0 = unknown. */

	int              load_format;

	uint_fast32_t    atk_end_loop_start;

	/* Populated by sampler chunk */
	unsigned         nloop;
	uint_fast32_t    loops[2*MAX_LOOP];

	struct as_data  *next;
};

struct memory_wave {
	float           *buffers; /* channels*chan_stride elements long */

	/* Format details. */
	unsigned         channels;
	unsigned         native_bits;
	uint_fast32_t    rate;

	/* Number of elements to stride over to get to the same time value for the
	 * next channel. */
	size_t           chan_stride;

	/* Attack sustain data. Set as.data_ptr to indicate there is no attack/sustain segment. */
	struct as_data   as;

	/* Release data. Set rel.data_ptr to indicate there is no release. */
	struct rel_data  rel;
};

struct pipe_v1 {
	struct dec_smpl attack;
	struct dec_smpl release;
	struct reltable reltable;
	double          frequency;
	unsigned long   sample_rate;
};

const char *
load_smpl_comp
	(struct pipe_v1             *pipe
	,const struct smpl_comp     *components
	,unsigned                    nb_components
	,struct aalloc              *allocator
	,struct fftset              *fftset
	,const float                *prefilt_kern
	,unsigned                    prefilt_kern_len
	,unsigned                    prefilt_real_fft_len
	,const struct fftset_fft    *prefilt_fft
	);

const char *
load_smpl_f
	(struct pipe_v1             *pipe
	,const char                 *filename
	,struct aalloc              *allocator
	,struct fftset              *fftset
	,const float                *prefilt_kern
	,unsigned                    prefilt_kern_len
	,unsigned                    prefilt_real_fft_len
	,const struct fftset_fft    *prefilt_fft
	,int                         load_type
	);

const char *load_smpl_mem(struct memory_wave *smpl, unsigned char *buf, unsigned long fsz, unsigned load_format);

const char *load_smpl(struct memory_wave *smpl, const char *fname, double *frequency);



#endif /* WAVELDR_H */
