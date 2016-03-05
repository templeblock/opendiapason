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

#ifdef _MSC_VER
#define _USE_MATH_DEFINES
#endif

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include "opendiapason/fastconv.h"
#include "cop/vec.h"
#include "cop/aalloc.h"

#ifndef V4F_EXISTS
#error this implementation requires a vector-4 type at the moment
#endif

/* If this macro is non-zero, the "ordered output" DFTs will use the Stockham
 * construction instead of in-place Cooley-Tukey butterfly passes with
 * explicit out-of-place reordering. */
#define ORDERED_PASSES_USE_STOCKHAM (1)

struct fastconv_pass {
	unsigned                    lfft;
	unsigned                    radix;
	float                      *twiddle;
	void                       *twidbase;

	/* The best next pass to use (this pass will have:
	 *      next->lfft = this->lfft / this->radix */
	const struct fastconv_pass *next_compat;

	/* If these are both null, this is the upload pass. Otherwise, these are
	 * both non-null. */
	void (*dit)(float *work_buf, unsigned nfft, unsigned lfft, const float *twid);
	void (*dif)(float *work_buf, unsigned nfft, unsigned lfft, const float *twid);

#if ORDERED_PASSES_USE_STOCKHAM
	void (*dif_stockham)(const float *in, float *out, const float *twid, unsigned ncol, unsigned nrow_div_radix);
#else
	void (*difreord)(const float *in_buf, float *out_buf, unsigned nfft, unsigned lfft);
	void (*ditreord)(const float *in_buf, float *out_buf, unsigned nfft, unsigned lfft);
#endif

	/* Position in list of all passes of this type (outer or inner pass). */
	struct fastconv_pass       *next;
};

#define FASTCONV_REAL_LEN_MULTIPLE (32)
#define FASTCONV_MAX_PASSES        (24)

COP_ATTR_NOINLINE static void fastconv_v4_upload(float *vec_output, const float *input, const float *coefs, unsigned fft_len)
{
	const unsigned fft_len_4 = fft_len / 4;
	unsigned i;
	assert((fft_len % 16) == 0);
	for (i = fft_len / 16
		;i
		;i--, coefs += 56, vec_output += 32, input += 4) {
		v4f r1    = v4f_ld(input + 0*fft_len_4);
		v4f r2    = v4f_ld(input + 1*fft_len_4);
		v4f r3    = v4f_ld(input + 2*fft_len_4);
		v4f r4    = v4f_ld(input + 3*fft_len_4);
		v4f i1    = v4f_ld(input + 4*fft_len_4);
		v4f i2    = v4f_ld(input + 5*fft_len_4);
		v4f i3    = v4f_ld(input + 6*fft_len_4);
		v4f i4    = v4f_ld(input + 7*fft_len_4);

		v4f twr1  = v4f_ld(coefs + 0);
		v4f twi1  = v4f_ld(coefs + 4);
		v4f twr2  = v4f_ld(coefs + 8);
		v4f twi2  = v4f_ld(coefs + 12);
		v4f or1a  = v4f_mul(twr1, r1);
		v4f or1b  = v4f_mul(twi1, i1);
		v4f oi1a  = v4f_mul(twi1, r1);
		v4f oi1b  = v4f_mul(twr1, i1);
		v4f or2a  = v4f_mul(twr2, r2);
		v4f or2b  = v4f_mul(twi2, i2);
		v4f oi2a  = v4f_mul(twi2, r2);
		v4f oi2b  = v4f_mul(twr2, i2);
		v4f or1   = v4f_add(or1a, or1b);
		v4f oi1   = v4f_sub(oi1a, oi1b);
		v4f or2   = v4f_add(or2a, or2b);
		v4f oi2   = v4f_sub(oi2a, oi2b);
		v4f twr3  = v4f_ld(coefs + 16);
		v4f twi3  = v4f_ld(coefs + 20);
		v4f twr4  = v4f_ld(coefs + 24);
		v4f twi4  = v4f_ld(coefs + 28);
		v4f or3a  = v4f_mul(twr3, r3);
		v4f or3b  = v4f_mul(twi3, i3);
		v4f oi3a  = v4f_mul(twi3, r3);
		v4f oi3b  = v4f_mul(twr3, i3);
		v4f or4a  = v4f_mul(twr4, r4);
		v4f or4b  = v4f_mul(twi4, i4);
		v4f oi4a  = v4f_mul(twi4, r4);
		v4f oi4b  = v4f_mul(twr4, i4);
		v4f or3   = v4f_add(or3a, or3b);
		v4f oi3   = v4f_sub(oi3a, oi3b);
		v4f or4   = v4f_add(or4a, or4b);
		v4f oi4   = v4f_sub(oi4a, oi4b);

		v4f t0ra  = v4f_add(or1, or3);
		v4f t0rs  = v4f_sub(or1, or3);
		v4f t1ra  = v4f_add(or2, or4);
		v4f t1rs  = v4f_sub(or2, or4);
		v4f t1is  = v4f_sub(oi2, oi4);
		v4f t1ia  = v4f_add(oi2, oi4);
		v4f t0is  = v4f_sub(oi1, oi3);
		v4f t0ia  = v4f_add(oi1, oi3);
		v4f mor0  = v4f_add(t0ra, t1ra);
		v4f mor2  = v4f_sub(t0ra, t1ra);
		v4f mor1  = v4f_add(t0rs, t1is);
		v4f mor3  = v4f_sub(t0rs, t1is);
		v4f moi1  = v4f_sub(t0is, t1rs);
		v4f moi3  = v4f_add(t0is, t1rs);
		v4f moi0  = v4f_add(t0ia, t1ia);
		v4f moi2  = v4f_sub(t0ia, t1ia);

		v4f ptwr1 = v4f_ld(coefs + 32);
		v4f ptwi1 = v4f_ld(coefs + 36);
		v4f ptwr2 = v4f_ld(coefs + 40);
		v4f ptwi2 = v4f_ld(coefs + 44);
		v4f ptwr3 = v4f_ld(coefs + 48);
		v4f ptwi3 = v4f_ld(coefs + 52);
		v4f tor1a = v4f_mul(mor1, ptwr1);
		v4f toi1a = v4f_mul(mor1, ptwi1);
		v4f tor1b = v4f_mul(moi1, ptwi1);
		v4f toi1b = v4f_mul(moi1, ptwr1);
		v4f tor2a = v4f_mul(mor2, ptwr2);
		v4f toi2a = v4f_mul(mor2, ptwi2);
		v4f tor2b = v4f_mul(moi2, ptwi2);
		v4f toi2b = v4f_mul(moi2, ptwr2);
		v4f tor3a = v4f_mul(mor3, ptwr3);
		v4f toi3a = v4f_mul(mor3, ptwi3);
		v4f tor3b = v4f_mul(moi3, ptwi3);
		v4f toi3b = v4f_mul(moi3, ptwr3);
		v4f tor1  = v4f_sub(tor1a, tor1b);
		v4f toi1  = v4f_add(toi1a, toi1b);
		v4f tor2  = v4f_sub(tor2a, tor2b);
		v4f toi2  = v4f_add(toi2a, toi2b);
		v4f tor3  = v4f_sub(tor3a, tor3b);
		v4f toi3  = v4f_add(toi3a, toi3b);

		V4F_TRANSPOSE_INPLACE(mor0, tor1, tor2, tor3);
		V4F_TRANSPOSE_INPLACE(moi0, toi1, toi2, toi3);

		v4f_st(vec_output + 0,  mor0);
		v4f_st(vec_output + 4,  moi0);
		v4f_st(vec_output + 8,  tor1);
		v4f_st(vec_output + 12, toi1);
		v4f_st(vec_output + 16, tor2);
		v4f_st(vec_output + 20, toi2);
		v4f_st(vec_output + 24, tor3);
		v4f_st(vec_output + 28, toi3);
	}
}

