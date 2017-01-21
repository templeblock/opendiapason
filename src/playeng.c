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

#include "playeng.h"
#include "cop/cop_thread.h"
#include "cop/cop_alloc.h"
#include <stdlib.h>

/* TODO: IMPLEMENT SCHEDULED CALLBACKS... FLAGS CURRENTLY HAVE NO EFFECT. */

struct playeng_instance {
	struct dec_state        *states[PLAYENG_MAX_DECODERS_PER_INSTANCE];
	void                    *userdata;
	unsigned                 nb_states;
	unsigned                 flags;
	unsigned                 signals;
	unsigned                 trigger_time;
	unsigned                 locked_signals;
	playeng_callback         callback;
	struct playeng_instance *next;
};

struct playeng_payloads {
	cop_mutex                   thread_lock;
	cop_cond                    thread_cond;
	struct playeng_thread_data *start;
	unsigned                    nb_channels; /* 0 == terminate */

	cop_mutex                   consumer_lock;
	cop_cond                    consumer_cond;
	struct playeng_thread_data *done;
};

struct playeng_thread_data {
	struct playeng_instance    *active;
	struct playeng_instance    *zombie;
	float   *COP_ATTR_RESTRICT *buffers;
	unsigned                    current_time;
	unsigned                    permitted_signal_mask;

	/* Not to be touched or viewed by threads! Only used by the engine calling
	 * thread. */
	struct playeng_thread_data *next;
	cop_thread                  thread;
};

struct playeng {
	cop_mutex                     list_lock;
	struct playeng_instance      *inactive_insts;
	struct dec_state            **inactive_decodes;
	unsigned                      nb_inactive_decodes;
	unsigned                      insertion_lock_level;
	struct playeng_instance      *ready_list;

	/* These are maintained by the audio thread. Active represents samples
	 * which are currently playing back. Zombie represents samples which have
	 * completed (or been stopped), but have not been relinquished by the
	 * calling process. */
	cop_mutex                     signal_lock;
	unsigned                      locked_permitted_signal_mask;

	unsigned                      current_time;

	unsigned                      nb_threads;
	unsigned                      next_thread_idx;
	struct playeng_thread_data   *threads;

	unsigned                      reblock_length;
	unsigned                      reblock_start;
	float     *COP_ATTR_RESTRICT *reblock_buffers;

	struct playeng_payloads       payloads;

	/* Memory allocator for everything in engine. */
	struct cop_alloc_virtual      allocator;
};

static void* playeng_thread_proc(void *context);

struct playeng *playeng_init(unsigned max_poly, unsigned nb_channels, unsigned nb_threads)
{
	struct cop_alloc_virtual a;
	struct cop_salloc_iface  mem;
	struct playeng_instance *insts_mem;
	struct dec_state        *decodes_mem;
	struct playeng          *pe;
	unsigned i;

	if (nb_threads < 2)
		nb_threads = 1;

	cop_alloc_virtual_init(&a, &mem, 33554432, 16, 16384);
	if ((pe = cop_salloc(&mem, sizeof(*pe), 0)) == NULL)
		return NULL;


	pe->next_thread_idx  = 0;
	pe->nb_threads       = nb_threads;
	insts_mem            = cop_salloc(&mem, sizeof(insts_mem[0]) * max_poly, 0);
	decodes_mem          = cop_salloc(&mem, sizeof(decodes_mem[0]) * max_poly, 0);
	pe->inactive_decodes = cop_salloc(&mem, sizeof(pe->inactive_decodes[0]) * max_poly, 0);
	pe->threads          = cop_salloc(&mem, sizeof(pe->threads[0]) * nb_threads, 0);
	if (insts_mem == NULL || decodes_mem == NULL || pe->inactive_decodes == NULL || pe->threads == NULL) {
		cop_alloc_virtual_free(&a);
		return NULL;
	}

	for (i = 0; i < nb_threads; i++) {
		if ((pe->threads[i].buffers = cop_salloc(&mem, sizeof(pe->threads[i].buffers[0]) * nb_channels, 0)) == NULL) {
			cop_alloc_virtual_free(&a);
			return NULL;
		};
	}

