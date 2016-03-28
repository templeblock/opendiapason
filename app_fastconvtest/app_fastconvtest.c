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
#include "cop/cop_alloc.h"
#include "opendiapason/fastconv.h"

static const float TEST_KERNEL[] = {0.25, 0.5, 0.75, 1.0, 0.5};

#define TEST_INSIZE  (50)
#define KERN_SIZE    (sizeof(TEST_KERNEL) / sizeof(TEST_KERNEL[0]))
#define TEST_OUTSIZE (TEST_INSIZE+KERN_SIZE-1)

int main(int argc, char *argv[])
{
	struct fastconv_fftset convs;
	const struct fastconv_pass *test;
	float *inbuf;
	float *outbuf;
	float *scratch;
	float *kern;
	unsigned i;
	unsigned fftlen;

	struct aalloc mem;

	fftlen = fftset_recommend_conv_length(KERN_SIZE, TEST_INSIZE);

	printf("using fft size of %d\n", fftlen);

	aalloc_init(&mem, 16, 32768);

	fastconv_fftset_init(&convs);

	test = fastconv_get_real_conv(&convs, fftlen);

	inbuf   = aalloc_align_alloc(&mem, fftlen * sizeof(float), 64);
	outbuf  = aalloc_align_alloc(&mem, fftlen * sizeof(float), 64);
	scratch = aalloc_align_alloc(&mem, fftlen * sizeof(float), 64);
	kern    = aalloc_align_alloc(&mem, fftlen * sizeof(float), 64);

	for (i = 0; i < KERN_SIZE; i++) {
		inbuf[i] = TEST_KERNEL[i] / fftlen;
	}
	for (; i < fftlen; i++) {
		inbuf[i] = 0.0;
	}

	fastconv_execute_fwd(test, inbuf, kern);

	for (i = 0; i < TEST_INSIZE; i++) {
		inbuf[i] = 1.0;//(i<30)?1.0:0.0;//cos(i*2.0*M_PI*0.5/TEST_INSIZE);
	}
	for (; i < fftlen; i++) {
		inbuf[i] = 0.0;
	}

	fastconv_execute_conv(test, inbuf, kern, outbuf, scratch);

	for (i = 0; i < TEST_OUTSIZE; i++) {
		printf("%d,%f\n", i, outbuf[i]);
	}

	for (i = 0; i < fftlen; i++) {
		inbuf[i] = i==3?1.0:0.0;//sin(i * 2.0 * M_PI * 10.5 / fftlen);
	}

	fastconv_execute_fwd_reord(test, inbuf, outbuf, scratch);



	for (i = 0; i < fftlen/2; i++) {
		printf("%d,%f,%f\n", i, outbuf[2*i], outbuf[2*i+1]);
	}
#if 1
	fastconv_execute_rev_reord(test, outbuf, inbuf, scratch);
	for (i = 0; i < fftlen; i++) {
		printf("%d,%f\n", i, inbuf[i]);
	}
#endif


#if 1
	fftlen  = 1024;
	test    = fastconv_get_real_conv(&convs, fftlen);
	inbuf   = aalloc_align_alloc(&mem, fftlen * sizeof(float), 64);
	outbuf  = aalloc_align_alloc(&mem, fftlen * sizeof(float), 64);
	scratch = aalloc_align_alloc(&mem, fftlen * sizeof(float), 64);

	for (i = 0; i < 3000000; i++) {
		unsigned j;
		for (j = 0; j < fftlen; j++) {
			inbuf[j] = 0.0f;
		}
		fastconv_execute_fwd(test, inbuf, outbuf);
//		fastconv_execute_fwd_reord(test, inbuf, outbuf, scratch);
	}
#endif




	fastconv_fftset_destroy(&convs);
	aalloc_free(&mem);

}
