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

#include <string.h>
#include <math.h>
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include "decode_least16x2.h"
#include "wavldr.h"
#include "wav_dumper.h"

struct wave_cks {
	uint_fast32_t  datasz;
	unsigned char *data;
	uint_fast32_t  smplsz;
	unsigned char *smpl;
	uint_fast32_t  fmtsz;
	unsigned char *fmt;
	uint_fast32_t  cuesz;
	unsigned char *cue;
	uint_fast32_t  adtlsz;
	unsigned char *adtl;
};

#define MAKE_FOURCC(a, b, c, d) \
	(   (uint_fast32_t)(a) \
	|   (((uint_fast32_t)(b)) << 8) \
	|   (((uint_fast32_t)(c)) << 16) \
	|   (((uint_fast32_t)(d)) << 24) \
	)

static const char *load_wave_cks(struct wave_cks *cks, unsigned char *buf, size_t bufsz)
{
	{
		uint_fast32_t sz;
		if (   bufsz < 12
		   ||  cop_ld_ule32(buf) != MAKE_FOURCC('R', 'I', 'F', 'F')
		   ||  ((sz = cop_ld_ule32(buf + 4)) < 4)
		   ||  cop_ld_ule32(buf + 8) != MAKE_FOURCC('W', 'A', 'V', 'E')
		   )
			return "not a wave file";

		buf   += 12; /* Seek past header data */
		bufsz -= 12; /* Remove header data from buffer size */
		sz    -= 4;  /* Remove wave form id from chunk size */

		/* If the riff chunk size is less than the file size, truncate the
		 * file size. */
		if (sz < bufsz)
			bufsz = sz;
	}

	memset(cks, 0, sizeof(*cks));

	while (bufsz > 8) {
		uint_fast32_t cksz;

		bufsz  -= 8;
		cksz = cop_ld_ule32(buf + 4);
		if (cksz > bufsz)
			cksz = bufsz;

		switch (cop_ld_ule32(buf)) {
			case MAKE_FOURCC('d', 'a', 't', 'a'):
				cks->data   = buf + 8;
				cks->datasz = cksz;
				break;
			case MAKE_FOURCC('c', 'u', 'e', ' '):
				cks->cue    = buf + 8;
				cks->cuesz  = cksz;
				break;
			case MAKE_FOURCC('s', 'm', 'p', 'l'):
				cks->smpl   = buf + 8;
				cks->smplsz = cksz;
				break;
			case MAKE_FOURCC('f', 'm', 't', ' '):
				cks->fmt    = buf + 8;
				cks->fmtsz  = cksz;
				break;
			case MAKE_FOURCC('L', 'I', 'S', 'T'):
				if (cksz >= 4 && cop_ld_ule32(buf + 8) == MAKE_FOURCC('a', 'd', 't', 'l')) {
					cks->adtl    = buf + 12;
					cks->adtlsz  = cksz - 4;
				}
				break;
		}

		cksz += (cksz & 1); /* pad byte */
		if (cksz > bufsz)
			break;

		buf    += 8 + cksz;
		bufsz  -= cksz;
	}

	return NULL;
}

#include "bufcvt.h"

static const char *setup_as(struct as_data *as, unsigned char *smpl, size_t smpl_sz, uint_fast32_t rate, uint_fast32_t total_length, unsigned load_format)
{
	uint_fast32_t spec_data_sz;
	double note;
	unsigned i;

	as->data        = NULL;
	as->next        = NULL;
	as->load_format = load_format;
	as->period      = 0.0f;
	as->nloop       = 0;
	as->length      = 0;

	if (smpl == NULL)
		return NULL;

	if (smpl_sz < 36)
		return "invalid sampler chunk";

	/* 0: mfg
	 * 4: product
	 * 8: period
	 * 12: unity note
	 * 16: pitch frac
	 * 20: smpte fmt
	 * 24: smpte offset
	 * 28: num loops
	 * 32: specific data sz */

	/* loop:
	 *   0: ident
	 *   4: type
	 *   8: start
	 *   12: end
	 *   16: frac
	 *   20: play count */
	spec_data_sz  = cop_ld_ule32(smpl + 32);
	note          = (cop_ld_ule32(smpl + 16) / (65536.0 * 65536.0)) + cop_ld_ule32(smpl + 12);
	as->nloop     = cop_ld_ule32(smpl + 28);
	as->period    = rate / (440.0f * powf(2.0f, (note - 69.0f) / 12.0f));

	if (as->nloop > MAX_LOOP)
		return "too many loops";

	if (smpl_sz < 36 + (uint_fast64_t)as->nloop * 24 + spec_data_sz)
		return "invalid sampler chunk";

	if (as->nloop == 0)
		return NULL;

	for (i = 0; i < as->nloop; i++) {
		as->loops[2*i+0] = cop_ld_ule32(smpl + 36 + i * 24 + 8);
		as->loops[2*i+1] = cop_ld_ule32(smpl + 36 + i * 24 + 12);
		if (as->loops[2*i+0] > as->loops[2*i+1] || as->loops[2*i+1] >= total_length) {
			as->length = 0;
			return "invalid sampler loop";
		}
		if (as->loops[2*i+1] >= as->length) {
			as->length             = as->loops[2*i+1] + 1;
			as->atk_end_loop_start = as->loops[2*i+0];
		}
	}

	return NULL;
}

