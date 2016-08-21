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

#ifndef DECODE_LEAST16X2
#define DECODE_LEAST16X2

#include "decode_types.h"
#include "cop/cop_conversions.h"

#define FADE_VEC_LEN (4)

static unsigned fade_process2(struct fade_state *state, float *COP_ATTR_RESTRICT *out, const float *in)
{
	float *out_l = out[0];
	float *out_r = out[1];
	unsigned i;
	v4f fade     = v4f_ld(state->state);
	v4f fade_inc = v4f_ld(state->delta);
	unsigned  fadefr   = state->nb_frames;

	for (i = 0; i < OUTPUT_SAMPLES; i += FADE_VEC_LEN) {
		v4f i1;
		v4f i2;
		v4f o1 = v4f_ld(out_l + i);
		v4f o2 = v4f_ld(out_r + i);
		i1     = v4f_ld(in + 2*i);
		i2     = v4f_ld(in + 2*i + FADE_VEC_LEN);
		i1     = v4f_mul(i1, fade);
		i2     = v4f_mul(i2, fade);
		o1     = v4f_add(o1, i1);
		o2     = v4f_add(o2, i2);
		if (COP_HINT_FALSE(fadefr)) {
			if (COP_HINT_FALSE(--fadefr)) {
				fade = v4f_add(fade, fade_inc);
			} else {
				fade = v4f_broadcast(state->target);
			}
		}
		v4f_st(out_l + i, o1);
		v4f_st(out_r + i, o2);
	}

	v4f_st(state->state, fade);
	state->nb_frames = fadefr;
	return fadefr;
}

static void fade_configure(struct fade_state *state, unsigned target_samples, float gain)
{
	if (target_samples != 0) {
		unsigned decay_frames = (target_samples + FADE_VEC_LEN - 1) / FADE_VEC_LEN;
		float current_gain = state->state[FADE_VEC_LEN-1];
		float gpf = (gain - current_gain) / (decay_frames);
		float gps = gpf * (1.0f / FADE_VEC_LEN);
		unsigned i;
		for (i = 0; i < FADE_VEC_LEN; i++)
		{
			state->state[i] = current_gain + (i + 1) * gps;
		}
		v4f_st(state->delta, v4f_broadcast(gpf));
		state->nb_frames = decay_frames;
	} else {
		v4f_st(state->state, v4f_broadcast(gain));
		state->nb_frames = 0;
	}
	state->target = gain;
}

static void COP_ATTR_ALWAYSINLINE decode2x12(const void *buf, float *a, float *b)
{
#if __LITTLE_ENDIAN__ && defined(__clang__)
/* A little explanation is probably required here...
 * I wanted to add support for 12-bit samples but trying to get plain-C code
 * to optimize the three loads into nice code was just not working. This
 * decoder requires that the buffer always at least contains one extra element
 * of initialized data and (to produce decent code) requires the compiler to
 * not call a standard-library version of memcpy...
 *
 * Clang produces very nice code here, but other compilers should be checked
 * and manually added to the above conditional logic when it is shown they
 * build good code.
 *
 * Also, I don't think the current guard is actually good enough... there are
 * probably more cases for this code to be broken. */
	uint32_t v; memcpy(&v, buf, sizeof(uint32_t));
#else
	uint32_t v = cop_ld_ule24(buf);
#endif
	*a = (float)(((int32_t)(v << 8)) >> 20);
	*b = (float)(((int32_t)(v << 20)) >> 20);
}
static void COP_ATTR_ALWAYSINLINE encode2x12(void *buf, int a, int b)
{
	uint32_t au = a;
	uint32_t bu = b;
	cop_st_ule24(buf, ((au & 0xFFF) << 12) | (bu & 0xFFF));
}