COP_ATTR_NOINLINE static void fastconv_v4_download(float *output, const float *vec_input, const float *coefs, unsigned fft_len)
{
	const unsigned fft_len_4 = fft_len / 4;
	unsigned i;
	assert((fft_len % 16) == 0);
	for (i = fft_len / 16
		;i
		;i--, vec_input += 32, coefs += 56, output += 4) {
		v4f r0   = v4f_ld(vec_input + 0);
		v4f i0   = v4f_ld(vec_input + 4);
		v4f r1   = v4f_ld(vec_input + 8);
		v4f i1   = v4f_ld(vec_input + 12);
		v4f r2   = v4f_ld(vec_input + 16);
		v4f i2   = v4f_ld(vec_input + 20);
		v4f r3   = v4f_ld(vec_input + 24);
		v4f i3   = v4f_ld(vec_input + 28);
		V4F_TRANSPOSE_INPLACE(r0, r1, r2, r3);
		V4F_TRANSPOSE_INPLACE(i0, i1, i2, i3);
		{
			v4f ptwr1 = v4f_ld(coefs + 32);
			v4f ptwi1 = v4f_ld(coefs + 36);
			v4f ptwr2 = v4f_ld(coefs + 40);
			v4f ptwi2 = v4f_ld(coefs + 44);
			v4f ptwr3 = v4f_ld(coefs + 48);
			v4f ptwi3 = v4f_ld(coefs + 52);
			v4f tor1a = v4f_mul(r1, ptwr1);
			v4f toi1a = v4f_mul(r1, ptwi1);
			v4f tor1b = v4f_mul(i1, ptwi1);
			v4f toi1b = v4f_mul(i1, ptwr1);
			v4f tor2a = v4f_mul(r2, ptwr2);
			v4f toi2a = v4f_mul(r2, ptwi2);
			v4f tor2b = v4f_mul(i2, ptwi2);
			v4f toi2b = v4f_mul(i2, ptwr2);
			v4f tor3a = v4f_mul(r3, ptwr3);
			v4f toi3a = v4f_mul(r3, ptwi3);
			v4f tor3b = v4f_mul(i3, ptwi3);
			v4f toi3b = v4f_mul(i3, ptwr3);
			v4f tor1  = v4f_sub(tor1a, tor1b);
			v4f toi1  = v4f_add(toi1a, toi1b);
			v4f tor2  = v4f_sub(tor2a, tor2b);
			v4f toi2  = v4f_add(toi2a, toi2b);
			v4f tor3  = v4f_sub(tor3a, tor3b);
			v4f toi3  = v4f_add(toi3a, toi3b);

			v4f t0ra  = v4f_add(r0,   tor2);
			v4f t0rs  = v4f_sub(r0,   tor2);
			v4f t1ra  = v4f_add(tor1, tor3);
			v4f t1rs  = v4f_sub(tor1, tor3);
			v4f t1is  = v4f_sub(toi1, toi3);
			v4f t1ia  = v4f_add(toi1, toi3);
			v4f t0is  = v4f_sub(i0,   toi2);
			v4f t0ia  = v4f_add(i0,   toi2);
			v4f mor0  = v4f_add(t0ra, t1ra);
			v4f mor2  = v4f_sub(t0ra, t1ra);
			v4f mor1  = v4f_add(t0rs, t1is);
			v4f mor3  = v4f_sub(t0rs, t1is);
			v4f moi1  = v4f_sub(t0is, t1rs);
			v4f moi3  = v4f_add(t0is, t1rs);
			v4f moi0  = v4f_add(t0ia, t1ia);
			v4f moi2  = v4f_sub(t0ia, t1ia);

			v4f twr1  = v4f_ld(coefs + 0);
			v4f twi1  = v4f_ld(coefs + 4);
			v4f twr2  = v4f_ld(coefs + 8);
			v4f twi2  = v4f_ld(coefs + 12);
			v4f or0a  = v4f_mul(twr1, mor0);
			v4f or0b  = v4f_mul(twi1, moi0);
			v4f oi0a  = v4f_mul(twi1, mor0);
			v4f oi0b  = v4f_mul(twr1, moi0);
			v4f or1a  = v4f_mul(twr2, mor1);
			v4f or1b  = v4f_mul(twi2, moi1);
			v4f oi1a  = v4f_mul(twi2, mor1);
			v4f oi1b  = v4f_mul(twr2, moi1);
			v4f or0   = v4f_sub(or0a, or0b);
			v4f oi0   = v4f_add(oi0a, oi0b);
			v4f or1   = v4f_sub(or1a, or1b);
			v4f oi1   = v4f_add(oi1a, oi1b);
			v4f twr3  = v4f_ld(coefs + 16);
			v4f twi3  = v4f_ld(coefs + 20);
			v4f twr4  = v4f_ld(coefs + 24);
			v4f twi4  = v4f_ld(coefs + 28);
			v4f or2a  = v4f_mul(twr3, mor2);
			v4f or2b  = v4f_mul(twi3, moi2);
			v4f oi2a  = v4f_mul(twi3, mor2);
			v4f oi2b  = v4f_mul(twr3, moi2);
			v4f or3a  = v4f_mul(twr4, mor3);
			v4f or3b  = v4f_mul(twi4, moi3);
			v4f oi3a  = v4f_mul(twi4, mor3);
			v4f oi3b  = v4f_mul(twr4, moi3);
			v4f or2   = v4f_sub(or2a, or2b);
			v4f oi2   = v4f_add(oi2a, oi2b);
			v4f or3   = v4f_sub(or3a, or3b);
			v4f oi3   = v4f_add(oi3a, oi3b);

			v4f_st(output + 0*fft_len_4, or0);
			v4f_st(output + 1*fft_len_4, or1);
			v4f_st(output + 2*fft_len_4, or2);
			v4f_st(output + 3*fft_len_4, or3);
			v4f_st(output + 4*fft_len_4, oi0);
			v4f_st(output + 5*fft_len_4, oi1);
			v4f_st(output + 6*fft_len_4, oi2);
			v4f_st(output + 7*fft_len_4, oi3);
		}
	}
}

