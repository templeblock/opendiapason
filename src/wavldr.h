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
#include "cop/cop_thread.h"
#include "fftset/fftset.h"
#include "opendiapason/odfilter.h"

#define SMPL_COMP_LOADFLAG_AUTO (0)
#define SMPL_COMP_LOADFLAG_AS   (1)
#define SMPL_COMP_LOADFLAG_R    (2)

struct smpl_comp {
	const char    *filename;

	unsigned char *data;
	size_t         size;

	/* Either SMPL_COMP_LOADFLAG_AUTO or a set of
	 * SMPL_COMP_LOADFLAG_* flags or'ed together. */
	unsigned       load_flags;

	int            load_format;
};

#define WAVLDR_MAX_RELEASES (4)

struct pipe_v1 {
	struct dec_smpl attack;
	struct dec_smpl releases[WAVLDR_MAX_RELEASES];
	struct reltable reltable;
	double          frequency;
	unsigned long   sample_rate;
};

#define LOAD_SET_GROW_RATE (500)

struct sample_load_info {
	const char              *filenames[1+WAVLDR_MAX_RELEASES];
	int                      load_flags[1+WAVLDR_MAX_RELEASES];
	unsigned                 num_files;
	unsigned                 harmonic_number;
	unsigned                 load_format;

	/* Where to load the data. */
	struct pipe_v1          *dest;

	void                    *ctx;
	void                   (*on_loaded)(const struct sample_load_info *ld_info);
};

struct sample_load_set {
	struct sample_load_info *elems;
	unsigned                 nb_elems;
	unsigned                 max_nb_elems;

	cop_mutex                pop_lock;
	cop_mutex                file_lock;

};

int sample_load_set_init(struct sample_load_set *load_set);

struct sample_load_info *sample_load_set_push(struct sample_load_set *load_set);

const char *
load_samples
	(struct sample_load_set  *load_set
	,struct cop_alloc_iface  *allocator
	,struct fftset           *fftset
	,const struct odfilter   *prefilter
	);


#endif /* WAVELDR_H */