unsigned u16c2_dec(struct dec_state *state, float *COP_ATTR_RESTRICT *buf)
{
	float VEC_ALIGN_BEST tmp[128];
	unsigned flags;
	const int_least16_t *data;
	uint_fast32_t rndstate;
	struct filter_state s0;
	struct filter_state s1;
	unsigned ipos, fpos, i;
	unsigned rate = state->rate;

	data     = state->s.uncms.data;
	rndstate = state->s.uncms.rndstate;
	ipos     = state->ipos;
	fpos     = state->fpos;
	s0       = state->s.uncms.resamp[0];
	s1       = state->s.uncms.resamp[1];

	for (i = 0; i < 2*OUTPUT_SAMPLES; i += 2*FADE_VEC_LEN) {

		/* This macro creates a stereo sample and shifts the left into OL_
		 * and the right into OR_. This can be called several times to
		 * fill a vector */
#define BUILD_SMPL_STEREO(OL_, OR_) \
		do { \
			const float * COP_ATTR_RESTRICT coefs_ = SMPL_INTERP[fpos]; \
			fpos += rate; \
			ACCUM_DUAL(s0, s1, coefs_, OL_, OR_); \
			while (fpos >= SMPL_POSITION_SCALE) { \
				float tf1_ = data[2*ipos+0]; \
				float tf2_ = data[2*ipos+1]; \
				INSERT_DUAL(s0, s1, &tf1_, &tf2_); \
				ipos++; \
				if (COP_HINT_FALSE(ipos > state->s.uncms.loopend.end_smpl)) { \
					const struct dec_loop_def *pdef = state->smpl->starts + state->s.uncms.loopend.start_idx; \
					ipos = pdef->start_smpl; \
					rndstate = update_rnd(rndstate); \
					state->s.uncms.loopend = state->smpl->ends[pdef->first_valid_end + rndstate % (state->smpl->nloop - pdef->first_valid_end)]; \
				} \
				fpos -= SMPL_POSITION_SCALE; \
			} \
		} while (0)

		{
			v4f s0l, s0r, s1l, s1r, s2l, s2r, s3l, s3r;
			v4f ox1, ox2, ox3, ox4, ox5, ox6, ox7, ox8;

			/* Build first two samples. */
			BUILD_SMPL_STEREO(s0l, s0r);          /* L0 L0 L0 L0 | R0 R0 R0 R0 */
			V4F_INTERLEAVE(ox5, ox6, s0l, s0r);   /* L0 R0 L0 R0 | L0 R0 L0 R0 */
			BUILD_SMPL_STEREO(s1l, s1r);          /* L1 L1 L1 L1 | R1 R1 R1 R1 */
			V4F_INTERLEAVE(ox7, ox8, s1l, s1r);   /* L1 R1 L1 R1 | L1 R1 L1 R1 */
			ox1 = v4f_add(ox5, ox6);              /* L0 R0 L0 R0 */
			ox2 = v4f_add(ox7, ox8);              /* L1 R1 L1 R1 */

			/* Build third and fourth sample and interleave with first. */
			BUILD_SMPL_STEREO(s2l, s2r);          /* L2 L2 L2 L2 | R2 R2 R2 R2 */
			V4F_INTERLEAVE(ox5, ox6, s2l, s2r);   /* L2 R2 L2 R2 | L2 R2 L2 R2 */
			BUILD_SMPL_STEREO(s3l, s3r);          /* L3 L3 L3 L3 | R3 R3 R3 R3 */
			V4F_INTERLEAVE(ox7, ox8, s3l, s3r);   /* L3 R3 L3 R3 | L3 R3 L3 R3 */
			ox3 = v4f_add(ox5, ox6);              /* L2 R2 L2 R2 */
			ox4 = v4f_add(ox7, ox8);              /* L3 R3 L3 R3 */
#undef BUILD_SMPL_STEREO

			V4F_INTERLEAVE(ox5, ox6, ox1, ox3);   /* L0 L2 R0 R2 | L0 L2 R0 R2 */
			V4F_INTERLEAVE(ox7, ox8, ox2, ox4);   /* L1 L3 R1 R3 | L1 L3 R1 R3 */
			ox1 = v4f_add(ox5, ox6);              /* L0 L2 R0 R2 */
			ox2 = v4f_add(ox7, ox8);              /* L1 L3 R1 R3 */

			/* Interleave and store output. */
			V4F_ST2INT(tmp + i, ox1, ox2);        /* L0 L1 L2 L3 | R0 R1 R2 R3 */
		}
	}

	state->s.uncms.rndstate  = rndstate;
	state->ipos            = ipos;
	state->fpos            = fpos;
	state->s.uncms.resamp[0] = s0;
	state->s.uncms.resamp[1] = s1;

	flags = 0;
	if (state->ipos >= state->smpl->starts[state->s.uncms.loopend.start_idx].start_smpl) {
		flags |= DEC_IS_LOOPING;
	}
	if (fade_process2(&state->s.uncms.fade, buf, tmp) > 0) {
		flags |= DEC_IS_FADING;
	}
	return flags;
}