static void fc_v4_dif_r2(float *work_buf, unsigned nfft, unsigned lfft, const float *twid)
{
	unsigned rinc = lfft * 4;
	lfft /= 2;
	do {
		unsigned j;
		for (j = 0; j < lfft; j++, work_buf += 8) {
			v4f nre   = v4f_ld(work_buf + 0);
			v4f nim   = v4f_ld(work_buf + 4);
			v4f fre   = v4f_ld(work_buf + rinc + 0);
			v4f fim   = v4f_ld(work_buf + rinc + 4);
			v4f tre   = v4f_broadcast(twid[2*j+0]);
			v4f tim   = v4f_broadcast(twid[2*j+1]);
			v4f onre  = v4f_add(nre, fre);
			v4f onim  = v4f_add(nim, fim);
			v4f ptre  = v4f_sub(nre, fre);
			v4f ptim  = v4f_sub(nim, fim);
			v4f ofrea = v4f_mul(ptre, tre);
			v4f ofreb = v4f_mul(ptim, tim);
			v4f ofima = v4f_mul(ptre, tim);
			v4f ofimb = v4f_mul(ptim, tre);
			v4f ofre  = v4f_sub(ofrea, ofreb);
			v4f ofim  = v4f_add(ofima, ofimb);
			v4f_st(work_buf + 0, onre);
			v4f_st(work_buf + 4, onim);
			v4f_st(work_buf + rinc + 0, ofre);
			v4f_st(work_buf + rinc + 4, ofim);
		}
		work_buf += rinc;
	} while (--nfft);
}