static const char *setup_rel(struct rel_data *rel, const unsigned char *cue, size_t cue_sz, const unsigned char *adtl, size_t adtl_sz, uint_fast32_t as_len, size_t block_align, uint_fast32_t total_length, unsigned load_format)
{
	uint_fast32_t last_rel_pos = 0;

	rel->data = NULL;
	rel->next = NULL;
	rel->load_format = load_format;
	rel->position = 0;
	rel->length = 0;

	if (cue != NULL) {
		uint_fast32_t nb_cue;
		uint_fast32_t i;

		if (cue_sz < 4)
			return "invalid cue chunk";
		nb_cue = cop_ld_ule32(cue + 0);
		if (cue_sz < nb_cue * 24 + 4)
			return "invalid cue chunk";

		/* 0             nb_cuepoints
		 * 4 + 24*i + 0  dwName
		 * 4 + 24*i + 4  dwPosition
		 * 4 + 24*i + 8  fccChunk
		 * 4 + 24*i + 12 dwChunkStart
		 * 4 + 24*i + 16 dwBlockStart
		 * 4 + 24*i + 20 dwSampleOffset
		 */

		for (i = 0; i < nb_cue; i++) {
			/* We ignore the cue ID because too much software incorrectly
			 * associates it with a loop. We instead look for a cue point that
			 * is past the end of the last loop. */
			uint_fast32_t cueid         = cop_ld_ule32(cue + 4 + i * 24 + 0);
			uint_fast32_t block_start   = cop_ld_ule32(cue + 4 + i * 24 + 16);
			uint_fast32_t sample_offset = cop_ld_ule32(cue + 4 + i * 24 + 20);
			uint_fast32_t relpos        = sample_offset + block_start / block_align;
			size_t        adpos;
			int           isloop = 0;

			if (cop_ld_ule32(cue + 4 + i * 24 + 12) != 0)
				continue;
			if (cop_ld_ule32(cue + 4 + i * 24 + 8) != 0x61746164ul)
				continue;

			for (adpos = 0; adpos + 12 <= adtl_sz; ) {
				uint_fast32_t fccsz = cop_ld_ule32(adtl + adpos + 4);
				if (fccsz >= 20) {
					uint_fast32_t dur = cop_ld_ule32(adtl + adpos + 12);
					if (cop_ld_ule32(adtl + adpos) == MAKE_FOURCC('l', 't', 'x', 't') && cop_ld_ule32(adtl + adpos + 8) == cueid && dur > 0) {
						isloop = 1;
						break;
					}
				}
				adpos += fccsz + 8;
			}

			if (isloop)
				continue;

			if (relpos > last_rel_pos)
				last_rel_pos = relpos;
		}
	}

	if (last_rel_pos == 0 && as_len)
		return NULL;

	if (as_len && last_rel_pos && last_rel_pos < as_len) {
		last_rel_pos = as_len;
		printf("shortened release as it was positioned inside a loop\n");
	}

	if (last_rel_pos >= total_length)
		return "invalid cue chunk";

	rel->position = last_rel_pos;
	rel->length   = total_length - last_rel_pos;

	return NULL;
}

