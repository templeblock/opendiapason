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

	/* The number of sync positions used to build the node. */
	unsigned         nb_sync_positions;

	/* The gain information associated with this node. */
	float            min_gain;
	float            max_gain;
	float            avg_gain;
	float            ideal_avg_gain;

	/* Error information associated with this node. */
	float            min_error;
	float            max_error;
	float            avg_error;
	float            ideal_avg_error;

	struct relnode  *left;
	struct relnode  *right;
};

double
reltable_find
	(const struct reltable *reltable
	,double                 sample
	,float                 *gain
	,float                 *avgerr
	,unsigned              *rel_id
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

	if (rel_id != NULL) {
		*rel_id = reltable->entry[i].rel_id;
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

/* Use the shape error to break the relnode into components (maybe also use
 * the gain vector)... */
static
void
build_relnode
	(struct relnode *rn
	,const unsigned *sync_positions
	,const float    *gain_vec
	,const float    *shape_error_vec
	,unsigned        start_position
	,unsigned        end_position
	,unsigned        error_vec_len
	)
{
	unsigned i;
	double mean_y = 0.0;
	double mean_x = 0.0;
	double sum_n, sum_d;

	assert(end_position > start_position);

	rn->nb_sync_positions  = (end_position - start_position) + 1;
	rn->startidx           = start_position;
	rn->endidx             = end_position;

	for (i = start_position; i <= end_position; i++) {
		unsigned sp   = sync_positions[i];
		mean_y       += sp;
		mean_x       += i;
	}

	/* Compute modfac and b as the least-squares line fit through all of the
	 * sync positions. So the position we jump to inside the release will be
	 *        (current_position + b) % modfac
	 */
	mean_y                /= rn->nb_sync_positions;
	mean_x                /= rn->nb_sync_positions;
	sum_n                  = 0.0;
	sum_d                  = 0.0;
	for (i = start_position; i <= end_position; i++) {
		sum_n += (i - mean_x) * (sync_positions[i] - mean_y);
		sum_d += (i - mean_x) * (i - mean_x);
	}
	rn->modfac = sum_n / sum_d;
	rn->b      = mean_y - rn->modfac * mean_x;
	rn->b      = rn->modfac * start_position + rn->b;

	/* TODO: CAN THIS BE CHANGED?? :( NEGATIVE VALUES CAUSE THE INTERPOLATION
	 * BELOW TO GO MENTAL - THERE MUST BE A BETTER WAY! */
	rn->b = fmax(rn->b, 0.0);

	{
		float min_tmp = 1.0f;
		float max_tmp = 0.0f;
		float avg_tmp = 0.0f;
		float min_tmp2 = 1.0f;
		float max_tmp2 = 0.0f;
		float avg_tmp2 = 0.0f;
		float act_serr = 0.0f;
		float act_gerr = 0.0f;

		for (i = start_position; i <= end_position; i++) {
			double   approx = (rn->b + (i-start_position) * rn->modfac);
			unsigned actual = sync_positions[i];

			unsigned x1 = (unsigned)floor(approx);
			float    interp = fmin(1.0f, fmax(0.0f, approx - x1));
			unsigned x2 = x1 + 1;
			float    ethis;
			float    gthis;

			x1 = (x1 >= error_vec_len) ? (error_vec_len - 1) : x1;
			x2 = (x2 >= error_vec_len) ? (error_vec_len - 1) : x2;

			ethis     = shape_error_vec[x1] * (1.0f - interp) + shape_error_vec[x2] * interp;
			gthis     = gain_vec[x1]        * (1.0f - interp) + gain_vec[x2] * interp;

			min_tmp   = (ethis < min_tmp) ? ethis : min_tmp;
			min_tmp2  = (gthis < min_tmp) ? gthis : min_tmp;
			max_tmp   = (ethis > max_tmp) ? ethis : max_tmp;
			max_tmp2  = (gthis > max_tmp) ? gthis : max_tmp;
			avg_tmp  += ethis;
			avg_tmp2 += gthis;
			act_serr += shape_error_vec[actual];
			act_gerr += gain_vec[actual];
		}

		rn->min_gain        = fmax(min_tmp2, 0.0f);
		rn->max_gain        = max_tmp2;
		rn->avg_gain        = avg_tmp2 / rn->nb_sync_positions;
		rn->ideal_avg_gain  = act_gerr / rn->nb_sync_positions;
		rn->min_error       = min_tmp;
		rn->max_error       = max_tmp;
		rn->avg_error       = avg_tmp / rn->nb_sync_positions;
		rn->ideal_avg_error = act_serr / rn->nb_sync_positions;
	}

	rn->left            = NULL;
	rn->right           = NULL;
}

/* Find a target node to split. */
static
struct relnode *
find_worst_node
	(struct relnode *root
	)
{
	if (root->left != NULL && root->right != NULL) {
		struct relnode *w1 = find_worst_node(root->left);
		struct relnode *w2 = find_worst_node(root->right);
		
		if (w1->nb_sync_positions <= 4)
			return w2;
		if (w2->nb_sync_positions <= 4)
			return w1;

		assert(w1->ideal_avg_gain > 0.0f);
		assert(w2->ideal_avg_gain > 0.0f);

		return (fabsf(w1->ideal_avg_gain - w1->avg_gain) > fabsf(w2->ideal_avg_gain - w2->avg_gain)) ? w1 : w2;



		return (w1->max_gain / (w1->min_gain + 0.01f) > w2->max_gain / (w2->min_gain + 0.01f)) ? w1 : w2;
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
	table->entry[table->nb_entry].rel_id = 0;
	table->entry[table->nb_entry].b      = b;
	table->entry[table->nb_entry].m      = m;
	table->entry[table->nb_entry].gain   = node->avg_gain;
	table->entry[table->nb_entry].avgerr = node->avg_error;
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
	,const float     *shape_error_vec
	,unsigned         error_vec_len
	)
{
	struct relnode root;
	struct relnode nodebuf[160];
	unsigned nbuf = 160;

	assert(nb_sync_positions);

	build_relnode(&root, sync_positions, gain_vec, shape_error_vec, 0, nb_sync_positions-1, error_vec_len);

	while (nbuf > 2) {
		struct relnode *w = find_worst_node(&root);
		unsigned i;
		double eh;

		unsigned stop1, start2;

		/* If find_worst_node returns too short a node, then we've actually
		 * used all the sync positions or have reached a termination
		 * condition. */
		if (w->endidx - w->startidx < 3)
			break;

		for (i = w->startidx, eh = 0.0; i <= w->endidx && eh < w->ideal_avg_error * w->nb_sync_positions; i++) {
			double   approx = (w->b + (i - w->startidx) * w->modfac);
			unsigned x1     = (unsigned)approx;
			double   interp = approx - x1;
			unsigned x2     = x1 + 1;

			x1 = (x1 >= error_vec_len) ? (error_vec_len - 1) : x1;
			x2 = (x2 >= error_vec_len) ? (error_vec_len - 1) : x2;

			eh += 2.0 * (shape_error_vec[x1] * (1.0 - interp) + shape_error_vec[x2] * interp);
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

		build_relnode(w->left, sync_positions, gain_vec, shape_error_vec, w->startidx, stop1, error_vec_len);
		build_relnode(w->right, sync_positions, gain_vec, shape_error_vec, start2, w->endidx, error_vec_len);
	}

	reltable->nb_entry = 0;
	recursive_construct_table
		(&root
		,reltable
		,sync_positions
		);
}

/* TODO: This algorithm is garbage. */
static
unsigned
reltable_find_correlation_peaks
	(const float *cbuf
	,const float *mbuf
	,unsigned    *obuf
	,unsigned     length
	,unsigned     tgt_period
	)
{
	unsigned opos = 0;
	unsigned olast;
	float a, b, c;

	olast = 0;
	while (olast + 2 < length) {
		a      = mbuf[olast];
		b      = mbuf[olast+1];
		olast += 2;
		while (olast < length) {
			c = mbuf[olast];
			if (b <= c && b < a && cbuf[olast] > 0.0f)
				break;
			a = b;
			b = c;
			olast++;
		}
		if (olast == length)
			break;

		olast--;

		if (opos > 0 && mbuf[olast] < mbuf[obuf[opos-1]] && olast - obuf[opos-1] < (7*tgt_period)/8) {
			obuf[opos-1] = olast;
		} else if (opos == 0 || olast - obuf[opos-1] > (7*tgt_period)/8) {
			obuf[opos++] = olast;
		}

		/* Make olast point at the index of the peak (b). */
		olast++;
	}

	return opos;
}

static
void
merge_reltables
	(struct reltable *rt_dest
	,struct reltable *rt_src
	,unsigned         new_src_id
	)
{
	unsigned src_entry = 0;
	unsigned i;
	for (i = 0; i < rt_dest->nb_entry; i++) {
		while (src_entry+1 < rt_src->nb_entry && rt_src->entry[src_entry+1].last_sample <= rt_dest->entry[i].last_sample)
			src_entry++;
		if (src_entry >= rt_src->nb_entry)
			break;
		/*  fabsf(1.0f-rt_src->entry[src_entry].gain) < fabsf(1.0f-rt_dest->entry[i].gain) */
		if (rt_src->entry[src_entry].avgerr < rt_dest->entry[i].avgerr) {
			rt_dest->entry[i]        = rt_src->entry[src_entry++];
			rt_dest->entry[i].rel_id = new_src_id;
		}
	}
}

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
	unsigned rel_idx;
	unsigned *error_positions = malloc(rel_stride * nb_rels * sizeof(unsigned));
	float    *shape_errors    = malloc(rel_stride * nb_rels * sizeof(float));
	unsigned *nb_syncs        = malloc(nb_rels * sizeof(unsigned));
	(void)debug_prefix;

	{
		float          *ms_errors = malloc(rel_stride * nb_rels * sizeof(float));

		for (rel_idx = 0; rel_idx < nb_rels; rel_idx++) {
			float        rel_power   = rel_powers[rel_idx];
			float       *ms_error    = ms_errors        + rel_idx * rel_stride;
			float       *shape_error = shape_errors     + rel_idx * rel_stride;
			const float *corrbuf     = correlation_bufs + rel_idx * rel_stride;
			unsigned    *epos        = error_positions  + rel_idx * rel_stride;
			unsigned     errpos;
			float        err;

			/* Find best release alignment position. */
			for (i = 0, err = rel_power, errpos = 0; i < error_vec_len; i++) {
				float scale = rel_power + envelope_buf[i];
				float f     = scale - 2.0f * corrbuf[i];

				/* TODO: WE ARE USING MS ERROR AGAIN TO BUILD TABLES - THIS
				 * GIVES THE BEST RESULT WHEN USING MULTIPLE RELEASES...
				 * CLEAN UP THE MESS! */
				shape_error[i] = f;
				ms_error[i]    = f;
			}

			nb_syncs[rel_idx] = reltable_find_correlation_peaks(corrbuf, ms_error, epos, error_vec_len, (unsigned)(period + 0.5));
		}

#ifdef OPENDIAPASON_VERBOSE_DEBUG
		if (strlen(debug_prefix) < 1024 - 50) {
			char      namebuf[1024];
			struct wav_dumper dump;
			sprintf(namebuf, "%s_reltable_mses.wav", debug_prefix);
			if (wav_dumper_begin(&dump, namebuf, nb_rels, 24, 44100, 1, 44100) == 0) {
				(void)wav_dumper_write_from_floats(&dump, ms_errors, error_vec_len, 1, rel_stride);
				wav_dumper_end(&dump);
			}
		}
#endif

		free(ms_errors);

#ifdef OPENDIAPASON_VERBOSE_DEBUG
		printf("period: %f,%u,%u\n", period, skip, lf);
#endif
	}

	for (rel_idx = 0; rel_idx < nb_rels; rel_idx++) {
		struct reltable tmp;
		float           rel_scale = 1.0f / rel_powers[rel_idx];
		float          *egain     = malloc(error_vec_len * sizeof(float));
		const float    *ecorr     = correlation_bufs + rel_idx * rel_stride;
		unsigned       *epos      = error_positions + rel_idx * rel_stride;

		for (i = 0; i < error_vec_len; i++) {
			egain[i] = ecorr[i] * rel_scale;
		}

		reltable_int
			((rel_idx == 0) ? reltable : &tmp
			,epos
			,nb_syncs[rel_idx]
			,egain
			,shape_errors + rel_idx * rel_stride
			,error_vec_len
			);

		if (rel_idx != 0) {
#ifdef OPENDIAPASON_VERBOSE_DEBUG
			for (i = 0; i < tmp.nb_entry; i++) {
				printf("%u) %f,%f,%f,%f\n", tmp.entry[i].last_sample, tmp.entry[i].m, tmp.entry[i].b, tmp.entry[i].gain, tmp.entry[i].avgerr);
			}
#endif
			merge_reltables
				(reltable
				,&tmp
				,rel_idx
				);
		}

		free(egain);
	}

	free(nb_syncs);
	free(error_positions);
	free(shape_errors);

#ifdef OPENDIAPASON_VERBOSE_DEBUG
	for (i = 0; i < reltable->nb_entry; i++) {
		printf("%u) %f,%f,%f,%f,%d\n", reltable->entry[i].last_sample, reltable->entry[i].m, reltable->entry[i].b, reltable->entry[i].gain, reltable->entry[i].avgerr, reltable->entry[i].rel_id);
	}
#endif
}




