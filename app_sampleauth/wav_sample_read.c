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

#include "wav_sample_read.h"
#include "cop/cop_conversions.h"
#include <stdio.h>
#include <string.h>

static struct wav_marker *get_new_marker(struct wav_sample *wav)
{
	struct wav_marker *m = wav->markers + wav->nb_marker++;
	m->id           = 0;
	m->name         = NULL;
	m->desc         = NULL;
	m->length       = 0;
	m->has_length   = 0;
	m->in_cue       = 0;
	m->in_smpl      = 0;
	m->position     = 0;
	return m;
}

static
struct wav_marker *
get_marker
	(struct wav_sample *wav
	,uint_fast32_t      id
	)
{
	unsigned i;
	struct wav_marker *marker;
	for (i = 0; i < wav->nb_marker; i++) {
		if (wav->markers[i].id == id)
			return &(wav->markers[i]);
	}
	marker = get_new_marker(wav);
	marker->id = id;
	return marker;
}

void sort_and_reassign_ids(struct wav_sample *wav)
{
	unsigned i;
	for (i = 1; i < wav->nb_marker; i++) {
		unsigned j;
		for (j = i; j < wav->nb_marker; j++) {
			int i_loop = wav->markers[i-1].has_length && wav->markers[i-1].length > 0;
			int j_loop = wav->markers[j].has_length && wav->markers[j].length > 0;
			uint_fast64_t i_key = (((uint_fast64_t)wav->markers[i-1].position) << 32) | (wav->markers[i-1].length ^ 0xFFFFFFFF);
			uint_fast64_t j_key = (((uint_fast64_t)wav->markers[j].position) << 32) | (wav->markers[j].length ^ 0xFFFFFFFF);
			if ((j_loop && !i_loop) || (j_loop == i_loop && j_key < i_key)) {
				struct wav_marker m = wav->markers[i-1];
				wav->markers[i-1] = wav->markers[j];
				wav->markers[j] = m;
			}
		}
		wav->markers[i-1].id = i;
	}
}

