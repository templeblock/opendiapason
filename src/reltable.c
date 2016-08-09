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

#include "reltable.h"
#include "interpdata.h"
#include "wav_dumper.h"
#include <stddef.h>
#include <math.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct relnode {
	double           modfac;
	double           b;

	unsigned         startidx;
	unsigned         endidx;

	/* ideal_error is the sum of all of the errors of the "ideal" positions
	 * to align the release starting at release sample zero. actual_error is
	 * the sum of the errors when the release is aligned according to b and
	 * modfac as defined in this node.
	 *
	 * These factors are used when building the release node tree to figure
	 * out the next node which should be split. While this method seems to
	 * work almost all of the time, it does have some issues with high-
	 * frequency pipes. Because we do not interpolate the correlation signal,
	 * the peaks that we find (i.e. points where the release would align "the-
	 * best" are almost always going to be slightly wrong). We mitigate this
	 * a tiny bit by using linear interpolation between errors, but this only
	 * gets you so far. The end result is that ideal_error could end up being
	 * smaller than actual_error, but actual_error in reality would be smaller
	 * if the signal had been upsampled prior to this operation taking
	 * place. Gross. */
	double           ideal_error;
	double           actual_error;

	/* The gain associated with this node. */
	float            rel_gain;
	float            avg_err;

	struct relnode  *left;
	struct relnode  *right;
};

double
reltable_find
	(const struct reltable *reltable
	,double                 sample
	,float                 *gain
	,float                 *avgerr
	)
{
	unsigned i;
	float tmp;

	assert(reltable->nb_entry);
	for (i = 0; i < reltable->nb_entry-1; i++) {
		if (sample <= reltable->entry[i].last_sample)
			break;
	}

	if (gain != NULL) {
		if (i == 0) {
			*gain = reltable->entry[i].gain;
		} else {
			float sg    = reltable->entry[i-1].gain;
			float eg    = reltable->entry[i].gain;
			unsigned ss = reltable->entry[i-1].last_sample;
			unsigned es = reltable->entry[i].last_sample;
			*gain = sg + (sample - (ss+1)) * (eg - sg) / (es - (ss+1));
		}
	}

	if (avgerr != NULL) {
		*avgerr = reltable->entry[i].avgerr;
	}

	/* TODO: FABS INSERTED TO PREVENT NEGATIVE OUTPUT! MAY BE A BUG ELSEWHERE. */
#if 0
	return fmod(fabs(sample - reltable->entry[i].b), reltable->entry[i].m);
#else
	/* Remember, the position that is passed into the function is the next
	 * position which will be read into the interpolator delay line. The
	 * delay line is of length 8 and we always will want to fill it completely
	 * when we instantiate the release (otherwise we may produce some unwanted
	 * samples at the beginning of the release). */
	tmp = sample - reltable->entry[i].b - (double)SMPL_INTERP_TAPS;
	while (tmp < 0.0)
		tmp += reltable->entry[i].m;
	return (double)SMPL_INTERP_TAPS + fmod(tmp, reltable->entry[i].m);
#endif
}

static
void
build_relnode
	(struct relnode *rn
	,const unsigned *sync_positions
	,const float    *gain_vec
	,const float    *mse_vec
	,unsigned        start_position
	,unsigned        end_position
	,unsigned        error_vec_len
	)
{
	unsigned i;

	assert(end_position > start_position);

	if (end_position - start_position == 0) {
		unsigned  sp = sync_positions[start_position];
		rn->b        = sp;
		rn->rel_gain = gain_vec[start_position];
		rn->modfac   = 1.0;
		rn->avg_err  = mse_vec[sp];
	} else {
		double mean_y = 0.0;
		double mean_x = 0.0;
		double sum_n, sum_d;
		rn->rel_gain = 0.0f;
		rn->avg_err  = 0.0f;
		for (i = start_position; i <= end_position; i++) {
			unsigned sp   = sync_positions[i];
			float tmp     = mse_vec[sp];
			mean_y       += sp;
			mean_x       += i;
			rn->rel_gain  = (gain_vec[i] > rn->rel_gain) ? gain_vec[i] : rn->rel_gain;
			rn->avg_err  += tmp;
		}
		rn->avg_err /= (end_position - start_position + 1);
		mean_y      /= (end_position - start_position + 1);
		mean_x      /= (end_position - start_position + 1);
		sum_n        = 0.0;
		sum_d        = 0.0;
		for (i = start_position; i <= end_position; i++) {
			sum_n += (i - mean_x) * (sync_positions[i] - mean_y);
			sum_d += (i - mean_x) * (i - mean_x);
		}
		rn->modfac = sum_n / sum_d;
		rn->b = mean_y - rn->modfac * mean_x;

		rn->b = rn->modfac * start_position + rn->b;
	}

	rn->startidx    = start_position;
	rn->endidx      = end_position;

	double emin = 0.0;
	double eapprox = 0.0;
	for (i = start_position; i <= end_position; i++) {
		double   approx = (rn->b + (i-start_position) * rn->modfac);
		unsigned actual = sync_positions[i];

		unsigned x1 = (unsigned)floor(approx);
		double   interp = fmin(1.0, fmax(0.0, approx - x1));
		unsigned x2 = x1 + 1;

		x1 = (x1 >= error_vec_len) ? (error_vec_len - 1) : x1;
		x2 = (x2 >= error_vec_len) ? (error_vec_len - 1) : x2;

		emin    += mse_vec[actual];
		eapprox += mse_vec[x1] * (1.0 - interp) + mse_vec[x2] * interp;
	}

	rn->ideal_error = emin;
	rn->actual_error = eapprox;
	rn->left = NULL;
	rn->right = NULL;
}