const char *load_smpl_mem(struct memory_wave *mw, unsigned char *buf, size_t fsz, unsigned load_format)
{
	struct wave_cks cks;
	unsigned        i, ch;
	size_t          block_align;
	uint_fast32_t   total_length;
	float          *buffers;

	const char *err = load_wave_cks(&cks, buf, fsz);
	if (err != NULL)
		return err;

	if (cks.data == NULL || cks.fmt == NULL)
		return "wave file missing format or data chunk";

	{
		/* 0 fmt_tag
		 * 2 channels
		 * 4 sample rate
		 * 8 avg bytes per sec
		 * 12 block align
		 * 14 bits per sample
		 * 16 cbsize */
		if (cks.fmtsz < 16)
			return "invalid format chunk";

		if (cop_ld_ule16(cks.fmt + 0) != 1)
			return "can only read PCM wave files";

		mw->native_bits = cop_ld_ule16(cks.fmt + 14);
		mw->channels    = cop_ld_ule16(cks.fmt + 2);
		mw->rate        = cop_ld_ule32(cks.fmt + 4);

		if (mw->native_bits != 16 && mw->native_bits != 24)
			return "can only load 16 or 24 bit PCM wave files";

		block_align  = mw->channels * ((mw->native_bits + 7) / 8);
		total_length = cks.datasz / block_align;
	}

	err = setup_as(&mw->as, cks.smpl, cks.smplsz, mw->rate, total_length, load_format);
	if (err != NULL)
		return err;

	err = setup_rel(&mw->rel, cks.cue, cks.cuesz, cks.adtl, cks.adtlsz, mw->as.length, block_align, total_length, load_format);
	if (err != NULL)
		return err;

	mw->rel.period             = mw->as.period;

	mw->chan_stride =
		(mw->as.length ? VLF_PAD_LENGTH(mw->as.length) : 0) +
		(mw->rel.length ? VLF_PAD_LENGTH(mw->rel.length) : 0);
	mw->as.chan_stride = mw->chan_stride;
	mw->rel.chan_stride = mw->chan_stride;

	buffers         = malloc(sizeof(float *) * mw->channels * mw->chan_stride);
	mw->buffers     = buffers;

	if (mw->as.length) {
		bufcvt_deinterleave
			(buffers
			,mw->chan_stride
			,cks.data
			,mw->as.length
			,mw->channels
			,(mw->native_bits == 24) ? BUFCVT_FMT_SLE24 : BUFCVT_FMT_SLE16
			,BUFCVT_FMT_FLOAT
			);
		for (ch = 0; ch < mw->channels; ch++) {
			for (i = mw->as.length; i < VLF_PAD_LENGTH(mw->as.length); i++) {
				buffers[ch*mw->chan_stride+i] = 0.0f;
			}
		}
		mw->as.data  = buffers;
		buffers      += VLF_PAD_LENGTH(mw->as.length);
	} else {
		mw->as.data = NULL;
	}

	if (mw->rel.length) {
		bufcvt_deinterleave
			(buffers
			,mw->chan_stride
			,cks.data + mw->rel.position*block_align
			,mw->rel.length
			,mw->channels
			,(mw->native_bits == 24) ? BUFCVT_FMT_SLE24 : BUFCVT_FMT_SLE16
			,BUFCVT_FMT_FLOAT
			);
		for (ch = 0; ch < mw->channels; ch++) {
			for (i = mw->rel.length; i < VLF_PAD_LENGTH(mw->rel.length); i++) {
				buffers[ch*mw->chan_stride+i] = 0.0f;
			}
		}
		mw->rel.data  = buffers;
		buffers      += VLF_PAD_LENGTH(mw->rel.length);
	} else {
		mw->rel.data = NULL;
	}

	return NULL;
}

const char *mw_load_from_file(struct memory_wave *mw, const char *fname, unsigned load_format)
{
	const char *err = NULL;
	FILE *f = fopen(fname, "rb");
	if (f != NULL) {
		if (fseek(f, 0, SEEK_END) == 0) {
			long fsz = ftell(f);
			if (fsz > 0) {
				if (fseek(f, 0, SEEK_SET) == 0) {
					unsigned char *fbuf = malloc(fsz);
					if (fbuf != NULL) {
						if (fread(fbuf, 1, fsz, f) == fsz) {
							err = load_smpl_mem(mw, fbuf, fsz, load_format);
						} else {
							err = "failed to read file";
						}
						free(fbuf);
					} else {
						err = "out of memory";
					}
				} else {
					err = "failed to see to start of file";
				}
			} else {
				err = "stream empty or ftell failed";
			}
		} else {
			err = "failed to seek to eof";
		}
		fclose(f);
	} else {
		err = "failed to open file";
	}
	return err;
}