static void fc_v4_dit_r2(float *work_buf, unsigned nfft, unsigned lfft, const float *twid)
{
	unsigned rinc = lfft * 4;
	lfft /= 2;
	do {
		unsigned j;
		for (j = 0; j < lfft; j++, work_buf += 8) {
			v4f nre  = v4f_ld(work_buf + 0);
			v4f nim  = v4f_ld(work_buf + 4);
			v4f ptre = v4f_ld(work_buf + rinc + 0);
			v4f ptim = v4f_ld(work_buf + rinc + 4);
			v4f tre  = v4f_broadcast(twid[2*j+0]);
			v4f tim  = v4f_broadcast(twid[2*j+1]);
			v4f frea = v4f_mul(ptre, tre);
			v4f freb = v4f_mul(ptim, tim);
			v4f fima = v4f_mul(ptre, tim);
			v4f fimb = v4f_mul(ptim, tre);
			v4f fre  = v4f_sub(frea, freb);
			v4f fim  = v4f_add(fima, fimb);
			v4f onre = v4f_add(nre, fre);
			v4f onim = v4f_add(nim, fim);
			v4f ofre = v4f_sub(nre, fre);
			v4f ofim = v4f_sub(nim, fim);
			v4f_st(work_buf + 0, onre);
			v4f_st(work_buf + 4, onim);
			v4f_st(work_buf + rinc + 0, ofre);
			v4f_st(work_buf + rinc + 4, ofim);
		}
		work_buf += rinc;
	} while (--nfft);
}

#if ORDERED_PASSES_USE_STOCKHAM

static void fc_v4_stock_r2(const float *in, float *out, const float *twid, unsigned ncol, unsigned nrow_div_radix)
{
	const unsigned ooffset = (2*4)*ncol;
	const unsigned ioffset = ooffset*nrow_div_radix;
	do {
		const float *in0 = in;
		const float *tp   = twid;
		unsigned     j    = ncol;
		do {
			v4f r0, i0, r1, i1;
			v4f or0, oi0, or1, oi1;
			v4f twr1, twi1;
			V4F_LD2(r0, i0, in0 + 0*ooffset);
			V4F_LD2(r1, i1, in0 + 1*ooffset);
			twr1     = v4f_broadcast(tp[0]);
			twi1     = v4f_broadcast(tp[1]);
			or1      = v4f_sub(r0, r1);
			oi1      = v4f_sub(i0, i1);
			or0      = v4f_add(r0, r1);
			oi0      = v4f_add(i0, i1);
			v4f or1a = v4f_mul(or1, twr1);
			v4f or1b = v4f_mul(oi1, twi1);
			v4f oi1a = v4f_mul(or1, twi1);
			v4f oi1b = v4f_mul(oi1, twr1);
			r1       = v4f_sub(or1a, or1b);
			i1       = v4f_add(oi1a, oi1b);
			v4f_st(out + 0*ioffset+0*4, or0);
			v4f_st(out + 0*ioffset+1*4, oi0);
			v4f_st(out + 1*ioffset+0*4, r1);
			v4f_st(out + 1*ioffset+1*4, i1);
			tp   += 2;
			out  += (2*4);
			in0  += (2*4);
		} while (--j);
		in = in + 2*ooffset;
	} while (--nrow_div_radix);
}

#else

static void fc_v4_dit_r2_reord(const float *in_buf, float *out_buf, unsigned nfft, unsigned lfft)
{
	unsigned rinc = lfft * 4;
	lfft /= 2;
	do {
		unsigned j = lfft;
		do {
			v4f re0 = v4f_ld(in_buf + 0);
			v4f im0 = v4f_ld(in_buf + 4);
			v4f re1 = v4f_ld(in_buf + 8);
			v4f im1 = v4f_ld(in_buf + 12);
			v4f_st(out_buf + 0, re0);
			v4f_st(out_buf + 4, im0);
			v4f_st(out_buf + rinc + 0, re1);
			v4f_st(out_buf + rinc + 4, im1);
			in_buf += 16; out_buf += 8;
		} while (--j);
		out_buf += rinc;
	} while (--nfft);
}

static void fc_v4_dif_r2_reord(const float *in_buf, float *out_buf, unsigned nfft, unsigned lfft)
{
	unsigned rinc = lfft * 4;
	lfft /= 2;
	do {
		unsigned j = lfft;
		do {
			v4f re0 = v4f_ld(in_buf + 0);
			v4f im0 = v4f_ld(in_buf + 4);
			v4f re1 = v4f_ld(in_buf + rinc + 0);
			v4f im1 = v4f_ld(in_buf + rinc + 4);
			v4f_st(out_buf + 0, re0);
			v4f_st(out_buf + 4, im0);
			v4f_st(out_buf + 8, re1);
			v4f_st(out_buf + 12, im1);
			in_buf += 8; out_buf += 16;
		} while (--j);
		in_buf += rinc;
	} while (--nfft);
}

#endif

void
fastconv_execute_fwd
	(const struct fastconv_pass *first_pass
	,const float                *input_buf
	,float                      *output_buf
	)
{
	unsigned nfft = 1;
	assert(first_pass->dif == NULL && first_pass->dit == NULL);
	fastconv_v4_upload(output_buf, input_buf, first_pass->twiddle, first_pass->lfft);
	while (first_pass->lfft != first_pass->radix) {
		first_pass = first_pass->next_compat;
		assert(first_pass != NULL);
		first_pass->dif(output_buf, nfft, first_pass->lfft, first_pass->twiddle);
		nfft *= first_pass->radix;
	}
}

