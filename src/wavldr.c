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
#include "smplwav/smplwav_mount.h"
#include "smplwav/smplwav_convert.h"

static const char *load_smpl_mem(struct memory_wave *mw, unsigned char *buf, size_t fsz, unsigned load_format)
{
	unsigned        i, ch;
	float          *buffers;
	struct smplwav  wav;

	if (SMPLWAV_ERROR_CODE(smplwav_mount(&wav, buf, fsz, SMPLWAV_MOUNT_PREFER_CUE_LOOPS)))
		return "could not parse wave file";

	if (wav.format.format == SMPLWAV_FORMAT_FLOAT32 || (wav.format.bits_per_sample != 16 && wav.format.bits_per_sample != 24))
		return "can only load 16 or 24 bit PCM wave files";

	mw->native_bits     = wav.format.bits_per_sample;
	mw->channels        = wav.format.channels;
	mw->rate            = wav.format.sample_rate;
	mw->as.data         = NULL;
	mw->as.next         = NULL;
	mw->rel.data        = NULL;
	mw->rel.next        = NULL;
	mw->as.load_format  = load_format;
	mw->rel.load_format = load_format;
	mw->as.nloop        = 0;
	mw->as.length       = 0;
	mw->rel.position    = 0;
	mw->as.period       = wav.format.sample_rate / (440.0f * powf(2.0f, (wav.pitch_info / (65536.0f * 65536.0f) - 69.0f) / 12.0f));
	mw->rel.period      = mw->as.period;

	for (i = 0; i < wav.nb_marker; i++) {
		if (wav.markers[i].length > 0) {
			uint_fast32_t loop_start = wav.markers[i].position;
			uint_fast32_t loop_end   = loop_start + wav.markers[i].length - 1;

			assert(loop_end < wav.data_frames);
			assert(loop_start <= loop_end);

			if (mw->as.nloop >= MAX_LOOP)
				return "too many loops";

			mw->as.loops[2*mw->as.nloop+0] = loop_start;
			mw->as.loops[2*mw->as.nloop+1] = loop_end;
			mw->as.nloop++;

			if (loop_end + 1 >= mw->as.length) {
				mw->as.length             = loop_end + 1;
				mw->as.atk_end_loop_start = loop_start;
			}
		} else if (wav.markers[i].position > mw->rel.position) {
			mw->rel.position = wav.markers[i].position;
		}
	}

	if (mw->rel.position >= mw->as.length) {
		mw->rel.length = wav.data_frames - mw->rel.position;
	} else {
		mw->rel.length = 0;
	}

	mw->chan_stride =
		(mw->as.length ? VLF_PAD_LENGTH(mw->as.length) : 0) +
		(mw->rel.length ? VLF_PAD_LENGTH(mw->rel.length) : 0);
	mw->as.chan_stride = mw->chan_stride;
	mw->rel.chan_stride = mw->chan_stride;

	buffers         = malloc(sizeof(float *) * mw->channels * mw->chan_stride);
	mw->buffers     = buffers;

	if (mw->as.length) {
		smplwav_convert_deinterleave_floats
			(buffers
			,mw->chan_stride
			,wav.data
			,mw->as.length
			,mw->channels
			,wav.format.format
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
		uint_fast32_t block_align = smplwav_format_container_size(wav.format.format) * wav.format.channels;
		smplwav_convert_deinterleave_floats
			(buffers
			,mw->chan_stride
			,(const unsigned char *)wav.data + mw->rel.position*block_align
			,mw->rel.length
			,mw->channels
			,wav.format.format
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

static void *load_file_to_memory(const char *fname, size_t *pfsz)
{
	FILE *f = fopen(fname, "rb");
	if (f != NULL) {
		if (fseek(f, 0, SEEK_END) == 0) {
			long fsz = ftell(f);
			if (fsz > 0) {
				if (fseek(f, 0, SEEK_SET) == 0) {
					unsigned char *fbuf = malloc(fsz);
					if (fbuf != NULL) {
						if (fread(fbuf, 1, fsz, f) == fsz) {
							fclose(f);
							*pfsz = (size_t)fsz;
							return fbuf;
						}
						free(fbuf);
						fbuf = NULL;
					}
				}
			}
		}
		fclose(f);
	}
	return NULL;
}

const char *mw_load_from_file(struct memory_wave *mw, const char *fname, unsigned load_format)
{
	const char *err = NULL;
	size_t fsz;
	void *data = load_file_to_memory(fname, &fsz);
	if (data == NULL)
		return "failed to load file";
	err = load_smpl_mem(mw, data, fsz, load_format);
	free(data);
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
		float maxr = 0.0f;
		float minr = 0.0f;

		for (j = 0; j < in_length; j++) {
			float s1 = in_bufs[j];
			float s2 = in_bufs[chan_stride+j];
			maxv = (s1 > maxv) ? s1 : maxv;
			minv = (s1 < minv) ? s1 : minv;
			maxr = (s2 > maxr) ? s2 : maxr;
			minr = (s2 < minr) ? s2 : minr;
		}

		maxv  = (maxv > maxr) ? maxv : maxr;
		minv  = (minv < minr) ? minv : minr;
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
			maxv  *= 1.0f / 32764.0f;
			boost  = ((float)((((uint_fast64_t)1u) << 33))) / maxv;

			/* 4091ms */
			for (j = 0; j < in_length; j++) {
				uint_fast32_t d1, d2, d3, d4;
				float f1, f2;
				int_fast64_t lch, rch;
				f1     = in_bufs[j];
				f2     = in_bufs[chan_stride+j];
				d1     = update_rnd(rseed);
				d2     = update_rnd(d1);
				d3     = update_rnd(d2);
				d4     = update_rnd(d3);
				f1    *= boost;
				f2    *= boost;
				rseed  = d4;
				lch    = (int_fast64_t)f1;
				rch    = (int_fast64_t)f2;
				lch   += (int_fast64_t)(d1 & 0xFFFFFFFFu);
				rch   += (int_fast64_t)(d2 & 0xFFFFFFFFu);
				lch   += (int_fast64_t)(d3 & 0xFFFFFFFFu);
				rch   += (int_fast64_t)(d4 & 0xFFFFFFFFu);
				lch    = lch >> 33;
				rch    = rch >> 33;

				out_buf[2*j+0] = (int_least16_t)lch;
				out_buf[2*j+1] = (int_least16_t)rch;
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
	unsigned ch;
	unsigned idx;
	struct odfilter_temporaries tmps;

	aalloc_push(allocator);
	odfilter_init_temporaries(&tmps, allocator, prefilter);

	/* Convolve the attack/sustain segments. */
	idx = 0;
	while (as != NULL) {
		for (ch = 0; ch < channels; ch++) {
			odfilter_run_inplace
				(/* input */           as->data + ch*as->chan_stride
				,/* sustain start */   as->atk_end_loop_start
				,/* total length */    as->length
				,/* pre-read */        (SMPL_INVERSE_FILTER_LEN-1)/2
				,/* is looped */       1
				,&tmps
				,prefilter
				);
		}

#if OPENDIAPASON_VERBOSE_DEBUG
		if (debug_prefix != NULL) {
			char              namebuf[1024];
			struct wav_dumper dump;
			sprintf(namebuf, "%s_prefilter_atk%02d.wav", debug_prefix, idx);
			if (wav_dumper_begin(&dump, namebuf, channels, 24, rate, 1, rate) == 0) {
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
				,/* pre-read */        (SMPL_INVERSE_FILTER_LEN-1)/2 + SMPL_INVERSE_FILTER_LEN/8
				,/* is looped */       0
				,&tmps
				,prefilter
			);
		}

#if OPENDIAPASON_VERBOSE_DEBUG
		if (debug_prefix != NULL) {
			char              namebuf[1024];
			struct wav_dumper dump;
			sprintf(namebuf, "%s_prefilter_rel%02d.wav", debug_prefix, idx);
			if (wav_dumper_begin(&dump, namebuf, 2, 24, rate, 1, rate) == 0) {
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
//		if (nb_releases != 1) {
//			fprintf(stderr, "sample contained %d release blocks. the max is 1.\n", nb_releases);
//			abort();
//		}
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

	{
		struct rel_data *rel = rel_bits;
		for (i = 0; i < nb_releases; i++, rel = rel->next) {
			pipe->releases[i].nloop                     = 1;
			pipe->releases[i].starts[0].start_smpl      = rel->length;
			pipe->releases[i].starts[0].first_valid_end = 0;
			pipe->releases[i].ends[0].end_smpl          = rel->length + release_slop - 1;
			pipe->releases[i].ends[0].start_idx         = 0;

			if (rel->load_format == 12 && channels == 2) {
				void *buf = aalloc_alloc(allocator, sizeof(unsigned char) * (rel->length + release_slop + 1) * 3);
				pipe->releases[i].gain =
					quantize_boost_interleave
						(buf
						,rel->data
						,rel->chan_stride
						,2
						,rel->length
						,rel->length + release_slop + 1
						,&rseed
						,12
						);
				pipe->releases[i].data = buf;
				pipe->releases[i].instantiate = u12c2_instantiate;
			} else if (rel->load_format == 16 && channels == 2) {
				void *buf = aalloc_alloc(allocator, sizeof(int_least16_t) * (rel->length + release_slop + 1) * 2);
				pipe->releases[i].gain =
					quantize_boost_interleave
						(buf
						,rel->data
						,rel->chan_stride
						,2
						,rel->length
						,rel->length + release_slop + 1
						,&rseed
						,16
						);
				pipe->releases[i].data = buf;
				pipe->releases[i].instantiate = u16c2_instantiate;
			} else {
				abort();
			}
		}
	}

	if (as_bits->load_format == 12 && channels == 2) {
		void *buf = aalloc_alloc(allocator, sizeof(unsigned char) * (as_bits->length + 1) * 3);
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
	} else if (as_bits->load_format == 16 && channels == 2) {
		void *buf = aalloc_alloc(allocator, sizeof(int_least16_t) * (as_bits->length + 1) * 2);
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
		float relpowers[16];
		float *powptr;
		unsigned ch;
		struct odfilter             filt;
		struct odfilter_temporaries filt_tmps;
		size_t buf_stride = VLF_PAD_LENGTH(as_bits->length);
		struct rel_data *r;

		aalloc_push(allocator);

		env_width    = (unsigned)(as_bits->period * 2.0f + 0.5f);

		odfilter_init_filter(&filt, allocator, fftset, env_width);
		odfilter_init_temporaries(&filt_tmps, allocator, &filt);

		envelope_buf = aalloc_align_alloc(allocator, sizeof(float) * (buf_stride * (1 + nb_releases)), 64);
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
		odfilter_build_rect(&filt, &filt_tmps, env_width, 1.0f / env_width);

		/* Get the envelope */
		odfilter_run_inplace
			(envelope_buf
			,as_bits->atk_end_loop_start
			,as_bits->length
			,env_width-1
			,1
			,&filt_tmps
			,&filt
			);

		r      = rel_bits;
		powptr = relpowers;
		while (r != NULL) {
			/* Build the cross correlation kernel. */
			float rel_power = 0.0f;
			for (ch = 0; ch < channels; ch++) {
				rel_power += odfilter_build_xcorr(&filt, &filt_tmps, env_width, r->data + ch*r->chan_stride, 1.0f / env_width);

				odfilter_run
					(as_bits->data + ch*as_bits->chan_stride
					,mse_buf
					,(ch != 0)
					,as_bits->atk_end_loop_start
					,as_bits->length
					,env_width-1
					,1
					,&filt_tmps
					,&filt
					);
			}

			rel_power /= env_width;

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
			if (wav_dumper_begin(&dump, namebuf, 1 + nb_releases, 24, norm_rate, 1, norm_rate) == 0) {
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

#define LOAD_SET_GROW_RATE (500)

int sample_load_set_init(struct sample_load_set *load_set)
{
	load_set->max_nb_elems = LOAD_SET_GROW_RATE;
	load_set->nb_elems = 0;
	load_set->elems = malloc(sizeof(struct sample_load_info) * load_set->max_nb_elems);
	if (load_set->elems == NULL)
		return -1;
	if (cop_mutex_create(&load_set->pop_lock)) {
		free(load_set->elems);
		return -1;
	}
	if (cop_mutex_create(&load_set->file_lock)) {
		cop_mutex_destroy(&load_set->pop_lock);
		free(load_set->elems);
		return -1;
	}
	return 0;
}

struct sample_load_info *sample_load_set_push(struct sample_load_set *load_set)
{
	struct sample_load_info *ns;
	unsigned new_ele_count;

	if (load_set->nb_elems < load_set->max_nb_elems)
		return load_set->elems + load_set->nb_elems++;

	assert(load_set->nb_elems == load_set->max_nb_elems);

	new_ele_count = load_set->max_nb_elems + LOAD_SET_GROW_RATE;
	ns = realloc(load_set->elems, sizeof(struct sample_load_info) * new_ele_count);
	if (ns != NULL) {
		load_set->elems = ns;
		load_set->max_nb_elems = new_ele_count;
		return load_set->elems + load_set->nb_elems++;
	}

	return NULL;
}

static struct sample_load_info *sample_load_set_pop(struct sample_load_set *load_set)
{
	struct sample_load_info *ret = NULL;
	cop_mutex_lock(&load_set->pop_lock);
	if (load_set->nb_elems)
		ret = load_set->elems + --load_set->nb_elems;
	cop_mutex_unlock(&load_set->pop_lock);
	return ret;
}

const char *
load_samples
	(struct sample_load_set *load_set
	,struct aalloc          *allocator
	,struct fftset          *fftset
	,const struct odfilter *prefilter
	)
{
	struct sample_load_info *li;
	while ((li = sample_load_set_pop(load_set)) != NULL) {
		const char *err;
		struct smpl_comp comps[1+WAVLDR_MAX_RELEASES];
		unsigned i;

		assert(li->num_files <= 1+WAVLDR_MAX_RELEASES);

		for (i = 0; i < li->num_files; i++) {
			comps[i].filename    = li->filenames[i];
			comps[i].load_flags  = li->load_flags[i];
			comps[i].load_format = li->load_format;
		}

		err = load_smpl_comp(li->dest, comps, li->num_files, allocator, fftset, prefilter);
		if (err != NULL)
			return err;

		if (li->on_loaded != NULL)
			li->on_loaded(li);
	}
	return NULL;
}
