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
#include <stdlib.h>

/* TODO: IMPLEMENT SCHEDULED CALLBACKS... FLAGS CURRENTLY HAVE NO EFFECT. */

/* HOW THIS THING WORKS.
 *
 * Engine
 *
 *
 *
 * Engine Instances
 *   Each instance has a flags member. The engine executes callbacks on all
 * samples which have been signalled and updates this value with the return
 * value of the callback. Flags indicates which decoders are active, any
 * termination conditions to automatically deactivate individual decoders and
 * a duration (specified in number-of-calls-to-process units) before the
 * callback will be called again regardless of the state of the signals. If
 * a callback returns zero (and hence updates the value of flags to zero) or
 * returns termination conditions which deactivate all of the decoders, the
 * instance will have completed and will be returned to the inactive queue at
 * the next opportune time. The ordering here is important. When a new decode
 * instance is created, flags are initialised to zero - this does not indicate
 * the sample s should be terminated as the callback has not yet been called.
 * A zero value in the flags indicates that the callback has not yet been
 * called and is waiting to be signalled. This means that if the caller
 * creates many sample instances with a particular signal mask while the
 * mask is blocked using playeng_signal_block(), the caller can guarantee that
 * all of the instance callbacks will be called at the same time once the
 * signal mask becomes unblocked. If there are no blocked signals, there is
 * no guarantee of when instances inserted by playeng_insert() will have
 * their callbacks made, nor of any sequencing. */

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

struct playeng_thread_data {
	struct playeng_instance    *active;
	struct playeng_instance    *zombie;
	float            *restrict *buffers;
	unsigned                    current_time;
	unsigned                    permitted_signal_mask;

	/* Not to be touched or viewed by threads! Only used by the engine calling
	 * thread. */
	struct playeng_thread_data *next;
	cop_thread                  thread;
};

struct playeng {
	struct playeng_instance    *insts_mem;
	struct dec_state           *decodes_mem;

	/* Both are lists containing max polyphony elements. */
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
	float                      ***buffers;
};


struct playeng *playeng_init(unsigned max_poly, unsigned nb_channels, unsigned nb_threads)
{
	struct playeng *pe;
	unsigned i;

	if ((pe = malloc(sizeof(*pe))) == NULL)
		return NULL;

	struct playeng_instance  *insts_mem;
	struct dec_state         *decodes_mem;

	if (cop_mutex_create(&pe->signal_lock)) {
		free(pe);
		return NULL;
	}
	if (cop_mutex_create(&pe->list_lock)) {
		cop_mutex_destroy(&pe->signal_lock);
		free(pe);
		return NULL;
	}

	if (nb_threads < 2)
		nb_threads = 1;

	pe->next_thread_idx  = 0;
	pe->nb_threads       = nb_threads;
	pe->insts_mem        = malloc(sizeof(pe->inactive_insts[0]) * max_poly);
	pe->decodes_mem      = malloc(sizeof(pe->decodes_mem[0]) * max_poly);
	pe->inactive_decodes = malloc(sizeof(pe->inactive_decodes[0]) * max_poly);
	pe->threads          = malloc(sizeof(pe->threads[0]) * nb_threads);
	if (nb_threads > 1) {
		pe->buffers          = malloc(sizeof(pe->buffers[0]) * (nb_threads-1));
		for (i = 0; i < nb_threads-1; i++) {
			unsigned j;
			pe->buffers[i] = malloc(sizeof(pe->buffers[0][0]) * nb_channels);
			for (j = 0; j < nb_channels; j++) {
				pe->buffers[i][j] = malloc(sizeof(pe->buffers[0][0][0]) * OUTPUT_SAMPLES);
			}
		}
	}

	if (pe->insts_mem == NULL || pe->inactive_decodes == NULL || pe->decodes_mem == NULL) {
		cop_mutex_destroy(&pe->signal_lock);
		cop_mutex_destroy(&pe->list_lock);
		free(pe->insts_mem);
		free(pe->inactive_decodes);
		free(pe->decodes_mem);
		free(pe);
		return NULL;
	}