void
fastconv_execute_conv
	(const struct fastconv_pass *first_pass
	,const float                *input_buf
	,const float                *kernel_buf
	,float                      *output_buf
	,float                      *work_buf
	)
{
	const struct fastconv_pass *pass_stack[FASTCONV_MAX_PASSES];
	unsigned si = 0;
	unsigned nfft = 1;
	unsigned i;
	assert(first_pass->dif == NULL && first_pass->dit == NULL);
	fastconv_v4_upload(work_buf, input_buf, first_pass->twiddle, first_pass->lfft);
	while (first_pass->lfft != first_pass->radix) {
		assert(si < FASTCONV_MAX_PASSES);
		pass_stack[si++] = first_pass;
		first_pass = first_pass->next_compat;
		assert(first_pass != NULL);
		first_pass->dif(work_buf, nfft, first_pass->lfft, first_pass->twiddle);
		nfft *= first_pass->radix;
	}
	for (i = 0; i < nfft; i++) {
		v4f dr =         v4f_ld(work_buf   + 8*i+0);
		v4f di = v4f_neg(v4f_ld(work_buf  + 8*i+4));
		v4f cr =         v4f_ld(kernel_buf + 8*i+0);
		v4f ci =         v4f_ld(kernel_buf + 8*i+4);
		v4f ra = v4f_mul(dr, cr);
		v4f rb = v4f_mul(di, ci);
		v4f ia = v4f_mul(di, cr);
		v4f ib = v4f_mul(dr, ci);
		v4f ro = v4f_add(ra, rb);
		v4f io = v4f_sub(ia, ib);
		v4f_st(work_buf + 8*i + 0, ro);
		v4f_st(work_buf + 8*i + 4, io);
	}
	while (first_pass->dit != NULL && first_pass->dif != NULL) {
		nfft /= first_pass->radix;
		first_pass->dit(work_buf, nfft, first_pass->lfft, first_pass->twiddle);
		first_pass = pass_stack[--si];
	}
	assert(nfft == 1);
	assert(si == 0);
	fastconv_v4_download(output_buf, work_buf, first_pass->twiddle, first_pass->lfft);
}

COP_ATTR_NOINLINE static void fwd_post_reorder(const float *in_buf, float *out_buf, unsigned lfft)
{
	unsigned i;
	for (i = 0; i < lfft / 8; i++) {
		v4f tor1, toi1, tor2, toi2; 
		v4f re1 = v4f_ld(in_buf + i*8 + 0);
		v4f im1 = v4f_ld(in_buf + i*8 + 4);
		v4f re2 = v4f_ld(in_buf + lfft*2 - i*8 - 8);
		v4f im2 = v4f_ld(in_buf + lfft*2 - i*8 - 4);
		re2     = v4f_reverse(re2);
		im2     = v4f_reverse(v4f_neg(im2));
		V4F_INTERLEAVE(tor1, tor2, re1, re2);
		V4F_INTERLEAVE(toi1, toi2, im1, im2);
		V4F_ST2X2INT(out_buf + i*16, out_buf + i*16 + 8, tor1, toi1, tor2, toi2);
	}
}

void
fastconv_execute_fwd_reord
	(const struct fastconv_pass *first_pass
	,const float                *input_buf
	,float                      *output_buf
	,float                      *work_buf
	)
{
	const struct fastconv_pass *pass_stack[FASTCONV_MAX_PASSES];
	unsigned si = 0;
	unsigned nfft = 1;
	assert(first_pass->dif == NULL && first_pass->dit == NULL);
	fastconv_v4_upload(work_buf, input_buf, first_pass->twiddle, first_pass->lfft);
#if ORDERED_PASSES_USE_STOCKHAM
	while (first_pass->lfft != first_pass->radix) {
		assert(si < FASTCONV_MAX_PASSES);
		pass_stack[si++] = first_pass;
		first_pass = first_pass->next_compat;
		first_pass->dif_stockham(work_buf, output_buf, first_pass->twiddle, first_pass->lfft/2, nfft);
		assert(first_pass != NULL);
		nfft *= first_pass->radix;
		float *tmp = output_buf;
		output_buf = work_buf;
		work_buf   = tmp;
	}
	first_pass = pass_stack[0];
#else
	while (first_pass->lfft != first_pass->radix) {
		assert(si < FASTCONV_MAX_PASSES);
		pass_stack[si++] = first_pass;
		first_pass = first_pass->next_compat;
		assert(first_pass != NULL);
		first_pass->dif(work_buf, nfft, first_pass->lfft, first_pass->twiddle);
		nfft *= first_pass->radix;
	}
	while (first_pass->dit != NULL && first_pass->dif != NULL) {
		nfft /= first_pass->radix;
		first_pass->difreord(work_buf, output_buf, nfft, first_pass->lfft);
		first_pass = pass_stack[--si];
		float *tmp = output_buf;
		output_buf = work_buf;
		work_buf   = tmp;
	}
	assert(nfft == 1);
	assert(si == 0);
#endif
	fwd_post_reorder(work_buf, output_buf, first_pass->lfft);
	memcpy(work_buf, output_buf, sizeof(float) * first_pass->lfft * 2);
}