	for (i = 0; i < nb_threads; i++) {
		unsigned j;
		for (j = 0; j < nb_channels; j++) {
			if ((pe->threads[i].buffers[j] = cop_salloc(&mem, sizeof(pe->threads[i].buffers[0][0]) * OUTPUT_SAMPLES, 64)) == NULL) {
				cop_alloc_virtual_free(&a);
				return NULL;
			}
		}
	}

	/* Create buffers for reblocking (nb_channels * OUTPUT_SAMPLES) */
	if ((pe->reblock_buffers = cop_salloc(&mem, sizeof(pe->reblock_buffers[0]) * nb_channels, 0)) == NULL) {
		cop_alloc_virtual_free(&a);
		return NULL;
	}
	for (i = 0; i < nb_channels; i++) {
		if ((pe->reblock_buffers[i] = cop_salloc(&mem, sizeof(pe->reblock_buffers[0][0]) * OUTPUT_SAMPLES, 64)) == NULL) {
			cop_alloc_virtual_free(&a);
			return NULL;
		}
	}

	pe->payloads.start       = NULL;
	pe->payloads.nb_channels = 1;
	pe->payloads.done        = NULL;
	cop_mutex_create(&(pe->payloads.consumer_lock));
	cop_mutex_create(&(pe->payloads.thread_lock));
	cop_cond_create(&(pe->payloads.consumer_cond));
	cop_cond_create(&(pe->payloads.thread_cond));

	if (cop_mutex_create(&pe->signal_lock)) {
		cop_alloc_virtual_free(&a);
		return NULL;
	}
	if (cop_mutex_create(&pe->list_lock)) {
		cop_mutex_destroy(&pe->signal_lock);
		cop_alloc_virtual_free(&a);
		return NULL;
	}

	pe->reblock_length               = 0;
	pe->reblock_start                = 0;
	pe->ready_list                   = NULL;
	pe->nb_inactive_decodes          = max_poly;
	pe->inactive_insts               = NULL;
	pe->locked_permitted_signal_mask = ~0u;
	pe->current_time                 = 0;
	pe->insertion_lock_level         = 0;
	for (i = 0; i < max_poly; i++) {
		pe->inactive_decodes[i] = &(decodes_mem[i]);
		insts_mem[i].next       = pe->inactive_insts;
		pe->inactive_insts      = &(insts_mem[i]);
	}
	for (i = 0; i < nb_threads; i++) {
		pe->threads[i].active                = NULL;
		pe->threads[i].zombie                = NULL;
		pe->threads[i].permitted_signal_mask = pe->locked_permitted_signal_mask;
		pe->threads[i].current_time          = 0;
		cop_thread_create(&(pe->threads[i].thread), playeng_thread_proc, &(pe->payloads), 0, 0);
	}

	pe->allocator        = a;

	return pe;
}

void playeng_destroy(struct playeng *eng)
{
	unsigned i;
	struct cop_alloc_virtual a = eng->allocator;

	cop_mutex_lock(&(eng->payloads.thread_lock));
	eng->payloads.nb_channels = 0; /* trigger termination */
	eng->payloads.start       = NULL;
	cop_cond_broadcast(&(eng->payloads.thread_cond));
	cop_mutex_unlock(&(eng->payloads.thread_lock));

	for (i = 0; i < eng->nb_threads; i++)
		cop_thread_join(eng->threads[i].thread, NULL);

	cop_mutex_destroy(&(eng->payloads.consumer_lock));
	cop_mutex_destroy(&(eng->payloads.thread_lock));
	cop_cond_destroy(&(eng->payloads.consumer_cond));
	cop_cond_destroy(&(eng->payloads.thread_cond));
	cop_mutex_destroy(&(eng->signal_lock));
	cop_mutex_destroy(&(eng->list_lock));
	cop_alloc_virtual_free(&a);
}

static
void
return_instance(struct playeng *eng, struct playeng_instance *inst)
{
	unsigned i;
	for (i = 0; i < inst->nb_states; i++)
		eng->inactive_decodes[eng->nb_inactive_decodes++] = inst->states[i];
	inst->next = eng->inactive_insts;
	eng->inactive_insts = inst;
}

