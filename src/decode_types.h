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

#ifndef DECODE_TYPES_H
#define DECODE_TYPES_H

#include <stdint.h>
#include "cop/vec.h"
#include "interp_data.inc"

#if SMPL_INTERP_TAPS == 8
struct filter_state {
	v4f s1;
	v4f s2;
};

#define INSERT_SINGLE(state0_, ch0_) do { \
	state0_.s1 = v4f_rotl(state0_.s1, state0_.s2); \
	state0_.s2 = v4f_rotl(state0_.s2, ch0_); \
} while (0)

#define ACCUM_SINGLE(state0_, coefs_, out0_) do { \
	v4f c1_ = v4f_ld(coefs_); \
	v4f c2_ = v4f_ld(coefs_ + 4); \
	v4f t3_ = v4f_mul(state0_.s1, c1_); \
	out0_   = v4f_mul(state0_.s2, c2_); \
	out0_   = v4f_add(out0_, t3_); \
} while (0)

#define INSERT_DUAL(state0_, state1_, ch0_, ch1_) do { \
	state0_.s1 = v4f_rotl(state0_.s1, state0_.s2); \
	state1_.s1 = v4f_rotl(state1_.s1, state1_.s2); \
	state0_.s2 = v4f_rotl(state0_.s2, ch0_); \
	state1_.s2 = v4f_rotl(state1_.s2, ch1_); \
} while (0)

#define ACCUM_DUAL(state0_, state1_, coefs_, out0_, out1_) do { \
	v4f c1_ = v4f_ld(coefs_); \
	v4f c2_ = v4f_ld(coefs_ + 4); \
	v4f t1_ = v4f_mul(state0_.s1, c1_); \
	v4f t2_ = v4f_mul(state1_.s1, c1_); \
	out0_   = v4f_mul(state0_.s2, c2_); \
	out1_   = v4f_mul(state1_.s2, c2_); \
	out0_   = v4f_add(out0_, t1_); \
	out1_   = v4f_add(out1_, t2_); \
} while (0)

#endif

#define OUTPUT_SAMPLES (64u)

#define DEC_IS_LOOPING (1u)
#define DEC_IS_FADING  (2u)

#define MAX_LOOP (16)

/* This LCG is used for loop jump selection. Having this static implementation
 * (which should be inlined) provides wildly better code than using rand()
 * calls. */
static uint_fast32_t update_rnd(uint_fast32_t rnd)
{
	return rnd * 1103515245 + 12345;
}

struct dec_state;

struct dec_loop_def {
	uint_fast32_t start_smpl;

	/* Specifies the first end point which this loop can jump out of. This
	 * implies that the end loop array is sorted by position of the end
	 * marker. */
	unsigned      first_valid_end;
};

struct dec_loop_end {
	uint_fast32_t end_smpl;

	/* Specifies the index of the loop-start definition which this point MUST
	 * jump to. */
	unsigned      start_idx;
};

struct dec_smpl {
	/* Gain which must be applied during decoding to achieve the correct
	 * output level. Fade gain is always relative to this number. */
	float                 gain;
	unsigned              nloop;
	struct dec_loop_def   starts[MAX_LOOP];
	struct dec_loop_end   ends[MAX_LOOP];

	/* Decoder-specific data */
	const void           *data;

	/* Instantiate a decoder state for this sample. If ipos and fpos are non-
	 * zero, the interpolation state will be pumped with the samples prior to
	 * the start position, otherwise, the state will be filled with zeros.
	 * This has implications for releases where if the state is not pumped,
	 * there may be non-ideal samples near the start of the release. */
	void (*instantiate)(struct dec_state *instance, const struct dec_smpl *sample, uint_fast32_t ipos, uint_fast32_t fpos);
};

struct fade_state {
	VEC_ALIGN_BEST float  delta[4];
	VEC_ALIGN_BEST float  state[4];
	unsigned              nb_frames;
};

struct dec_state {
	/* You must not touch anything in this union. It is reserved completely
	 * for use by the decoder implementations. It is specified purely for
	 * ease of use of the decoder state and prevent unnecessary dereferencing
	 * operations. */
	union {
		struct {
			struct filter_state   resamp[2]; /* left/right */
			struct fade_state     fade;
			const int_least16_t  *data;
			struct dec_loop_end   loopend;
			uint_fast32_t         rndstate;
		} u16;
	} s;

	/* This is a reference to the sample. It is undefined for the sample to be
	 * modified while a dec_state instance holds it. */
	const struct dec_smpl *smpl;

	/* You may read these values, but under no circumstance should they be
	 * modified. ipos specifies the integer playback position of the input
	 * signal at the original sample rate. fpos specifies the fractional
	 * playback position and can range from 0 to SMPL_POSITION_SCALE. i.e. you
	 * can compute the playback position at any time by computing:
	 *   position = ipos + (fpos / (double)SMPL_POSITION_SCALE)
	 * The position is the next data read position in the input, so when
	 * decoding, there will always be an additional latency due to the
	 * resampling filter of SMPL_POSITION_TAPS/2. */
	uint_fast32_t fpos;
	uint_fast32_t ipos;

	/* You may set the following value to configure the playback rate of the
	 * sample. It scales the playback rate by (rate / SMPL_POSITION_SCALE).
	 * Setting this value too high will introduce aliasing (dependent on the
	 * interpolation filter being used). You have been warned. */
	uint_fast32_t rate;

	/* Triggers a fade on the sample. "target_samples" specifies the desired
	 * number of samples to fade over. The actual number used is guaranteed to
	 * be at least the value specified, but may be more. All decode
	 * implementations have the same behavior in the actual number which will
	 * be used so there is no need to worry about synchronisation (TODO:
	 * if this is shared across all implementations, maybe move that code up a
	 * level?). gain specifies the new gain value once the fade is completed.
	 * */
	void (*setfade)(struct dec_state *state, unsigned target_samples, float gain);

	/* Decode an instance of this sample into the buffers pointed to in buf.
	 * The number of buffer pointers is dependent on the channel count of the
	 * sample. Exactly OUTPUT_SAMPLES of data will be SUMMED into each output
	 * buffer. "rate" controls the fractional playback rate and is specified
	 * relative to SMPL_POSITION_SCALE. i.e. a value of 2*SMPL_POSITION_SCALE
	 * plays back at double speed, SMPL_POSITION_SCALE plays back at original
	 * speed.
	 *
	 * The return value is a set of DEC_* flags. If DEC_IS_LOOPING is
	 * signaled, the sample has entered a loop section (i.e. it can be used to
	 * detect if the attack portion of the sample is completed). If
	 * DEC_IS_FADING is signaled, there is still a fade occuring. The
	 * intention here is to be able to share common decode code for both
	 * looped samples and one-shot/release samples. Typically when a release
	 * is triggered, it will fade-in while the looped segment fades-out. When
	 * the looped segment fades out, DEC_IS_FADING will be cleared. If the
	 * release gets a short loop of zeros added to the end, completion can be
	 * detected by the DEC_IS_LOOPING flag becoming set. I think this has
	 * simplified the API... if it hasn't, oh well. */
	unsigned (*decode)(struct dec_state *state, float *restrict *buf);
};


#endif /* DECODE_TYPES_H */
