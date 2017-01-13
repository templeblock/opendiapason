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

struct sample_load_set;

struct loader_thread_state {
	struct sample_load_set     *lstate;

	struct cop_salloc_iface     if1;
	struct cop_salloc_iface     if2;
	struct cop_alloc_grp_temps  if1_impl;
	struct cop_alloc_grp_temps  if2_impl;
	struct odfilter_temporaries tmps;
	cop_thread                  thread_handle;
};

#define WAVLDR_MAX_LOAD_THREADS (4)

struct sample_load_set {
	/* Things which are read-only by threads. */
	const struct odfilter   *prefilter;
	unsigned                 nb_elems;
	unsigned                 cur_elem;
	unsigned                 max_nb_elems;

	/* Thread states. */
	unsigned                 nb_threads;
	struct loader_thread_state thread_states[WAVLDR_MAX_LOAD_THREADS];

	/* This lock is used when accessing files. This is purely to stop multiple
	 * files being read at the same time (prevents hdd thrashing). Each thread
	 * must acquire this lock before performing file IO. */
	cop_mutex                read_lock;

	/* Things which must only be used by threads which hold locks. */
	cop_mutex                file_lock;
	

	cop_mutex                state_lock;
	struct sample_load_info *elems;
	const char              *error;
	struct cop_alloc_iface  *protected_allocator;
	struct cop_alloc_iface   allocator;
	struct fftset           *fftset;
};


/* The process is
 *   1) initialise
 *   2) add samples using push()
 *   3) start the load using begin
 *   4) optionally call query_progress()
 *   5) call wavldr_finish which blocks until completion.
 */

int wavldr_initialise(struct sample_load_set *load_set);

struct sample_load_info *wavldr_add_sample(struct sample_load_set *load_set);

/* Begins loader threads. nb_threads must be less than WAVLDR_MAX_LOAD_THREADS. */
const char *
wavldr_begin_load
	(struct sample_load_set  *load_set
	,struct cop_alloc_iface  *allocator
	,struct fftset           *fftset
	,const struct odfilter   *prefilter
	,unsigned                 nb_threads
	);

/* Returns number of samples left to load */
int wavldr_query_progress(struct sample_load_set *ls, unsigned *nb_samples);

/* Wait for the load process to finish. */
const char *wavldr_finish(struct sample_load_set *ls);


#endif /* WAVELDR_H */