static
struct relnode *
find_worst_node
	(struct relnode *root
	)
{
	if (root->left != NULL && root->right != NULL) {
		struct relnode *w1 = find_worst_node(root->left);
		struct relnode *w2 = find_worst_node(root->right);
		if (w1->startidx == w1->endidx)
			return w2;
		if (w2->startidx == w2->endidx)
			return w1;
		return (w1->actual_error - w1->ideal_error > w2->actual_error - w2->ideal_error) ? w1 : w2;
	}
	return root;
}

/* Takes an unbalanced release node tree and serialises the leaf nodes from
 * left to right into the release table. */
static
void
recursive_construct_table
	(const struct relnode *node
	,struct reltable      *table
	,const unsigned       *sync_positions
	)
{
	double m;
	double b;
	if (node->left != NULL && node->right != NULL) {
		recursive_construct_table(node->left, table, sync_positions);
		recursive_construct_table(node->right, table, sync_positions);
		return;
	}
	assert(table->nb_entry < RELTABLE_MAX_ENTRIES);
	table->entry[table->nb_entry].last_sample = sync_positions[node->endidx];
	m = node->modfac;
	b = node->b;
	table->entry[table->nb_entry].b = b;
	table->entry[table->nb_entry].m = m;
	table->entry[table->nb_entry].gain = node->rel_gain;
	table->entry[table->nb_entry].avgerr = node->avg_err;
	table->nb_entry++;
}

/* Build an unbalanced tree of release nodes. The worst leaf node is
 * searched for and split until the buffer of nodes is exhausted. */
static
void
reltable_int
	(struct reltable *reltable
	,const unsigned  *sync_positions
	,unsigned         nb_sync_positions
	,const float     *gain_vec
	,const float     *mse_vec
	,unsigned         error_vec_len
	)
{
	struct relnode root;
	struct relnode nodebuf[160];
	unsigned nbuf = 160;

	assert(nb_sync_positions);

	build_relnode(&root, sync_positions, gain_vec, mse_vec, 0, nb_sync_positions-1, error_vec_len);

	while (nbuf > 2) {
		struct relnode *w = find_worst_node(&root);
		unsigned i;
		double eh;

		unsigned stop1, start2;

		if (w->actual_error - w->ideal_error < 0.005 || (w->endidx - w->startidx < 3))
			break;

		for (i = w->startidx, eh = 0.0; i <= w->endidx && eh < w->actual_error; i++) {
			double   approx = (w->b + (i - w->startidx) * w->modfac);
			unsigned x1     = (unsigned)approx;
			double   interp = approx - x1;
			unsigned x2     = x1 + 1;

			x1 = (x1 >= error_vec_len) ? (error_vec_len - 1) : x1;
			x2 = (x2 >= error_vec_len) ? (error_vec_len - 1) : x2;

			eh += 2.0 * (mse_vec[x1] * (1.0 - interp) + mse_vec[x2] * interp);
		}

		i--;

		assert(i >= w->startidx);

		if (i == w->startidx) {
			stop1  = i+1;
			start2 = i+2;
		} else if (i == w->endidx) {
			stop1  = i-2;
			start2 = i-1;
		} else if (i-1 == w->startidx) {
			stop1  = i;
			start2 = i+1;
		} else {
			stop1  = i-1;
			start2 = i;
		}

		w->left = &(nodebuf[--nbuf]);
		w->right = &(nodebuf[--nbuf]);

		build_relnode(w->left, sync_positions, gain_vec, mse_vec, w->startidx, stop1, error_vec_len);
		build_relnode(w->right, sync_positions, gain_vec, mse_vec, start2, w->endidx, error_vec_len);
	}

	reltable->nb_entry = 0;
	recursive_construct_table
		(&root
		,reltable
		,sync_positions
		);
}