static
unsigned
load_markers
	(struct wav_sample *wav
	,const char        *filename
	,unsigned           flags
	,struct wav_chunk  *adtl_ck
	,struct wav_chunk  *cue_ck
	,struct wav_chunk  *smpl_ck
	)
{
	unsigned i;
	unsigned warnings = 0;

	/* Reset the marker count to zero. */
	wav->nb_marker = 0;
	wav->has_pitch_info = 0;
	wav->pitch_info = 0;

	/* Load metadata strings and labelled text durations first. */
	if (adtl_ck != NULL) {
		size_t adtl_len        = adtl_ck->size;
		unsigned char *adtl    = adtl_ck->data;

		/* This condition should have been checked by the calling code. */
		assert(adtl_len >= 4);

		/* Move past "adtl" list identifier. */
		adtl_len -= 4;
		adtl     += 4;

		/* Read all of the metadata chunks in the adtl list. */
		while (adtl_len >= 8) {
			uint_fast32_t      meta_class = cop_ld_ule32(adtl);
			uint_fast32_t      meta_size  = cop_ld_ule32(adtl + 4);
			unsigned char     *meta_base  = adtl + 8;
			int                is_ltxt;
			int                is_note;
			struct wav_marker *marker;
			uint_fast64_t      cksz;

			/* Move the adtl pointer to the start of the next chunk.*/
			cksz = 8 + (uint_fast64_t)meta_size + (meta_size & 1);
			if (cksz > adtl_len)
				return warnings | WSR_ERROR_ADTL_INVALID;

			adtl     += cksz;
			adtl_len -= cksz;

			/* Make sure this is a chunk we can actually understand. If there
			 * are chunks in the adtl list which are unknown to us, we could
			 * end up breaking metadata. */
			is_ltxt = (meta_class == RIFF_ID('l', 't', 'x', 't'));
			is_note = (meta_class == RIFF_ID('n', 'o', 't', 'e'));
			if  (  !(is_ltxt && meta_size >= 20)
			    && !((is_note || (meta_class == RIFF_ID('l', 'a', 'b', 'l'))) && meta_size >= 4)
			    )
				return warnings | WSR_ERROR_ADTL_INVALID;

			/* The a metadata item with the given ID associated with this adtl
			 * metadata chunk. */
			marker = get_marker(wav, cop_ld_ule32(meta_base));
			if (marker == NULL)
				return warnings | WSR_ERROR_TOO_MANY_MARKERS;

			/* Skip over the metadata ID */
			meta_base += 4;
			meta_size -= 4;

			if (is_ltxt) {
				if (marker->has_length)
					return WSR_ERROR_ADTL_DUPLICATES;
				marker->has_length = 1;
				marker->length     = cop_ld_ule32(meta_base);
			} else {
				if (!meta_size || meta_base[meta_size-1] != 0) {
					warnings |= WSR_WARNING_ADTL_UNTERMINATED_STRINGS;
					continue;
				}

				if (is_note) {
					if (marker->desc != NULL)
						return warnings | WSR_ERROR_ADTL_DUPLICATES;
					marker->desc = (char *)meta_base;
				} else {
					if (marker->name != NULL)
						return warnings | WSR_ERROR_ADTL_DUPLICATES;
					marker->name = (char *)meta_base;
				}
			}
		}
	}

	/* Next we read the cue points into the marker list. */
	if (cue_ck != NULL) {
		size_t         cue_len = cue_ck->size;
		unsigned char *cue     = cue_ck->data;
		uint_fast32_t  ncue;

		if (cue_len < 4 || cue_len < 4 + 24 * (ncue = cop_ld_ule32(cue)))
			return warnings | WSR_ERROR_CUE_INVALID;

		cue += 4;
		while (ncue--) {
			uint_fast32_t cue_id      = cop_ld_ule32(cue);
			struct wav_marker *marker = get_marker(wav, cue_id);
			if (marker == NULL)
				return warnings | WSR_ERROR_TOO_MANY_MARKERS;

			if (marker->in_cue)
				return warnings | WSR_ERROR_CUE_DUPLICATE_IDS;

			marker->position     = cop_ld_ule32(cue + 20);
			marker->in_cue = 1;
			cue += 24;
		}
	}

	if (smpl_ck != NULL) {
		size_t         smpl_len      = smpl_ck->size;
		unsigned char *smpl          = smpl_ck->data;
		uint_fast32_t  nloop;

		if (smpl_len < 36 || (smpl_len < (36 + (nloop = cop_ld_ule32(smpl + 28)) * 24 + cop_ld_ule32(smpl + 32))))
			return warnings | WSR_ERROR_SMPL_INVALID;

		wav->has_pitch_info = 1;
		wav->pitch_info = (((uint_fast64_t)cop_ld_ule32(smpl + 12)) << 32) | cop_ld_ule32(smpl + 16);

		smpl += 36;
		for (i = 0; i < nloop; i++) {
			uint_fast32_t      id    = cop_ld_ule32(smpl);
			uint_fast32_t      start = cop_ld_ule32(smpl + 8);
			uint_fast32_t      end   = cop_ld_ule32(smpl + 12);
			uint_fast32_t      length;
			unsigned           j;
			struct wav_marker *marker;

			if (start > end)
				return warnings | WSR_ERROR_SMPL_INVALID;

			length = end - start + 1;

			for (j = 0; j < wav->nb_marker; j++) {
				/* If the loop has the same id as some metadata, but the
				 * metadata has no cue point, we can associate it with this
				 * metadata item. */
				if (id == wav->markers[j].id && !wav->markers[j].in_cue)
					break;

				/* If the loop matches an existing loop, we can associate it
				 * with this metadata item. */
				if  (   (wav->markers[j].in_cue && wav->markers[j].position == start)
					&&  (   (wav->markers[j].has_length && wav->markers[j].length == length)
				        || !wav->markers[j].has_length
				        )
				    )
					break;
			}

			if (j < wav->nb_marker) {
				marker        = wav->markers + j;
			} else {
				marker        = get_new_marker(wav);
			}

			marker->position   = start;
			marker->in_smpl    = 1;
			marker->length     = length;
			marker->has_length = 1;

			smpl += 24;
		}
	}

	{
		unsigned dest_idx;
		unsigned nb_smpl_only_loops = 0;
		unsigned nb_cue_only_loops = 0;

		/* Remove markers which contain metadata which does not correspond to
		 * any actual data and count the number of loops which are only found
		 * in cue points and the number that are only in smpl loops. If both
		 * of these are non-zero, one set of them needs to be deleted as we
		 * cannot determine which metadata is correct. */
		for (i = 0, dest_idx = 0; i < wav->nb_marker; i++) {
			/* Metadata not corresponding to an item. Remove. */
			if (!wav->markers[i].in_smpl && !wav->markers[i].in_cue)
				continue;

			if (wav->markers[i].has_length && wav->markers[i].length > 0) {
				if (wav->markers[i].in_smpl && !wav->markers[i].in_cue)
					nb_smpl_only_loops++;
				if (!wav->markers[i].in_smpl && wav->markers[i].in_cue)
					nb_cue_only_loops++;
			}

			if (i != dest_idx)
				wav->markers[dest_idx] = wav->markers[i];
			dest_idx++;
		}
		wav->nb_marker = dest_idx;

		/* Were there both independent sampler loops or independent cue
		 * loops? */
		if (nb_smpl_only_loops && nb_cue_only_loops) {
			/* If the caller has not specified a flag for what do do in this
			 * situation, print some information and fail. Otherwise, delete
			 * the markers belonging to the group we do not care about. */
			if (flags & (FLAG_PREFER_CUE_LOOPS | FLAG_PREFER_SMPL_LOOPS)) {
				for (i = 0, dest_idx = 0; i < wav->nb_marker; i++) {
					int is_loop = wav->markers[i].has_length && wav->markers[i].length > 0;
					if (is_loop && wav->markers[i].in_smpl && !wav->markers[i].in_cue && (flags & FLAG_PREFER_CUE_LOOPS))
						continue;
					if (is_loop && !wav->markers[i].in_smpl && wav->markers[i].in_cue && (flags & FLAG_PREFER_SMPL_LOOPS))
						continue;
					if (i != dest_idx)
						wav->markers[dest_idx] = wav->markers[i];
					dest_idx++;
				}
				wav->nb_marker = dest_idx;
			} else {
				fprintf(stderr, "%s has sampler loops that conflict with loops in the cue chunk. you must specify --prefer-smpl-loops or --prefer-cue-loops to load it. here are the details:\n", filename);
				fprintf(stderr, "common loops (position/duration):\n");
				for (i = 0; i < wav->nb_marker; i++)
					if (wav->markers[i].in_cue && wav->markers[i].in_smpl && wav->markers[i].has_length && wav->markers[i].length > 0)
						fprintf(stderr, "  %lu/%lu\n", (unsigned long)wav->markers[i].position, (unsigned long)wav->markers[i].length);
				fprintf(stderr, "sampler loops (position/duration):\n");
				for (i = 0; i < wav->nb_marker; i++)
					if (!wav->markers[i].in_cue && wav->markers[i].in_smpl && wav->markers[i].has_length && wav->markers[i].length > 0)
						fprintf(stderr, "  %lu/%lu\n", (unsigned long)wav->markers[i].position, (unsigned long)wav->markers[i].length);
				fprintf(stderr, "cue loops (position/duration):\n");
				for (i = 0; i < wav->nb_marker; i++)
					if (wav->markers[i].in_cue && !wav->markers[i].in_smpl && wav->markers[i].has_length && wav->markers[i].length > 0)
						fprintf(stderr, "  %lu/%lu\n", (unsigned long)wav->markers[i].position, (unsigned long)wav->markers[i].length);
				return -1;
			}
		}
	}

	return warnings;
}

