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
	unsigned long  datasz;
	unsigned char *data;
	unsigned long  smplsz;
	unsigned char *smpl;
	unsigned long  fmtsz;
	unsigned char *fmt;
	unsigned long  cuesz;
	unsigned char *cue;
};

static int fcc_are_different(const char *f1, const char *f2)
{
	return (f1[0] == 0 || f1[0] != f2[0] || f1[1] == 0 || f1[1] != f2[1] ||
	        f1[2] == 0 || f1[2] != f2[2] || f1[3] == 0 || f1[3] != f2[3]);
}

static unsigned long parse_le32(const void *buf)
{
	const unsigned char *b = buf;
	return
		(((unsigned long)b[3]) << 24) |
		(((unsigned long)b[2]) << 16) |
		(((unsigned long)b[1]) << 8) |
		(((unsigned long)b[0]) << 0);
}


static unsigned parse_le16(const void *buf)
{
	const unsigned char *b = buf;
	return
		(((unsigned)b[1]) << 8) |
		(((unsigned)b[0]) << 0);
}

static const char *load_wave_cks(struct wave_cks *cks, unsigned char *buf, unsigned long bufsz)
{
	{
		unsigned long sz;
		if (   bufsz < 12
		   ||  fcc_are_different((char *)buf, "RIFF")
		   ||  ((sz = parse_le32(buf + 4)) < 4)
		   ||  fcc_are_different((char *)(buf + 8), "WAVE")
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
		unsigned long cksz;

		bufsz  -= 8;
		cksz = parse_le32(buf + 4);
		if (cksz > bufsz)
			cksz = bufsz;

		if (!fcc_are_different((char *)buf, "data")) {
			cks->data   = buf + 8;
			cks->datasz = cksz;
		} else if (!fcc_are_different((char *)buf, "cue ")) {
			cks->cue    = buf + 8;
			cks->cuesz  = cksz;
		} else if (!fcc_are_different((char *)buf, "smpl")) {
			cks->smpl   = buf + 8;
			cks->smplsz = cksz;
		} else if (!fcc_are_different((char *)buf, "fmt ")) {
			cks->fmt    = buf + 8;
			cks->fmtsz  = cksz;
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

const char *load_smpl_mem(struct memory_wave *mw, unsigned char *buf, unsigned long fsz)
{
	struct wave_cks cks;
	unsigned        i, ch;
	uint_fast32_t   atk_length = 0;
	uint_fast32_t   atk_end_loop_start = 0;
	unsigned char  *rel_data = NULL;
	uint_fast32_t   rel_length = 0;
	size_t          block_align;
	uint_fast32_t   total_length;

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

		if (parse_le16(cks.fmt + 0) != 1)
			return "can only read PCM wave files";

		mw->native_bits = parse_le16(cks.fmt + 14);
		mw->channels    = parse_le16(cks.fmt + 2);
		mw->rate        = parse_le32(cks.fmt + 4);

		if (mw->native_bits != 16 && mw->native_bits != 24)
			return "can only load 16 or 24 bit PCM wave files";

		block_align  = mw->channels * ((mw->native_bits + 7) / 8);
		total_length = cks.datasz / block_align;
	}

	if (cks.smpl == NULL) {
		mw->loops     = NULL;
		mw->nloop     = 0;
		mw->frequency = -1.0;
	} else if (cks.smplsz >= 36) {
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
		unsigned end_loop_idx = 0;
		unsigned long spec_data_sz = parse_le32(cks.smpl + 32);
		double note                = (parse_le32(cks.smpl + 16) / (65536.0 * 65536.0)) + parse_le32(cks.smpl + 12);
		mw->nloop                  = parse_le32(cks.smpl + 28);
		mw->frequency              = 440.0f * powf(2.0f, (note - 69.0f) / 12.0f);

		if (cks.smplsz < 36 + mw->nloop * 24 + spec_data_sz)
			return "invalid sampler chunk";

		mw->loops = malloc(mw->nloop * 2 * sizeof(mw->loops[0]));
		for (i = 0; i < mw->nloop; i++) {
			mw->loops[2*i+0] = parse_le32(cks.smpl + 36 + i * 24 + 8);
			mw->loops[2*i+1] = parse_le32(cks.smpl + 36 + i * 24 + 12);
			if (mw->loops[2*i+0] > mw->loops[2*i+1] || mw->loops[2*i+1] >= total_length)
				return "invalid sampler loop";
			if (mw->loops[2*i+1] >= atk_length) {
				atk_length = mw->loops[2*i+1] + 1;
				end_loop_idx = i;
			}
			atk_end_loop_start = mw->loops[2*end_loop_idx+0];
		}
	} else {
		return "invalid sampler chunk";
	}

	if (cks.cue != NULL) {
		unsigned long nb_cue;
		unsigned last_release = 0;

		if (cks.cuesz < 4)
			return "invalid cue chunk";

		nb_cue = parse_le32(cks.cue + 0);

		if (cks.cuesz < nb_cue * 24 + 4)
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
			unsigned long block_start   = parse_le32(cks.cue + 4 + i * 24 + 16);
			unsigned long sample_offset = parse_le32(cks.cue + 4 + i * 24 + 20);
			unsigned long relpos        = sample_offset + block_start / (mw->channels * ((mw->native_bits + 7) / 8));

			if (parse_le32(cks.cue + 4 + i * 24 + 12) != 0)
				continue;
			if (parse_le32(cks.cue + 4 + i * 24 + 8) != 0x61746164ul)
				continue;
			if (relpos > last_release)
				last_release = relpos;
		}

		if (last_release < atk_length) {
			last_release = atk_length;
			printf("shortened release as it was positioned inside a loop\n");
		}

		if (last_release >= total_length)
			return "invalid cue chunk";

		rel_data   = cks.data + last_release*block_align;
		rel_length = total_length - last_release;
	} else {
		return "no cue chunk";
	}

	{
		float *buffers;
		mw->rel_length  = rel_length;
		mw->atk_length  = atk_length;
		mw->atk_end_loop_start = atk_end_loop_start;
		mw->chan_stride = VLF_PAD_LENGTH(atk_length) + VLF_PAD_LENGTH(rel_length);
		buffers         = malloc(sizeof(float *) * mw->channels * mw->chan_stride);
		mw->buffers     = buffers;
		if (atk_length) {
			bufcvt_deinterleave
				(buffers
				,mw->chan_stride
				,cks.data
				,atk_length
				,mw->channels
				,(mw->native_bits == 24) ? BUFCVT_FMT_SLE24 : BUFCVT_FMT_SLE16
				,BUFCVT_FMT_FLOAT
				);
			for (ch = 0; ch < mw->channels; ch++) {
				for (i = atk_length; i < VLF_PAD_LENGTH(atk_length); i++) {
					buffers[ch*mw->chan_stride+i] = 0.0f;
				}
			}
			mw->atk_data  = buffers;
			buffers      += VLF_PAD_LENGTH(atk_length);
		} else {
			mw->atk_data = NULL;
		}
		if (rel_length) {
			bufcvt_deinterleave
				(buffers
				,mw->chan_stride
				,rel_data
				,rel_length
				,mw->channels
				,(mw->native_bits == 24) ? BUFCVT_FMT_SLE24 : BUFCVT_FMT_SLE16
				,BUFCVT_FMT_FLOAT
				);
			for (ch = 0; ch < mw->channels; ch++) {
				for (i = rel_length; i < VLF_PAD_LENGTH(rel_length); i++) {
					buffers[ch*mw->chan_stride+i] = 0.0f;
				}
			}
			mw->rel_data  = buffers;
			buffers      += VLF_PAD_LENGTH(rel_length);
		} else {
			mw->rel_data = NULL;
		}
	}

	return NULL;
}

const char *mw_load_from_file(struct memory_wave *mw, const char *fname)
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
							err = load_smpl_mem(mw, fbuf, fsz);
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

static
void
inplace_convolve
	(const float                *data
	,float                      *output
	,int                         add_to_output
	,unsigned long               susp_start
	,unsigned long               length
	,unsigned                    pre_read
	,int                         is_looped
	,float                      *sc1
	,float                      *sc2
	,float                      *sc3
	,const float                *prefilt_kern
	,unsigned                    prefilt_kern_len
	,unsigned                    prefilt_real_fft_len
	,const struct fftset_fft    *prefilt_fft
	)
{
	unsigned max_in = prefilt_real_fft_len - prefilt_kern_len + 1;
	float *old_data = malloc(sizeof(float) * length);
	unsigned input_read;
	unsigned input_pos;

	if (old_data == NULL)
		abort();

	/* Copy input buffer to temp buffer. */
	memcpy(old_data, data, sizeof(float) * length);

	/* Zero output buffer. */
	if (!add_to_output) {
		memset(output, 0, sizeof(float) * length);
	}

	/* Run trivial overlap add convolution */
	input_read = 0;
	input_pos = 0;
	while (1) {
		unsigned j = 0;
		int output_pos = (int)input_read-(int)pre_read;

		/* Build input buffer */
		if (is_looped) {
			unsigned op = 0;
			while (op < max_in) {
				unsigned max_read;

				/* How much can we read before we hit the end of the buffer? */
				max_read = length - input_pos;

				/* How much SHOULD we read? */
				if (max_read + op > max_in)
					max_read = max_in - op;

				/* Read it. */
				for (j = 0; j < max_read; j++)
					sc1[j + op] = old_data[j + input_pos];

				/* Increment offsets. */
				input_pos += max_read;
				op        += max_read;

				/* If we read to the end of the buffer, move to the sustain
				 * start. */
				if (input_pos == length)
					input_pos = susp_start;
			}
			for (; op < prefilt_real_fft_len; op++)
				sc1[op] = 0.0f;
		} else {
			for (j = 0; j < max_in && input_read+j < length; j++) sc1[j] = old_data[input_read+j];
			for (; j < prefilt_real_fft_len;            j++)      sc1[j] = 0.0f;
		}

		/* Convolve! */
		fftset_fft_conv(prefilt_fft, sc2, sc1, prefilt_kern, sc3);

		/* Sc2 contains the convolved buffer. */
		for (j = 0; j < prefilt_real_fft_len; j++) {
			int x = output_pos+(int)j;
			if (x < 0)
				continue;
			/* Cast is safe because we check earlier for negative. */
			if ((unsigned)x >= length)
				break;
			output[x] += sc2[j];
		}

		/* added nothing to output buffer! */
		if (j == 0)
			break;

		input_read += max_in;
	};

	free(old_data);
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
	(struct memory_wave         *mw
	,struct aalloc              *allocator
	,const float                *prefilt_kern
	,unsigned                    prefilt_kern_len
	,unsigned                    prefilt_real_fft_len
	,const struct fftset_fft    *prefilt_fft
	,const char                 *debug_prefix
	)
{
	float *scratch;
	float *inbuf;
	float *outbuf;
	unsigned ch;
	aalloc_push(allocator);
	inbuf   = aalloc_align_alloc(allocator, sizeof(float) * prefilt_real_fft_len, 64);
	outbuf  = aalloc_align_alloc(allocator, sizeof(float) * prefilt_real_fft_len, 64);
	scratch = aalloc_align_alloc(allocator, sizeof(float) * prefilt_real_fft_len, 64);
	for (ch = 0; ch < mw->channels; ch++) {
		/* Convolve the attack/sustain segment. */
		inplace_convolve
			(/* input */           mw->atk_data + ch*mw->chan_stride
			,/* output */          mw->atk_data + ch*mw->chan_stride
			,/* sum into output */ 0
			,/* sustain start */   mw->atk_end_loop_start
			,/* total length */    mw->atk_length
			,/* pre-read */        prefilt_kern_len / 2 + 1
			,/* is looped */       1
			,inbuf
			,outbuf
			,scratch
			,prefilt_kern
			,prefilt_kern_len
			,prefilt_real_fft_len
			,prefilt_fft
			);

		/* Convolve the release segment. Don't include all of the pre-ringing
		 * at the start. i.e. we are shaving some samples away from the start
		 * of the release. */
		inplace_convolve
			(/* input */           mw->rel_data + ch*mw->chan_stride
			,/* output */          mw->rel_data + ch*mw->chan_stride
			,/* sum into output */ 0
			,/* sustain start */   0
			,/* total length */    mw->rel_length
			,/* pre-read */        prefilt_kern_len / 2 + prefilt_kern_len / 8
			,/* is looped */       0
			,inbuf
			,outbuf
			,scratch
			,prefilt_kern
			,prefilt_kern_len
			,prefilt_real_fft_len
			,prefilt_fft
			);
	}
	aalloc_pop(allocator);

#if OPENDIAPASON_VERBOSE_DEBUG
	if (debug_prefix != NULL && mw->channels == 2) {
		char      namebuf[1024];
		struct wav_dumper dump;

		sprintf(namebuf, "%s_prefilter_atk.wav", debug_prefix);
		if (wav_dumper_begin(&dump, namebuf, 2, 24, mw->rate) == 0) {
			(void)wav_dumper_write_from_floats(&dump, mw->atk_data, mw->atk_length, 1, mw->chan_stride);
			wav_dumper_end(&dump);
		}

		sprintf(namebuf, "%s_prefilter_rel.wav", debug_prefix);
		if (wav_dumper_begin(&dump, namebuf, 2, 24, mw->rate) == 0) {
			(void)wav_dumper_write_from_floats(&dump, mw->rel_data, mw->rel_length, 1, mw->chan_stride);
			wav_dumper_end(&dump);
		}
	}
#else
	(void)debug_prefix;
#endif
}

const char *
load_smpl_comp
	(struct pipe_v1             *pipe
	,const struct smpl_comp     *components
	,unsigned                    nb_components
	,struct aalloc              *allocator
	,struct fftset              *fftset
	,const float                *prefilt_kern
	,unsigned                    prefilt_kern_len
	,unsigned                    prefilt_real_fft_len
	,const struct fftset_fft    *prefilt_fft
	)
{
	struct memory_wave mw;
	const char *err;
	unsigned i;
	uint_fast32_t rseed = rand();
	void *buf;
	/* release has 32 samples of extra zero slop for a fake loop */
	const unsigned release_slop   = 32;

	assert(nb_components == 1 /* TODO!!! */);

	/* Load the wave file. */
	err = mw_load_from_file(&mw, components[0].filename);
	if (err != NULL)
		return err;

	/* Make sure the wave has at least one loop and a release marker. */
	if (mw.nloop <= 0 || mw.nloop > MAX_LOOP || mw.rel_data == NULL || mw.atk_data == NULL)
		return "no/too-many loops or no release";

	/* Prefilter and adjust audio. */
	apply_prefilter
		(&mw
		,allocator
		,prefilt_kern
		,prefilt_kern_len
		,prefilt_real_fft_len
		,prefilt_fft
		,components[0].filename
		);

	pipe->frequency   = mw.frequency;
	pipe->sample_rate = mw.rate;
	pipe->attack.nloop = mw.nloop;
	for (i = 0; i < mw.nloop; i++) {
		pipe->attack.starts[i].start_smpl      = mw.loops[2*i+0];
		pipe->attack.starts[i].first_valid_end = 0;
		pipe->attack.ends[i].end_smpl          = mw.loops[2*i+1];
		pipe->attack.ends[i].start_idx         = i;

		/* These conditions should be checked when the wave is being loaded. */
		assert(pipe->attack.ends[i].end_smpl < mw.atk_length);
		assert(pipe->attack.ends[i].end_smpl >= pipe->attack.starts[i].start_smpl);
	}

	/* Filthy bubble sort */
	for (i = 0; i < mw.nloop; i++) {
		unsigned j;
		for (j = i + 1; j < mw.nloop; j++) {
			if (pipe->attack.ends[j].end_smpl < pipe->attack.ends[i].end_smpl) {
				struct dec_loop_end tmp = pipe->attack.ends[j];
				pipe->attack.ends[j] = pipe->attack.ends[i];
				pipe->attack.ends[i] = tmp;
			}
		}
	}

	/* Compute first valid end indices */
	for (i = 0; i < mw.nloop; i++) {
		unsigned loop_start = pipe->attack.starts[i].start_smpl;
		while (pipe->attack.ends[pipe->attack.starts[i].first_valid_end].end_smpl <= loop_start)
			pipe->attack.starts[i].first_valid_end++;
	}

	assert(pipe->attack.ends[mw.nloop-1].end_smpl+1 == mw.atk_length);
	assert(pipe->attack.starts[pipe->attack.ends[mw.nloop-1].start_idx].start_smpl == mw.atk_end_loop_start);

	pipe->release.nloop                     = 1;
	pipe->release.starts[0].start_smpl      = mw.rel_length;
	pipe->release.starts[0].first_valid_end = 0;
	pipe->release.ends[0].end_smpl          = mw.rel_length + release_slop - 1;
	pipe->release.ends[0].start_idx         = 0;

	if (components[0].load_format == 12 && mw.channels == 2) {
		buf = aalloc_alloc(allocator, sizeof(unsigned char) * (mw.rel_length + release_slop + 1) * 3);
		pipe->release.gain =
			quantize_boost_interleave
				(buf
				,mw.rel_data
				,mw.chan_stride
				,2
				,mw.rel_length
				,mw.rel_length + release_slop + 1
				,&rseed
				,12
				);
		pipe->release.data = buf;
		pipe->release.instantiate = u12c2_instantiate;

		buf = aalloc_alloc(allocator, sizeof(unsigned char) * (mw.atk_length + 1) * 3);
		pipe->attack.gain =
			quantize_boost_interleave
				(buf
				,mw.atk_data
				,mw.chan_stride
				,2
				,mw.atk_length
				,mw.atk_length + 1
				,&rseed
				,12
				);
		pipe->attack.data = buf;
		pipe->attack.instantiate = u12c2_instantiate;
	} else if (components[0].load_format == 16 && mw.channels == 2) {
		buf = aalloc_alloc(allocator, sizeof(int_least16_t) * (mw.rel_length + release_slop + 1) * 2);
		pipe->release.gain =
			quantize_boost_interleave
				(buf
				,mw.rel_data
				,mw.chan_stride
				,2
				,mw.rel_length
				,mw.rel_length + release_slop + 1
				,&rseed
				,16
				);
		pipe->release.data = buf;
		pipe->release.instantiate = u16c2_instantiate;

		buf = aalloc_alloc(allocator, sizeof(int_least16_t) * (mw.atk_length + 1) * 2);
		pipe->attack.gain =
			quantize_boost_interleave
				(buf
				,mw.atk_data
				,mw.chan_stride
				,2
				,mw.atk_length
				,mw.atk_length + 1
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
		float rel_power;
		unsigned ch;

		aalloc_push(allocator);

		env_width    = (unsigned)((1.0f / mw.frequency) * mw.rate * 2.0f + 0.5f);
		cor_fft_sz   = fftset_recommend_conv_length(env_width, 512) * 2;
		fft          = fftset_create_fft(fftset, FFTSET_MODULATION_FREQ_OFFSET_REAL, cor_fft_sz / 2);
		envelope_buf = aalloc_align_alloc(allocator, sizeof(float) * mw.atk_length, 64);
		mse_buf      = aalloc_align_alloc(allocator, sizeof(float) * mw.atk_length, 64);
		kern_buf     = aalloc_align_alloc(allocator, sizeof(float) * cor_fft_sz, 64);
		scratch_buf  = aalloc_align_alloc(allocator, sizeof(float) * cor_fft_sz, 64);
		scratch_buf2 = aalloc_align_alloc(allocator, sizeof(float) * cor_fft_sz, 64);
		scratch_buf3 = aalloc_align_alloc(allocator, sizeof(float) * cor_fft_sz, 64);

		if (mw.channels == 2) {
			for (i = 0; i < mw.atk_length; i++) {
				float fl = mw.atk_data[i];
				float fr = mw.atk_data[i+mw.chan_stride];
				envelope_buf[i] = fl * fl + fr * fr;
			}
		} else {
			assert(mw.channels == 1);
			for (i = 0; i < mw.atk_length; i++) {
				float fm = mw.atk_data[i];
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
		/* Get the envelope */
		inplace_convolve
			(envelope_buf
			,envelope_buf
			,0
			,mw.atk_end_loop_start
			,mw.atk_length
			,env_width-1
			,1
			,scratch_buf
			,scratch_buf2
			,scratch_buf3
			,kern_buf
			,env_width
			,cor_fft_sz
			,fft
			);

		/* Build the cross correlation kernel. */
		rel_power = 0.0f;
		for (ch = 0; ch < mw.channels; ch++) {
			float ch_power = 0.0f;
			for (i = 0; i < env_width; i++) {
				float s = mw.rel_data[ch*mw.chan_stride + env_width - 1 - i];
				scratch_buf[i] = s * (2.0f / (cor_fft_sz * env_width));
				ch_power += s * s;
			}
			for (; i < cor_fft_sz; i++)
				scratch_buf[i] = 0.0f;
			rel_power += ch_power;
			fftset_fft_conv_get_kernel(fft, kern_buf, scratch_buf);
			inplace_convolve
				(mw.atk_data + ch*mw.chan_stride
				,mse_buf
				,(ch != 0)
				,mw.atk_end_loop_start
				,mw.atk_length
				,env_width-1
				,1
				,scratch_buf
				,scratch_buf2
				,scratch_buf3
				,kern_buf
				,env_width
				,cor_fft_sz
				,fft
				);
		}

		rel_power /= env_width;

		reltable_build(&pipe->reltable, envelope_buf, mse_buf, rel_power, mw.atk_length, (1.0f / mw.frequency) * mw.rate, components[0].filename);

		aalloc_pop(allocator);
	}
#endif


#if OPENDIAPASON_VERBOSE_DEBUG
	for (i = 0; i < mw.nloop; i++) {
		printf("loop %u) %u,%u,%u\n", i, pipe->attack.starts[pipe->attack.ends[i].start_idx].first_valid_end, pipe->attack.starts[pipe->attack.ends[i].start_idx].start_smpl , pipe->attack.ends[i].end_smpl);
	}
#endif

	free(mw.loops);
	free(mw.buffers);

	return NULL;
}

/* This is really just a compatibility function now. To be deleted when more
 * things start using the other loader. */
const char *
load_smpl_f
	(struct pipe_v1             *pipe
	,const char                 *filename
	,struct aalloc              *allocator
	,struct fftset              *fftset
	,const float                *prefilt_kern
	,unsigned                    prefilt_kern_len
	,unsigned                    prefilt_real_fft_len
	,const struct fftset_fft    *prefilt_fft
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
			,prefilt_kern
			,prefilt_kern_len
			,prefilt_real_fft_len
			,prefilt_fft
			);
}
