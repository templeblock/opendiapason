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
#include <stddef.h>
#include <math.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

struct relnode {
	double           modfac;
	double           b;

	unsigned         startidx;
	unsigned         endidx;

	double           ideal_error;
	double           actual_error;

	float            rel_gain;

	struct relnode  *left;
	struct relnode  *right;
};

double
reltable_find
	(const struct reltable *reltable
	,double                 sample
	,float                 *gain
	)
{
	unsigned i;
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


	/* TODO: FABS INSERTED TO PREVENT NEGATIVE OUTPUT! MAY BE A BUG ELSEWHERE. */
	return fmod(fabs(sample - reltable->entry[i].b), reltable->entry[i].m);
}

static
void
build_relnode
	(struct relnode *rn
	,const unsigned *sync_positions
	,const float    *gain_vec
	,unsigned        start_position
	,unsigned        end_position
	,const float    *error_vec
	,unsigned        error_vec_len
	)
{
	unsigned i;

	if (end_position - start_position == 0) {
		rn->b        = sync_positions[start_position];
		rn->rel_gain = gain_vec[start_position];
		rn->modfac   = 1.0;
	} else {
		double mean_y = 0.0;
		double mean_x = 0.0;
		double sum_n, sum_d;
		rn->rel_gain = 0.0f;
		for (i = start_position; i <= end_position; i++) {
			mean_y += sync_positions[i];
			mean_x += i;
			rn->rel_gain = (gain_vec[i] > rn->rel_gain) ? gain_vec[i] : rn->rel_gain;
		}
		mean_y = mean_y / (end_position - start_position + 1);
		mean_x = mean_x / (end_position - start_position + 1);
		sum_n = 0.0;
		sum_d = 0.0;
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

		emin    += error_vec[actual];
		eapprox += error_vec[x1] * (1.0 - interp) + error_vec[x2] * interp;
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
	while (b > sync_positions[node->startidx]) {
		b -= node->modfac;
	}
	table->entry[table->nb_entry].b = b;
	table->entry[table->nb_entry].m = m;
	table->entry[table->nb_entry].gain = node->rel_gain;
	table->nb_entry++;
}

#if 0
struct reltable {
	unsigned nb_entry;
	struct {
		unsigned last_sample;
		double   m;
		double   b;
	} entry[RELTABLE_MAX_ENTRIES];
};
#endif

static
void
printnodes(struct reltable *root)
{
	unsigned i;
	for (i = 0; i < root->nb_entry; i++) {
		printf("%u) %f,%f,%f\n", root->entry[i].last_sample, root->entry[i].m, root->entry[i].b, root->entry[i].gain);
	}
}

static
void
reltable_int
	(struct reltable *reltable
	,const unsigned  *sync_positions
	,const float     *gain_vec
	,unsigned         nb_positions
	,const float     *error_vec
	,unsigned         error_vec_len
	)
{
	struct relnode root;
	struct relnode nodebuf[160];
	unsigned nbuf = 160;

	assert(nb_positions);

	build_relnode(&root, sync_positions, gain_vec, 0, nb_positions-1, error_vec, error_vec_len);

	while (nbuf > 2) {
		struct relnode *w = find_worst_node(&root);
		unsigned i;
		double eh;

		unsigned stop1, start2;

		if (w->actual_error - w->ideal_error < 0.3 || w->startidx == w->endidx)
			break;

		for (i = w->startidx, eh = 0.0; i <= w->endidx && eh < w->actual_error; i++) {
			double   approx = (w->b + (i - w->startidx) * w->modfac);
			unsigned x1     = (unsigned)approx;
			double   interp = approx - x1;
			unsigned x2     = x1 + 1;

			x1 = (x1 >= error_vec_len) ? (error_vec_len - 1) : x1;
			x2 = (x2 >= error_vec_len) ? (error_vec_len - 1) : x2;

			eh += 2.0 * (error_vec[x1] * (1.0 - interp) + error_vec[x2] * interp);
		}

		i--;

		if (i == w->startidx) {
			stop1  = i;
			start2 = i+1;
		} else {
			stop1  = i-1;
			start2 = i;
		}

		w->left = &(nodebuf[--nbuf]);
		w->right = &(nodebuf[--nbuf]);

		build_relnode(w->left, sync_positions, gain_vec, w->startidx, stop1, error_vec, error_vec_len);
		build_relnode(w->right, sync_positions, gain_vec, start2, w->endidx, error_vec, error_vec_len);
	}

	reltable->nb_entry = 0;
	recursive_construct_table
		(&root
		,reltable
		,sync_positions
		);

#if 1
	unsigned i;
	for (i = 0; i < nb_positions && i < 20; i++) {
		float f;
		float t = reltable_find(reltable, sync_positions[i], &f);
		printf("%u->%f,%f\n", sync_positions[i], t, f);


	}


	printnodes(reltable);
#endif
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
	,float           *error_vec
	,const float     *corr_vec
	,float            rel_power
	,unsigned         error_vec_len
	,float            period
	)
{
	double err = 0.0;
	unsigned errpos = 0;
	unsigned i;
	unsigned *epos  = malloc(error_vec_len * sizeof(unsigned));
	float    *egain = malloc(error_vec_len * sizeof(float));
	unsigned positions = 0;
	unsigned lf   = 2 * fmax(1.0, (unsigned)(period / 15.0));
	unsigned skip = (unsigned)fmax(1.0, period - lf/2);
	float    rel_scale = 1.0 / rel_power;

	/* Find best release alignment position. */
	for (i = 0; i < error_vec_len; i++) {
		float f = rel_power + error_vec[i] - 2.0f * corr_vec[i]; /* Scaled up by the width of the release! */
		error_vec[i] = (f <= 0.0f) ? 0.0f : sqrtf(f);
		if (i == 0 || error_vec[i] < err) {
			err = error_vec[i];
			errpos = i;
		}
	}

	printf("period: %f,%u,%u\n", period, skip, lf);

	/* Find positions before best sync position */
	for (i = errpos; i > skip; ) {
		i -= skip;
		unsigned lep = i;
		float    le  = error_vec[lep];
		unsigned j;
		for (j = 1; (j < lf) && (i >= j); j++) {
			if (error_vec[i-j] < le) {
				lep = i-j;
				le = error_vec[lep];
			}
		}
		egain[positions]  = corr_vec[lep] * rel_scale;
		epos[positions++] = lep;
		i = lep;
	}

	/* Reverse initial position list */
	for (i = 0; i < positions/2; i++) {
		float tmp2           = egain[i];
		unsigned tmp         = epos[i];
		egain[i]             = egain[positions-1-i];
		epos[i]              = epos[positions-1-i];
		egain[positions-1-i] = tmp2;
		epos[positions-1-i]  = tmp;
	}

	/* Insert best position */
	egain[positions]  = corr_vec[errpos] * rel_scale;
	epos[positions++] = errpos;

	/* Insert all later positions. */
	for (i = errpos + skip; i < error_vec_len; i += skip) {
		unsigned lep = i;
		float    le = error_vec[lep];
		unsigned j;
		for (j = 1; (j < lf) && (i + j < error_vec_len); j++) {
			if (error_vec[i+j] < le) {
				lep = i+j;
				le = error_vec[lep];
			}
		}
		egain[positions]  = corr_vec[lep] * rel_scale;
		epos[positions++] = lep;
		i = lep;
	}

	reltable_int
		(reltable
		,epos
		,egain
		,positions
		,error_vec
		,error_vec_len
		);

	free(epos);
	free(egain);
}




