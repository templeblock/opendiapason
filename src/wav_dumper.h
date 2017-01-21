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

#ifndef WAV_DUMPER_H
#define WAV_DUMPER_H

#include <stdio.h>
#include <stdint.h>
#include "cop/cop_thread.h"

struct wav_dumper_buffer {
	unsigned                   nb_frames;
	float                     *buf;
};

struct wav_dumper {
	/* Constants */
	uint_fast32_t              max_frames;
	unsigned                   channels;
	unsigned                   bits_per_sample;
	unsigned                   block_align;

	/* These are completely owned by the thread. */
	uint_fast32_t              rseed;
	unsigned                   buffer_frames;
	unsigned char             *write_buffer;
	int                        write_error;
	FILE                      *f;

	/* Don't touch unless thread_lock is held. */
	struct wav_dumper_buffer  *buffers;
	unsigned                   nb_buffers;
	unsigned                   length;
	unsigned                   in_pos;
	unsigned                   out_pos;
	int                        end_thread;

	/* These are completely owned by the calling code. */
	void                      *mem_base;
	uint_fast32_t              nb_frames;

	/* Synchronisation stuff. */
	cop_thread                 thread;
	cop_cond                   thread_cond;
	cop_mutex                  thread_lock;
};

/* Starts a wave dumper with the given format configuration.
 * Return value is zero if the wave file was opened and initialised
 * successfully. */
int
wav_dumper_begin
	(struct wav_dumper *dump
	,const char        *filename
	,unsigned           channels
	,unsigned           bits_per_sample
	,uint_fast32_t      rate
	,unsigned           nb_buffers
	,unsigned           buffer_length
	);

/* Write the given floating point data into the wave file.
 *
 * The return value is the number of floats written. In the multi-threaded
 * case, this represents the number of samples that were successfully queued
 * for writing. In the single threaded case, this may be limited due to
 * being unable to write any more samples into the file or if a write error
 * has occured. */
unsigned
wav_dumper_write_from_floats
	(struct wav_dumper *dump
	,const float       *data
	,unsigned           num_samples
	,unsigned           sample_stride
	,unsigned           channel_stride
	);

/* Close the wave dumper.
 * Undefined to close a dumper that is not opened.
 * Returns zero if the header was updated successfully. */
int wav_dumper_end(struct wav_dumper *dump);

#endif /* WAV_DUMPER_H */
