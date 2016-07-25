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

#include <stdio.h>
#include <math.h>
#include "cop/cop_vec.h"
#include "cop/cop_filemap.h"
#include "cop/cop_alloc.h"
#include "smplwav/smplwav_mount.h"
#include "smplwav/smplwav_serialise.h"
#include "smplwav/smplwav_convert.h"
#include "opendiapason/odfilter.h"

/* How the algorithm works
 *
 * 1) Find the total RMS power of the input signal and using this, trim the
 *    ends of the signal so we don't hunt for loops in releases or the attack.
 *    This is a somewhat abitrary condition...
 * 2) Find the power-envelope over LONG_WINDOW_LENGTH samples throughout the
 *    selected region.
 * 3) Find a rolling 5-sample (SHORT_WINDOW_LENGTH-sample) RMS window of the
 *    entire input signal. Collect all of the peaks in this (along with their
 *    sample indexes into a list) and sort the list based on the RMS value.
 *    This gives a list where if there are many values which all have very
 *    similar RMS values, they are "likely" to make good loop points.
 * 4) Find ranges of very similar RMS values in the above list and create a
 *    correlation matrix which maps each sample to each other sample over a
 *    period of LONG_WINDOW_LENGTH. This is CPU intensive and means that the
 *    ranges we search over should be limited. The complexity goes up with
 *    sample pitch.
 * 5) Using the correlation matrix and the envelop, we can convert the
 *    correlation matrix into a mean-squared-error matrix mapping the error of
 *    looping between each possible point. The values closest to zero will
 *    introduce the minimum overall tonal change when using the two loop
 *    points.
 * 6) For each possible loop in the range found in 4, we can now measure the
 *    short term energy difference (using the short RMS window values and a
 *    short correlation measurement between the points) which in-a-way maps to
 *    the likelyhood of hearing any click and we have a long-term correlation
 *    measurement representing the tambor shift. We use a heuristic to pick
 *    the best sections to loop using these two metrics.
 *
 * Because of the grouping in 4, many samples in the same vicinity end up
 * getting picked. This is a good thing, but it also means that we can end up
 * with many loops which all end up having the same duration with slightly
 * different offsets. We prune these off the list at the end. */

#define SHORT_WINDOW_LENGTH (5)    /* ~ 100us at 48 kHz */
#define LONG_WINDOW_LENGTH  (3801) /* ~ 100ms at 48 kHz */
#define MAX_NB_XCDATA       (256)

#define BUILD_MERGESORT(name_, data_, comparison_) \
void sort_ ## name_(data_ *inout, data_ *scratch, uint_fast32_t nb_elements) \
{ \
	if (nb_elements <= 8) { \
		uint_fast32_t i, j; \
		for (i = 0; i < nb_elements; i++) { \
			uint_fast32_t biggest = i; \
			for (j = i+1; j < nb_elements; j++) { \
				if (comparison_((inout + biggest), (inout + j))) \
					biggest = j; \
			} \
			if (biggest != i) { \
				data_ tmp = inout[biggest]; \
				inout[biggest] = inout[i]; \
				inout[i] = tmp; \
			} \
		} \
	} else { \
		uint_fast32_t l1 = nb_elements / 2; \
		uint_fast32_t l2 = nb_elements - l1; \
		data_ *p1 = scratch; \
		data_ *p2 = scratch + l1; \
		memcpy(scratch, inout, nb_elements * sizeof(data_)); \
		sort_ ## name_(p1, inout, l1); \
		sort_ ## name_(p2, inout, l2); \
		while (l1 && l2) { \
			if (comparison_(p1, p2)) { \
				*inout++ = *p2++; \
				l2--; \
			} else { \
				*inout++ = *p1++; \
				l1--; \
			} \
		} \
		while (l1--) \
			*inout++ = *p1++; \
		while (l2--) \
			*inout++ = *p2++; \
	} \
}

struct scaninfo {
	uint_fast32_t position;
	float         rms3;
	float         rms_long;
};

struct xcdata {
	uint_fast32_t p1;
	uint_fast32_t p2;
	float         xc;
	float         pratio;
	float         mratio;
};

#define SCINFO_COMPARISON(a_, b_) ((a_)->rms3 < (b_)->rms3)
BUILD_MERGESORT(scinfo, struct scaninfo, SCINFO_COMPARISON)
#undef SCINFO_COMPARISON

#define XCINFO_COMPARISON(a_, b_) ((a_)->xc > (b_)->xc)
BUILD_MERGESORT(xcinfo, struct xcdata, XCINFO_COMPARISON)
#undef XCINFO_COMPARISON

