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

#include "wav_sample.h"
#include "cop/cop_conversions.h"
#include <string.h>
#include <assert.h>

static void serialise_dumb_chunk(const struct wav_chunk *ck, unsigned char *buf, size_t *size)
{
	size_t ckpos = *size;

	if (buf != NULL) {
		buf += ckpos;
		cop_st_ule32(buf, ck->id);
		cop_st_ule32(buf + 4, ck->size);
		memcpy(buf + 8, ck->data, ck->size);
		if (ck->size & 1)
			buf[8+ck->size] = 0;
	}

	*size = ckpos + 8 + ck->size + (ck->size & 1);
}

static void serialise_ltxt(unsigned char *buf, size_t *size, uint_fast32_t id, uint_fast32_t length)
{
	if (buf != NULL) {
		buf += *size;
		cop_st_ule32(buf + 0, RIFF_ID('l', 't', 'x', 't'));
		cop_st_ule32(buf + 4, 20);
		cop_st_ule32(buf + 8, id);
		cop_st_ule32(buf + 12, length);
		cop_st_ule32(buf + 16, 0);
		cop_st_ule16(buf + 20, 0);
		cop_st_ule16(buf + 22, 0);
		cop_st_ule16(buf + 24, 0);
		cop_st_ule16(buf + 26, 0);
	}
	*size += 28;
}

static void serialise_notelabl(unsigned char *buf, size_t *size, uint_fast32_t ctyp, uint_fast32_t id, const char *s)
{
	size_t len = strlen(s) + 1;
	if (buf != NULL) {
		buf += *size;
		cop_st_ule32(buf + 0, ctyp);
		cop_st_ule32(buf + 4, 4 + len);
		cop_st_ule32(buf + 8, id);
		memcpy(buf + 12, s, len);
		if (len & 1)
			buf[12+len] = 0;
	}
	*size += 12 + len + (len & 1);
}

static void serialise_adtl(const struct wav_sample *wav, unsigned char *buf, size_t *size, int store_cue_loops)
{
	size_t old_sz = *size;
	size_t new_sz = old_sz + 12;
	unsigned i;

	/* Serialise any metadata that might exist. */
	for (i = 0; i < wav->nb_marker; i++) {
		if (wav->markers[i].has_length && store_cue_loops)
			serialise_ltxt(buf, &new_sz, i + 1, wav->markers[i].length);
		if (wav->markers[i].name != NULL)
			serialise_notelabl(buf, &new_sz, RIFF_ID('l', 'a', 'b', 'l'), i + 1, wav->markers[i].name);
		if (wav->markers[i].desc != NULL)
			serialise_notelabl(buf, &new_sz, RIFF_ID('n', 'o', 't', 'e'), i + 1, wav->markers[i].desc);
	}

	/* Only bother serialising if there were actually metadata items
	 * written. */
	if (new_sz != old_sz + 12) {
		assert(new_sz > old_sz + 12);
		if (buf != NULL) {
			buf += old_sz;
			cop_st_ule32(buf + 0, RIFF_ID('L', 'I', 'S', 'T'));
			cop_st_ule32(buf + 4, new_sz - old_sz - 8);
			cop_st_ule32(buf + 8, RIFF_ID('a', 'd', 't', 'l'));
		}
		*size = new_sz;
	}
}

static void serialise_cue(const struct wav_sample *wav, unsigned char *buf, size_t *size, int store_cue_loops)
{
	unsigned i;
	unsigned nb_cue = 0;

	if (buf != NULL)
		buf += *size;

	for (i = 0; i < wav->nb_marker; i++) {
		if (store_cue_loops || wav->markers[i].length == 0) {
			if (buf != NULL) {
				cop_st_ule32(buf + 12 + nb_cue * 24, i + 1);
				cop_st_ule32(buf + 16 + nb_cue * 24, 0);
				cop_st_ule32(buf + 20 + nb_cue * 24, RIFF_ID('d', 'a', 't', 'a'));
				cop_st_ule32(buf + 24 + nb_cue * 24, 0);
				cop_st_ule32(buf + 28 + nb_cue * 24, 0);
				cop_st_ule32(buf + 32 + nb_cue * 24, wav->markers[i].position);
			}
			nb_cue++;
		}
	}

	if (nb_cue) {
		if (buf != NULL) {
			cop_st_ule32(buf + 0, RIFF_ID('c', 'u', 'e', ' '));
			cop_st_ule32(buf + 4, 4 + nb_cue * 24);
			cop_st_ule32(buf + 8, nb_cue);
		}
		*size += 12 + nb_cue * 24;
	}
}

static void serialise_smpl(const struct wav_sample *wav, unsigned char *buf, size_t *size)
{
	unsigned i;
	unsigned nb_loop = 0;

	if (buf != NULL)
		buf += *size;

	for (i = 0; i < wav->nb_marker; i++) {
		if (wav->markers[i].has_length && wav->markers[i].length > 0) {
			if (buf != NULL) {
				cop_st_ule32(buf + 44 + 24 * nb_loop, i + 1);
				cop_st_ule32(buf + 48 + 24 * nb_loop, 0);
				cop_st_ule32(buf + 52 + 24 * nb_loop, wav->markers[i].position);
				cop_st_ule32(buf + 56 + 24 * nb_loop, wav->markers[i].position + wav->markers[i].length - 1);
				cop_st_ule32(buf + 60 + 24 * nb_loop, 0);
				cop_st_ule32(buf + 64 + 24 * nb_loop, 0);
			}
			nb_loop++;
		}
	}

	if (nb_loop || wav->has_pitch_info) {
		if (buf != NULL) {
			cop_st_ule32(buf + 0, RIFF_ID('s', 'm', 'p', 'l'));
			cop_st_ule32(buf + 4, 36 + nb_loop * 24);
			cop_st_ule32(buf + 8, 0);
			cop_st_ule32(buf + 12, 0);
			cop_st_ule32(buf + 16, 0);
			cop_st_ule32(buf + 20, (wav->pitch_info) >> 32 & 0xFFFFFFFFu);
			cop_st_ule32(buf + 24, wav->pitch_info & 0xFFFFFFFFu);
			cop_st_ule32(buf + 28, 0);
			cop_st_ule32(buf + 32, 0);
			cop_st_ule32(buf + 36, nb_loop);
			cop_st_ule32(buf + 40, 0);
		}
		*size += 44 + nb_loop * 24;
	}
}

void wav_sample_serialise(const struct wav_sample *wav, unsigned char *buf, size_t *size, int store_cue_loops)
{
	struct wav_chunk *ck;
	*size = 12;

	assert(wav->fmt != NULL);
	serialise_dumb_chunk(wav->fmt, buf, size);
	if (wav->fact != NULL)
		serialise_dumb_chunk(wav->fact, buf, size);
	assert(wav->data != NULL);
	serialise_dumb_chunk(wav->data, buf, size);

	serialise_adtl(wav, buf, size, store_cue_loops);
	serialise_cue(wav, buf, size, store_cue_loops);
	serialise_smpl(wav, buf, size);

	ck = wav->unsupported;
	while (ck != NULL) {
		serialise_dumb_chunk(ck, buf, size);
		ck = ck->next;
	}

	if (buf != NULL) {
		cop_st_ule32(buf, RIFF_ID('R', 'I', 'F', 'F'));
		cop_st_ule32(buf + 4, *size - 8);
		cop_st_ule32(buf + 8, RIFF_ID('W', 'A', 'V', 'E'));
	}
}
