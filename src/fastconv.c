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

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include "opendiapason/fastconv.h"
#include "cop/vec.h"
#include "cop/aalloc.h"

#ifndef V4F_EXISTS
#error this implementation requires a vector type at the moment
#endif

struct fastconv_pass {
	unsigned                    lfft;
	unsigned                    radix;
	float                      *twiddle;
	void                       *twidbase;

	/* The best next pass to use (this pass will have:
	 *      next->lfft = this->lfft / this->radix
	 */
	const struct fastconv_pass *next_compat;

	/* If these are both null, this is the upload pass. Otherwise, these are
	 * both non-null. */
	void (*dit)(float *work_buf, unsigned nfft, unsigned lfft, const float *twid);
	void (*dif)(float *work_buf, unsigned nfft, unsigned lfft, const float *twid);
	void (*difreord)(const float *in_buf, float *out_buf, unsigned nfft, unsigned lfft);
	void (*ditreord)(const float *in_buf, float *out_buf, unsigned nfft, unsigned lfft);

	/* Position in list of all passes of this type (outer or inner pass). */
	struct fastconv_pass       *next;
};

#define FASTCONV_BUFFER_ALIGN      (64)
#define FASTCONV_VECTOR_LEN        (4)
#define FASTCONV_REAL_LEN_MULTIPLE (32)
#define FASTCONV_MAX_PASSES        (24)

static void fastconv_v4_upload(float *vec_output, const float *input, const float *coefs, unsigned fft_len)
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
		v4f or1   = r1 * twr1 + i1 * twi1;
		v4f oi1   = r1 * twi1 - i1 * twr1;
		v4f or2   = r2 * twr2 + i2 * twi2;
		v4f oi2   = r2 * twi2 - i2 * twr2;
		v4f twr3  = v4f_ld(coefs + 16);
		v4f twi3  = v4f_ld(coefs + 20);
		v4f twr4  = v4f_ld(coefs + 24);
		v4f twi4  = v4f_ld(coefs + 28);
		v4f or3   = r3 * twr3 + i3 * twi3;
		v4f oi3   = r3 * twi3 - i3 * twr3;
		v4f or4   = r4 * twr4 + i4 * twi4;
		v4f oi4   = r4 * twi4 - i4 * twr4;
		v4f t0ra  = or1 + or3;
		v4f t0rs  = or1 - or3;
		v4f t1ra  = or2 + or4;
		v4f t1rs  = or2 - or4;
		v4f t1is  = oi2 - oi4;
		v4f t1ia  = oi2 + oi4;
		v4f t0is  = oi1 - oi3;
		v4f t0ia  = oi1 + oi3;
		v4f mor0  = t0ra + t1ra;
		v4f mor2  = t0ra - t1ra;
		v4f mor1  = t0rs + t1is;
		v4f mor3  = t0rs - t1is;
		v4f moi1  = t0is - t1rs;
		v4f moi3  = t0is + t1rs;
		v4f moi0  = t0ia + t1ia;
		v4f moi2  = t0ia - t1ia;
		v4f ptwr1 = v4f_ld(coefs + 32);
		v4f ptwi1 = v4f_ld(coefs + 36);
		v4f ptwr2 = v4f_ld(coefs + 40);
		v4f ptwi2 = v4f_ld(coefs + 44);
		v4f ptwr3 = v4f_ld(coefs + 48);
		v4f ptwi3 = v4f_ld(coefs + 52);
		v4f tor1  = mor1 * ptwr1 - moi1 * ptwi1;
		v4f toi1  = mor1 * ptwi1 + moi1 * ptwr1;
		v4f tor2  = mor2 * ptwr2 - moi2 * ptwi2;
		v4f toi2  = mor2 * ptwi2 + moi2 * ptwr2;
		v4f tor3  = mor3 * ptwr3 - moi3 * ptwi3;
		v4f toi3  = mor3 * ptwi3 + moi3 * ptwr3;
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

