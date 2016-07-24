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

#define SHORT_WINDOW_LENGTH (5)
#define LONG_WINDOW_LENGTH  (4095) /* ~ 43 ms at 48000 */
#define NB_XCDATA           (256)

struct scaninfo {
	uint_fast32_t position;
	float         rms3;
	float         rms_long;
};

void sort_scinfo(struct scaninfo *inout, struct scaninfo *scratch, uint_fast32_t nb_scinfo)
{
	uint_fast32_t l1, l2;
	struct scaninfo *p1, *p2;

	if (nb_scinfo == 2) {
		if (inout[0].rms3 < inout[1].rms3) {
			struct scaninfo tmp = inout[0];
			inout[0] = inout[1];
			inout[1] = tmp;
		}
		return;
	}

	memcpy(scratch, inout, nb_scinfo * sizeof(struct scaninfo));
	l1 = nb_scinfo / 2;
	l2 = nb_scinfo - l1;
	p1 = scratch;
	p2 = scratch + l1;
	if (l1 > 1)
		sort_scinfo(p1, inout, l1);
	if (l2 > 1)
		sort_scinfo(p2, inout, l2);

	while (l1 && l2) {
		if (p1->rms3 > p2->rms3) {
			*inout++ = *p1++;
			l1--;
		} else {
			*inout++ = *p2++;
			l2--;
		}
	}

	while (l1--)
		*inout++ = *p1++;

	while (l2--)
		*inout++ = *p2++;
}

struct xcdata {
	uint_fast32_t p1;
	uint_fast32_t p2;
	float         xc;
	float         pratio;
	float         mratio;
};

void sort_xcinfo(struct xcdata *inout, struct xcdata *scratch, uint_fast32_t nb_xcinfo)
{
	uint_fast32_t l1, l2;
	struct xcdata *p1, *p2;

	if (nb_xcinfo == 2) {
		if (inout[0].xc > inout[1].xc) {
			struct xcdata tmp = inout[0];
			inout[0] = inout[1];
			inout[1] = tmp;
		}
		return;
	}

	memcpy(scratch, inout, nb_xcinfo * sizeof(struct xcdata));
	l1 = nb_xcinfo / 2;
	l2 = nb_xcinfo - l1;
	p1 = scratch;
	p2 = scratch + l1;
	if (l1 > 1)
		sort_xcinfo(p1, inout, l1);
	if (l2 > 1)
		sort_xcinfo(p2, inout, l2);

	while (l1 && l2) {
		if (p1->xc < p2->xc) {
			*inout++ = *p1++;
			l1--;
		} else {
			*inout++ = *p2++;
			l2--;
		}
	}

	while (l1--)
		*inout++ = *p1++;

	while (l2--)
		*inout++ = *p2++;
}

static float cross(float *a, float *b, unsigned len)
{
	float acc = 0.0f;
	while (len--) {
		acc += *a++ * *b++;
	}
	return acc;
}

