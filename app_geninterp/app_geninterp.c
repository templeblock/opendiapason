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

#include <math.h>
#include <stdio.h>
#include <assert.h>
#include "cop/cop_vec.h"
#include "cop/cop_alloc.h"
#include "fftset/fftset.h"
#include "svgplot/svgplot.h"

/* This thing is disgusting... it vomits out three things:
 *
 *   1) the interpolation filters to be used
 *   2) a filter kernel which should be applied before interpolation to
 *      flatten out the frequency response
 *   3) an SVG graph showing the performance of the interpolation filter, the
 *      inverse interpolation filter and the combined response.
 *
 * The first two get dumped to stdout and should be piped directly into the
 * coefficients file. The third gets dumped into a file called "responses.svg"
 * in the current working directory.
 *
 * There is no optimal design algorithm in this code - the filters are Kaiser
 * windowed frequency-domain designed. There are several coefficients which
 * control the characteristics of the interpolation filter which are baked
 * into this code (the Kaiser window parameter, the "curve" of the frequency
 * domain spectrum and the cutoff frequency of the frequency domain filter).
 *
 * You probably don't really want to play around with this.
 *
 * TODO: Make this code nicer.
 * TODO: Get rid of the stupid compressor and use a target curve to create
 *       the inverse response. */

#define SMPL_POSITION_SCALE  (16384)
#define SMPL_INTERP_TAPS     (8)
#define FILTER_LEN           (SMPL_POSITION_SCALE * SMPL_INTERP_TAPS)
#define INVERSE_FILTER_LEN   (192)

static double filter[FILTER_LEN];

/* Implements the 0th order modified Bessel function of the first kind. */
static double I0(double x)
{
	static const double tol = 1e-12;
	double ds  = 1.0;
	double d   = 0.0;
	double sum = 1.0;

	x *= x;

	do {
		d   += 2;
		ds  *= x / (d * d);
		sum += ds;
	} while (ds > tol * sum);

	return sum;
}

static void apply_kaiser(double *data, unsigned N, double alpha)
{
	unsigned i;
	double denom = 1.0 / I0(M_PI * alpha);
	for (i = 0; i < N; i++) {
		double n = i * 2.0 / (N - 1.0) - 1.0;
		data[i] *= denom * I0(M_PI * alpha * sqrt(1.0 - n * n));
	}
}

static double bent_sinc(double f, double alpha)
{
	unsigned i;
	double r = 0.0;
	double m = 0.0;
	for (i = 0; i < 200; i++) {
		double g = pow(alpha, i / 100.0);
		m += g;
		r += cos(f * (i / 200.0)) * g;
	}
	r = r / m;
	return r;
}

static void l1_norm(double *filter, unsigned len, double scale)
{
	unsigned i;
	double norm = 0.0;
	for (i = 0; i < len; i++)
		norm += filter[i];
	norm = scale / norm;
	for (i = 0; i < len; i++)
		filter[i] *= norm;
}