static void fastconv_v4_download(float *output, const float *vec_input, const float *coefs, unsigned fft_len)
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
			v4f tor1  = r1 * ptwr1 - i1 * ptwi1;
			v4f toi1  = r1 * ptwi1 + i1 * ptwr1;
			v4f tor2  = r2 * ptwr2 - i2 * ptwi2;
			v4f toi2  = r2 * ptwi2 + i2 * ptwr2;
			v4f tor3  = r3 * ptwr3 - i3 * ptwi3;
			v4f toi3  = r3 * ptwi3 + i3 * ptwr3;
			v4f t0ra  = r0   + tor2;
			v4f t0rs  = r0   - tor2;
			v4f t1ra  = tor1 + tor3;
			v4f t1rs  = tor1 - tor3;
			v4f t1is  = toi1 - toi3;
			v4f t1ia  = toi1 + toi3;
			v4f t0is  = i0   - toi2;
			v4f t0ia  = i0   + toi2;
			v4f mor0  = t0ra + t1ra;
			v4f mor2  = t0ra - t1ra;
			v4f mor1  = t0rs + t1is;
			v4f mor3  = t0rs - t1is;
			v4f moi1  = t0is - t1rs;
			v4f moi3  = t0is + t1rs;
			v4f moi0  = t0ia + t1ia;
			v4f moi2  = t0ia - t1ia;
			v4f twr1  = v4f_ld(coefs + 0);
			v4f twi1  = v4f_ld(coefs + 4);
			v4f twr2  = v4f_ld(coefs + 8);
			v4f twi2  = v4f_ld(coefs + 12);
			v4f or0   = mor0 * twr1 - moi0 * twi1;
			v4f oi0   = mor0 * twi1 + moi0 * twr1;
			v4f or1   = mor1 * twr2 - moi1 * twi2;
			v4f oi1   = mor1 * twi2 + moi1 * twr2;
			v4f twr3  = v4f_ld(coefs + 16);
			v4f twi3  = v4f_ld(coefs + 20);
			v4f twr4  = v4f_ld(coefs + 24);
			v4f twi4  = v4f_ld(coefs + 28);
			v4f or2   = mor2 * twr3 - moi2 * twi3;
			v4f oi2   = mor2 * twi3 + moi2 * twr3;
			v4f or3   = mor3 * twr4 - moi3 * twi4;
			v4f oi3   = mor3 * twi4 + moi3 * twr4;
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
			v4f nre  = v4f_ld(work_buf + 0);
			v4f nim  = v4f_ld(work_buf + 4);
			v4f fre  = v4f_ld(work_buf + rinc + 0);
			v4f fim  = v4f_ld(work_buf + rinc + 4);
			v4f tre  = v4f_broadcast(twid[2*j+0]);
			v4f tim  = v4f_broadcast(twid[2*j+1]);
			v4f onre = v4f_add(nre, fre);
			v4f onim = v4f_add(nim, fim);
			v4f ptre = v4f_sub(nre, fre);
			v4f ptim = v4f_sub(nim, fim);
			v4f ofre = ptre * tre - ptim * tim;
			v4f ofim = ptre * tim + ptim * tre;
			v4f_st(work_buf + 0, onre);
			v4f_st(work_buf + 4, onim);
			v4f_st(work_buf + rinc + 0, ofre);
			v4f_st(work_buf + rinc + 4, ofim);
		}
		work_buf += rinc;
	} while (--nfft);
}

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
			v4f fre  = ptre * tre - ptim * tim;
			v4f fim  = ptre * tim + ptim * tre;
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
		v4f dr = v4f_ld(work_buf   + 8*i+0);
		v4f di = v4f_ld(work_buf   + 8*i+4);
		v4f cr = v4f_ld(kernel_buf + 8*i+0);
		v4f ci = v4f_ld(kernel_buf + 8*i+4);
		v4f_st(work_buf + 8*i + 0,   dr * cr - di * ci);
		v4f_st(work_buf + 8*i + 4, -(dr * ci + di * cr));
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
	while (first_pass->dit != NULL && first_pass->dif != NULL) {
		nfft /= first_pass->radix;
		first_pass->difreord(work_buf, output_buf, nfft, first_pass->lfft);
		first_pass = pass_stack[--si];
		float *tmp = output_buf;
		output_buf = work_buf;
		work_buf = tmp;
	}
	for (i = 0; i < first_pass->lfft / 8; i++) {
		v4f tor1, toi1, tor2, toi2; 
		v4f re1 = v4f_ld(work_buf + i*8 + 0);
		v4f im1 = v4f_ld(work_buf + i*8 + 4);
		v4f re2 = v4f_ld(work_buf + first_pass->lfft*2 - i*8 - 8);
		v4f im2 = v4f_ld(work_buf + first_pass->lfft*2 - i*8 - 4);
		re2     = v4f_reverse(re2);
		im2     = v4f_reverse(v4f_neg(im2));
		V4F_INTERLEAVE(tor1, tor2, re1, re2);
		V4F_INTERLEAVE(toi1, toi2, im1, im2);
		V4F_INTERLEAVE_STORE(output_buf + i*16,     tor1, toi1);
		V4F_INTERLEAVE_STORE(output_buf + i*16 + 8, tor2, toi2);
	}
	/* We have no idea which buffer is which because of the reindexing. Copy
	 * the output buffer into the work buffer. */
	memcpy(work_buf, output_buf, sizeof(float) * first_pass->lfft * 2);
	assert(nfft == 1);
	assert(si == 0);
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
		V4F_LD2(re1, re2, input_buf + i*16);
		V4F_LD2(im1, im2, input_buf + i*16 + 8);
		V4F_DEINTERLEAVE(tor1, toi1, re1, re2);
		V4F_DEINTERLEAVE(tor2, toi2, im1, im2);
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
		pass->lfft     = length;
		pass->radix    = 2;
		pass->dif      = fc_v4_dif_r2;
		pass->dit      = fc_v4_dit_r2;
		pass->difreord = fc_v4_dif_r2_reord;
		pass->ditreord = fc_v4_dit_r2_reord;
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

	pass->lfft        = length;
	pass->radix       = 4;
	pass->dif         = NULL;
	pass->dit         = NULL;
	pass->difreord    = NULL;
	pass->ditreord    = NULL;

	/* Generate twiddle */
	for (i = 0; i < length / 16; i++) {
		pass->twiddle[56*i+0]  = cos((4*i+0) * -0.5 * M_PI / length);
		pass->twiddle[56*i+1]  = cos((4*i+1) * -0.5 * M_PI / length);
		pass->twiddle[56*i+2]  = cos((4*i+2) * -0.5 * M_PI / length);
		pass->twiddle[56*i+3]  = cos((4*i+3) * -0.5 * M_PI / length);
		pass->twiddle[56*i+4]  = sin((4*i+0) * -0.5 * M_PI / length);
		pass->twiddle[56*i+5]  = sin((4*i+1) * -0.5 * M_PI / length);
		pass->twiddle[56*i+6]  = sin((4*i+2) * -0.5 * M_PI / length);
		pass->twiddle[56*i+7]  = sin((4*i+3) * -0.5 * M_PI / length);
		pass->twiddle[56*i+8]  = cos((4*i+0+1*length/4) * -0.5 * M_PI / length);
		pass->twiddle[56*i+9]  = cos((4*i+1+1*length/4) * -0.5 * M_PI / length);
		pass->twiddle[56*i+10] = cos((4*i+2+1*length/4) * -0.5 * M_PI / length);
		pass->twiddle[56*i+11] = cos((4*i+3+1*length/4) * -0.5 * M_PI / length);
		pass->twiddle[56*i+12] = sin((4*i+0+1*length/4) * -0.5 * M_PI / length);
		pass->twiddle[56*i+13] = sin((4*i+1+1*length/4) * -0.5 * M_PI / length);
		pass->twiddle[56*i+14] = sin((4*i+2+1*length/4) * -0.5 * M_PI / length);
		pass->twiddle[56*i+15] = sin((4*i+3+1*length/4) * -0.5 * M_PI / length);
		pass->twiddle[56*i+16] = cos((4*i+0+2*length/4) * -0.5 * M_PI / length);
		pass->twiddle[56*i+17] = cos((4*i+1+2*length/4) * -0.5 * M_PI / length);
		pass->twiddle[56*i+18] = cos((4*i+2+2*length/4) * -0.5 * M_PI / length);
		pass->twiddle[56*i+19] = cos((4*i+3+2*length/4) * -0.5 * M_PI / length);
		pass->twiddle[56*i+20] = sin((4*i+0+2*length/4) * -0.5 * M_PI / length);
		pass->twiddle[56*i+21] = sin((4*i+1+2*length/4) * -0.5 * M_PI / length);
		pass->twiddle[56*i+22] = sin((4*i+2+2*length/4) * -0.5 * M_PI / length);
		pass->twiddle[56*i+23] = sin((4*i+3+2*length/4) * -0.5 * M_PI / length);
		pass->twiddle[56*i+24] = cos((4*i+0+3*length/4) * -0.5 * M_PI / length);
		pass->twiddle[56*i+25] = cos((4*i+1+3*length/4) * -0.5 * M_PI / length);
		pass->twiddle[56*i+26] = cos((4*i+2+3*length/4) * -0.5 * M_PI / length);
		pass->twiddle[56*i+27] = cos((4*i+3+3*length/4) * -0.5 * M_PI / length);
		pass->twiddle[56*i+28] = sin((4*i+0+3*length/4) * -0.5 * M_PI / length);
		pass->twiddle[56*i+29] = sin((4*i+1+3*length/4) * -0.5 * M_PI / length);
		pass->twiddle[56*i+30] = sin((4*i+2+3*length/4) * -0.5 * M_PI / length);
		pass->twiddle[56*i+31] = sin((4*i+3+3*length/4) * -0.5 * M_PI / length);
		pass->twiddle[56*i+32] = cos(-2.0 * (4*i+0) * M_PI * 1.0 / length);
		pass->twiddle[56*i+33] = cos(-2.0 * (4*i+1) * M_PI * 1.0 / length);
		pass->twiddle[56*i+34] = cos(-2.0 * (4*i+2) * M_PI * 1.0 / length);
		pass->twiddle[56*i+35] = cos(-2.0 * (4*i+3) * M_PI * 1.0 / length);
		pass->twiddle[56*i+36] = sin(-2.0 * (4*i+0) * M_PI * 1.0 / length);
		pass->twiddle[56*i+37] = sin(-2.0 * (4*i+1) * M_PI * 1.0 / length);
		pass->twiddle[56*i+38] = sin(-2.0 * (4*i+2) * M_PI * 1.0 / length);
		pass->twiddle[56*i+39] = sin(-2.0 * (4*i+3) * M_PI * 1.0 / length);
		pass->twiddle[56*i+40] = cos(-2.0 * (4*i+0) * M_PI * 2.0 / length);
		pass->twiddle[56*i+41] = cos(-2.0 * (4*i+1) * M_PI * 2.0 / length);
		pass->twiddle[56*i+42] = cos(-2.0 * (4*i+2) * M_PI * 2.0 / length);
		pass->twiddle[56*i+43] = cos(-2.0 * (4*i+3) * M_PI * 2.0 / length);
		pass->twiddle[56*i+44] = sin(-2.0 * (4*i+0) * M_PI * 2.0 / length);
		pass->twiddle[56*i+45] = sin(-2.0 * (4*i+1) * M_PI * 2.0 / length);
		pass->twiddle[56*i+46] = sin(-2.0 * (4*i+2) * M_PI * 2.0 / length);
		pass->twiddle[56*i+47] = sin(-2.0 * (4*i+3) * M_PI * 2.0 / length);
		pass->twiddle[56*i+48] = cos(-2.0 * (4*i+0) * M_PI * 3.0 / length);
		pass->twiddle[56*i+49] = cos(-2.0 * (4*i+1) * M_PI * 3.0 / length);
		pass->twiddle[56*i+50] = cos(-2.0 * (4*i+2) * M_PI * 3.0 / length);
		pass->twiddle[56*i+51] = cos(-2.0 * (4*i+3) * M_PI * 3.0 / length);
		pass->twiddle[56*i+52] = sin(-2.0 * (4*i+0) * M_PI * 3.0 / length);
		pass->twiddle[56*i+53] = sin(-2.0 * (4*i+1) * M_PI * 3.0 / length);
		pass->twiddle[56*i+54] = sin(-2.0 * (4*i+2) * M_PI * 3.0 / length);
		pass->twiddle[56*i+55] = sin(-2.0 * (4*i+3) * M_PI * 3.0 / length);
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