static float cross(float *a, float *b, unsigned len)
{
	float acc = 0.0f;
	while (len--) {
		acc += *a++ * *b++;
	}
	return acc;
}

/* A recursive summation to increase floating point accuracy in summation. */
static float accusum(float *buf, unsigned len)
{
	unsigned i;
	float sum;

	if (len > 64) {
		unsigned pivot = len/2;
		return accusum(buf, pivot) + accusum(buf + pivot, len - pivot);
	}

	for (i = 0, sum = 0.0f; i < len; i++)
		sum += buf[i];

	return sum;
}

int
do_processing
	(struct smplwav *sample
	,struct aalloc  *mem
	,float          *wave_data
	,float          *square_buf
	,float          *envelope_buf
	,uint_fast32_t   chanstride
	,uint_fast32_t   buf_start
	,uint_fast32_t   buf_len
	)
{
	struct scaninfo    *scinfo;
	float              *tmp_buf;
	unsigned            i, j;
	uint_fast32_t       nb_scinfo;
	uint_fast32_t       nb_xc, sc_start;
	struct xcdata      *xcbuf;
	struct xcdata      *xcscratch;
	struct xcdata      *xcresults;
	struct xcdata      *xcresults2;
	struct xcdata      *xcresults3;
	unsigned            nb_results = 0;

	if  (   (tmp_buf      = aalloc_align_alloc(mem, sizeof(float) * buf_len, 32)) == NULL
	    ||  (scinfo       = aalloc_align_alloc(mem, sizeof(*scinfo) * buf_len * 2, 32)) == NULL
	    ||  (xcbuf        = aalloc_align_alloc(mem, sizeof(*xcbuf) * (MAX_NB_XCDATA * (MAX_NB_XCDATA + 1) / 2), 32)) == NULL
	    ||  (xcscratch    = aalloc_align_alloc(mem, sizeof(*xcscratch) * (MAX_NB_XCDATA * (MAX_NB_XCDATA + 1) / 2), 32)) == NULL
	    ||  (xcresults    = aalloc_align_alloc(mem, sizeof(*xcscratch) * MAX_NB_XCDATA, 32)) == NULL
	    ||  (xcresults2   = aalloc_align_alloc(mem, sizeof(*xcscratch) * MAX_NB_XCDATA, 32)) == NULL
	    ||  (xcresults3   = aalloc_align_alloc(mem, sizeof(*xcscratch) * MAX_NB_XCDATA, 32)) == NULL
	    ) {
		fprintf(stderr, "out of memory\n");
		return -1;
	}

	/* Build the very short-term power info. This is effectively the power of
	 * a SHORT_WINDOW_LENGTH window of the input. It is meant to be almost
	 * the instantaneous power. */
	{
#if SHORT_WINDOW_LENGTH == 5
		float a = square_buf[buf_start-2];
		float b = square_buf[buf_start-1];
		float c = square_buf[buf_start+0];
		float d = square_buf[buf_start+1];
		for (i = 0; i < buf_len; i++) {
			float e    = square_buf[buf_start+2+i];
			tmp_buf[i] = a + b + c + d + e;
			a          = b;
			b          = c;
			c          = d;
			d          = e;
		}
#else
#error "reimplement this if you change SHORT_WINDOW_LENGTH"
#endif
	}

	/* Find all of the peaks in the short-term power window. The positions and
	 * values of these peaks get put into the scinfo buffer. */
	{
		float a = tmp_buf[0];
		float b = tmp_buf[1];
		for (i = 0, nb_scinfo = 0; i + 2 < buf_len; i++) {
			float c = tmp_buf[2+i];
			if (b > a && b > c) {
				scinfo[nb_scinfo].position = buf_start+i+1;
				scinfo[nb_scinfo].rms3     = sqrtf(b);
				nb_scinfo++;
			}
			a = b;
			b = c;
		}
	}

	/* Sort the list of peaks by their short-term power levels. This will give
	 * us a list where things which we can join should all live fairly close
	 * to each other. */
	sort_scinfo(scinfo, scinfo + nb_scinfo, nb_scinfo);

	/* look for good search regions... */
	sc_start = 0;
	while (sc_start+1 < nb_scinfo) {
		float base = scinfo[sc_start].rms3;
		uint_fast32_t sc_end = sc_start+1;
		while (sc_end < nb_scinfo && sc_end-sc_start < MAX_NB_XCDATA) {
			float new = scinfo[sc_end].rms3;
			if (new / base < 0.99)
				break;
			sc_end++;
		}

		if (sc_end - sc_start > 32) {
			uint_fast32_t nb_xcd = sc_end - sc_start;
			unsigned new_results = 0;
			struct xcdata *swaptmp;

			/* Get long RMS power levels. */
			for (i = 0; i < nb_xcd; i++) {
				scinfo[sc_start+i].rms_long = envelope_buf[scinfo[sc_start+i].position];
			}

			/* Build the triangular correlation matrix. */
			nb_xc = 0;
			for (i = 1; i < nb_xcd; i++) {
				for (j = 0; j < i; j++, nb_xc++) {
					float    *wd = wave_data;
					unsigned  ch;
					float     acc1 = 0.0f;
					float     acc2 = 0.0f;
					float     tmp1;
					float     tmp2;

					for (ch = 0; ch < sample->format.channels; ch++, wd += chanstride) {
						float *ps1;
						float *ps2;

						ps1 = wd + scinfo[sc_start+i].position - (LONG_WINDOW_LENGTH-1)/2;
						ps2 = wd + scinfo[sc_start+j].position - (LONG_WINDOW_LENGTH-1)/2;
						acc1 += cross(ps1, ps2, LONG_WINDOW_LENGTH);

						ps1 = wd + scinfo[sc_start+i].position - (SHORT_WINDOW_LENGTH-1)/2;
						ps2 = wd + scinfo[sc_start+j].position - (SHORT_WINDOW_LENGTH-1)/2;
						acc2 += cross(ps1, ps2, SHORT_WINDOW_LENGTH);
					}

					xcbuf[nb_xc].p1 = i+sc_start;
					xcbuf[nb_xc].p2 = j+sc_start;

					tmp1 = scinfo[sc_start+i].rms_long;
					tmp2 = scinfo[sc_start+j].rms_long;
					xcbuf[nb_xc].xc = (tmp2 + tmp1 - 2.0f * acc1) / (tmp2 + tmp1);

					tmp1 = scinfo[sc_start+i].rms3;
					tmp2 = scinfo[sc_start+j].rms3;
					tmp1 *= tmp1;
					tmp2 *= tmp2;
					xcbuf[nb_xc].pratio = (tmp1 + tmp2 - 2.0f * acc2) / (tmp2 + tmp1);
					xcbuf[nb_xc].mratio = tmp1 / tmp2;
				}
			}

			/* Sort sc elements. */
			sort_xcinfo(xcbuf, xcscratch, nb_xc);
			for (i = 0, j = 0; i < nb_xc && j < MAX_NB_XCDATA; i++) {
				unsigned p1 = scinfo[xcbuf[i].p1].position;
				unsigned p2 = scinfo[xcbuf[i].p2].position;
				unsigned ps = (p1 > p2) ? p2 : p1;
				unsigned pe = (p1 > p2) ? p1 : p2;
				if (pe - ps > 24000) {
					xcresults2[j] = xcbuf[i];
					j++;
				}
			}

			nb_xc = j;

			swaptmp = xcresults3;
			xcresults3 = xcresults;
			xcresults = swaptmp;

			/* Merge. */
			i = 0;
			j = 0;
			new_results = 0;
			while (i < nb_xc && j < nb_results && new_results < MAX_NB_XCDATA) {
				if (xcresults2[i].xc < xcresults3[j].xc) {
					xcresults[new_results++] = xcresults2[i++];
				} else {
					xcresults[new_results++] = xcresults3[j++];
				}
			}
			while (i < nb_xc && new_results < MAX_NB_XCDATA) {
				xcresults[new_results++] = xcresults2[i++];
			}
			while (j < nb_results && new_results < MAX_NB_XCDATA) {
				xcresults[new_results++] = xcresults3[j++];
			}

			nb_results = new_results;
		}

		sc_start = sc_end;
	}

	for (i = 0; i < nb_results; i++) {
		unsigned p1 = scinfo[xcresults[i].p1].position;
		unsigned p2 = scinfo[xcresults[i].p2].position;
		unsigned ps = (p1 > p2) ? p2 : p1;
		unsigned pe = (p1 > p2) ? p1 : p2;
		printf("%u,%u,%f,%f,%f\n", ps, pe-ps, 10.0 * log10(xcresults[i].xc), 10.0 * log10(xcresults[i].pratio), 10.0 * log10(xcresults[i].mratio));


	}

	return 0;
}