static unsigned load_sample_format(struct wav_sample_format *format, unsigned char *fmt_ptr, size_t fmt_sz)
{
	static const unsigned char EXTENSIBLE_GUID_SUFFIX[14] = {/* AA, BB, */ 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};

	uint_fast16_t format_tag;
	uint_fast16_t container_bytes;
	uint_fast16_t block_align;
	uint_fast16_t channels;
	uint_fast16_t bits_per_sample;

	if (fmt_sz < 16)
		return WSR_ERROR_FMT_INVALID;

	format_tag              = cop_ld_ule16(fmt_ptr + 0);
	channels                = cop_ld_ule16(fmt_ptr + 2);
	format->sample_rate     = cop_ld_ule32(fmt_ptr + 4);
	block_align             = cop_ld_ule16(fmt_ptr + 12);
	bits_per_sample         = cop_ld_ule16(fmt_ptr + 14);
	container_bytes         = (bits_per_sample + 7) / 8;

	if (format_tag == 0xFFFEu) {
		uint_fast16_t cbsz;

		if  (   (bits_per_sample % 8)
			||  (fmt_sz < 18)
			||  ((cbsz = cop_ld_ule16(fmt_ptr + 16)) < 22)
			||  (fmt_sz < 18 + cbsz)
			)
			return WSR_ERROR_FMT_INVALID;

		bits_per_sample = cop_ld_ule16(fmt_ptr + 18);
		format_tag      = cop_ld_ule16(fmt_ptr + 24);

		if (memcmp(fmt_ptr + 26, EXTENSIBLE_GUID_SUFFIX, 14) != 0)
			return WSR_ERROR_FMT_UNSUPPORTED;
	}

	format->bits_per_sample = bits_per_sample;
	format->channels = channels;

	if (format_tag == 1 && container_bytes == 2)
		format->format = WAV_SAMPLE_PCM16;
	else if (format_tag == 1 && container_bytes == 3)
		format->format = WAV_SAMPLE_PCM24;
	else if (format_tag == 1 && container_bytes == 4)
		format->format = WAV_SAMPLE_PCM32;
	else if (format_tag == 3 && container_bytes == 4)
		format->format = WAV_SAMPLE_FLOAT32; 
	else
		return WSR_ERROR_FMT_UNSUPPORTED;

	if  (   (block_align != channels * container_bytes)
		||  (bits_per_sample > container_bytes * 8)
		)
		return WSR_ERROR_FMT_INVALID;

	return 0;
}