	pe->ready_list = NULL;
	pe->nb_inactive_decodes = max_poly;
	pe->inactive_insts = NULL;
	pe->locked_permitted_signal_mask = ~0u;
	pe->current_time = 0;
	pe->insertion_lock_level = 0;
	for (i = 0; i < max_poly; i++) {
		pe->inactive_decodes[i] = &(pe->decodes_mem[i]);
		pe->insts_mem[i].next = pe->inactive_insts;
		pe->inactive_insts = &(pe->insts_mem[i]);
	}
	for (i = 0; i < nb_threads; i++) {
		pe->threads[i].active = NULL;
		pe->threads[i].zombie = NULL;
		pe->threads[i].permitted_signal_mask = pe->locked_permitted_signal_mask;
		pe->threads[i].buffers = NULL;
		pe->threads[i].current_time = 0;
	}

	return pe;
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
		unsigned i;
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
	unsigned i;

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

		/* What is the point of this guy? When an object gets inserted into
		 * the engine, all of the active bits are zero and it may (depending
		 * on how playeng_insert() was called) have no signal bits set. We
		 * do not want to remove the sample at this point as the caller may
		 * be reserving it to be signalled later (i.e. guarantee playback).
		 * This only gets set if either the callback is called due to
		 * signalling and returns no active bits OR if the sample has active
		 * components they become deactive given the loop/fade-termination
		 * conditions specified in the flags. */
		int discard_sample      = 0;

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

static void* playeng_thread_proc(void *context)
{
	playeng_thread_data_execute(context);
	return NULL;
}


/* Create a single output block of audio. */
void playeng_process(struct playeng *eng, float **buffers, unsigned nb_channels, unsigned nb_samples)
{
	struct playeng_thread_data *otherthreads = NULL;
	struct playeng_thread_data *thisthread = NULL;
	unsigned i;

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

	/* TEMP HACK - RUN THREAD PROCS IN AUDIO THREAD. */
	for (i = 0; i < eng->nb_threads; i++) {
		if (eng->threads[i].active != NULL) {
			if (thisthread == NULL) {
				thisthread = &(eng->threads[i]);
			} else {
				eng->threads[i].next = otherthreads;
				otherthreads = &(eng->threads[i]);
			}
			eng->threads[i].current_time = eng->current_time;
		}
	}

	/* Spawn all other processing threads (if there are any). If a thread
	 * fails to build (for whatever reason), the calling thread will process
	 * the samples that were supposed to be processed by the thread directly
	 * into the output buffer. The otherthreads list will be rebuilt and will
	 * only contain threads that actually ran in another thread. This is done
	 * so that when otherthreads is parsed later, we can actually call join()
	 * on threads that were active and sum output buffers which contain non-
	 * zero data into the output. */
	if (otherthreads != NULL) {
		struct playeng_thread_data   *td = otherthreads;
		float                      ***tbs = eng->buffers;
		otherthreads = NULL;
		while (td != NULL) {
			int err;
			unsigned j;

			/* Zero the thread's data buffer. */
			td->buffers = *tbs++;
			for (j = 0; j < nb_channels; j++) {
				unsigned k;
				for (k = 0; k < OUTPUT_SAMPLES; k++) {
					td->buffers[j][k] = 0.0f;
				}
			}

			/* If the thread failed to be created, run this thread locally
			 * into the main output buffer. */
			err = cop_thread_create(&td->thread, playeng_thread_proc, td, 0, 0);
			if (err) {
				td->buffers = buffers;
				playeng_thread_data_execute(td);
				td = td->next;
			} else {
				/* Thread executed successfully, put the thread item back
				 * into the otherthreads list. */
				struct playeng_thread_data *tmp = td->next;
				td->next = otherthreads;
				otherthreads = td;
				td = tmp;
			}
		}
	}

	if (thisthread != NULL) {
		/* Run this thread into the user supplied output buffer. */
		thisthread->buffers = buffers;
		playeng_thread_data_execute(thisthread);

		/* Wait for all other threads to finish and sum their output. */
		while (otherthreads != NULL) {
			unsigned j;

			cop_thread_join(otherthreads->thread, NULL);
			cop_thread_destroy(otherthreads->thread);

			for (j = 0; j < nb_channels; j++) {
				unsigned k;
				for (k = 0; k < OUTPUT_SAMPLES; k++) {
					buffers[j][k] += otherthreads->buffers[j][k];
				}
			}

			otherthreads = otherthreads->next;
		}
	} else {
		/* If there were no threads in the primary list, there had better not
		 * be any sitting in threads - otherwise we will not have waited for
		 * them to finish. */
		assert(otherthreads == NULL);
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

	eng->current_time = (eng->current_time + 1) & 0x7FFFFFFFu;
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