static
struct playeng_instance *
get_instance(struct playeng *eng, unsigned ndec)
{
	assert(ndec);
	if (ndec <= eng->nb_inactive_decodes) {
		struct playeng_instance *ei;

		assert(eng->inactive_insts != NULL);

		ei = eng->inactive_insts;
		eng->inactive_insts = ei->next;
		ei->nb_states = ndec;
		do {
			ei->states[--ndec] = eng->inactive_decodes[--eng->nb_inactive_decodes];
		} while (ndec);
		return ei;
	}
	return NULL;
}

void playeng_push_block_insertion(struct playeng *eng)
{
	cop_mutex_lock(&eng->list_lock);
	eng->insertion_lock_level++;
	cop_mutex_unlock(&eng->list_lock);
}

void playeng_pop_block_insertion(struct playeng *eng)
{
	cop_mutex_lock(&eng->list_lock);
	assert(eng->insertion_lock_level);
	eng->insertion_lock_level--;
	cop_mutex_unlock(&eng->list_lock);
}

struct playeng_instance *
playeng_insert
	(struct playeng          *eng
	,unsigned                 ndec
	,unsigned                 sigmask
	,playeng_callback         callback
	,void                    *userdata
	)
{
	struct playeng_instance *ei;

	/* Get an instance and decoders if they are available. */
	cop_mutex_lock(&eng->list_lock);
	ei = get_instance(eng, ndec);
	cop_mutex_unlock(&eng->list_lock);

	/* Not enough polyphony? */
	if (ei == NULL)
		return NULL;

	ei->userdata       = userdata;
	ei->callback       = callback;
	ei->signals        = sigmask;
	ei->locked_signals = 0;
	ei->flags          = 0;
	ei->trigger_time   = ~0u;
	ei->next           = eng->ready_list;
	eng->ready_list    = ei;

	return ei;
}


static void playeng_thread_data_execute(struct playeng_thread_data *td)
{
	struct playeng_instance    *active_list = td->active;
	struct playeng_instance    *new_active_list = NULL;
	while (active_list != NULL) {
		unsigned masked_signals;
		unsigned flags          = active_list->flags;
		unsigned active_bits    = PLAYENG_GET_CALLBACK_ACTIVE(flags);

		/* What is the point of this guy? A: When an object gets inserted into
		 * the engine, all of the active bits are zero and it may (depending
		 * on how playeng_insert() was called) have no signal bits set. We
		 * do not want to remove the sample at this point as the caller may
		 * be reserving it to be signalled later (i.e. guarantee playback).
		 * Samples only get discarded if either:
		 *   - the callback gets fired and returns no active bits OR
		 *   - the sample had active components which became deactive given
		 *     the loop/fade-termination conditions specified in the flags. */
		int discard_sample = 0;

		/* Check if the callback has been signalled. */
		masked_signals = active_list->signals & td->permitted_signal_mask;
		if (masked_signals) {
			flags                = active_list->callback(active_list->userdata, active_list->states, masked_signals, flags, td->current_time);
			active_list->flags   = flags;
			active_bits          = PLAYENG_GET_CALLBACK_ACTIVE(flags);
			discard_sample       = active_bits == 0;
			active_list->signals = active_list->signals ^ masked_signals;
		}

		if (!discard_sample && active_bits) {
			unsigned l_conds     = PLAYENG_GET_CALLBACK_LOOPTER(flags);
			unsigned f_conds     = PLAYENG_GET_CALLBACK_FADETER(flags);
			unsigned new_active_bits = active_bits;
			unsigned select = 1;
			unsigned i;
			for (i = 0; active_bits; i++, select <<= 1, active_bits >>= 1) {
				if (active_bits & 1) {
					int flg = active_list->states[i]->decode(active_list->states[i], td->buffers);
					if  (   (!(flg & DEC_IS_FADING) && (f_conds & select))
					    ||  ((flg & DEC_IS_LOOPING) && (l_conds & select))
					    ) {
						new_active_bits ^= select;
					}
				}
			}
			if (new_active_bits == 0) {
				discard_sample = 1;
			} else {
				active_list->flags = PLAYENG_SET_CALLBACK_ACTIVE(active_list->flags, new_active_bits);
			}
		}

		if (discard_sample) {
			struct playeng_instance *next = active_list->next;
			active_list->next = td->zombie;
			td->zombie = active_list;
			active_list = next;
		} else {
			struct playeng_instance *next = active_list->next;
			active_list->next = new_active_list;
			new_active_list = active_list;
			active_list = next;
		}
	}
	td->active = new_active_list;
}