unsigned u12c2_dec(struct dec_state *state, float *COP_ATTR_RESTRICT *buf)
{
	float VEC_ALIGN_BEST tmp[128];
	unsigned flags;
	const unsigned char *data;
	uint_fast32_t rndstate;
	struct filter_state s0;
	struct filter_state s1;
	unsigned ipos, fpos, i;
	unsigned rate = state->rate;

	data     = state->s.uncms.data;
	rndstate = state->s.uncms.rndstate;
	ipos     = state->ipos;
	fpos     = state->fpos;
	s0       = state->s.uncms.resamp[0];
	s1       = state->s.uncms.resamp[1];

	for (i = 0; i < 2*OUTPUT_SAMPLES; i += 2*FADE_VEC_LEN) {

		/* This macro creates a stereo sample and shifts the left into OL_
		 * and the right into OR_. This can be called several times to
		 * fill a vector */
#define BUILD_SMPL_STEREO(OL_, OR_) \
		do { \
			const float * COP_ATTR_RESTRICT coefs_ = SMPL_INTERP[fpos]; \
			fpos += rate; \
			ACCUM_DUAL(s0, s1, coefs_, OL_, OR_); \
			while (fpos >= SMPL_POSITION_SCALE) { \
				float tf1_, tf2_; \
				decode2x12(data + 3*ipos, &tf1_, &tf2_); \
				INSERT_DUAL(s0, s1, &tf1_, &tf2_); \
				if (COP_HINT_FALSE(ipos >= state->s.uncms.loopend.end_smpl)) { \
					const struct dec_loop_def *pdef = state->smpl->starts + state->s.uncms.loopend.start_idx; \
					ipos     = pdef->start_smpl; \
					rndstate = update_rnd(rndstate); \
					state->s.uncms.loopend = state->smpl->ends[pdef->first_valid_end + rndstate % (state->smpl->nloop - pdef->first_valid_end)]; \
				} else { \
					ipos++; \
				} \
				fpos -= SMPL_POSITION_SCALE; \
			} \
		} while (0)

		{
			v4f s0l, s0r, s1l, s1r, s2l, s2r, s3l, s3r;
			v4f ox1, ox2, ox3, ox4, ox5, ox6, ox7, ox8;

			/* Build first two samples. */
			BUILD_SMPL_STEREO(s0l, s0r);          /* L0 L0 L0 L0 | R0 R0 R0 R0 */
			V4F_INTERLEAVE(ox5, ox6, s0l, s0r);   /* L0 R0 L0 R0 | L0 R0 L0 R0 */
			BUILD_SMPL_STEREO(s1l, s1r);          /* L1 L1 L1 L1 | R1 R1 R1 R1 */
			V4F_INTERLEAVE(ox7, ox8, s1l, s1r);   /* L1 R1 L1 R1 | L1 R1 L1 R1 */
			ox1 = v4f_add(ox5, ox6);              /* L0 R0 L0 R0 */
			ox2 = v4f_add(ox7, ox8);              /* L1 R1 L1 R1 */

			/* Build third and fourth sample and interleave with first. */
			BUILD_SMPL_STEREO(s2l, s2r);          /* L2 L2 L2 L2 | R2 R2 R2 R2 */
			V4F_INTERLEAVE(ox5, ox6, s2l, s2r);   /* L2 R2 L2 R2 | L2 R2 L2 R2 */
			BUILD_SMPL_STEREO(s3l, s3r);          /* L3 L3 L3 L3 | R3 R3 R3 R3 */
			V4F_INTERLEAVE(ox7, ox8, s3l, s3r);   /* L3 R3 L3 R3 | L3 R3 L3 R3 */
			ox3 = v4f_add(ox5, ox6);              /* L2 R2 L2 R2 */
			ox4 = v4f_add(ox7, ox8);              /* L3 R3 L3 R3 */
#undef BUILD_SMPL_STEREO

			V4F_INTERLEAVE(ox5, ox6, ox1, ox3);   /* L0 L2 R0 R2 | L0 L2 R0 R2 */
			V4F_INTERLEAVE(ox7, ox8, ox2, ox4);   /* L1 L3 R1 R3 | L1 L3 R1 R3 */
			ox1 = v4f_add(ox5, ox6);              /* L0 L2 R0 R2 */
			ox2 = v4f_add(ox7, ox8);              /* L1 L3 R1 R3 */

			/* Interleave and store output. */
			V4F_ST2INT(tmp + i, ox1, ox2);        /* L0 L1 L2 L3 | R0 R1 R2 R3 */
		}
	}

	state->s.uncms.rndstate  = rndstate;
	state->ipos              = ipos;
	state->fpos              = fpos;
	state->s.uncms.resamp[0] = s0;
	state->s.uncms.resamp[1] = s1;

	flags = 0;
	if (state->ipos >= state->smpl->starts[state->s.uncms.loopend.start_idx].start_smpl) {
		flags |= DEC_IS_LOOPING;
	}
	if (fade_process2(&state->s.uncms.fade, buf, tmp) > 0) {
		flags |= DEC_IS_FADING;
	}
	return flags;
}