int main(int argc, char *argv[])
{
	struct cop_filemap  infile;
	struct smplwav      sample;
	struct aalloc       mem;
	int                 err;
	float              *wave_data;
	uint_fast32_t       chanstride;
	float              *square_buf;
	float              *envelope_buf;
	uint_fast32_t       i;

	uint_fast32_t               start_search;
	uint_fast32_t               end_search;
	struct odfilter             filter;
	struct odfilter_temporaries filter_temps;
	struct fftset               fftset;

	if (argc < 2) {
		fprintf(stderr, "need a filename\n");
		return -1;
	}

	if (cop_filemap_open(&infile, argv[1], COP_FILEMAP_FLAG_R)) {
		fprintf(stderr, "could not open file '%s'\n", argv[1]);
		return -1;
	}

	{
		unsigned uerr;
		if (SMPLWAV_ERROR_CODE(uerr = smplwav_mount(&sample, infile.ptr, infile.size, 0))) {
			cop_filemap_close(&infile);
			fprintf(stderr, "could not load '%s' as a waveform sample %u\n", argv[1], SMPLWAV_ERROR_CODE(uerr));
			return -1;
		}
		if (uerr)
			fprintf(stderr, "%s had issues (%u). check the output file carefully.\n", argv[1], uerr);
	}

	if (sample.data_frames < 2*LONG_WINDOW_LENGTH) {
		cop_filemap_close(&infile);
		fprintf(stderr, "not enought data in '%s' to loop.\n", argv[1]);
		return -1;
	}

	aalloc_init(&mem, 1024*1024*256, 32, 1024*1024);
	fftset_init(&fftset);

	/* Allocate memory for the various awful things this program does. */
	chanstride = ((sample.data_frames + VLF_WIDTH - 1) / VLF_WIDTH) * VLF_WIDTH;
	if  (   (wave_data    = aalloc_align_alloc(&mem, sizeof(float) * chanstride * sample.format.channels, 32)) == NULL
		||  (square_buf   = aalloc_align_alloc(&mem, sizeof(float) * sample.data_frames, 32)) == NULL
		||  (envelope_buf = aalloc_align_alloc(&mem, sizeof(float) * sample.data_frames, 32)) == NULL
		||  (odfilter_init_filter(&filter, &mem, &fftset, LONG_WINDOW_LENGTH))
		||  (odfilter_init_temporaries(&filter_temps, &mem, &filter))
		) {
		fftset_destroy(&fftset);
		aalloc_free(&mem);
		cop_filemap_close(&infile);
		fprintf(stderr, "out of memory\n");
		return -1;
	}

	/* Build the long filter to get the envelope of the input. */
	odfilter_build_rect(&filter, &filter_temps, LONG_WINDOW_LENGTH, 1.0f);

	/* Convert the wave data into floating point. */
	smplwav_convert_deinterleave_floats(wave_data, chanstride, sample.data, sample.data_frames, sample.format.channels, sample.format.format);

	/* For every sample, sum the squares of each channel into square_buf. */
	if (sample.format.channels == 2) {
		float *left  = wave_data;
		float *right = wave_data + chanstride;
		for (i = 0; i < sample.data_frames; i++) {
			square_buf[i] = left[i] * left[i] + right[i] * right[i];
		}
	} else {
		float *chptr = wave_data;
		unsigned ch;
		for (i = 0; i < sample.data_frames; i++) {
			square_buf[i] = chptr[i] * chptr[i];
		}
		for (ch = 1; ch < sample.format.channels; ch++) {
			chptr += chanstride;
			for (i = 0; i < sample.data_frames; i++) {
				square_buf[i] += chptr[i] * chptr[i];
			}
		}
	}

	/* Create the envelope of the signal. */
	odfilter_run(square_buf, envelope_buf, 0, 0, sample.data_frames, (LONG_WINDOW_LENGTH-1)/2, 0, &filter_temps, &filter);

	/* Find a decent search region. This hopefully chops the releases off the
	 * search region. */
	{
		float total_ms_power = LONG_WINDOW_LENGTH * accusum(square_buf, sample.data_frames) / sample.data_frames;
		start_search = (LONG_WINDOW_LENGTH-1)/2;
		end_search   = sample.data_frames - start_search;
		for (; start_search < end_search && envelope_buf[start_search] < total_ms_power * 0.5; start_search++);
		for (; start_search < end_search && envelope_buf[end_search] < total_ms_power * 0.5; end_search--);
	}

	err =
		do_processing
			(&sample
			,&mem
			,wave_data
			,square_buf
			,envelope_buf
			,chanstride
			,start_search
			,end_search - start_search + 1
			);

	fftset_destroy(&fftset);
	aalloc_free(&mem);
	cop_filemap_close(&infile);

	return err;
}