int main(int argc, char *argv[])
{
	unsigned i;
	const unsigned fft_size      = 2048;
	float  VEC_ALIGN_BEST fft_buf[2048];
	float  VEC_ALIGN_BEST tmp_buf[2048];
	float  VEC_ALIGN_BEST tmp2_buf[2048];
	double VEC_ALIGN_BEST inv_buf[2048];
	double plot_x_buf[1024];
	double plot_interp_filter[1024];
	double plot_inverse_filter[1024];
	double plot_combined_filter[1024];
	double restored_y_buf[1024];
	double tmp_double[2048];

	struct fftset convs;
	const struct fftset_fft *fft;

	fftset_init(&convs);

	fft     = fftset_create_fft(&convs, FFTSET_MODULATION_FREQ_OFFSET_REAL, fft_size / 2);

	/* 1) Build the interpolation filter and normalise the DC component to
	 *    have unity gain. */
	for (i = 0; i < FILTER_LEN-1; i++) {
		int    t  = (int)i - (int)(SMPL_POSITION_SCALE * SMPL_INTERP_TAPS / 2 - 1);
		double f  = 0.5 / SMPL_POSITION_SCALE;
		double sv = bent_sinc(M_PI * t * f, 1.7);
		filter[i] = sv;
	}
	apply_kaiser(filter, FILTER_LEN-1, 1.8);
	filter[i] = 0;
	l1_norm(filter, FILTER_LEN, SMPL_POSITION_SCALE);

	/* 2) Find the combined magnitude spectrum of the interpolation filter by
	 *    summing all of the polyphase components. */
	for (i = 0; i < fft_size; i++) {
		fft_buf[i] = 0.0;
	}
	for (i = 0; i < SMPL_INTERP_TAPS; i++) {
		unsigned j;
		for (j = 0; j < SMPL_POSITION_SCALE; j++) {
			fft_buf[i] += (float)(filter[i * SMPL_POSITION_SCALE + j] / SMPL_POSITION_SCALE);
		}
	}
	fftset_fft_forward(fft, fft_buf, tmp_buf, tmp2_buf);

	/* 3) Create plot of interpolation filter magnitude response and create a
	 *    response for the inverse interpolation filter. */
	for (i = 0; i < fft_size/2; i++) {
		double re = tmp_buf[2*i];
		double im = tmp_buf[2*i+1];
		double gain = 10.0 * log10(re * re + im * im);

		assert(re == re && im == im);
		plot_x_buf[i] = 44100 * ((i + 0.5) / fft_size);
		plot_interp_filter[i] = fmin(100.0, fmax(-300, gain));

		double w = (i + 0.5) / (float)(fft_size / 2);
		double co   = 18000.0 * 2.0 / 44100.0;
		double cooe = 21500.0 * 2.0 / 44100.0;
		double target = 10.0 * log10(1.0 / (1.0 + pow(w / co, 38.0)));
		double interp = pow(fmin(fmax(0.0, (w - co) / (cooe - co)), 1.0), 5);
		gain = (1.0 - interp) * (target - gain) + interp * -40.0;

		/* Invert magnitude. */
		gain = pow(10.0, gain * 0.05);

		tmp_buf[2*i+0] = gain * cos(-(INVERSE_FILTER_LEN*0.5-1) * M_PI * (i + 0.5) / (double)(fft_size/2)) / (fft_size/2);
		tmp_buf[2*i+1] = gain * sin(-(INVERSE_FILTER_LEN*0.5-1) * M_PI * (i + 0.5) / (double)(fft_size/2)) / (fft_size/2);
	}

	/* 4) Convert the inverse filter response back into the time-domain,
	 *    truncate it to the required length and window it with a Kaiser
	 *    window (to smooth it out). */
	fftset_fft_inverse(fft, tmp_buf, fft_buf, tmp2_buf);
	for (i = 0; i < INVERSE_FILTER_LEN-1; i++) {
		tmp_double[i] = fft_buf[i] / SMPL_POSITION_SCALE;
	}
	apply_kaiser(tmp_double, INVERSE_FILTER_LEN-1, 3.5);
	for (i = 0; i < INVERSE_FILTER_LEN-1; i++) {
		fft_buf[i] = tmp_double[i];
	}
	for (; i < fft_size; i++) {
		fft_buf[i] = 0.0;
	}
	inv_buf[0] = 0.0;
	for (i = 0; i < INVERSE_FILTER_LEN-1; i++) {
		inv_buf[i+1] = fft_buf[i] * SMPL_POSITION_SCALE;
	}
	fftset_fft_forward(fft, fft_buf, tmp_buf, tmp2_buf);
	for (i = 0; i < fft_size/2; i++) {
		double re = tmp_buf[2*i] * SMPL_POSITION_SCALE;
		double im = tmp_buf[2*i+1] * SMPL_POSITION_SCALE;
		double magnitude = sqrt(re * re + im * im);
		assert(re == re && im == im);
		plot_inverse_filter[i]  = fmin(100.0, fmax(-300, 20.0 * log10(magnitude)));
		plot_combined_filter[i] = plot_interp_filter[i] + plot_inverse_filter[i];
	}

	printf("/* The filter is symmetric and of odd order and introduces a latency of\n");
	printf(" * (INVERSE_FILTER_LEN-1)/2. */\n");
	printf("#define SMPL_INVERSE_FILTER_LEN (%uu)\n", INVERSE_FILTER_LEN-1);
	printf("#define SMPL_POSITION_SCALE     (%uu)\n", SMPL_POSITION_SCALE);
	printf("#define SMPL_INTERP_TAPS        (%uu)\n", SMPL_INTERP_TAPS);
	printf("static const float SMPL_INVERSE_COEFS[SMPL_INVERSE_FILTER_LEN+1] =\n");
	for (i = 0; i < INVERSE_FILTER_LEN; i++) {
		if (i == 0)
			printf("{");
		else
			printf(",");
		printf("%+.10ef\n", inv_buf[i]);
	}
	printf("};\n");

	/* Print the minimum and maximum polyphase filter DC gain value and RMS
	 * power. */
	{
		double n1min, n1max, n2min, n2max;
		for (i = 0; i < SMPL_POSITION_SCALE; i++) {
			unsigned j;
			double fnorm1 = 0.0;
			double fnorm2 = 0.0;
			for (j = 0; j < SMPL_INTERP_TAPS; j++) {
				double in = filter[j * SMPL_POSITION_SCALE + i];
				fnorm1 += in;
				fnorm2 += in * in;
			}
			if (i == 0) {
				n1min = fnorm1;
				n1max = fnorm1;
				n2min = fnorm2;
				n2max = fnorm2;
			} else {
				n1min = (fnorm1 < n1min) ? fnorm1 : n1min;
				n1max = (fnorm1 > n1max) ? fnorm1 : n1max;
				n2min = (fnorm2 < n2min) ? fnorm2 : n2min;
				n2max = (fnorm2 > n2max) ? fnorm2 : n2max;
			}
		}
		printf("/* %f-%f,%f-%f,(%f,%f) */\n", n1min, n1max, sqrt(n2min), sqrt(n2max), n1max - n1min, sqrt(n2max) - sqrt(n2min));
	}

	printf("static const float SMPL_INTERP[%uu][%uu] =\n", SMPL_POSITION_SCALE, SMPL_INTERP_TAPS);
	for (i = 0; i < SMPL_POSITION_SCALE; i++) {
		unsigned j;
		if (i == 0)
			printf("{   {");
		else
			printf(",   {");
		for (j = 0; j < SMPL_INTERP_TAPS; j++) {
			printf("%+.6ef", filter[j * SMPL_POSITION_SCALE + SMPL_POSITION_SCALE-1-i]);
			if (j != SMPL_INTERP_TAPS - 1)
				printf(",");
			else
				printf("}\n");
		}
	}
	printf("};\n\n");

	/* Create and save the response plot. */
	{
		FILE *f = fopen("responses.svg", "w");
		struct svgplot_gridinfo gi;
		struct svgplot plot;
		svgplot_create(&plot);
		svgplot_add_data(&plot, plot_x_buf, plot_interp_filter,   fft_size/2);
		svgplot_add_data(&plot, plot_x_buf, plot_inverse_filter,  fft_size/2);
		svgplot_add_data(&plot, plot_x_buf, plot_combined_filter, fft_size/2);
#if 0
		gi.x.is_log = 1;
		gi.x.sub_divisions = 9;
		gi.x.major_interval = 10;
		gi.x.auto_size = 1;
#else
		gi.x.is_log = 0;
		gi.x.sub_divisions = 5;
		gi.x.major_interval = 5000;
		gi.x.auto_size = 1;
#endif
		gi.x.is_visible = 1;
		gi.x.show_text = 1;
		gi.y.is_log = 0;
		gi.y.is_visible = 1;
		gi.y.major_interval = 10;
		gi.y.sub_divisions = 5;
		gi.y.show_text = 1;
		gi.y.auto_size = 0;
		gi.y.start = -130;
		gi.y.end   = 30;
		svgplot_finalise(&plot, &gi, 12, 12*3/4, 0.2, f);
		fclose(f);
	}
}