int main(int argc, char *argv[])
{
	struct cop_filemap  infile;
	struct smplwav      sample;
	struct aalloc       mem;
	unsigned            err;
	float              *wave_data;
	uint_fast32_t       chanstride;
	struct scaninfo    *scinfo;
	float              *tmp_buf;
	float              *square_buf;
	uint_fast32_t       i, j;
	uint_fast32_t       nb_scinfo;
	uint_fast32_t       nb_xc;
	struct xcdata      *xcbuf;
	struct xcdata      *xcscratch;

	if (argc < 2) {
		fprintf(stderr, "need a filename\n");
		return -1;
	}

	if (cop_filemap_open(&infile, argv[1], COP_FILEMAP_FLAG_R)) {
		fprintf(stderr, "could not open file '%s'\n", argv[1]);
		return -1;
	}

	if (SMPLWAV_ERROR_CODE(err = smplwav_mount(&sample, infile.ptr, infile.size, 0))) {
		cop_filemap_close(&infile);
		fprintf(stderr, "could not load '%s' as a waveform sample %u\n", argv[1], SMPLWAV_ERROR_CODE(err));
		return -1;
	}

	if (err)
		fprintf(stderr, "%s had issues (%u). check the output file carefully.\n", argv[1], err);

	aalloc_init(&mem, 1024*1024*256, 32, 1024*1024);

	/* Allocate memory for the various awful things this program does. */
	chanstride = ((sample.data_frames + VLF_WIDTH - 1) / VLF_WIDTH) * VLF_WIDTH;
	if  (   (wave_data  = aalloc_align_alloc(&mem, sizeof(float) * chanstride * sample.format.channels, 32)) == NULL
		||  (tmp_buf    = aalloc_align_alloc(&mem, sizeof(float) * sample.data_frames, 32)) == NULL
		||  (square_buf = aalloc_align_alloc(&mem, sizeof(float) * sample.data_frames, 32)) == NULL
		||  (scinfo     = aalloc_align_alloc(&mem, sizeof(*scinfo) * sample.data_frames * 2, 32)) == NULL
		||  (xcbuf      = aalloc_align_alloc(&mem, sizeof(*xcbuf) * (NB_XCDATA * (NB_XCDATA + 1) / 2), 32)) == NULL
		||  (xcscratch  = aalloc_align_alloc(&mem, sizeof(*xcscratch) * (NB_XCDATA * (NB_XCDATA + 1) / 2), 32)) == NULL
		) {
		cop_filemap_close(&infile);
		aalloc_free(&mem);
		fprintf(stderr, "out of memory\n");
		return -1;
	}

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

	/* Build the very short-term power info. This is effectively the power of
	 * a SHORT_WINDOW_LENGTH window of the input. It is meant to be almost
	 * the instantaneous power. */
	{
#if SHORT_WINDOW_LENGTH == 5
		float a, b, c, d, e;
		a = square_buf[0];
		b = square_buf[1];
		c = square_buf[2];
		d = square_buf[3];
		e = square_buf[4];
		tmp_buf[0] = 0.0f;
		tmp_buf[1] = 0.0f;
		for (i = 0; i + 5 < sample.data_frames; i++) {
			tmp_buf[i+2] = a + b + c + d + e;
			a = b;
			b = c;
			c = d;
			d = e;
			e = square_buf[i+5];
		}
		tmp_buf[i+2] = 0.0f;
		tmp_buf[i+3] = 0.0f;
#else
#error "reimplement this if you change SHORT_WINDOW_LENGTH"
#endif
	}

	/* Find all of the peaks in the short-term power window. The positions and
	 * values of these peaks get put into the scinfo buffer. */
	for (i = 0, nb_scinfo = 0; i + 2 < sample.data_frames; i++) {
		float a = tmp_buf[i];
		float b = tmp_buf[i+1];
		float c = tmp_buf[i+2];

		if (i+1 < (LONG_WINDOW_LENGTH-1)/2)
			continue;
		if ((i+1) + (LONG_WINDOW_LENGTH+1)/2 > sample.data_frames)
			continue;

		if (b > a && b > c) {
			scinfo[nb_scinfo].position = i+1;
			scinfo[nb_scinfo].rms3 = sqrtf(b);
			nb_scinfo++;
		}
	}

	/* Sort the list of peaks by their short-term power levels. This will give
	 * us a list where things which we can join should all live fairly close
	 * to each other. */
	sort_scinfo(scinfo, scinfo + nb_scinfo, nb_scinfo);

	/* Trim the end of the list. We don't want to look for loops in a
	 * release. We do this by finding the average power of the first half of
	 * the list and then pruning the end of the list until we reach a value
	 * within 3-dB of the average power. */
	{
		float thr = 0.0f;
		for (i = 0; i < nb_scinfo / 2; i++) 
			thr += scinfo[i].rms3;
		thr *= 0.5f / (nb_scinfo / 2);
		while (nb_scinfo && scinfo[nb_scinfo-1].rms3 < thr)
			nb_scinfo--;
	}

	/* Find best range of NB_XCDATA elements. */
	{
		unsigned start_range = 0;
		unsigned nb_xcd;
		float best_range = scinfo[0].rms3;

		for (i = 0; i + NB_XCDATA < nb_scinfo; i++) {
			float diff = scinfo[i].rms3 - scinfo[i+NB_XCDATA].rms3;
			if (diff < best_range) {
				best_range = diff;
				start_range = i;
			}
		}

		if (start_range + NB_XCDATA > nb_scinfo) {
			nb_xcd = nb_scinfo - start_range;
		} else {
			nb_xcd = NB_XCDATA;
		}

		/* Get long RMS power levels. */
		for (i = 0; i < nb_xcd; i++) {
			float acc = 0.0f;
			float *wd = square_buf + scinfo[start_range+i].position - (LONG_WINDOW_LENGTH-1)/2;
			unsigned k;

			for (k = 0; k < LONG_WINDOW_LENGTH; k++)
				acc += wd[k];

			scinfo[start_range+i].rms_long = acc;
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

				for (ch = 0; ch < sample.format.channels; ch++, wd += chanstride) {
					float *ps1;
					float *ps2;

					ps1 = wd + scinfo[start_range+i].position - (LONG_WINDOW_LENGTH-1)/2;
					ps2 = wd + scinfo[start_range+j].position - (LONG_WINDOW_LENGTH-1)/2;
					acc1 += cross(ps1, ps2, LONG_WINDOW_LENGTH);

					ps1 = wd + scinfo[start_range+i].position - (SHORT_WINDOW_LENGTH-1)/2;
					ps2 = wd + scinfo[start_range+j].position - (SHORT_WINDOW_LENGTH-1)/2;
					acc2 += cross(ps1, ps2, SHORT_WINDOW_LENGTH);
				}

				xcbuf[nb_xc].p1 = i;
				xcbuf[nb_xc].p2 = j;

				tmp1 = scinfo[start_range+i].rms_long;
				tmp2 = scinfo[start_range+j].rms_long;
				xcbuf[nb_xc].xc = (tmp2 + tmp1 - 2.0f * acc1) / (tmp2 + tmp1);

				tmp1 = scinfo[start_range+i].rms3;
				tmp2 = scinfo[start_range+j].rms3;
				tmp1 *= tmp1;
				tmp2 *= tmp2;
				xcbuf[nb_xc].pratio = (tmp1 + tmp2 - 2.0f * acc2) / (tmp2 + tmp1);
				xcbuf[nb_xc].mratio = tmp1 / tmp2;
			}
		}

		/* Sort sc elements. */
		sort_xcinfo(xcbuf, xcscratch, nb_xc);
		for (i = 0, j = 0; i < nb_xc && j < 50; i++) {
			unsigned p1 = scinfo[xcbuf[i].p1+start_range].position;
			unsigned p2 = scinfo[xcbuf[i].p2+start_range].position;
			unsigned ps = (p1 > p2) ? p2 : p1;
			unsigned pe = (p1 > p2) ? p1 : p2;

			if (pe - ps > 24000) {
				printf("%u,%u,%f,%f,%f\n", ps, pe-ps, 10.0 * log10(xcbuf[i].xc), 10.0 * log10(xcbuf[i].pratio), 10.0 * log10(xcbuf[i].mratio));
				j++;
			}
		}

	}

	aalloc_free(&mem);
	cop_filemap_close(&infile);

	return 0;
}
