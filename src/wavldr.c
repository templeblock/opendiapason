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

		bufsz  -= 8;

		cksz += (cksz & 1); /* pad byte */
		if (cksz > bufsz)
			break;

		buf    += 8 + cksz;
		bufsz  -= cksz;
	}

	return NULL;
}

const char *load_smpl_mem(struct memory_wave *mw, unsigned char *buf, unsigned long fsz)
{
	struct wave_cks cks;

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
		unsigned i;

		if (cks.fmtsz < 16)
			return "invalid format chunk";

		if (parse_le16(cks.fmt + 0) != 1)
			return "can only read PCM wave files";

		mw->native_bits = parse_le16(cks.fmt + 14);
		mw->channels    = parse_le16(cks.fmt + 2);
		mw->rate        = parse_le32(cks.fmt + 4);
		mw->length      = cks.datasz / (mw->channels * ((mw->native_bits + 7) / 8));

		if (mw->native_bits != 16 && mw->native_bits != 24)
			return "can only load 16 or 24 bit PCM wave files";

		mw->data = malloc(sizeof(void *) * mw->channels);
		for (i = 0; i < mw->channels; i++)
			mw->data[i] = malloc(sizeof(float) * mw->length);

		for (i = 0; i < mw->channels; i++) {
			unsigned j;
			for (j = 0; j < mw->length; j++) {
				unsigned long s;
				long t;
				if (mw->native_bits == 24) {
					s =            cks.data[3*(i+mw->channels*j)+2];
					s = (s << 8) | cks.data[3*(i+mw->channels*j)+1];
					s = (s << 8) | cks.data[3*(i+mw->channels*j)+0];
					t = (s & 0x800000u) ? -(long)(((~s) & 0x7FFFFFu) + 1) : (long)s;
				} else {
					s =            cks.data[2*(i+mw->channels*j)+1];
					s = (s << 8) | cks.data[2*(i+mw->channels*j)+0];
					t = (s & 0x8000u)   ? -(long)(((~s) & 0x7FFFu) + 1) : (long)s;
					t *= 256;
				}
				mw->data[i][j] = t / (float)0x1000000;
			}
		}
	}



	if (cks.smpl == NULL) {
		mw->loops = NULL;
		mw->nloop = 0;
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
		unsigned i;
		unsigned long spec_data_sz = parse_le32(cks.smpl + 32);
		double note                = (parse_le32(cks.smpl + 16) / (65536.0 * 65536.0)) + parse_le32(cks.smpl + 12);
		mw->nloop                  = parse_le32(cks.smpl + 28);
		mw->frequency              = 440.0 * pow(2.0, (note - 69) / 12);

		if (cks.smplsz < 36 + mw->nloop * 24 + spec_data_sz)
			return "invalid sampler chunk";

		mw->loops = malloc(mw->nloop * 2 * sizeof(mw->loops[0]));
		for (i = 0; i < mw->nloop; i++) {
			mw->loops[2*i+0] = parse_le32(cks.smpl + 36 + i * 24 + 8);
			mw->loops[2*i+1] = parse_le32(cks.smpl + 36 + i * 24 + 12);
		}
	} else {
		return "invalid sampler chunk";
	}

	if (cks.cue == NULL) {
		mw->release_pos = 0;
	} else if (cks.cuesz >= 4) {
		unsigned long nb_cue = parse_le32(cks.cue + 0);
		unsigned i;

		/* 0             nb_cuepoints
		 * 4 + 24*i + 0  dwName
		 * 4 + 24*i + 4  dwPosition
		 * 4 + 24*i + 8  fccChunk
		 * 4 + 24*i + 12 dwChunkStart
		 * 4 + 24*i + 16 dwBlockStart
		 * 4 + 24*i + 20 dwSampleOffset
		 */

		if (cks.cuesz < nb_cue * 24 + 4)
			return "invalid cue chunk";

		for (i = 0; i < nb_cue; i++) {
			unsigned j;
			unsigned long cue_id        = parse_le32(cks.cue + 4 + i * 24 + 0);
			unsigned long block_start   = parse_le32(cks.cue + 4 + i * 24 + 16);
			unsigned long sample_offset = parse_le32(cks.cue + 4 + i * 24 + 20);
			unsigned long relpos = sample_offset + block_start / (mw->channels * ((mw->native_bits + 7) / 8));

			if (parse_le32(cks.cue + 4 + i * 24 + 12) != 0)
				continue;
			if (parse_le32(cks.cue + 4 + i * 24 + 8) != 0x61746164ul)
				continue;

			for (j = 0; j < mw->nloop; j++) {
				unsigned long loop_id = parse_le32(cks.smpl + 36 + i * 24 + 0);
				if (cue_id == loop_id || relpos <= mw->loops[2*j+1])
					break;
			}

			/* If j==mw->nloop, the cue id was not found among the loops. */
			if (j == mw->nloop) {
				mw->release_pos = relpos;
				printf("release @%lu,%lu\n", sample_offset, mw->release_pos);
				break;
			}
		}

	} else {
		return "invalid cue chunk";
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


//#include "reltable.h"
//#include "interp_prefilter.h"

float *eval_mse(const float *in1, const float *in2, unsigned l1, unsigned l2, unsigned *length);

static
void
convolve_load_helper
	(const float  *input
	,float        *output
	,int           is_looped
	,unsigned long read_pos
	,unsigned      nb_samples
	,unsigned long length
	,unsigned long restart_position
	)
{
	unsigned i;
	if (is_looped) {
		/* Optimize this garbage. */
		unsigned long loop_len = length - restart_position;
		for (i = 0; i < nb_samples; i++) {
			unsigned j = read_pos + i;
			unsigned src = (j >= length) ? (((j - length) % loop_len) + restart_position) : j;
			output[i] = input[src];
		}
	} else {
		for (i = 0; i < nb_samples; i++) {
			unsigned j = read_pos + i;
			output[i] = (j >= length) ? 0.0f : input[j];
		}
	}
}




static
void
inplace_convolve
	(const float                *data
	,float                      *output
	,int                         add_to_output
	,unsigned long               susp_start
	,unsigned long               length
	,unsigned                    prering_sample_skip
	,int                         is_looped
	,float                      *sc1
	,float                      *sc2
	,float                      *sc3
	,const float                *prefilt_kern
	,unsigned                    prefilt_kern_len
	,unsigned                    prefilt_real_fft_len
	,const struct fastconv_pass *prefilt_fft
	)
{
	const unsigned max_in        = 1 + prefilt_real_fft_len - prefilt_kern_len;
	const unsigned valid_outputs = max_in - 2*prefilt_kern_len + 1;
	unsigned long read_pos       = 0;
	const unsigned pre_read      = prefilt_kern_len / 2 - prering_sample_skip;
	unsigned i;
	convolve_load_helper(data, sc1, is_looped, read_pos, max_in, length, susp_start);
	for (i = max_in; i < prefilt_real_fft_len; i++) {
		sc1[i] = 0.0f;
	}
	while ((read_pos + pre_read < length) || ((read_pos == 0) && length)) {
		fastconv_execute_conv(prefilt_fft, sc1, prefilt_kern, sc2, sc3);
		read_pos += valid_outputs;
		if (read_pos + pre_read < length) {
			convolve_load_helper(data, sc1, is_looped, read_pos, max_in, length, susp_start);
		}
		read_pos -= valid_outputs;
		if (add_to_output) {
			if (read_pos == 0) {
				for (i = 0; i < pre_read && i < length; i++) {
					output[i] += sc2[i + prefilt_kern_len - pre_read];
				}
			}
			for (i = 0; i < valid_outputs && (read_pos + i + pre_read < length); i++) {
				output[read_pos + i + pre_read] += sc2[i + prefilt_kern_len];
			}
		} else {
			if (read_pos == 0) {
				for (i = 0; i < pre_read && i < length; i++) {
					output[i] = sc2[i + prefilt_kern_len - pre_read];
				}
			}
			for (i = 0; i < valid_outputs && (read_pos + i + pre_read < length); i++) {
				output[read_pos + i + pre_read] = sc2[i + prefilt_kern_len];
			}
		}
		read_pos += valid_outputs;
	}
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
	,unsigned long               susp_start
	,unsigned long               susp_end
	,unsigned long               rel_start
	,struct aalloc              *allocator
	,const float                *prefilt_kern
	,unsigned                    prefilt_kern_len
	,unsigned                    prefilt_real_fft_len
	,const struct fastconv_pass *prefilt_fft
	)
{
	float *scratch;
	float *inbuf;
	float *outbuf;
	unsigned ch;
	unsigned i;
	aalloc_push(allocator);
	inbuf   = aalloc_align_alloc(allocator, sizeof(float) * prefilt_real_fft_len, 64);
	outbuf  = aalloc_align_alloc(allocator, sizeof(float) * prefilt_real_fft_len, 64);
	scratch = aalloc_align_alloc(allocator, sizeof(float) * prefilt_real_fft_len, 64);
	for (ch = 0; ch < mw->channels; ch++) {
		/* Convolve the attack/sustain segment. */
		inplace_convolve
			(mw->data[ch]
			,mw->data[ch]
			,0
			,susp_start
			,susp_end+1
			,0
			,1
			,inbuf
			,outbuf
			,scratch
			,prefilt_kern
			,prefilt_kern_len
			,prefilt_real_fft_len
			,prefilt_fft
			);
		/* Zero any crap between the end of the last loop and the start of the
		 * release (we don't really need to do this, it just makes things
		 * nicer if we dump the memory wave back to a file). */
		for (i = susp_end + 1; i < rel_start; i++)
			mw->data[ch][i] = 0.0f;
		/* Convolve the release segment. Don't include all of the pre-ringing
		 * at the start. i.e. we are shaving some samples away from the start
		 * of the release. */
		inplace_convolve
			(mw->data[ch] + rel_start
			,mw->data[ch] + rel_start
			,0
			,0
			,mw->length - rel_start
			,prefilt_kern_len / 8
			,0
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

#if 0
	FILE *f = fopen("prefilter_debug2.raw", "wb");
	for (i = 0; i < mw->length; i++) {
		unsigned j;
		for (j = 0; j < mw->channels; j++) {
			double val = 32768 * mw->data[j][i] * 2 + (rand() + (double)rand()) * 0.5 / (double)RAND_MAX;
			int s = (int)(val + 50000.0) - 50000;
			short v = (s > 32767) ? 32767 : ((s < -32768) ? -32768 : s);
			fwrite(&v, 1, 2, f);
		}
	}
	fclose(f);
#endif
}

const char *
load_smpl_f
	(struct pipe_v1             *pipe
	,const char                 *filename
	,struct aalloc              *allocator
	,struct fastconv_fftset     *fftset
	,const float                *prefilt_kern
	,unsigned                    prefilt_kern_len
	,unsigned                    prefilt_real_fft_len
	,const struct fastconv_pass *prefilt_fft
	)
{
	struct memory_wave mw;
	const char *err;
	unsigned i;
	void *samples;
	int_least16_t *relsamples;
	unsigned long dlen;

	err = mw_load_from_file(&mw, filename);
	if (err != NULL)
		return err;

	if (mw.nloop <= 0 || mw.nloop > MAX_LOOP || mw.release_pos == 0)
		return "no/too-many loops or no release";

	pipe->frequency = mw.frequency;
	pipe->sample_rate = mw.rate;

	pipe->attack.nloop = mw.nloop;
	for (i = 0; i < mw.nloop; i++) {
		pipe->attack.starts[i].start_smpl      = mw.loops[2*i+0];
		pipe->attack.starts[i].first_valid_end = 0;
		pipe->attack.ends[i].end_smpl          = mw.loops[2*i+1];
		pipe->attack.ends[i].start_idx         = i;

		if (pipe->attack.ends[i].end_smpl < pipe->attack.starts[i].start_smpl)
			return "invalid loop";
		if (pipe->attack.ends[i].end_smpl >= mw.length)
			return "invalid loop";
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

	const unsigned long as_length = pipe->attack.ends[mw.nloop-1].end_smpl;
	const unsigned long as_loop_start = pipe->attack.starts[pipe->attack.ends[mw.nloop-1].start_idx].start_smpl;

#if 1
	/* Prefilter and adjust audio. */
	apply_prefilter
		(&mw
		,as_loop_start
		,as_length
		,mw.release_pos
		,allocator
		,prefilt_kern
		,prefilt_kern_len
		,prefilt_real_fft_len
		,prefilt_fft
		);
#endif

#if 1
	{
		float *envelope_buf;
		float *mse_buf;
		unsigned env_width;
		unsigned cor_fft_sz;
		const struct fastconv_pass *fft;
		float *scratch_buf;
		float *scratch_buf2;
		float *scratch_buf3;
		float *kern_buf;
		float rel_power;
		unsigned ch;

		aalloc_push(allocator);

		env_width    = (unsigned)((1.0f / mw.frequency) * mw.rate * 2.0f + 0.5f);
		cor_fft_sz   = fastconv_recommend_length(env_width, 512);
		fft          = fastconv_get_real_conv(fftset, cor_fft_sz);
		envelope_buf = aalloc_align_alloc(allocator, sizeof(float) * as_length, 64);
		mse_buf      = aalloc_align_alloc(allocator, sizeof(float) * as_length, 64);
		kern_buf     = aalloc_align_alloc(allocator, sizeof(float) * cor_fft_sz, 64);
		scratch_buf  = aalloc_align_alloc(allocator, sizeof(float) * cor_fft_sz, 64);
		scratch_buf2 = aalloc_align_alloc(allocator, sizeof(float) * cor_fft_sz, 64);
		scratch_buf3 = aalloc_align_alloc(allocator, sizeof(float) * cor_fft_sz, 64);

		if (mw.channels == 2) {
			for (i = 0; i < as_length; i++) {
				float fl = mw.data[0][i];
				float fr = mw.data[1][i];
				envelope_buf[i] = fl * fl + fr * fr;
			}
		} else {
			assert(mw.channels == 1);
			for (i = 0; i < as_length; i++) {
				float fm = mw.data[0][i];
				envelope_buf[i] = fm * fm;
			}
		}

		/* Build the evelope kernel. */
		for (i = 0; i < env_width;  i++) scratch_buf[i] = 2.0f / cor_fft_sz;
		for (     ; i < cor_fft_sz; i++) scratch_buf[i] = 0.0f;
		fastconv_execute_fwd
			(fft
			,scratch_buf
			,kern_buf
			);
		/* Get the envelope */
		inplace_convolve
			(envelope_buf
			,envelope_buf
			,0
			,as_loop_start
			,as_length
			,0
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
			for (i = 0; i < env_width;  i++) {
				float s = mw.data[ch][mw.release_pos + env_width - 1 - i];
				scratch_buf[i] = s * (2.0f / cor_fft_sz);
				ch_power += s * s;
			}
			for (; i < cor_fft_sz; i++)
				scratch_buf[i] = 0.0f;
			rel_power += ch_power;
			fastconv_execute_fwd(fft, scratch_buf, kern_buf);
			inplace_convolve
				(mw.data[ch]
				,mse_buf
				,(ch != 0)
				,as_loop_start
				,as_length
				,0
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

#if 0
		unsigned rellvltab_len = 0;
		unsigned rellvltab_scale = 0;
		for (i = 0; i < as_length; i++) {
			float f = (rel_power + envelope_buf[i] - 2.0f * mse_buf[i]) / env_width;
			float v = envelope_buf[i] / rel_power;
			mse_buf[i] = (f <= 0.0f) ? 0.0f : sqrtf(f);
			envelope_buf[i] = (v <= 0.0f) ? 0.0f : (v >= 1.0f ? 1.0f : sqrtf(v));
			if (envelope_buf[i] > 0.95f && rellvltab_len == 0) {
				rellvltab_len = i;
			}
		}

		while (rellvltab_len > 512) {
			rellvltab_len >>= 1;
			rellvltab_scale++;
		}

		pipe->rellvltab_len = rellvltab_len;
		pipe->rellvltab_scale = rellvltab_scale;
		pipe->rellvltab = malloc(sizeof(pipe->rellvltab[0]) * rellvltab_len);
		for (i = 0; i < rellvltab_len; i++) {
			float v = envelope_buf[i << rellvltab_scale] * 32768 + 0.5;
			if (v >= 32767.0f) {
				pipe->rellvltab[i] = 32767;
			} else if (v <= 0.0f) {
				pipe->rellvltab[i] = 0;
			} else {
				pipe->rellvltab[i] = (int_least16_t)v;
			}
		}
#endif

#if 0
		FILE *f = fopen("env_correlator_debug.raw", "wb");
		for (i = 0; i < as_length; i++) {
			unsigned j;
			double val;
			short v;
			int s;
			val = 32768 * envelope_buf[i] * 2 + (rand() + (double)rand()) * 0.5 / (double)RAND_MAX;
			s = (int)(val + 50000.0) - 50000;
			v = (s > 32767) ? 32767 : ((s < -32768) ? -32768 : s);
			fwrite(&v, 1, 2, f);
//			val = 32768 * mse_buf[i] * 2 + (rand() + (double)rand()) * 0.5 / (double)RAND_MAX;
			val = envelope_buf[i] / rel_power;
			val = 32768 * sqrt(val >= 1.0 ? 1.0 : (val < 0.0 ? 0.0 : val)) + (rand() + (double)rand()) * 0.5 / (double)RAND_MAX;
			s = (int)(val + 50000.0) - 50000;
			v = (s > 32767) ? 32767 : ((s < -32768) ? -32768 : s);
			fwrite(&v, 1, 2, f);
		}
		fclose(f);
#endif

		reltable_build(&pipe->reltable, envelope_buf, mse_buf, rel_power, as_length, (1.0f / mw.frequency) * mw.rate);

		aalloc_pop(allocator);
	}
#endif

#if 0
	{
		struct attackana_s ai;
		unsigned env_width = (unsigned)((1.0f / mw.frequency) * mw.rate * 2.0f + 0.5f);
		unsigned fftsz = pow2_rnd_up(env_width * 8);
		struct fftrksf *fft = fftrksf_create(fftsz);
		float *tmp = malloc(sizeof(float) * fftsz * 2);

		build_ana_info
			(&ai
			,mw.data
			,mw.channels
			,pipe->attack.ends[mw.nloop-1].end_smpl + 1
			,env_width
			,fft
			,fftsz
			,tmp
			);

		FILE *envf = fopen("ENV.raw", "wb");
		FILE *filtoutf = fopen("FILTOUT.raw", "wb");

		for (i = 0; i < pipe->attack.ends[mw.nloop-1].end_smpl + 1; i++) {

			short t;
			float s;

			s  = (ai.f0[0][i]) * 32768;
			s += (rand() * 0.5 + rand() * 0.5) / RAND_MAX;
			t  = (short)((int)(s + 32768) - 32768);
			fwrite(&t, 1, 2, filtoutf);
			s  = (ai.f0[1][i]) * 32768;
			s += (rand() * 0.5 + rand() * 0.5) / RAND_MAX;
			t  = (short)((int)(s + 32768) - 32768);
			fwrite(&t, 1, 2, filtoutf);

			s  = sqrt(fabsf(ai.env[i]) / env_width) * 32768;
			s += (rand() * 0.5 + rand() * 0.5) / RAND_MAX;
			t  = (short)((int)(s + 32768) - 32768);
			fwrite(&t, 1, 2, envf);

		}
		
		fclose(envf);
		fclose(filtoutf);

	}
#endif

#if 0
	{
		unsigned rsamples = (unsigned)((1.0 / mw.frequency) * mw.rate * 3.0 + 0.5);
		unsigned clen;
//		float *corr = eval_mse(mw.data[0], mw.data[0] + mw.release_pos, pipe->attack.ends[mw.nloop-1].end_smpl + 1, rsamples, &clen);
//		reltable_build(tab, corr, clen, (1.0f / mw.frequency) * mw.rate);

		{
			FILE *f;
			char danger[1024];
			sprintf(danger, "%s_rcorr.raw", filename);
			f = fopen(danger, "wb");

			for (i = 0; i < clen; i++) {
				short t;
				float s;
				s  = sqrt(corr[i]) * 32.0;
				s += (rand() * 0.5 + rand() * 0.5) / RAND_MAX;
				t  = (short)((int)(s + 32768) - 32768);

				fwrite(&t, 1, 2, f);
			}

			fclose(f);
		}
	}
#endif



	dlen = pipe->attack.ends[mw.nloop-1].end_smpl + 1;
	samples = malloc(sizeof(int_least16_t) * dlen * mw.channels);

	/* release has 32 samples of extra zero slop for a fake loop */
	unsigned release_length = mw.length - mw.release_pos;
	unsigned release_slop   = 32;
	relsamples = malloc(sizeof(int_least16_t) * (release_length + release_slop) * mw.channels);

	pipe->release.instantiate = u16c2_instantiate;
	pipe->release.nloop = 1;
	pipe->release.starts[0].start_smpl      = release_length;
	pipe->release.starts[0].first_valid_end = 0;
	pipe->release.ends[0].end_smpl          = release_length + release_slop - 1;
	pipe->release.ends[0].start_idx         = 0;
	pipe->release.gain = 1.0 / 32768.0;
	for (i = 0; i < mw.channels; i++) {
		unsigned j;
		for (j = 0; j < release_length; j++) {
			float s = mw.data[i][j+mw.release_pos] * 32768.0f;
			float dither = (rand() * 0.5f + rand() * 0.5f) / RAND_MAX;
			long v = (long)(dither + s + 32768.0f) - 32768l;
			relsamples[mw.channels*j+i] = (int_least16_t)((v > 32767) ? 32767 : ((v < -32768) ? - 32768 : v));
		}
		for (; j < release_length + release_slop; j++) {
			relsamples[mw.channels*j+i] = 0;
		}
	}
	pipe->release.data = relsamples;

	pipe->attack.gain = 1.0 / 32768;
	for (i = 0; i < mw.channels; i++) {
		unsigned j;
		for (j = 0; j < dlen; j++) {
			float s = mw.data[i][j] * 32768.0f;
			float dither = (rand() * 0.5f + rand() * 0.5f) / RAND_MAX;
			long v = (long)(dither + s + 32768.0f) - 32768l;
			((int_least16_t *)samples)[mw.channels*j+i] = (int_least16_t)((v > 32767) ? 32767 : ((v < -32768) ? - 32768 : v));
		}
	}

	pipe->attack.data = samples;
	pipe->attack.instantiate = u16c2_instantiate;

	for (i = 0; i < mw.nloop; i++) {
		printf("%u,%u,%u,%u\n", i, pipe->attack.starts[pipe->attack.ends[i].start_idx].first_valid_end, pipe->attack.starts[pipe->attack.ends[i].start_idx].start_smpl , pipe->attack.ends[i].end_smpl);
	}

	return NULL;
}


