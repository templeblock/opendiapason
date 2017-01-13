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

/* This wavldr module is responsible for loading wave samples into sample data
 * structures. The API is being structured in a way to support loading with
 * a memory-mapped cache at some point later.
 *
 * You tell the module all the information for every sample which needs to be
 * loaded along with a state structure (pipe_v1) to store the decodeable
 * audio. Once this information has been provided, there is a method which
 * begins the load process across multiple threads. This dramatically speeds
 * up the load time (pretty much by a factor of almost the number of threads).
 */

/* The loader cannot handle samples with more than this number of releases. */
#define WAVLDR_MAX_RELEASES     (4)

/* This structure contains the sampler playback structures which can have
 * decoders instantiated on them. */
struct pipe_v1 {
	struct dec_smpl attack;
	struct dec_smpl releases[WAVLDR_MAX_RELEASES];
	struct reltable reltable;
	double          frequency;
	unsigned long   sample_rate;
};

/* The loader cannot handle using more than this many threads during load. */
#define WAVLDR_MAX_LOAD_THREADS (4)

/* These load flags are passed to the load_flags member of the
 * sample_load_info structure. They determine what should be loaded from the
 * given audio file. If AUTO is specified, the loader will attempt to
 * automatically detect what can be loaded based on the audio file. */
#define SMPL_COMP_LOADFLAG_AUTO (0)
#define SMPL_COMP_LOADFLAG_AS   (1)
#define SMPL_COMP_LOADFLAG_R    (2)

/* The sample_load_info structure is what must be populated which corresponds
 * to one sample (which may consist of multiple files). */
struct sample_load_info {
	const char              *filenames[1+WAVLDR_MAX_RELEASES];
	int                      load_flags[1+WAVLDR_MAX_RELEASES];
	unsigned                 num_files;
	unsigned                 harmonic_number;
	unsigned                 load_format;
	struct pipe_v1          *dest;
};

/* The wavldr structure has all the data required to load all the samples. It
 * is defined later in this header file so you can put it on the stack - but
 * do not access it's members directly.
 *
 * The process to use this is:
 *   1) Initialise the loader using wavldr_initialise()
 *   2) Use wavldr_add_sample() once for each sample.
 *   3) Once all samples have been added, use wavldr_begin_load() to begin the
 *      load process.
 *   4) Optionally call wavldr_query_progress() several times to get the
 *      progress of the load operation.
 *   5) call wavldr_finish() which blocks until the load has completed. */
struct wavldr;

/* Initialise the wavldr instance. */
int wavldr_initialise(struct wavldr *load_set);

struct sample_load_info *wavldr_add_sample(struct wavldr *load_set);

/* Begins loader threads. nb_threads must be less than WAVLDR_MAX_LOAD_THREADS. */
const char *
wavldr_begin_load
	(struct wavldr  *load_set
	,struct cop_alloc_iface  *allocator
	,struct fftset           *fftset
	,const struct odfilter   *prefilter
	,unsigned                 nb_threads
	);

/* Returns number of samples left to load */
int wavldr_query_progress(struct wavldr *ls, unsigned *nb_samples);

/* Wait for the load process to finish. */
const char *wavldr_finish(struct wavldr *ls);

/* Private Parts
 * ---------------------------------------------------------------------------
 * Don't touch them. Only defined so you can bung them on the stack. */

struct loader_thread_state {
	struct wavldr     *lstate;

	struct cop_salloc_iface     if1;
	struct cop_salloc_iface     if2;
	struct cop_alloc_grp_temps  if1_impl;
	struct cop_alloc_grp_temps  if2_impl;
	struct odfilter_temporaries tmps;
	cop_thread                  thread_handle;
};

struct wavldr {
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

#endif /* WAVELDR_H */