static float quantize_boost_interleave
	(void           *obuf
	,float          *in_bufs
	,size_t          chan_stride
	,unsigned        channels
	,unsigned        in_length
	,unsigned        out_length
	,uint_fast32_t  *dither_seed
	,unsigned        fmtbits
	)
{
	float maxv = 0.0f;
	uint_fast32_t rseed = *dither_seed;
	float boost;

	if (channels == 2) {
		unsigned j;
		float minv = 0.0f;

		for (j = 0; j < in_length; j++) {
			float s1 = in_bufs[j];
			float s2 = in_bufs[chan_stride+j];
			maxv = (s1 > maxv) ? s1 : maxv;
			minv = (s1 < minv) ? s1 : minv;
			maxv = (s2 > maxv) ? s2 : maxv;
			minv = (s2 < minv) ? s2 : minv;
		}

		maxv += 1.0f;
		maxv  = (maxv > -minv) ? maxv : -minv;

		if (fmtbits == 12 && channels == 2) {
			unsigned char *out_buf = obuf;
			maxv  *= 1.0f / 2048.0f;
			boost  = 1.0f / maxv;
			for (j = 0; j < in_length; j++) {
				float s1         = in_bufs[j] * boost;
				float s2         = in_bufs[chan_stride+j] * boost;
				int_fast32_t r1  = (rseed = update_rnd(rseed)) & 0x3FFFFFFFu;
				int_fast32_t r2  = (rseed = update_rnd(rseed)) & 0x3FFFFFFFu;
				int_fast32_t r3  = (rseed = update_rnd(rseed)) & 0x3FFFFFFFu;
				int_fast32_t r4  = (rseed = update_rnd(rseed)) & 0x3FFFFFFFu;
				float d1         = (r1 + r2) * (1.0f / 0x7FFFFFFF);
				float d2         = (r3 + r4) * (1.0f / 0x7FFFFFFF);
				int_fast32_t v1  = (int_fast32_t)(d1 + s1 + 2048.0f) - 2048;
				int_fast32_t v2  = (int_fast32_t)(d2 + s2 + 2048.0f) - 2048;
				v1 = (v1 > (int)0x7FF) ? 0x7FF : ((v1 < -(int)0x800) ? -(int)0x800 : v1);
				v2 = (v2 > (int)0x7FF) ? 0x7FF : ((v2 < -(int)0x800) ? -(int)0x800 : v2);
				encode2x12(out_buf + 3*j, v1, v2);
			}
			for (; j < out_length; j++) {
				encode2x12(out_buf + 3*j, 0, 0);
			}
		} else if (fmtbits == 16 && channels == 2) {
			int_least16_t *out_buf = obuf;
			maxv  *= 1.0f / 32768.0f;
			boost  = 1.0f / maxv;
			for (j = 0; j < in_length; j++) {
				float s1         = in_bufs[j] * boost;
				float s2         = in_bufs[chan_stride+j] * boost;
				int_fast32_t r1  = (rseed = update_rnd(rseed)) & 0x3FFFFFFFu;
				int_fast32_t r2  = (rseed = update_rnd(rseed)) & 0x3FFFFFFFu;
				int_fast32_t r3  = (rseed = update_rnd(rseed)) & 0x3FFFFFFFu;
				int_fast32_t r4  = (rseed = update_rnd(rseed)) & 0x3FFFFFFFu;
				float d1         = (r1 + r2) * (1.0f / 0x7FFFFFFF);
				float d2         = (r3 + r4) * (1.0f / 0x7FFFFFFF);
				int_fast32_t v1  = (int_fast32_t)(d1 + s1 + 32768.0f) - 32768;
				int_fast32_t v2  = (int_fast32_t)(d2 + s2 + 32768.0f) - 32768;
				v1 = (v1 > (int)0x7FFF) ? 0x7FFF : ((v1 < -(int)0x8000) ? -(int)0x8000 : v1);
				v2 = (v2 > (int)0x7FFF) ? 0x7FFF : ((v2 < -(int)0x8000) ? -(int)0x8000 : v2);
				out_buf[2*j+0] = (int_least16_t)v1;
				out_buf[2*j+1] = (int_least16_t)v2;
			}
			for (; j < out_length; j++) {
				out_buf[2*j+0] = 0;
				out_buf[2*j+1] = 0;
			}
		} else {
			abort();
		}

	} else {
		abort();
	}

	*dither_seed = rseed;
	return maxv;
}

/* This takes a memory wave and overwrites all of its channels with filtered
 * versions. This is designed to compensate for the high-frequency roll-off
 * which is introduced by the interpolation filters.
 *
 * For audio which contains loops, the filtered audio is phase-aligned with
 * the original. The audio is assumed to begin with infinite zeroes and is
 * assumed to repeatedly execute the last loop. A very short sine window is
 * applied at the start of the attack just in-case.
 *
 * The release segment is not-phase aligned - a small number of samples are
 * dropped from the start of the release. There is no sensible way to guess
 * what comes before the release starts, so we assume that it is all zeroes.
 * This introduces ringing, but we chop off the most noticable part of the
 * ringing. With the current 192 sample pre-filter, we chop off 24 samples
 * which is about half a millisecond at 44.1 kHz. The release is assumed to be
 * followed by infinitely many zeroes.
 *
 * TODO: For one-shot samples, we should filter and phase-align the entire
 * audio data and introduce a very short ramp-in and ramp-out at the
 * extremities. */