static void *playeng_thread_proc(void *context)
{
	struct playeng_payloads *pl = context;
	while (1) {
		struct playeng_thread_data *td;
		unsigned nb_channels;
		unsigned j;
		cop_mutex_lock(&(pl->thread_lock));
		while (pl->start == NULL && ((nb_channels = pl->nb_channels) != 0))
			cop_cond_wait(&(pl->thread_cond), &(pl->thread_lock));
		if (nb_channels) {
			td = pl->start;
			pl->start = td->next;
		} else {
			td = NULL;
		}
		cop_mutex_unlock(&(pl->thread_lock));
		
		if (td == NULL)
			break;

		for (j = 0; j < nb_channels; j++) {
			unsigned k;
			for (k = 0; k < OUTPUT_SAMPLES; k++) {
				td->buffers[j][k] = 0.0f;
			}
		}

		playeng_thread_data_execute(td);

		cop_mutex_lock(&(pl->consumer_lock));
		td->next = pl->done;
		pl->done = td;
		cop_cond_signal(&(pl->consumer_cond));
		cop_mutex_unlock(&(pl->consumer_lock));
	}
	return NULL;
}
#include <stdio.h>
/* Create a single output block of audio. */
void playeng_process(struct playeng *eng, float *buffers, unsigned nb_channels, unsigned nb_samples)
{
	unsigned i;
	unsigned out_offset = 0;

	/* Insert new instances in the playback list. */
	if (cop_mutex_trylock(&eng->list_lock)) {
		if (eng->insertion_lock_level == 0) {
			struct playeng_instance *ready_list = eng->ready_list;
			while (ready_list != NULL) {
				struct playeng_instance *tmp = ready_list->next;
				ready_list->next = eng->threads[eng->next_thread_idx].active;
				eng->threads[eng->next_thread_idx].active = ready_list;
				eng->next_thread_idx = (eng->next_thread_idx + 1) % eng->nb_threads;
				ready_list = tmp;
			}
			eng->ready_list = ready_list /* = NULL */;
		}
		cop_mutex_unlock(&eng->list_lock);
	}

	/* Try to get signal lock to copy updated signals. */
	if (cop_mutex_trylock(&eng->signal_lock)) {
		for (i = 0; i < eng->nb_threads; i++) {
			struct playeng_instance *active_list = eng->threads[i].active;
			eng->threads[i].permitted_signal_mask = eng->locked_permitted_signal_mask;
			while (active_list != NULL) {
				active_list->signals = active_list->signals | active_list->locked_signals;
				active_list->locked_signals = 0;
				active_list = active_list->next;
			}
		}
		cop_mutex_unlock(&eng->signal_lock);
	}

	/* First, if there is anything sitting in the reblocking buffer, use as
	 * much of that as we can. */
	if (eng->reblock_length && nb_samples) {
		/* We will read the minimum of the number of samples requested and the
		 * total number of samples in the reblocking buffer. i.e. we are
		 * either going to completely empty the reblocking buffer, or we are
		 * going to take some samples from it and not run any samplers. */
		unsigned reblock_read = (eng->reblock_length > nb_samples) ? nb_samples : eng->reblock_length;
		nb_samples -= reblock_read;

		/* Will the read wrap past the end of the buffer? The reblock_buffers
		 * are ring buffers so the data may start somewhere in the middle and
		 * wrap around to the start again. If this is going to happen we will
		 * perform the read in two components. This could be done with a
		 * single loop using modulo arithmetic - but lets not do that because
		 * we like doing premature optimization. */
		if (eng->reblock_start + reblock_read >= OUTPUT_SAMPLES) {
			unsigned end_read = OUTPUT_SAMPLES - eng->reblock_start;
			for (i = 0; i < nb_channels; i++) {
				unsigned j;
				for (j = 0; j < end_read; j++) {
					buffers[nb_channels*(out_offset+j)+i] = eng->reblock_buffers[i][eng->reblock_start + j];
				}
			}
			out_offset            += end_read;
			reblock_read          -= end_read;
			eng->reblock_length   -= end_read;
			eng->reblock_start     = 0;
		}

		/* Read the rest... */
		for (i = 0; i < nb_channels; i++) {
			unsigned j;
			for (j = 0; j < reblock_read; j++) {
				buffers[nb_channels*(out_offset+j)+i] = eng->reblock_buffers[i][eng->reblock_start + j];
			}
		}

		out_offset          += reblock_read;
		eng->reblock_length -= reblock_read;
		eng->reblock_start  += reblock_read;
	}

	while (nb_samples) {
		unsigned nb_other = 0;
		struct playeng_thread_data *otherthreads = NULL;
		struct playeng_thread_data *thisthread = NULL;

		/* The first thread we find that has entries in the active list will
		 * be actually be processed by the calling thread. All other threads
		 * with entries in the active list will be spawned in other
		 * threads. */
		for (i = 0; i < eng->nb_threads; i++) {
			if (eng->threads[i].active != NULL) {
				if (thisthread == NULL) {
					thisthread = &(eng->threads[i]);
				} else {
					eng->threads[i].next = otherthreads;
					otherthreads = &(eng->threads[i]);
					nb_other++;
				}
				eng->threads[i].current_time = eng->current_time;
			}
		}

		if (thisthread != NULL) {
			unsigned j;

			/* Spawn all other processing threads (if there are any). If a thread
			 * fails to build (for whatever reason), the calling thread will
			 * process the samples that were supposed to be processed by the
			 * thread directly into the output buffer. The otherthreads list will
			 * be rebuilt and will only contain threads that actually ran in
			 * another thread. This is done so that when otherthreads is parsed
			 * later, we can actually call join() on threads that were active and
			 * sum output buffers which contain non-zero data into the output. */
			if (otherthreads != NULL) {
				assert(thisthread != NULL);
				cop_mutex_lock(&(eng->payloads.thread_lock));
				eng->payloads.start = otherthreads;
				eng->payloads.nb_channels = nb_channels;
				cop_cond_broadcast(&(eng->payloads.thread_cond));
				cop_mutex_unlock(&(eng->payloads.thread_lock));

				cop_mutex_lock(&(eng->payloads.consumer_lock));
				eng->payloads.done = NULL;
			}

			/* Zero the buffer that will be written to by the calling thread. We
			 * do this here because when threads fail to spawn, we execute them
			 * in this thread (we don't want audio to ever fail to be written). */
			for (j = 0; j < nb_channels; j++) {
				unsigned k;
				for (k = 0; k < OUTPUT_SAMPLES; k++) {
					thisthread->buffers[j][k] = 0.0f;
				}
			}

			/* Run this thread into the user supplied output buffer. */
			playeng_thread_data_execute(thisthread);

			/* Run all other threads and sum the output into the buffer
			 * produced by the calling thread. */
			if (otherthreads != NULL) {
				while (nb_other) {
					unsigned j;
					while (eng->payloads.done == NULL)
						cop_cond_wait((&eng->payloads.consumer_cond), &(eng->payloads.consumer_lock));

					assert(eng->payloads.done != NULL);
					do {
						assert(nb_other);
						for (j = 0; j < nb_channels; j++) {
							unsigned k;
							for (k = 0; k < OUTPUT_SAMPLES; k++) {
								thisthread->buffers[j][k] += eng->payloads.done->buffers[j][k];
							}
						}
						nb_other--;
						eng->payloads.done = eng->payloads.done->next;
					} while (eng->payloads.done != NULL);
				}
				cop_mutex_unlock(&(eng->payloads.consumer_lock));
			}

			/* At this point, thisthread's buffer contains all of the output
			 * samples produced by all of the playback instances. Some of
			 * those samples need to be written into the output buffer, and
			 * some may need to be shoved into the reblocking buffer. We do
			 * this here. */
			if (nb_samples >= OUTPUT_SAMPLES) {
				unsigned j;
				for (j = 0; j < nb_channels; j++) {
					unsigned k;
					for (k = 0; k < OUTPUT_SAMPLES; k++) {
						buffers[nb_channels*(out_offset+k)+j] = thisthread->buffers[j][k];
					}
				}
			} else {
				unsigned j;
				/* nb_samples remaining is less than the number of samples
				 * produced by the threads. Copy the required samples to the
				 * output buffer and then insert the rest into the re-blocking
				 * ring buffer. */
				for (j = 0; j < nb_channels; j++) {
					unsigned k;
					for (k = 0; k < nb_samples; k++) {
						buffers[nb_channels*(out_offset+k)+j] = thisthread->buffers[j][k];
					}
					for (; k < OUTPUT_SAMPLES; k++) {
						eng->reblock_buffers[j][(eng->reblock_start + (k - nb_samples)) % OUTPUT_SAMPLES] = thisthread->buffers[j][k];
					}
				}
				assert(eng->reblock_length == 0);
				eng->reblock_length = OUTPUT_SAMPLES - nb_samples;
			}

		} else {
			/* There were no threads in the primary list (i.e. there were not
			 * samples scheduled for playback), so there had better not be any
			 * threads which were scheduled to run. This is a logical sanity
			 * check. */
			assert(otherthreads == NULL);

			/* We still need to put zeroes in the output buffer and the
			 * reblocking buffer, otherwise we are going to get weird timing
			 * problems. */
			if (nb_samples >= OUTPUT_SAMPLES) {
				unsigned j;
				for (j = 0; j < nb_channels; j++) {
					unsigned k;
					for (k = 0; k < OUTPUT_SAMPLES; k++) {
						buffers[nb_channels*(out_offset+k)+j] = 0.0f;
					}
				}
			} else {
				unsigned j;
				for (j = 0; j < nb_channels; j++) {
					unsigned k;
					for (k = 0; k < nb_samples; k++) {
						buffers[nb_channels*(out_offset+k)+j] = 0.0f;
					}
					for (; k < OUTPUT_SAMPLES; k++) {
						eng->reblock_buffers[j][(eng->reblock_start + (k - nb_samples)) % OUTPUT_SAMPLES] = 0.0f;
					}
				}
			}
		}

		eng->current_time = (eng->current_time + 1) & 0x7FFFFFFFu;

		if (nb_samples > OUTPUT_SAMPLES) {
			nb_samples -= OUTPUT_SAMPLES;
			out_offset += OUTPUT_SAMPLES;
		} else {
			/* We don't need to modifiy out_offset as we have finished
			 * writing to the output buffer. This assignment of nb_samples
			 * could be replaced by a break. */
			nb_samples = 0;
		}
	}

	/* Try to get instance lock to return free instances. */
	if (cop_mutex_trylock(&eng->list_lock)) {
		for (i = 0; i < eng->nb_threads; i++) {
			struct playeng_instance *zombie = eng->threads[i].zombie;
			while (zombie != NULL) {
				struct playeng_instance *tmp = zombie->next;
				return_instance(eng, zombie);
				zombie = tmp;
			}
			eng->threads[i].zombie = zombie /* = NULL */;
		}
		cop_mutex_unlock(&eng->list_lock);
	}
}

void playeng_signal_block(struct playeng *eng, unsigned sigmask)
{
	assert(eng != NULL);
	assert(sigmask && "trying to block no signals.");
	cop_mutex_lock(&eng->signal_lock);
	eng->locked_permitted_signal_mask &= ~sigmask;
	cop_mutex_unlock(&eng->signal_lock);
}

void playeng_signal_unblock(struct playeng *eng, unsigned sigmask)
{
	assert(eng != NULL);
	assert(sigmask && "trying to unblock no signals.");
	cop_mutex_lock(&eng->signal_lock);
	eng->locked_permitted_signal_mask |= sigmask;
	cop_mutex_unlock(&eng->signal_lock);
}

void playeng_signal_instance(struct playeng *eng, struct playeng_instance *inst, unsigned sigmask)
{
	assert(eng != NULL && inst != NULL);
	assert(sigmask && "trying to set no signals.");
	cop_mutex_lock(&eng->signal_lock);
	inst->locked_signals |= sigmask;
	cop_mutex_unlock(&eng->signal_lock);
}