void
fastconv_execute_rev_reord
	(const struct fastconv_pass *first_pass
	,const float                *input_buf
	,float                      *output_buf
	,float                      *work_buf
	)
{
	const struct fastconv_pass *pass_stack[FASTCONV_MAX_PASSES];
	unsigned si = 0;
	unsigned nfft = 1;
	unsigned i;
	assert(first_pass->dif == NULL && first_pass->dit == NULL);

	for (i = 0; i < first_pass->lfft / 8; i++) {
		v4f tor1, toi1, tor2, toi2, re1, im1, re2, im2;
		V4F_LD2X2DINT(tor1, toi1, tor2, toi2, input_buf + i*16, input_buf + i*16 + 8);
		V4F_DEINTERLEAVE(re1, re2, tor1, tor2);
		V4F_DEINTERLEAVE(im1, im2, toi1, toi2);
		re2 = v4f_reverse(re2);
		im2 = v4f_reverse(im2);
		im1 = v4f_neg(im1);
		v4f_st(work_buf + i*8 + 0, re1);
		v4f_st(work_buf + i*8 + 4, im1);
		v4f_st(work_buf + first_pass->lfft*2 - i*8 - 8, re2);
		v4f_st(work_buf + first_pass->lfft*2 - i*8 - 4, im2);
	}

#if ORDERED_PASSES_USE_STOCKHAM
	while (first_pass->lfft != first_pass->radix) {
		assert(si < FASTCONV_MAX_PASSES);
		pass_stack[si++] = first_pass;
		first_pass = first_pass->next_compat;
		first_pass->dif_stockham(work_buf, output_buf, first_pass->twiddle, first_pass->lfft/2, nfft);
		assert(first_pass != NULL);
		nfft *= first_pass->radix;
		float *tmp = output_buf;
		output_buf = work_buf;
		work_buf = tmp;
	}
	first_pass = pass_stack[0];
#else
	while (first_pass->lfft != first_pass->radix) {
		assert(si < FASTCONV_MAX_PASSES);
		pass_stack[si++] = first_pass;
		first_pass = first_pass->next_compat;
		assert(first_pass != NULL);
		first_pass->ditreord(work_buf, output_buf, nfft, first_pass->lfft);
		nfft *= first_pass->radix;

		float *tmp = output_buf;
		output_buf = work_buf;
		work_buf = tmp;
	}
	while (first_pass->dit != NULL && first_pass->dif != NULL) {
		nfft /= first_pass->radix;
		first_pass->dit(work_buf, nfft, first_pass->lfft, first_pass->twiddle);
		first_pass = pass_stack[--si];
	}
	assert(nfft == 1);
	assert(si == 0);
#endif

	fastconv_v4_download(output_buf, work_buf, first_pass->twiddle, first_pass->lfft);
	/* We have no idea which buffer is which because of the reindexing. Copy
	 * the output buffer into the work buffer. */
	memcpy(work_buf, output_buf, sizeof(float) * first_pass->lfft * 2);
}

void fastconv_fftset_init(struct fastconv_fftset *fc)
{
	fc->first_outer = NULL;
	fc->first_inner = NULL;
}

void fastconv_fftset_destroy(struct fastconv_fftset *fc)
{
	while (fc->first_outer != NULL) {
		struct fastconv_pass *p = fc->first_outer;
		fc->first_outer = p->next;
		free(p->twidbase);
		free(p);
	}
	while (fc->first_inner != NULL) {
		struct fastconv_pass *p = fc->first_inner;
		fc->first_inner = p->next;
		free(p->twidbase);
		free(p);
	}
}

static struct fastconv_pass *fastconv_get_inner_pass(struct fastconv_fftset *fc, unsigned length)
{
	struct fastconv_pass *pass;
	struct fastconv_pass **ipos;
	unsigned i;

	/* Search for the pass. */
	for (pass = fc->first_inner; pass != NULL; pass = pass->next) {
		if (pass->lfft == length && pass->dif != NULL)
			return pass;
		if (pass->lfft < length)
			break;
	}

	/* Create new inner pass. */
	pass = malloc(sizeof(*pass));
	if (pass == NULL)
		return NULL;

	/* Detect radix. */
	if (length % 2 == 0) {
		pass->twiddle = aalloc_malloc(sizeof(float) * length, 64, &pass->twidbase);
		if (pass->twiddle == NULL) {
			free(pass);
			return NULL;
		}
		for (i = 0; i < length / 2; i++) {
			pass->twiddle[2*i+0] = cosf(i * (-(float)M_PI * 2.0f) / length);
			pass->twiddle[2*i+1] = sinf(i * (-(float)M_PI * 2.0f) / length);
		}
		pass->lfft         = length;
		pass->radix        = 2;
		pass->dif          = fc_v4_dif_r2;
		pass->dit          = fc_v4_dit_r2;
#if ORDERED_PASSES_USE_STOCKHAM
		pass->dif_stockham = fc_v4_stock_r2;
#else
		pass->difreord     = fc_v4_dif_r2_reord;
		pass->ditreord     = fc_v4_dit_r2_reord;
#endif
	} else {
		/* Only support radix-2. */
		abort();
	}

	/* Make next pass if required */
	if (pass->lfft != pass->radix) {
		assert(pass->lfft % pass->radix == 0);
		pass->next_compat = fastconv_get_inner_pass(fc, pass->lfft / pass->radix);
		if (pass->next_compat == NULL) {
			free(pass->twidbase);
			free(pass);
			return NULL;
		}
	} else {
		pass->next_compat = NULL;
	}

