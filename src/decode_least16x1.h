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

#ifndef DECODE_LEAST16X1_H
#define DECODE_LEAST16X1_H

unsigned u16c1_dec(struct dec_state *state, float *restrict *buf, unsigned rate)
{
	float MCCL_VEC_ALIGN tmp[64];

	/* The conditional here is to improve code generation for the interpolator
	 * data-path. Changing the "if (fpos >= SMPL_POSITION_SCALE)" into a while
	 * loop causes the disassembly to look like garbage. */
	if (1 || rate <= SMPL_POSITION_SCALE) {
		const int_least16_t *data = state->s.u16.data;
		uint_fast32_t rndstate   = state->s.u16.rndstate;
		struct filter_state s0;
		unsigned ipos, fpos, i;

		ipos = state->ipos;
		fpos = state->fpos;
		s0   = state->s.u16.resamp[0];

		for (i = 0; i < OUTPUT_SAMPLES; i += MCCL_FVEC_LEN) {

			/* This macro creates a stereo sample and shifts the left into OL_
			 * and the right into OR_. This can be called several times to
			 * fill a vector */
#define BUILD_SMPL_MONO(OL_) \
			do { \
				const float * restrict coefs_ = SMPL_INTERP[fpos]; \
				fpos += rate; \
				ACCUM_SINGLE(s0, coefs_, OL_); \
				while (fpos >= SMPL_POSITION_SCALE) { \
					float tf1_ = data[ipos]; \
					INSERT_SINGLE(s0, mccl_fld0(&tf1_)); \
					ipos++; \
					if (__builtin_expect(ipos > state->s.u16.loopend.end_smpl, 0)) { \
						const struct dec_loop_def *pdef = state->smpl->starts + state->s.u16.loopend.start_idx; \
						ipos = pdef->start_smpl; \
						rndstate = update_rnd(rndstate); \
						state->s.u16.loopend = state->smpl->ends[pdef->first_valid_end + rndstate % (state->smpl->nloop - pdef->first_valid_end)]; \
					} \
					fpos -= SMPL_POSITION_SCALE; \
				} \
			} while (0)

			{
				mccl_fvec s0l, s0r, s1l, s1r, s2l, s2r, s3l, s3r;
				mccl_fvec ox1, ox2, ox3, ox4, ox5, ox6, ox7, ox8;
				/* Build first two samples. */
				BUILD_SMPL_MONO(s0l);                 /* L0 L0 L0 L0 */
				BUILD_SMPL_MONO(s1l);                 /* L1 L1 L1 L1 */
				/* Build third sample and interleave with first. */
				BUILD_SMPL_MONO(s2l);                 /* L2 L2 L2 L2 */
				MCCL_FINTERLEAVE(ox5, ox6, s0l, s2l); /* L0 L2 L0 L2 | L0 L2 L0 L2 */
				ox3 = mccl_fadd(ox5, ox6);            /* L0 L2 L0 L2 */
				/* Build fourth sample and interleave with second. */
				BUILD_SMPL_MONO(s3l);                 /* L3 L3 L3 L3 */
				MCCL_FINTERLEAVE(ox7, ox8, s1l, s3l); /* L1 L3 L1 L3 | L1 L3 L1 L3 */
				ox4 = mccl_fadd(ox7, ox8);            /* L1 L3 L1 L3 */
				/* Interleave and store output. */
				MCCL_FINTERLEAVE(ox1, ox2, ox3, ox4); /* L0 L1 L2 L3 | L0 L1 L2 L3 */
				ox1 = mccl_fadd(ox1, ox2);            /* L0 L1 L2 L3 */
				mccl_fst(tmp + i, ox1);
			}
		}

		state->s.u16.rndstate  = rndstate;
		state->ipos            = ipos;
		state->fpos            = fpos;
		state->s.u16.resamp[0] = s0;
	} else {
		abort();
	}

	{
		float *out_l = buf[0];
		unsigned i;
		mccl_fvec fade     = mccl_fld(state->s.u16.fade);
		mccl_fvec fade_inc = mccl_fld(state->s.u16.fade_inc);
		unsigned  fadefr   = state->s.u16.fadeframes;
	
		for (i = 0; i < OUTPUT_SAMPLES; i += MCCL_FVEC_LEN) {
			mccl_fvec i1;
			mccl_fvec o1;

			o1 = mccl_fld(out_l + i);
			i1 = mccl_fld(tmp + i);
			o1 = mccl_fmac(o1, i1, fade);
			if (__builtin_expect(fadefr, 0)) {
				fade = mccl_fadd(fade, fade_inc);
				fadefr--;
			}
			mccl_fst(out_l + i, o1);
		}

		mccl_fst(state->s.u16.fade, fade);
		state->s.u16.fadeframes = fadefr;

		return fadefr;
	}
}

#endif