static void u16c2_setfade(struct dec_state *state, unsigned target_samples, float gain)
{
	fade_configure(&state->s.uncms.fade, target_samples, state->smpl->gain * gain);
}

static COP_ATTR_ALWAYSINLINE void uc2_instantiate(struct dec_state *instance, const struct dec_smpl *sample, uint_fast32_t ipos, uint_fast32_t fpos, unsigned bits)
{
	struct filter_state s0;
	struct filter_state s1;
	
	/* TODO: remove this crap */
	memset(instance, 0, sizeof(*instance));
	memset(&s0, 0, sizeof(s0));
	memset(&s1, 0, sizeof(s1));

	instance->smpl = sample;
	instance->fpos = fpos;
	instance->ipos = ipos;
	fade_configure(&instance->s.uncms.fade, 0, sample->gain);
	instance->s.uncms.data = sample->data;
	instance->s.uncms.loopend = sample->ends[0];

	/* FILL FILTER STATE */
	if (bits == 16) {
		unsigned first = (ipos > SMPL_INTERP_TAPS) ? (ipos - SMPL_INTERP_TAPS) : 0;
		unsigned i;
		for (i = first; i < ipos; i++) {
			float tf1 = ((const int_least16_t *)(sample->data))[2*i+0];
			float tf2 = ((const int_least16_t *)(sample->data))[2*i+1];
			INSERT_DUAL(s0, s1, &tf1, &tf2);
		}
		instance->setfade         = u16c2_setfade;
		instance->decode          = u16c2_dec;
	} else if (bits == 12) {
		unsigned first = (ipos > SMPL_INTERP_TAPS) ? (ipos - SMPL_INTERP_TAPS) : 0;
		unsigned i;
		for (i = first; i < ipos; i++) {
			float tf1, tf2;
			decode2x12((((const unsigned char *)sample->data) + 3*i), &tf1, &tf2);
			INSERT_DUAL(s0, s1, &tf1, &tf2);
		}
		instance->setfade         = u16c2_setfade;
		instance->decode          = u12c2_dec;
	} else {
		abort();
	}

	instance->s.uncms.resamp[0] = s0;
	instance->s.uncms.resamp[1] = s1;
}

static COP_ATTR_UNUSED void u16c2_instantiate(struct dec_state *instance, const struct dec_smpl *sample, uint_fast32_t ipos, uint_fast32_t fpos)
{
	uc2_instantiate(instance, sample, ipos, fpos, 16);
}

static COP_ATTR_UNUSED void u12c2_instantiate(struct dec_state *instance, const struct dec_smpl *sample, uint_fast32_t ipos, uint_fast32_t fpos)
{
	uc2_instantiate(instance, sample, ipos, fpos, 12);
}

#endif