static
void
apply_prefilter
	(struct as_data             *as
	,struct rel_data            *rel
	,unsigned                    channels
	,uint_fast32_t               rate
	,struct aalloc              *allocator
	,const struct odfilter      *prefilter
	,const char                 *debug_prefix
	)
{
	float *scratch;
	float *inbuf;
	float *outbuf;
	unsigned ch;
	unsigned idx;
	aalloc_push(allocator);
	inbuf   = aalloc_align_alloc(allocator, sizeof(float) * prefilter->conv_len, 64);
	outbuf  = aalloc_align_alloc(allocator, sizeof(float) * prefilter->conv_len, 64);
	scratch = aalloc_align_alloc(allocator, sizeof(float) * prefilter->conv_len, 64);

	/* Convolve the attack/sustain segments. */
	idx = 0;
	while (as != NULL) {
		for (ch = 0; ch < channels; ch++) {
			odfilter_run_inplace
				(/* input */           as->data + ch*as->chan_stride
				,/* sustain start */   as->atk_end_loop_start
				,/* total length */    as->length
				,/* pre-read */        SMPL_INVERSE_FILTER_LEN / 2 + 1
				,/* is looped */       1
				,inbuf
				,outbuf
				,scratch
				,prefilter
				);
		}

#if OPENDIAPASON_VERBOSE_DEBUG
		if (debug_prefix != NULL) {
			char              namebuf[1024];
			struct wav_dumper dump;
			sprintf(namebuf, "%s_prefilter_atk%02d.wav", debug_prefix, idx);
			if (wav_dumper_begin(&dump, namebuf, channels, 24, rate) == 0) {
				(void)wav_dumper_write_from_floats(&dump, as->data, as->length, 1, as->chan_stride);
				wav_dumper_end(&dump);
			}
		}
#endif

		as = as->next;
		idx++;
	}

	/* Convolve the release segments. Don't include all of the pre-ringing
	 * at the start. i.e. we are shaving some samples away from the start
	 * of the release. */
	idx = 0;
	while (rel != NULL) {
		for (ch = 0; ch < channels; ch++) {
			odfilter_run_inplace
				(/* input */           rel->data + ch*rel->chan_stride
				,/* sustain start */   0
				,/* total length */    rel->length
				,/* pre-read */        SMPL_INVERSE_FILTER_LEN / 2 + SMPL_INVERSE_FILTER_LEN / 8
				,/* is looped */       0
				,inbuf
				,outbuf
				,scratch
				,prefilter
			);
		}

#if OPENDIAPASON_VERBOSE_DEBUG
		if (debug_prefix != NULL) {
			char              namebuf[1024];
			struct wav_dumper dump;
			sprintf(namebuf, "%s_prefilter_rel%02d.wav", debug_prefix, idx);
			if (wav_dumper_begin(&dump, namebuf, 2, 24, rate) == 0) {
				(void)wav_dumper_write_from_floats(&dump, rel->data, rel->length, 1, rel->chan_stride);
				wav_dumper_end(&dump);
			}
		}
#endif

		rel = rel->next;
		idx++;
	}
	aalloc_pop(allocator);
}