	/* Insert into list. */
	ipos = &(fc->first_inner);
	while (*ipos != NULL && length < (*ipos)->lfft) {
		ipos = &(*ipos)->next;
	}
	pass->next = *ipos;
	*ipos = pass;

	return pass;
}

/* fastconv_get_outer_real_pass()
 *
 * Searches the given fftset for a real input DFT of the specified length. If
 * the pass is found, it will be returned. If the pass is not found, it will
 * be created, stored in the fftset for later use and returned. The function
 * can only fail if memory is exhausted. Unsupported values of length are
 * undefined. */
const struct fastconv_pass *fastconv_get_real_conv(struct fastconv_fftset *fc, unsigned real_length)
{
	unsigned i;
	unsigned length;
	struct fastconv_pass *pass;
	struct fastconv_pass **ipos;

	assert(real_length % 32 == 0 && "the real length must be even and divisible by 32");

	length = real_length / 2;

	/* Find the pass. */
	for (pass = fc->first_outer; pass != NULL; pass = pass->next) {
		if (pass->lfft == length && pass->dif == NULL)
			return pass;
		if (pass->lfft < length)
			break;
	}

	/* Create new outer pass and insert it into the list. */
	pass = malloc(sizeof(*pass));
	if (pass == NULL)
		return NULL;

	/* Create memory for twiddle coefficients. */
	pass->twiddle = aalloc_malloc(sizeof(float) * 56 * length / 16, 64, &pass->twidbase);
	if (pass->twiddle == NULL) {
		free(pass);
		return NULL;
	}

	/* Create inner passes recursively. */
	pass->next_compat = fastconv_get_inner_pass(fc, length / 4);
	if (pass->next_compat == NULL) {
		free(pass->twidbase);
		free(pass);
		return NULL;
	}

	pass->lfft         = length;
	pass->radix        = 4;
	pass->dif          = NULL;
	pass->dit          = NULL;
#if ORDERED_PASSES_USE_STOCKHAM
	pass->dif_stockham = NULL;
#else
	pass->difreord     = NULL;
	pass->ditreord     = NULL;
#endif

	/* Generate twiddle */
	for (i = 0; i < length / 16; i++) {
		pass->twiddle[56*i+0]  = cosf((4*i+0) * -0.5 * M_PI / length);
		pass->twiddle[56*i+1]  = cosf((4*i+1) * -0.5 * M_PI / length);
		pass->twiddle[56*i+2]  = cosf((4*i+2) * -0.5 * M_PI / length);
		pass->twiddle[56*i+3]  = cosf((4*i+3) * -0.5 * M_PI / length);
		pass->twiddle[56*i+4]  = sinf((4*i+0) * -0.5 * M_PI / length);
		pass->twiddle[56*i+5]  = sinf((4*i+1) * -0.5 * M_PI / length);
		pass->twiddle[56*i+6]  = sinf((4*i+2) * -0.5 * M_PI / length);
		pass->twiddle[56*i+7]  = sinf((4*i+3) * -0.5 * M_PI / length);
		pass->twiddle[56*i+8]  = cosf((4*i+0+1*length/4) * -0.5 * M_PI / length);
		pass->twiddle[56*i+9]  = cosf((4*i+1+1*length/4) * -0.5 * M_PI / length);
		pass->twiddle[56*i+10] = cosf((4*i+2+1*length/4) * -0.5 * M_PI / length);
		pass->twiddle[56*i+11] = cosf((4*i+3+1*length/4) * -0.5 * M_PI / length);
		pass->twiddle[56*i+12] = sinf((4*i+0+1*length/4) * -0.5 * M_PI / length);
		pass->twiddle[56*i+13] = sinf((4*i+1+1*length/4) * -0.5 * M_PI / length);
		pass->twiddle[56*i+14] = sinf((4*i+2+1*length/4) * -0.5 * M_PI / length);
		pass->twiddle[56*i+15] = sinf((4*i+3+1*length/4) * -0.5 * M_PI / length);
		pass->twiddle[56*i+16] = cosf((4*i+0+2*length/4) * -0.5 * M_PI / length);
		pass->twiddle[56*i+17] = cosf((4*i+1+2*length/4) * -0.5 * M_PI / length);
		pass->twiddle[56*i+18] = cosf((4*i+2+2*length/4) * -0.5 * M_PI / length);
		pass->twiddle[56*i+19] = cosf((4*i+3+2*length/4) * -0.5 * M_PI / length);
		pass->twiddle[56*i+20] = sinf((4*i+0+2*length/4) * -0.5 * M_PI / length);
		pass->twiddle[56*i+21] = sinf((4*i+1+2*length/4) * -0.5 * M_PI / length);
		pass->twiddle[56*i+22] = sinf((4*i+2+2*length/4) * -0.5 * M_PI / length);
		pass->twiddle[56*i+23] = sinf((4*i+3+2*length/4) * -0.5 * M_PI / length);
		pass->twiddle[56*i+24] = cosf((4*i+0+3*length/4) * -0.5 * M_PI / length);
		pass->twiddle[56*i+25] = cosf((4*i+1+3*length/4) * -0.5 * M_PI / length);
		pass->twiddle[56*i+26] = cosf((4*i+2+3*length/4) * -0.5 * M_PI / length);
		pass->twiddle[56*i+27] = cosf((4*i+3+3*length/4) * -0.5 * M_PI / length);
		pass->twiddle[56*i+28] = sinf((4*i+0+3*length/4) * -0.5 * M_PI / length);
		pass->twiddle[56*i+29] = sinf((4*i+1+3*length/4) * -0.5 * M_PI / length);
		pass->twiddle[56*i+30] = sinf((4*i+2+3*length/4) * -0.5 * M_PI / length);
		pass->twiddle[56*i+31] = sinf((4*i+3+3*length/4) * -0.5 * M_PI / length);
		pass->twiddle[56*i+32] = cosf(-2.0 * (4*i+0) * M_PI * 1.0 / length);
		pass->twiddle[56*i+33] = cosf(-2.0 * (4*i+1) * M_PI * 1.0 / length);
		pass->twiddle[56*i+34] = cosf(-2.0 * (4*i+2) * M_PI * 1.0 / length);
		pass->twiddle[56*i+35] = cosf(-2.0 * (4*i+3) * M_PI * 1.0 / length);
		pass->twiddle[56*i+36] = sinf(-2.0 * (4*i+0) * M_PI * 1.0 / length);
		pass->twiddle[56*i+37] = sinf(-2.0 * (4*i+1) * M_PI * 1.0 / length);
		pass->twiddle[56*i+38] = sinf(-2.0 * (4*i+2) * M_PI * 1.0 / length);
		pass->twiddle[56*i+39] = sinf(-2.0 * (4*i+3) * M_PI * 1.0 / length);
		pass->twiddle[56*i+40] = cosf(-2.0 * (4*i+0) * M_PI * 2.0 / length);
		pass->twiddle[56*i+41] = cosf(-2.0 * (4*i+1) * M_PI * 2.0 / length);
		pass->twiddle[56*i+42] = cosf(-2.0 * (4*i+2) * M_PI * 2.0 / length);
		pass->twiddle[56*i+43] = cosf(-2.0 * (4*i+3) * M_PI * 2.0 / length);
		pass->twiddle[56*i+44] = sinf(-2.0 * (4*i+0) * M_PI * 2.0 / length);
		pass->twiddle[56*i+45] = sinf(-2.0 * (4*i+1) * M_PI * 2.0 / length);
		pass->twiddle[56*i+46] = sinf(-2.0 * (4*i+2) * M_PI * 2.0 / length);
		pass->twiddle[56*i+47] = sinf(-2.0 * (4*i+3) * M_PI * 2.0 / length);
		pass->twiddle[56*i+48] = cosf(-2.0 * (4*i+0) * M_PI * 3.0 / length);
		pass->twiddle[56*i+49] = cosf(-2.0 * (4*i+1) * M_PI * 3.0 / length);
		pass->twiddle[56*i+50] = cosf(-2.0 * (4*i+2) * M_PI * 3.0 / length);
		pass->twiddle[56*i+51] = cosf(-2.0 * (4*i+3) * M_PI * 3.0 / length);
		pass->twiddle[56*i+52] = sinf(-2.0 * (4*i+0) * M_PI * 3.0 / length);
		pass->twiddle[56*i+53] = sinf(-2.0 * (4*i+1) * M_PI * 3.0 / length);
		pass->twiddle[56*i+54] = sinf(-2.0 * (4*i+2) * M_PI * 3.0 / length);
		pass->twiddle[56*i+55] = sinf(-2.0 * (4*i+3) * M_PI * 3.0 / length);
	}

	/* Insert into list. */
	ipos = &(fc->first_outer);
	while (*ipos != NULL && length < (*ipos)->lfft) {
		ipos = &(*ipos)->next;
	}
	pass->next = *ipos;
	*ipos = pass;
	return pass;
}

