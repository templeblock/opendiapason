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

static int serialise_format(const struct wav_sample_format *fmt, unsigned char *buf, size_t *size)
{
	static const unsigned char EXTENSIBLE_GUID_SUFFIX[14] = {/* AA, BB, */ 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
	int           format_code      = fmt->format;
	uint_fast16_t container_size   = get_container_size(format_code);
	uint_fast16_t container_bits   = container_size * 8;
	uint_fast16_t bits_per_sample  = fmt->bits_per_sample;
	int           extensible       = container_bits != bits_per_sample;
	uint_fast16_t basic_format_tag = (format_code == WAV_SAMPLE_FLOAT32) ? 0x0003u : 0x0001u;
	uint_fast16_t format_tag       = (extensible) ? 0xFFFEu : basic_format_tag;
	const size_t  fmt_sz           = (extensible) ? 48 : ((basic_format_tag == 1) ? 24 : 26);

	if (buf != NULL) {
		uint_fast16_t channels    = fmt->channels;
		uint_fast32_t sample_rate = fmt->sample_rate;
		uint_fast16_t block_align = container_size * channels;

		buf += *size;

		cop_st_ule32(buf + 0, RIFF_ID('f', 'm', 't', ' '));
		cop_st_ule32(buf + 4, fmt_sz - 8);
		cop_st_ule16(buf + 8, format_tag);
		cop_st_ule16(buf + 10, channels);
		cop_st_ule32(buf + 12, sample_rate);
		cop_st_ule32(buf + 16, sample_rate * block_align);
		cop_st_ule16(buf + 20, block_align);
		cop_st_ule16(buf + 22, container_bits);
		if (extensible || basic_format_tag != 1) {
			cop_st_ule16(buf + 24, fmt_sz - 26);
		}
		if (extensible) {
			cop_st_ule16(buf + 26, bits_per_sample);
			cop_st_ule32(buf + 28, 0);
			cop_st_ule16(buf + 32, basic_format_tag);
			memcpy(buf + 34, EXTENSIBLE_GUID_SUFFIX, 14);
		}
	}

	*size += fmt_sz;

	return (format_tag != 1);
}

static void serialise_fact(uint_fast32_t data_frames, unsigned char *buf, size_t *size)
{
	if (buf != NULL) {
		buf += *size;
		cop_st_ule32(buf,     RIFF_ID('f', 'a', 'c', 't'));
		cop_st_ule32(buf + 4, 4);
		cop_st_ule32(buf + 8, data_frames);
	}
	*size += 12;
}

static void serialise_blob(uint_fast32_t id, const unsigned char *ckdata, uint_fast32_t cksize, unsigned char *buf, size_t *size)
{
	if (buf != NULL) {
		buf += *size;
		cop_st_ule32(buf, id);
		cop_st_ule32(buf + 4, cksize);
		memcpy(buf + 8, ckdata, cksize);
		if (cksize & 1)
			buf[8+cksize] = 0;
	}
	*size += 8 + cksize + (cksize & 1);
}

static void serialise_data(const struct wav_sample_format *format, void *data, uint_fast32_t data_frames, unsigned char *buf, size_t *size)
{
	uint_fast16_t container_size = get_container_size(format->format);
	uint_fast16_t block_align = container_size * format->channels;
	uint_fast32_t data_size = data_frames * block_align;
	serialise_blob(RIFF_ID('d', 'a', 't', 'a'), data, data_size, buf, size);
}

static void serialise_zstrblob(uint_fast32_t id, const char *value, unsigned char *buf, size_t *size)
{
	size_t len;
	if (value != NULL && (len = strlen(value)) > 0)
		serialise_blob(id, (const unsigned char *)value, len + 1, buf, size);
}

static void serialise_info(const struct wav_sample_info_set *infoset, unsigned char *buf, size_t *size)
{
	size_t old_sz = *size;
	size_t new_sz = old_sz + 12;
	unsigned i;

	for (i = 0; i < infoset->nb_info; i++) {
		serialise_zstrblob(infoset->info[i].id, infoset->info[i].value, buf, &new_sz);
	}

	/* Only bother serialising if there were actually metadata items
	 * written. */
	if (new_sz != old_sz + 12) {
		assert(new_sz > old_sz + 12);
		if (buf != NULL) {
			buf += old_sz;
			cop_st_ule32(buf + 0, RIFF_ID('L', 'I', 'S', 'T'));
			cop_st_ule32(buf + 4, new_sz - old_sz - 8);
			cop_st_ule32(buf + 8, RIFF_ID('I', 'N', 'F', 'O'));
		}
		*size = new_sz;
	}
}

void wav_sample_serialise(const struct wav_sample *wav, unsigned char *buf, size_t *size, int store_cue_loops)
{
	struct wav_chunk *ck;
	*size = 12;

	serialise_info(&(wav->info), buf, size);
	if (serialise_format(&wav->format, buf, size)) {
		serialise_fact(wav->data_frames, buf, size);
	}
	serialise_data(&wav->format, wav->data, wav->data_frames, buf, size);
	serialise_adtl(wav, buf, size, store_cue_loops);
	serialise_cue(wav, buf, size, store_cue_loops);
	serialise_smpl(wav, buf, size);

	ck = wav->unsupported;
	while (ck != NULL) {
		serialise_blob(ck->id, ck->data, ck->size, buf, size);
		ck = ck->next;
	}

	if (buf != NULL) {
		cop_st_ule32(buf, RIFF_ID('R', 'I', 'F', 'F'));
		cop_st_ule32(buf + 4, *size - 8);
		cop_st_ule32(buf + 8, RIFF_ID('W', 'A', 'V', 'E'));
	}
}
