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

#ifndef PLAYENG_H_
#define PLAYENG_H_

#include "decode_types.h"

/* Playback engine instances can be in one of three states:
 *
 *   1) inactive - this is an instance which can be reserved by the caller and
 *      inserted into the active list via an engine instruction.
 *   2) active - the instance is now owned by the audio thread and is being
 *      played back the engine.
 *   3) zombie - the instance is still owned by the audio thread, but is no
 *      longer being played. This generally happens when an active instance
 *      has completed playback. This state exists to permit control threads to
 *      still send instructions to state after playback (this is not useful,
 *      but is necessary because of threading).
 *
 * Instances always start in the inactive state, then enter the active state
 * then enter the zombie state, and the cycle continues. */

struct playeng;
struct playeng_instance;

#define PLAYENG_MAX_DECODERS_PER_INSTANCE (2)

#define PLAYENG_PACK_CALLBACK_STATUS(delay, activemask, fadetermmask, looptermmask) (((delay & 0xFFFFu) << 16) | (activemask & 0xFu) | ((fadetermmask & 0xFu) << 4) | ((looptermmask & 0xFu) << 8))
#define PLAYENG_SET_CALLBACK_ACTIVE(flags, activemask)                              (((flags) & ~0x00Fu) | ((activemask) & 0xFu))
#define PLAYENG_SET_CALLBACK_FADETER(flags, fadetermmask)                           (((flags) & ~0x0F0u) | (((fadetermmask) & 0xFu) << 4))
#define PLAYENG_SET_CALLBACK_LOOPTER(flags, looptermmask)                           (((flags) & ~0xF00u) | (((looptermmask) & 0xFu) << 8))
#define PLAYENG_GET_CALLBACK_ACTIVE(flags)                                          ((flags) & 0xFu)
#define PLAYENG_GET_CALLBACK_FADETER(flags)                                         (((flags) >> 4) & 0xFu)
#define PLAYENG_GET_CALLBACK_LOOPTER(flags)                                         (((flags) >> 8) & 0xFu)

/* The return value should be constructed using PLAYENG_PACK_CALLBACK_STATUS.
 * Once there are no active states, the sample will terminate and the decode
 * instances will be returned to the pool. */
typedef unsigned (*playeng_callback)(void *userdata, struct dec_state **states, unsigned sigmask, unsigned old_flags, unsigned sampler_time);

/* Create an instance of a playback engine with the specified maximum
 * polyphony. */
struct playeng *playeng_init(unsigned max_poly, unsigned nb_channels, unsigned nb_threads);

/* Destroy a playback engine. */
void playeng_destroy(struct playeng *eng);

/* Get ninst free decode states from the engine and assign them to a playback
 * instance. If there are not ninst decode states available, the function
 * returns NULL and the internal state is not modified. A valid return
 * instance pointer may immediately be touched by playeng_signal_instance().
 *
 *
 * To think about: maybe we need two versions of this function. If the
 * callback gets called by the calling process, clearly we need to ignore
 * the signal mask. The only way this can work properly (along with all
 * callbacks getting the same timestamp, is if they are called by the audio
 * engine)...
 *
 * XXXXXXXXXXXXXXX The supplied callback will be called. This may also be called from the
 * thread which called playeng_insert(). */
struct playeng_instance *playeng_insert(struct playeng *eng, unsigned ndec, unsigned sigmask, playeng_callback callback, void *userdata);

/* Create a single output block of audio. */
void playeng_process(struct playeng *eng, float *buffers, unsigned nb_channels, unsigned nb_samples);

/* Set the given signal mask bits. If the bits are set and are not blocked (
 * using playeng_signal_block), a callback will be triggered from the audio
 * thread specifying the triggering bits. They will be cleared immediately
 * after the callback has been triggered. */
void playeng_signal_instance(struct playeng *eng, struct playeng_instance *inst, unsigned sigmask);

/* Engine synchronisation primitives.
 *
 * These (if used correctly), have no effect on the playback engine if it is
 * being operated within a single thread. They are useful for guaranteeing
 * synchronisation for certain aspects of playback when playeng_process() is
 * being called from a different thread to playeng_insert() or
 * playeng_signal_instance(). It is worth mentioning at this point that
 * playeng_process() never blocks; primarily, this means that these
 * synchronisation mechanisms are useful for guaranteeing that things happen
 * at the "same time", but there is no guarantee made for when they actually
 * happen. This is deemed acceptable as the calling thread should not be
 * "hammering" playeng_insert() all the time.
 *
 * playeng_push_block_insertion() and
 * playeng_pop_block_insertion() are related to sample insertion
 *
 * playeng_signal_unblock() and
 * playeng_signal_block() are related to signalling information
 *
 * playeng_push_block_insertion() can be thought of as incrementing a number
 * which when non-zero blocks samples in the "wait" state (see
 * playeng_insert()) from being moved into the active state where they will
 * be played/have callbacks made. playeng_pop_block_insertion() decrements
 * the number. These functions have no influence over already active samples.
 *
 * playeng_signal_block() and playeng_signal_unblock() impact samples
 * already in the active state. playeng_signal_block() is used to temporarily
 * provide a mask of signals to ignore i.e. if they are set in the sample,
 * they will not trigger a callback, but the state will also not be cleared.
 * This permits many different sample instances to be signalled, but the
 * signal will only be processed once it has been unblocked by the engine.
 * The format of the mask for block is 1s indicate signals to block.
 * The format of the mask for unblock is 1s indicate signals to unblock. */
void playeng_push_block_insertion(struct playeng *eng);
void playeng_pop_block_insertion(struct playeng *eng);
void playeng_signal_block(struct playeng *eng, unsigned sigmask);
void playeng_signal_unblock(struct playeng *eng, unsigned sigmask);

#endif /* PLAYENG_H_ */