static unsigned rounduptonearestfactorisation(unsigned min)
{
	unsigned length = 2;
	while (length < min) {
		length    *= 2;
	}
	return length;
}

unsigned fastconv_recommend_length(unsigned kernel_length, unsigned max_block_size)
{
	/* We found that input blocks of 8 times the kernel length is a good
	 * performance point. */
	const unsigned target_max_block_size = 8 * kernel_length;

	/* If the user specifies they will never pass a block larger than
	 * max_block_size and our target is less than this value, use the
	 * maximum of the two. Recall, if a user specifies a max_block_size of
	 * zero this indicates that they will deal with whatever maximum is
	 * returned by fastconv_kernel_max_block_size() after initialization
	 * (which is likely to be close to our target value). */
	const unsigned real_max_block_size = (max_block_size > target_max_block_size) ? max_block_size : target_max_block_size;

	/* The minimum real dft length is just the length of the maximum convolved
	 * output sequence. */
	const unsigned min_real_dft_length = kernel_length + real_max_block_size - 1;

	/* We do our processing on vectors if we can. The optimal way of doing
	 * this (which avoids any scalar loads/stores on input/output vectors)
	 * requires particular length multiples. */
	return FASTCONV_REAL_LEN_MULTIPLE * rounduptonearestfactorisation((min_real_dft_length + FASTCONV_REAL_LEN_MULTIPLE - 1) / FASTCONV_REAL_LEN_MULTIPLE);
}