const char *
load_smpl_lists
	(struct pipe_v1             *pipe
	,struct as_data             *as_bits
	,struct rel_data            *rel_bits
	,unsigned                    channels
	,uint_fast32_t               norm_rate
	,struct aalloc              *allocator
	,struct fftset              *fftset
	,const struct odfilter      *prefilter
	,const char                 *file_ref
	)
{
	/* What this function needs to do (but doesn't at the moment):
	 *
	 * 1) prefilter all the releases. (these could immediately be quantised
	 *    into the releases for the sample at this point...)
	 * 2) for each AS segment:
	 *    a) get a buffer of N samples (where N is about the period of the
	 *       sample) using the interpolation filter to make the rate align
	 *       with the rate of the AS sample. This allows releases to be tuned
	 *       individually from attacks. Because the releases were pre-filtered
	 *       the frequency response of the interpolated data is extremely
	 *       close to the currently unfiltered response of the AS sample.
	 *    b) Get the envelope of the unfiltered AS sample, sum the power of
	 *       the small release block, and find the correlation of the releases
	 *       to the AS sample. This allows us to compute the MSE buffers which
	 *       are required to align the release properly.
	 *    c) Compute release alignment tables using the data collected in "b".
	 *    d) Prefilter the AS data and quantise it into the sample. */

	/* release has 32 samples of extra zero slop for a fake loop */
	const unsigned release_slop   = 32;
	uint_fast32_t rseed = rand();
	unsigned i;
	unsigned nb_releases;

	{
		struct as_data *tmp;
		struct rel_data *tmp2;

		for (i = 0, tmp = as_bits; tmp != NULL; i++, tmp = tmp->next);
		if (i != 1) {
			fprintf(stderr, "sample contained %d attack/sustain blocks. the max is 1.\n", i);
			abort();
		}

		for (nb_releases = 0, tmp2 = rel_bits; tmp2 != NULL; nb_releases++, tmp2 = tmp2->next);
		if (nb_releases != 1) {
			fprintf(stderr, "sample contained %d release blocks. the max is 1.\n", nb_releases);
//			abort();
		}
	}

	/* Prefilter and adjust audio. */
	apply_prefilter
		(as_bits
		,rel_bits
		,channels
		,norm_rate
		,allocator
		,prefilter
		,file_ref
		);

	pipe->frequency   = norm_rate / as_bits->period;
	pipe->sample_rate = norm_rate;
	pipe->attack.nloop = as_bits->nloop;
	for (i = 0; i < as_bits->nloop; i++) {
		pipe->attack.starts[i].start_smpl      = as_bits->loops[2*i+0];
		pipe->attack.starts[i].first_valid_end = 0;
		pipe->attack.ends[i].end_smpl          = as_bits->loops[2*i+1];
		pipe->attack.ends[i].start_idx         = i;

		/* These conditions should be checked when the wave is being loaded. */
		assert(pipe->attack.ends[i].end_smpl < as_bits->length);
		assert(pipe->attack.ends[i].end_smpl >= pipe->attack.starts[i].start_smpl);
	}

	/* Filthy bubble sort */
	for (i = 0; i < as_bits->nloop; i++) {
		unsigned j;
		for (j = i + 1; j < as_bits->nloop; j++) {
			if (pipe->attack.ends[j].end_smpl < pipe->attack.ends[i].end_smpl) {
				struct dec_loop_end tmp = pipe->attack.ends[j];
				pipe->attack.ends[j] = pipe->attack.ends[i];
				pipe->attack.ends[i] = tmp;
			}
		}
	}

	/* Compute first valid end indices */
	for (i = 0; i < as_bits->nloop; i++) {
		unsigned loop_start = pipe->attack.starts[i].start_smpl;
		while (pipe->attack.ends[pipe->attack.starts[i].first_valid_end].end_smpl <= loop_start)
			pipe->attack.starts[i].first_valid_end++;
	}

	assert(pipe->attack.ends[as_bits->nloop-1].end_smpl+1 == as_bits->length);
	assert(pipe->attack.starts[pipe->attack.ends[as_bits->nloop-1].start_idx].start_smpl == as_bits->atk_end_loop_start);

	pipe->release.nloop                     = 1;
	pipe->release.starts[0].start_smpl      = rel_bits->length;
	pipe->release.starts[0].first_valid_end = 0;
	pipe->release.ends[0].end_smpl          = rel_bits->length + release_slop - 1;
	pipe->release.ends[0].start_idx         = 0;

	if (rel_bits->load_format == 12 && channels == 2) {
		void *buf = aalloc_alloc(allocator, sizeof(unsigned char) * (rel_bits->length + release_slop + 1) * 3);
		pipe->release.gain =
			quantize_boost_interleave
				(buf
				,rel_bits->data
				,rel_bits->chan_stride
				,2
				,rel_bits->length
				,rel_bits->length + release_slop + 1
				,&rseed
				,12
				);
		pipe->release.data = buf;
		pipe->release.instantiate = u12c2_instantiate;

		buf = aalloc_alloc(allocator, sizeof(unsigned char) * (as_bits->length + 1) * 3);
		pipe->attack.gain =
			quantize_boost_interleave
				(buf
				,as_bits->data
				,as_bits->chan_stride
				,2
				,as_bits->length
				,as_bits->length + 1
				,&rseed
				,12
				);
		pipe->attack.data = buf;
		pipe->attack.instantiate = u12c2_instantiate;
	} else if (rel_bits->load_format == 16 && channels == 2) {
		void *buf = aalloc_alloc(allocator, sizeof(int_least16_t) * (rel_bits->length + release_slop + 1) * 2);
		pipe->release.gain =
			quantize_boost_interleave
				(buf
				,rel_bits->data
				,rel_bits->chan_stride
				,2
				,rel_bits->length
				,rel_bits->length + release_slop + 1
				,&rseed
				,16
				);
		pipe->release.data = buf;
		pipe->release.instantiate = u16c2_instantiate;

		buf = aalloc_alloc(allocator, sizeof(int_least16_t) * (as_bits->length + 1) * 2);
		pipe->attack.gain =
			quantize_boost_interleave
				(buf
				,as_bits->data
				,as_bits->chan_stride
				,2
				,as_bits->length
				,as_bits->length + 1
				,&rseed
				,16
				);
		pipe->attack.data = buf;
		pipe->attack.instantiate = u16c2_instantiate;
	} else {
		abort();
	}

#if 1
	{
		float *envelope_buf;
		float *mse_buf;
		unsigned env_width;
		unsigned cor_fft_sz;
		const struct fftset_fft *fft;
		float *scratch_buf;
		float *scratch_buf2;
		float *scratch_buf3;
		float *kern_buf;
		float relpowers[16];
		float *powptr;
		unsigned ch;
		struct odfilter filt;
		size_t buf_stride = VLF_PAD_LENGTH(as_bits->length);
		struct rel_data *r;

		aalloc_push(allocator);

		env_width    = (unsigned)(as_bits->period * 2.0f + 0.5f);
		cor_fft_sz   = fftset_recommend_conv_length(env_width, 512) * 2;
		fft          = fftset_create_fft(fftset, FFTSET_MODULATION_FREQ_OFFSET_REAL, cor_fft_sz / 2);
		envelope_buf = aalloc_align_alloc(allocator, sizeof(float) * (buf_stride * (1 + nb_releases)), 64);
		kern_buf     = aalloc_align_alloc(allocator, sizeof(float) * cor_fft_sz, 64);
		scratch_buf  = aalloc_align_alloc(allocator, sizeof(float) * cor_fft_sz, 64);
		scratch_buf2 = aalloc_align_alloc(allocator, sizeof(float) * cor_fft_sz, 64);
		scratch_buf3 = aalloc_align_alloc(allocator, sizeof(float) * cor_fft_sz, 64);
		mse_buf      = envelope_buf + buf_stride;

		if (channels == 2) {
			for (i = 0; i < as_bits->length; i++) {
				float fl = as_bits->data[i];
				float fr = as_bits->data[i+as_bits->chan_stride];
				envelope_buf[i] = fl * fl + fr * fr;
			}
		} else {
			assert(channels == 1);
			for (i = 0; i < as_bits->length; i++) {
				float fm = as_bits->data[i];
				envelope_buf[i] = fm * fm;
			}
		}

		/* Build the evelope kernel. */
		for (i = 0; i < env_width;  i++) scratch_buf[i] = 2.0f / (cor_fft_sz * env_width);
		for (     ; i < cor_fft_sz; i++) scratch_buf[i] = 0.0f;
		fftset_fft_conv_get_kernel
			(fft
			,kern_buf
			,scratch_buf
			);

		filt.kernel   = kern_buf;
		filt.conv     = fft;
		filt.conv_len = cor_fft_sz;
		filt.kern_len = env_width;

		/* Get the envelope */
		odfilter_run_inplace
			(envelope_buf
			,as_bits->atk_end_loop_start
			,as_bits->length
			,env_width-1
			,1
			,scratch_buf
			,scratch_buf2
			,scratch_buf3
			,&filt
			);

		r      = rel_bits;
		powptr = relpowers;
		while (r != NULL) {
			/* Build the cross correlation kernel. */
			float rel_power = 0.0f;
			for (ch = 0; ch < channels; ch++) {
				float ch_power = 0.0f;
				for (i = 0; i < env_width; i++) {
					float s = r->data[ch*r->chan_stride + env_width - 1 - i];
					scratch_buf[i] = s * (2.0f / (cor_fft_sz * env_width));
					ch_power += s * s;
				}
				for (; i < cor_fft_sz; i++)
					scratch_buf[i] = 0.0f;
				rel_power += ch_power;
				fftset_fft_conv_get_kernel(fft, kern_buf, scratch_buf);

				odfilter_run
					(as_bits->data + ch*as_bits->chan_stride
					,mse_buf
					,(ch != 0)
					,as_bits->atk_end_loop_start
					,as_bits->length
					,env_width-1
					,1
					,scratch_buf
					,scratch_buf2
					,scratch_buf3
					,&filt
					);
			}

			rel_power /= env_width;

#if 0
			for (i = 0; i < buf_stride; i++) {
				mse_buf[i] = envelope_buf[i] + rel_power - 2.0f * mse_buf[i];
			}
#endif

			*powptr  = rel_power;
			powptr  += 1;
			mse_buf += buf_stride;
			r        = r->next;
		}

		mse_buf      = envelope_buf + buf_stride;

#if OPENDIAPASON_VERBOSE_DEBUG
		if (strlen(file_ref) < 1024 - 50) {
			char      namebuf[1024];
			struct wav_dumper dump;
			sprintf(namebuf, "%s_reltable_inputs_nomin.wav", file_ref);
			if (wav_dumper_begin(&dump, namebuf, 1 + nb_releases, 24, norm_rate) == 0) {
				(void)wav_dumper_write_from_floats(&dump, envelope_buf, as_bits->length, 1, buf_stride);
				wav_dumper_end(&dump);
			}
		}
#endif

		reltable_build(&pipe->reltable, envelope_buf, mse_buf, relpowers, nb_releases, buf_stride, as_bits->length, as_bits->period, file_ref);

		aalloc_pop(allocator);
	}
#endif


#if OPENDIAPASON_VERBOSE_DEBUG
	for (i = 0; i < as_bits->nloop; i++) {
		printf("loop %u) %u,%u,%u\n", i, pipe->attack.starts[pipe->attack.ends[i].start_idx].first_valid_end, pipe->attack.starts[pipe->attack.ends[i].start_idx].start_smpl , pipe->attack.ends[i].end_smpl);
	}
#endif

	return NULL;
}