/* See doucmentation in the API for how to call this function.
 *
 * Alignment of releases to the playback position is pretty important in organ
 * simulation. When a release is not aligned properly, cancellation of the
 * signal can occur during the crossfade into the release leading to
 * artefacts. Also, if the release is transitioned in very shortly after the
 * attack, we may end up with a gain mismatch - this is also a significant
 * problem. This module is trying to solve these issues.
 *
 * What this thing does:
 *   1) Find the best position in the attack to align the release.
 *   2) Starting at that position, go backwards in increments of "period"
 *      samples searching a small region for the best alignment position. Then
 *      do the same thing going forwards. After this we have a list of
 *      positions which we would ideally jump into the release at sample 0
 *      from.
 * TODO: finish this. */
void
reltable_build
	(struct reltable *reltable
	,const float     *envelope_buf
	,const float     *correlation_bufs
	,const float     *rel_powers
	,unsigned         nb_rels
	,size_t           rel_stride
	,unsigned         error_vec_len
	,float            period
	,const char      *debug_prefix
	)
{
	unsigned i;
	float    rel_power = rel_powers[0];
	unsigned *epos    = malloc(error_vec_len * sizeof(unsigned));
	float    *emse    = malloc(error_vec_len * sizeof(float));
	unsigned positions = 0;

	(void)debug_prefix;

	{
		unsigned  lf   = 2 * fmax(1.0, (unsigned)(period / 15.0));
		unsigned  skip = (unsigned)fmax(1.0, period - lf/2);
		float    *emsebuf = malloc(error_vec_len * sizeof(float));
		unsigned  errpos = 0;
		float     err = 0.0;

		/* Find best release alignment position. */
		err = rel_power;
		for (i = 0; i < error_vec_len; i++) {
			float scale = rel_power + envelope_buf[i];
			float f     = scale - 2.0f * correlation_bufs[i];
			emse[i]     = f / scale;
			emsebuf[i]  = f;
			if (f < err) {
				err    = f;
				errpos = i;
			}
		}

		/* Find positions before best sync position */
		for (i = errpos; i > skip; ) {
			unsigned lep = i - skip;
			float    le  = emsebuf[lep];
			unsigned j;
			i = lep;
			for (j = 1; (j < lf) && (i >= j); j++) {
				if (emsebuf[i-j] < le) {
					lep = i-j;
					le = emsebuf[lep];
				}
			}
			epos[positions++] = lep;
			i = lep;
		}

		/* Reverse initial position list */
		for (i = 0; i < positions/2; i++) {
			unsigned tmp         = epos[i];
			epos[i]              = epos[positions-1-i];
			epos[positions-1-i]  = tmp;
		}

		/* Insert best position */
		epos[positions++] = errpos;

		/* Insert all later positions. */
		for (i = errpos + skip; i < error_vec_len; i += skip) {
			unsigned lep = i;
			float    le = emsebuf[lep];
			unsigned j;
			for (j = 1; (j < lf) && (i + j < error_vec_len); j++) {
				if (emsebuf[i+j] < le) {
					lep = i+j;
					le = emsebuf[lep];
				}
			}
			epos[positions++] = lep;
			i = lep;
		}

		free(emsebuf);

#ifdef OPENDIAPASON_VERBOSE_DEBUG
		printf("period: %f,%u,%u\n", period, skip, lf);
#endif
	}

	{
		float  rel_scale = 1.0f / rel_power;
		float *egain     = malloc(positions * sizeof(float));

		for (i = 0; i < positions; i++) {
			unsigned j = epos[i];
			float cbj  = correlation_bufs[j];
			egain[i]   = cbj * rel_scale;
		}

#if OPENDIAPASON_VERBOSE_DEBUG
		if (strlen(debug_prefix) < 1024 - 50) {
			char              namebuf[1024];
			struct wav_dumper dump;
			sprintf(namebuf, "%s_reltable_samples.wav", debug_prefix);
			if (wav_dumper_begin(&dump, namebuf, 1, 24, 44100, 1, 44100) == 0) {
				(void)wav_dumper_write_from_floats(&dump, emse, error_vec_len, 1, error_vec_len);
				wav_dumper_end(&dump);
			}
		}
#endif

		reltable_int
			(reltable
			,epos
			,positions
			,egain
			,emse
			,error_vec_len
			);

		free(egain);
	}

	free(epos);
	free(emse);

#ifdef OPENDIAPASON_VERBOSE_DEBUG
	for (i = 0; i < reltable->nb_entry; i++) {
		printf("%u) %f,%f,%f,%f\n", reltable->entry[i].last_sample, reltable->entry[i].m, reltable->entry[i].b, reltable->entry[i].gain, reltable->entry[i].avgerr);
	}
#endif
}