static unsigned load_info(char **infoset, unsigned char *buf, size_t sz)
{
	unsigned warnings = 0;

	assert(sz >= 4);
	sz  -= 4;
	buf += 4;

	while (sz >= 8) {
		uint_fast32_t ckid = cop_ld_ule32(buf);
		uint_fast32_t cksz = cop_ld_ule32(buf + 4);
		char *base;
		unsigned i;

		sz   -= 8;
		buf  += 8;
		base  = (char *)buf;

		if (cksz >= sz) {
			cksz = sz;
			sz   = 0;
		} else {
			buf += cksz + (cksz & 1);
			sz  -= cksz + (cksz & 1);
		}

		if (cksz == 0 || base[cksz-1] != 0) {
			warnings |= WSR_WARNING_INFO_UNTERMINATED_STRINGS;
			continue;
		}

		for (i = 0; i < NB_SUPPORTED_INFO_TAGS; i++) {
			if (SUPPORTED_INFO_TAGS[i] == ckid) {
				infoset[i] = base;
				break;
			}
		}

		if (i == NB_SUPPORTED_INFO_TAGS)
			return WSR_ERROR_INFO_UNSUPPORTED;
	}

	return warnings;
}

unsigned load_wave_sample(struct wav_sample *wav, unsigned char *buf, size_t bufsz, const char *filename, unsigned flags)
{
	uint_fast32_t riff_sz;
	unsigned warnings = 0;

	struct wav_chunk info;
	struct wav_chunk adtl;
	struct wav_chunk cue;
	struct wav_chunk smpl;
	struct wav_chunk fact;
	struct wav_chunk data;
	struct wav_chunk fmt;

	if  (   (bufsz < 12)
	    ||  (cop_ld_ule32(buf) != RIFF_ID('R', 'I', 'F', 'F'))
	    ||  ((riff_sz = cop_ld_ule32(buf + 4)) < 4)
	    ||  (cop_ld_ule32(buf + 8) != RIFF_ID('W', 'A', 'V', 'E'))
	    )
		return WSR_ERROR_NOT_A_WAVE;

	riff_sz -= 4;
	bufsz   -= 12;
	buf     += 12;

	if (riff_sz > bufsz) {
		warnings |= WSR_WARNING_FILE_TRUNCATION;
		riff_sz = (uint_fast32_t)bufsz;
	}

	wav->nb_unsupported = 0;

	info.data = NULL;
	adtl.data = NULL;
	cue.data = NULL;
	smpl.data = NULL;
	fact.data = NULL;
	data.data = NULL;
	fmt.data = NULL;

	memset(wav->info, 0, sizeof(wav->info));

	while (riff_sz >= 8) {
		uint_fast32_t      ckid   = cop_ld_ule32(buf);
		uint_fast32_t      cksz   = cop_ld_ule32(buf + 4);
		int                required_chunk = 0;
		unsigned char     *ckbase = buf + 8;
		struct wav_chunk  *known_ptr = NULL;

		riff_sz -= 8;
		buf     += 8;

		if (cksz >= riff_sz) {
			cksz    = riff_sz;
			riff_sz = 0;
		} else {
			buf     += cksz + (cksz & 1);
			riff_sz -= cksz + (cksz & 1);
		}

		if (ckid == RIFF_ID('L', 'I', 'S', 'T') && cksz >= 4) {
			switch (cop_ld_ule32(ckbase)) {
				case RIFF_ID('a', 'd', 't', 'l'):
					known_ptr = &adtl;
					break;
				case RIFF_ID('I', 'N', 'F', 'O'):
					known_ptr = &info;
					break;
				default:
					break;
			}
		} else {
			switch (ckid) {
				case RIFF_ID('d', 'a', 't', 'a'):
					known_ptr = &data;
					required_chunk = 1;
					break;
				case RIFF_ID('f', 'm', 't', ' '):
					known_ptr = &fmt;
					required_chunk = 1;
					break;
				case RIFF_ID('f', 'a', 'c', 't'):
					known_ptr = &fact;
					required_chunk = 1;
					break;
				case RIFF_ID('c', 'u', 'e', ' '):
					known_ptr = &cue;
					break;
				case RIFF_ID('s', 'm', 'p', 'l'):
					known_ptr = &smpl;
					break;
				default:
					break;
			}
		}

		if  (   required_chunk
		    ||  (known_ptr != NULL && !(flags & FLAG_RESET))
		    ||  (known_ptr == NULL && (flags & FLAG_PRESERVE_UNKNOWN))
		    ) {

			if (known_ptr != NULL) {
				/* There are no chunks which we know how to interpret which
				 * can occur more than once. */
				if (known_ptr->data != NULL)
					return warnings | WSR_ERROR_DUPLICATE_CHUNKS;
			} else {
				if (wav->nb_unsupported >= MAX_CHUNKS)
					return warnings | WSR_ERROR_TOO_MANY_CHUNKS;

				known_ptr = wav->unsupported + wav->nb_unsupported++;
			}

			known_ptr->id   = ckid;
			known_ptr->size = cksz;
			known_ptr->data = ckbase;
		}
	}

	if (info.data != NULL) {
		if (WSR_ERROR_CODE(warnings |= load_info(wav->info, info.data, info.size)))
			return warnings;
	}

	if (fmt.data == NULL || data.data == NULL)
		return warnings | WSR_ERROR_NOT_A_WAVE;

	if (WSR_ERROR_CODE(warnings |= load_sample_format(&(wav->format), fmt.data, fmt.size)))
		return warnings;

	{
		uint_fast16_t block_align = (wav->format.channels * get_container_size(wav->format.format));
		wav->data        = data.data;
		wav->data_frames = data.size / block_align;
		if (data.size % block_align)
			return warnings | WSR_ERROR_DATA_INVALID;
	}

	return warnings | load_markers(wav, filename, flags, &adtl, &cue, &smpl);
}