const char *
load_smpl_comp
	(struct pipe_v1             *pipe
	,const struct smpl_comp     *components
	,unsigned                    nb_components
	,struct aalloc              *allocator
	,struct fftset              *fftset
	,const struct odfilter      *prefilter
	)
{
	struct memory_wave *mw;
	const char *err = NULL;
	unsigned i;

	assert(nb_components);

	mw = malloc(sizeof(*mw) * nb_components);
	if (mw == NULL)
		return "out of memory";

	/* Load contributing samples. */
	for (i = 0; i < nb_components; i++) {
		err = mw_load_from_file(mw + i, components[i].filename, components[i].load_format);
		if (err != NULL)
			break;
	}
	if (err != NULL) {
		while (i--)
			free(mw[i].buffers);
		return err;
	}

	/* Combine the segments. */
	{
		unsigned last_attack = 0;
		unsigned last_release = 0;
		for (i = 1; i < nb_components && err == NULL; i++) {
			if (mw[i].channels != mw[0].channels) {
				err = "mixed channel formats for sample";
				break;
			}
			if (mw[i].as.data != NULL) {
				if (mw[last_attack].as.data == NULL) {
					mw[last_attack].as = mw[i].as;
				} else {
					mw[last_attack].as.next = &(mw[i].as);
					last_attack = i;
				}
			}
			if (mw[i].rel.data != NULL) {
				if (mw[last_release].rel.data == NULL) {
					mw[last_release].rel = mw[i].rel;
				} else {
					mw[last_release].rel.next = &(mw[i].rel);
					last_release = i;
				}
			}
		}
	}

	/* Make sure the wave has at least one loop and a release marker. */
	if (err == NULL) {
		if (mw[0].as.nloop <= 0 || mw[0].as.nloop > MAX_LOOP || mw[0].rel.data == NULL || mw[0].as.data == NULL)
			err = "no/too-many loops or no release";
	}

	if (err == NULL) {
		err =
			load_smpl_lists
				(pipe
				,&(mw[0].as)
				,&(mw[0].rel)
				,mw[0].channels
				,mw[0].rate
				,allocator
				,fftset
				,prefilter
				,components[0].filename
				);
	}

	for (i = 0; i < nb_components; i++)
		free(mw[i].buffers);

	return err;
}

/* This is really just a compatibility function now. To be deleted when more
 * things start using the other loader. */
const char *
load_smpl_f
	(struct pipe_v1             *pipe
	,const char                 *filename
	,struct aalloc              *allocator
	,struct fftset              *fftset
	,const struct odfilter      *prefilter
	,int                         load_type
	)
{
	struct smpl_comp cmp;
	cmp.filename    = filename;
	cmp.load_flags  = SMPL_COMP_LOADFLAG_AS | SMPL_COMP_LOADFLAG_R;
	cmp.load_format = load_type;
	return 
		load_smpl_comp
			(pipe
			,&cmp
			,1
			,allocator
			,fftset
			,prefilter
			);
}
