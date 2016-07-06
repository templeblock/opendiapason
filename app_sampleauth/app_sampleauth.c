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
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include "cop/cop_conversions.h"
#include "wav_sample.h"
#include "wav_sample_write.h"

#define MAX_SET_ITEMS             (32)

#define FLAG_RESET                (1)
#define FLAG_PRESERVE_UNKNOWN     (2)
#define FLAG_PREFER_SMPL_LOOPS    (4)
#define FLAG_PREFER_CUE_LOOPS     (8)
#define FLAG_STRIP_EVENT_METADATA (16)
#define FLAG_WRITE_CUE_LOOPS      (32)
#define FLAG_OUTPUT_METADATA      (64)
#define FLAG_INPUT_METADATA       (128)

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

static void sort_and_reassign_ids(struct wav_sample *wav)
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
int
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
			if (cksz > adtl_len) {
				fprintf(stderr, "the adtl chunk was not properly formed\n");
				return -1;
			}
			adtl     += cksz;
			adtl_len -= cksz;

			/* Make sure this is a chunk we can actually understand. If there
			 * are chunks in the adtl list which are unknown to us, we could
			 * end up breaking metadata. */
			is_ltxt = (meta_class == RIFF_ID('l', 't', 'x', 't'));
			is_note = (meta_class == RIFF_ID('n', 'o', 't', 'e'));
			if  (  !(is_ltxt && meta_size >= 20)
			    && !((is_note || (meta_class == RIFF_ID('l', 'a', 'b', 'l'))) && meta_size >= 4)
			    ) {
				fprintf(stderr, "sample contained unsupported or invalid adtl metadata\n");
				return -1;
			}

			/* The a metadata item with the given ID associated with this adtl
			 * metadata chunk. */
			marker = get_marker(wav, cop_ld_ule32(meta_base));
			if (marker == NULL) {
				fprintf(stderr, "sample contained too much metadata\n");
				return -1;
			}

			/* Skip over the metadata ID */
			meta_base += 4;
			meta_size -= 4;

			if (is_ltxt) {
				if (marker->has_length) {
					fprintf(stderr, "sample contained multiple ltxt chunks for a single cue point\n");
					return -1;
				}
				marker->has_length = 1;
				marker->length     = cop_ld_ule32(meta_base);
			} else {
				if (!meta_size || meta_base[meta_size-1] != 0) {
					fprintf(stderr, "adtl contained note or labl chunks which were not null-terminated\n");
					continue;
				}

				if (is_note) {
					if (marker->desc != NULL) {
						fprintf(stderr, "sample contained multiple note chunks for a single cue point\n");
						return -1;
					}
					marker->desc = (char *)meta_base;
				} else {
					if (marker->name != NULL) {
						fprintf(stderr, "sample contained multiple labl chunks for a single cue point\n");
						return -1;
					}
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

		if (cue_len < 4 || cue_len < 4 + 24 * (ncue = cop_ld_ule32(cue))) {
			fprintf(stderr, "cue chunk was malformed\n");
			return -1;
		}

		cue += 4;
		while (ncue--) {
			uint_fast32_t cue_id      = cop_ld_ule32(cue);
			struct wav_marker *marker = get_marker(wav, cue_id);
			if (marker == NULL) {
				fprintf(stderr, "sample contained too much metadata\n");
				return -1;
			}

			if (marker->in_cue) {
				fprintf(stderr, "sample countained multiple cue points with the same identifier\n");
				return -1;
			}

			marker->position     = cop_ld_ule32(cue + 20);
			marker->in_cue = 1;
			cue += 24;
		}
	}

	if (smpl_ck != NULL) {
		size_t         smpl_len      = smpl_ck->size;
		unsigned char *smpl          = smpl_ck->data;
		uint_fast32_t  nloop;

		if (smpl_len < 36 || (smpl_len < (36 + (nloop = cop_ld_ule32(smpl + 28)) * 24 + cop_ld_ule32(smpl + 32)))) {
			fprintf(stderr, "smpl chunk was malformed\n");
			return -1;
		}

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

			if (start > end) {
				fprintf(stderr, "smpl chunk had invalid loops\n");
				return -1;
			}

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

	/* Sort the marker list. Bubble-it. */
	if (flags & FLAG_STRIP_EVENT_METADATA) {
		for (i = 0; i < wav->nb_marker; i++) {
			wav->markers[i].name = NULL;
			wav->markers[i].desc = NULL;
		}
	}

	return 0;
}

void print_usage(FILE *f, const char *pname)
{
	fprintf(f, "Usage:\n  %s\n", pname);
	fprintf(f, "    [ \"--output-inplace\" | ( \"--output\" ( filename ) ) ]\n");
	fprintf(f, "    [ \"--output-metadata\" ] [ \"--reset\" ] [ \"--write-cue-loops\" ]\n");
	fprintf(f, "    [ \"--prefer-cue-loops\" | \"--prefer-smpl-loops\" ]\n");
	fprintf(f, "    [ \"--strip-event-metadata\" ] ( sample filename )\n\n");
	fprintf(f, "This tool is used to modify or repair the metadata associated with a sample. It\n");
	fprintf(f, "operates according to the following flow:\n");
	fprintf(f, "1) The sample is loaded. If \"--reset\" is specified, all known chunks which are\n");
	fprintf(f, "   not required for the sample to be considered waveform audio will be deleted.\n");
	fprintf(f, "   Chunks which are not known are always deleted unless the\n");
	fprintf(f, "   \"--preserve-unknown-chunks\" flag is specified. The known and required chunks\n");
	fprintf(f, "   are 'fmt ', 'fact' and 'data'. The known and unrequired chunks are 'INFO',\n");
	fprintf(f, "   'adtl', 'smpl', 'cue '.\n");
	fprintf(f, "2) The 'smpl', 'cue ' and 'adtl' chunks (if any exist) will be parsed to obtain\n");
	fprintf(f, "   loop and release markers. Tools and audio editors which manipulate these\n");
	fprintf(f, "   chunks have proven to occasionally corrupt the data in them. This tool uses\n");
	fprintf(f, "   some (safe) heuristics to correct these issues. There is one issue which\n");
	fprintf(f, "   cannot be corrected automatically: when there are loops in both the cue and\n");
	fprintf(f, "   smpl chunks which do not match. When this happens, the default behavior is to\n");
	fprintf(f, "   abort the load process and terminate with an error message which details what\n");
	fprintf(f, "   the different loops are. If the \"--prefer-cue-loops\" flag is given, loops\n");
	fprintf(f, "   will be taken from the cue chunk. If the \"--prefer-smpl-loops\" flag is\n");
	fprintf(f, "   specified, loops will be taken from the smpl chunk. These two flags only have\n");
	fprintf(f, "   an effect when there is actually an unresolvable issue. i.e. specifying\n");
	fprintf(f, "   \"--prefer-cue-loops\" will not remove loops from the smpl chunk if there are\n");
	fprintf(f, "   no loops in the cue chunk.\n");
	fprintf(f, "3) If \"--strip-event-metadata\" is specified, any *textual* metadata which is\n");
	fprintf(f, "   associated with loops or cue points will be deleted.\n");
	fprintf(f, "4) If \"--input-metadata\" is specified, lines will be read from stdin and will\n");
	fprintf(f, "   be treated as if each one were passed to the \"--set\" option (see below).\n");
	fprintf(f, "5) The \"--set\" argument may be supplied multiple times to add or replace\n");
	fprintf(f, "   metadata elements in the sample. A set string is a command followed by one\n");
	fprintf(f, "   or more whitespace separated parameters. Parameters may be quoted. The\n");
	fprintf(f, "   following commands exist:\n");
	fprintf(f, "     loop ( start sample ) ( duration ) ( name ) ( description )\n");
	fprintf(f, "       Add a loop to the sample. duration must be at least 1.\n");
	fprintf(f, "     cue ( sample ) ( name ) ( description )\n");
	fprintf(f, "       Add a cue point to the sample.\n");
	fprintf(f, "     smpl-pitch [ smpl pitch ]\n");
	fprintf(f, "       Store pitch information in sampler chunk. The value is the MIDI note\n");
	fprintf(f, "       multiplied by 2^32. This is to deal with the way the value is stored in\n");
	fprintf(f, "       the smpl chunk. If the argument is not supplied, the pitch information\n");
	fprintf(f, "       will be removed (this has no effect if the sample contains loops).\n");
	fprintf(f, "     info-XXXX [ string ]\n");
	fprintf(f, "       Store string in the RIFF INFO chunk where XXXX is the ID of the info\n");
	fprintf(f, "       key. See the RIFF MCI spec for a list of keys. Some include:\n");
	fprintf(f, "         info-IARL   Archival location.\n");
	fprintf(f, "         info-IART   Artist.\n");
	fprintf(f, "         info-ICOP   Copyright information.\n");
	fprintf(f, "       If the argument is not supplied, the metadata item will be removed.\n");
	fprintf(f, "6) If \"--output-metadata\" is specified, the metadata which has been loaded and\n");
	fprintf(f, "   potentially modified will be dumped to stdout in a format which can be used\n");
	fprintf(f, "   by \"--input-metadata\".\n");
	fprintf(f, "7) If \"--output-inplace\" is specified, the input file will be re-written with\n");
	fprintf(f, "   the updated metadata. Otherwise if \"--output\" is given, the output file will\n");
	fprintf(f, "   be written to the specified filename. These flags cannot both be specified\n");
	fprintf(f, "   simultaneously. The default behavior is that loops will only be written to\n");
	fprintf(f, "   the smpl chunk and markers will only be written to the cue chunk as this is\n");
	fprintf(f, "   the most compatible form. If \"--write-cue-loops\" is specified, loops will\n");
	fprintf(f, "   also be stored in the cue chunk. This may assist in checking them in editor\n");
	fprintf(f, "   software.\n\n");
	fprintf(f, "Examples:\n");
	fprintf(f, "   %s --reset sample.wav --output-inplace\n", pname);
	fprintf(f, "   Removes all non-essential wave chunks from sample.wav and overwrites the\n");
	fprintf(f, "   existing file.\n\n");
	fprintf(f, "   %s in.wav --output-metadata | grep '^smpl-pitch' | %s dest.wav --input-metadata --output-inplace\n", pname, pname);
	fprintf(f, "   Copy the pitch information from in.wav into dest.wav.\n\n");
}


static int read_entire_file(const char *filename, size_t *sz, unsigned char **buf)
{
	FILE *f = fopen(filename, "rb");
	if (f == NULL) {
		fprintf(stderr, "could not open file %s\n", filename);
		return -1;
	}

	if (fseek(f, 0, SEEK_END) == 0) {
		long fsz = ftell(f);
		if (fsz >= 0) {
			if (fseek(f, 0, SEEK_SET) == 0) {
				unsigned char *fbuf;
				fbuf = malloc(fsz+1);
				if (fbuf != NULL) {
					if (fread(fbuf, 1, fsz, f) == fsz) {
						*sz = fsz;
						*buf = fbuf;
						return 0;
					} else {
						free(fbuf);
						fprintf(stderr, "could not read %s\n", filename);
					}
				} else {
					fprintf(stderr, "out of memory\n");
				}
			} else {
				fprintf(stderr, "could not seek %s\n", filename);
			}
		} else {
			fprintf(stderr, "could not ftell %s\n", filename);
		}
	} else {
		fprintf(stderr, "could not seek eof on %s\n", filename);
	}

	fclose(f);
	return -1;
}

static int load_sample_format(struct wav_sample_format *format, struct wav_chunk *fmt_ck)
{
	static const unsigned char EXTENSIBLE_GUID_SUFFIX[14] = {/* AA, BB, */ 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
	const unsigned char *fmt_ptr = fmt_ck->data;
	const size_t         fmt_sz  = fmt_ck->size;

	uint_fast16_t format_tag;
	uint_fast16_t container_bytes;
	uint_fast16_t block_align;
	uint_fast16_t channels;
	uint_fast16_t bits_per_sample;

	if (fmt_sz < 16) {
		fprintf(stderr, "corrupt format chunk\n");
		return -1;
	}

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
			) {
			fprintf(stderr, "corrupt format chunk\n");
			return -1;
		}

		bits_per_sample = cop_ld_ule16(fmt_ptr + 18);
		format_tag      = cop_ld_ule16(fmt_ptr + 24);

		if (memcmp(fmt_ptr + 26, EXTENSIBLE_GUID_SUFFIX, 14) != 0) {
			fprintf(stderr, "unsupported wave format for sample data\n");
			return -1;
		}
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
	else {
		fprintf(stderr, "unsupported wave format for sample data\n");
		return -1;
	}

	if (block_align != channels * container_bytes) {
		fprintf(stderr, "unsupported wave format for sample data\n");
		return -1;
	}

	if (bits_per_sample > container_bytes * 8) {
		fprintf(stderr, "corrupt format chunk\n");
		return -1;
	}

	return 0;
}

static int load_info(char **infoset, struct wav_chunk *info)
{
	unsigned char *buf;
	size_t sz;

	sz   = info->size;
	buf  = info->data;
	assert(info->size >= 4);
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

		if (cksz == 0 || base[cksz-1] != 0)
			continue;

		for (i = 0; i < NB_SUPPORTED_INFO_TAGS; i++) {
			if (SUPPORTED_INFO_TAGS[i] == ckid) {
				infoset[i] = base;
				break;
			}
		}

		if (i == NB_SUPPORTED_INFO_TAGS) {
			fprintf(stderr,"unsupported RIFF info tag found\n");
			return -1;
		}
	}

	return 0;
}

static int load_wave_sample(struct wav *wav, unsigned char *buf, size_t bufsz, const char *filename, unsigned flags)
{
	uint_fast32_t riff_sz;
	struct wav_chunk **next_unsupported;

	if  (   (bufsz < 12)
	    ||  (cop_ld_ule32(buf) != RIFF_ID('R', 'I', 'F', 'F'))
	    ||  ((riff_sz = cop_ld_ule32(buf + 4)) < 4)
	    ||  (cop_ld_ule32(buf + 8) != RIFF_ID('W', 'A', 'V', 'E'))
	    ) {
		fprintf(stderr, "%s is not a wave file\n", filename);
		return -1;
	}

	riff_sz -= 4;
	bufsz   -= 12;
	buf     += 12;
	if (riff_sz > bufsz) {
		fprintf(stderr, "%s appears to have been truncated\n", filename);
		riff_sz = (uint_fast32_t)bufsz;
	}

	wav->nb_chunks = 0;
	wav->info = NULL;
	wav->fmt  = NULL;
	wav->fact = NULL;
	wav->data = NULL;
	wav->adtl = NULL;
	wav->cue  = NULL;
	wav->smpl = NULL;
	next_unsupported = &(wav->sample.unsupported);

	while (riff_sz >= 8) {
		uint_fast32_t      ckid   = cop_ld_ule32(buf);
		uint_fast32_t      cksz   = cop_ld_ule32(buf + 4);
		int                required_chunk = 0;
		unsigned char     *ckbase = buf + 8;
		struct wav_chunk **known_ptr = NULL;

		riff_sz -= 8;
		buf     += 8;

		if (cksz >= riff_sz) {
			cksz    = riff_sz;
			riff_sz = 0;
		} else {
			buf     += cksz + (cksz & 1);
			riff_sz -= cksz + (cksz & 1);
		}

		if (wav->nb_chunks >= MAX_CHUNKS) {
			fprintf(stderr, "%s contained too many chunks for the authoring tool to manipulate\n", filename);
			return -1;
		}

		if (ckid == RIFF_ID('L', 'I', 'S', 'T') && cksz >= 4) {
			switch (cop_ld_ule32(ckbase)) {
				case RIFF_ID('a', 'd', 't', 'l'):
					known_ptr = &wav->adtl;
					break;
				case RIFF_ID('I', 'N', 'F', 'O'):
					known_ptr = &wav->info;
					break;
				default:
					break;
			}
		} else {
			switch (ckid) {
				case RIFF_ID('d', 'a', 't', 'a'):
					known_ptr = &wav->data;
					required_chunk = 1;
					break;
				case RIFF_ID('f', 'm', 't', ' '):
					known_ptr = &wav->fmt;
					required_chunk = 1;
					break;
				case RIFF_ID('f', 'a', 'c', 't'):
					known_ptr = &wav->fact;
					required_chunk = 1;
					break;
				case RIFF_ID('c', 'u', 'e', ' '):
					known_ptr = &wav->cue;
					break;
				case RIFF_ID('s', 'm', 'p', 'l'):
					known_ptr = &wav->smpl;
					break;
				default:
					break;
			}
		}

		if  (   required_chunk
		    ||  (known_ptr != NULL && !(flags & FLAG_RESET))
		    ||  (known_ptr == NULL && (flags & FLAG_PRESERVE_UNKNOWN))
		    ) {
			struct wav_chunk *ck = wav->chunks + wav->nb_chunks++;

			if (known_ptr != NULL) {
				/* There are no chunks which we know how to interpret which
				 * can occur more than once. */
				if (*known_ptr != NULL) {
					fprintf(stderr, "%s contained duplicate wave chunks\n", filename);
					return -1;
				}
				ck->next   = NULL;
				*known_ptr = ck;
			} else {
				*next_unsupported = ck;
				next_unsupported  = &(ck->next);
			}

			ck->id         = ckid;
			ck->size       = cksz;
			ck->data       = ckbase;
		}
	}

	*next_unsupported = NULL;

	if (wav->fmt == NULL || wav->data == NULL) {
		fprintf(stderr, "the wave file is missing the format or data chunk\n");
		return -1;
	}

	if (load_sample_format(&(wav->sample.format), wav->fmt))
		return -1;

	/* Compute number of frames. */
	{
		uint_fast16_t block_align = (wav->sample.format.channels * get_container_size(wav->sample.format.format));
		wav->sample.data        = wav->data->data;
		wav->sample.data_frames = wav->data->size / block_align;
		if (wav->data->size % block_align) {
			fprintf(stderr, "the wave data chunk was corrupt or of invalid length\n");
			return -1;
		}
	}

	memset(wav->sample.info, 0, sizeof(wav->sample.info));
	if (wav->info != NULL) {
		if (load_info(wav->sample.info, wav->info))
			return -1;
	}

	return load_markers(&(wav->sample), filename, flags, wav->adtl, wav->cue, wav->smpl);
}

struct wavauth_options {
	const char  *input_filename;
	const char  *output_filename;

	unsigned     flags;

	unsigned     nb_set_items;
	char        *set_items[MAX_SET_ITEMS];
};

static int handle_options(struct wavauth_options *opts, char **argv, unsigned argc)
{
	int output_inplace = 0;

	opts->input_filename  = NULL;
	opts->output_filename = NULL;
	opts->flags           = 0;
	opts->nb_set_items    = 0;

	while (argc) {

		if (!strcmp(*argv, "--reset")) {
			argv++;
			argc--;
			opts->flags |= FLAG_RESET;
		} else if (!strcmp(*argv, "--preserve-unknown-chunks")) {
			argv++;
			argc--;
			opts->flags |= FLAG_PRESERVE_UNKNOWN;
		} else if (!strcmp(*argv, "--prefer-smpl-loops")) {
			argv++;
			argc--;
			opts->flags |= FLAG_PREFER_SMPL_LOOPS;
		} else if (!strcmp(*argv, "--prefer-cue-loops")) {
			argv++;
			argc--;
			opts->flags |= FLAG_PREFER_CUE_LOOPS;
		} else if (!strcmp(*argv, "--strip-event-metadata")) {
			argv++;
			argc--;
			opts->flags |= FLAG_STRIP_EVENT_METADATA;
		} else if (!strcmp(*argv, "--write-cue-loops")) {
			argv++;
			argc--;
			opts->flags |= FLAG_WRITE_CUE_LOOPS;
		} else if (!strcmp(*argv, "--output-metadata")) {
			argv++;
			argc--;
			opts->flags |= FLAG_OUTPUT_METADATA;
		} else if (!strcmp(*argv, "--input-metadata")) {
			argv++;
			argc--;
			opts->flags |= FLAG_INPUT_METADATA;
		} else if (!strcmp(*argv, "--output-inplace")) {
			argv++;
			argc--;
			output_inplace = 1;
		} else if (!strcmp(*argv, "--set")) {
			argv++;
			argc--;
			if (!argc) {
				fprintf(stderr, "--set requires an argument.\n");
				return -1;
			}
			if (opts->nb_set_items >= MAX_SET_ITEMS) {
				fprintf(stderr, "too many --set options\n");
				return -1;
			}
			opts->set_items[opts->nb_set_items++] = *argv;
			argv++;
			argc--;
		} else if (!strcmp(*argv, "--output")) {
			argv++;
			argc--;
			if (!argc) {
				fprintf(stderr, "--output requires an argument.\n");
				return -1;
			}
			opts->output_filename = *argv;
			argv++;
			argc--;
		} else if (opts->input_filename == NULL) {
			opts->input_filename = *argv;
			argv++;
			argc--;
		} else {
			fprintf(stderr, "cannot set input file '%s'. already set to '%s'.\n", *argv, opts->input_filename);
			return -1;
		}
	}

	if ((opts->flags & (FLAG_PREFER_CUE_LOOPS | FLAG_PREFER_SMPL_LOOPS)) == (FLAG_PREFER_CUE_LOOPS | FLAG_PREFER_SMPL_LOOPS)) {
		fprintf(stderr, "--prefer-smpl-loops and --prefer-cue-loops are exclusive options\n");
		return -1;
	}

	if (opts->input_filename == NULL) {
		fprintf(stderr, "a wave filename must be specified.\n");
		return -1;
	}

	if (output_inplace) {
		if (opts->output_filename != NULL) {
			fprintf(stderr, "--output cannot be specified with --output-inplace.\n");
			return -1;
		}
		opts->output_filename = opts->input_filename;
	}


	return 0;
}

void printstr(const char *s)
{
	if (s != NULL) {
		printf("\"");
		while (*s != '\0') {
			if (*s == '\"') {
				printf("\\\"");
			} else if (*s == '\\') {
				printf("\\\\");
			} else {
				printf("%c", *s);
			}
			s++;
		}
		printf("\"");
	} else {
		printf("null");
	}
}

static void dump_metadata(const struct wav_sample *wav)
{
	unsigned i;

	for (i = 0; i < NB_SUPPORTED_INFO_TAGS; i++) {
		if (wav->info[i] != NULL) {
			uint_fast32_t id = SUPPORTED_INFO_TAGS[i];
			printf("info-%c%c%c%c ", id & 0xFF, (id >> 8) & 0xFF, (id >> 16) & 0xFF, (id >> 24) & 0xFF); printstr(wav->info[i]); printf("\n");
		}
	}

	if (wav->has_pitch_info)
		printf("smpl-pitch %llu\n", wav->pitch_info);
	for (i = 0; i < wav->nb_marker; i++) {
		assert(wav->markers[i].in_cue || wav->markers[i].in_smpl);
		if (wav->markers[i].has_length && wav->markers[i].length > 0) {
			printf("loop %u %u ", wav->markers[i].position, wav->markers[i].length);
			printstr(wav->markers[i].name);
			printf(" ");
			printstr(wav->markers[i].desc);
		} else {
			printf("cue %u ", wav->markers[i].position);
			printstr(wav->markers[i].name);
			printf(" ");
			printstr(wav->markers[i].desc);
		}
		printf("\n");
	}
}

static int dump_sample(const struct wav *wav, const char *filename, int store_cue_loops)
{
	int err = 0;
	size_t sz;
	size_t sz2;
	unsigned char *data;
	FILE *f;

	/* Find size of entire wave file then allocate memory for it. */
	wav_sample_serialise(&wav->sample, NULL, &sz, store_cue_loops);
	if ((data = malloc(sz)) == NULL) {
		fprintf(stderr, "out of memory\n");
		return -1;
	}

	/* Serialise the wave file to memory. */
	wav_sample_serialise(&wav->sample, data, &sz2, store_cue_loops);

	/* serialise should always return the same size that was queried. */
	assert(sz2 == sz);

	/* Open the output file and write the entire buffer to it. */
	if ((f = fopen(filename, "wb")) != NULL) {
		if (fwrite(data, 1, sz, f) != sz) {
			fprintf(stderr, "could not write to file %s\n", filename);
			err = -1;
		}
		fclose(f);
	} else {
		fprintf(stderr, "could not open %s\n", filename);
		err = -1;
	}

	free(data);

	return 0;
}

static int is_whitespace(char c)
{
	return (c == ' ') || (c == '\t') || (c == '\n') || (c == '\r');
}

static char *handle_identifier(char **cmd_str)
{
	char *s = *cmd_str;
	char *t = s;

	if (*t == '\0' || is_whitespace(*t))
		return NULL;

	do {
		t++;
	} while (*t != '\0' && !is_whitespace(*t));

	if (*t != '\0') {
		*t++ = '\0';
	}
	*cmd_str = t;

	return s;
}

static void eat_whitespace(char **cmd_str)
{
	char *s = *cmd_str;
	while (is_whitespace(*s))
		s++;
	*cmd_str = s;
}

static int expect_whitespace(char **cmd_str)
{
	char *s = *cmd_str;
	if (!is_whitespace(*s))
		return -1;
	while (is_whitespace(*++s));
	*cmd_str = s;
	return 0;
}

static int expect_string(char **output_str, char **cmd_str)
{
	char *s       = *cmd_str;
	char c;
	char *ret_ptr;

	if (*s != '\"')
		return -1;

	ret_ptr     = s;
	c           = *++s;
	*output_str = ret_ptr;

	while (c != '\0' && c != '\"') {
		if (c != '\\') {
			*ret_ptr++ = c;
			c = *++s;
			continue;
		}

		c = *++s;

		switch (c) {
			case '\"':
			case '\\':
				break;
			case 'n':
				c = '\n';
				break;
			case '\0':
			default:
				return -1;
		}

		*ret_ptr++ = c;
		c = *++s;
	}

	if (c != '\"')
		return -1;

	s++;
	*ret_ptr = '\0';
	*cmd_str = s;
	return 0;
}

static int expect_null(char **cmd_str)
{
	char *s = *cmd_str;
	if (s[0] != 'n' || s[1] != 'u' || s[2] != 'l' || s[3] != 'l') {
		return -1;
	}
	*cmd_str = s + 4;
	return 0;
}

static int expect_null_or_str(char **str, char **cmd_str)
{
	char *s = *cmd_str;
	if (*s == '\"')
		return expect_string(str, &s);
	if (expect_null(&s)) {
		fprintf(stderr, "expected a quoted string or 'null'\n");
		return -1;
	}
	*str     = NULL;
	*cmd_str = s;
	return 0;
}

static int expect_int(uint_fast64_t *ival, char **cmd_str)
{
	char *s = *cmd_str;
	uint_fast64_t rv = 0;
	if (*s < '0' || *s > '9')
		return -1;
	do {
		rv = rv * 10 + (*s++ - '0');
	} while (*s >= '0' && *s <= '9');
	*ival    = rv;
	*cmd_str = s;
	return 0;
}

static int expect_end_of_args(char **cmd_str)
{
	char *s = *cmd_str;
	eat_whitespace(&s);
	if (*s != '\0')
		return -1;
	*cmd_str = s;
	return 0;
}

static int handle_loop(struct wav *wav, char *cmd_str)
{
	uint_fast64_t start;
	uint_fast64_t duration;
	char *name;
	char *desc;
	if  (   expect_int(&start, &cmd_str)
	    ||  expect_whitespace(&cmd_str)
	    ||  expect_int(&duration, &cmd_str)
	    ||  expect_whitespace(&cmd_str)
	    ||  expect_null_or_str(&name, &cmd_str)
	    ||  expect_whitespace(&cmd_str)
	    ||  expect_null_or_str(&desc, &cmd_str)
	    ||  expect_end_of_args(&cmd_str)
	    ) {
		fprintf(stderr, "loop command expects two integer arguments followed by two string or null arguments\n");
		return -1;
	}

	if (wav->sample.nb_marker >= MAX_MARKERS) {
		fprintf(stderr, "cannot add another loop - too much marker metadata\n");
		return -1;
	}

	wav->sample.markers[wav->sample.nb_marker].name       = name;
	wav->sample.markers[wav->sample.nb_marker].desc       = desc;
	wav->sample.markers[wav->sample.nb_marker].length     = duration;
	wav->sample.markers[wav->sample.nb_marker].has_length = 1;
	wav->sample.markers[wav->sample.nb_marker].position   = start;
	wav->sample.nb_marker++;

	return 0;
}

static int handle_cue(struct wav *wav, char *cmd_str)
{
	return 0;
}

static int handle_smplpitch(struct wav *wav, char *cmd_str)
{
	uint_fast64_t pitch;

	if (*cmd_str == '\0') {
		wav->sample.has_pitch_info = 0;
		return 0;
	}

	if (expect_int(&pitch, &cmd_str))
		return -1;

	eat_whitespace(&cmd_str);

	if (*cmd_str != '\0') {
		fprintf(stderr, "smpl-pitch command requires one numeric argument\n");
		return -1;
	}

	wav->sample.pitch_info = pitch;
	wav->sample.has_pitch_info = 1;

	return 0;
}

static int handle_info(struct wav *wav, char *ck, char *cmd_str)
{
	if (strlen(ck) == 4) {
		unsigned i;
		uint_fast32_t id = ((uint_fast32_t)ck[0]) | (((uint_fast32_t)ck[1]) << 8) | (((uint_fast32_t)ck[2]) << 16) | (((uint_fast32_t)ck[3]) << 24);
		for (i = 0; i < NB_SUPPORTED_INFO_TAGS; i++) {
			if (id == SUPPORTED_INFO_TAGS[i]) {
				if (expect_null_or_str(&(wav->sample.info[i]), &cmd_str) || expect_end_of_args(&cmd_str)) {
					fprintf(stderr, "info commands requires exactly one string or 'null' argument\n");
					return -1;
				}
				return 0;
			}
		}
	}
	fprintf(stderr, "'%s' is an unsupported INFO chunk\n", ck);
	return -1;
}

static int handle_metastring(struct wav *wav, char *cmd_str)
{
	char *metastring = cmd_str;
	char *command;
	
	eat_whitespace(&cmd_str);

	if ((command = handle_identifier(&cmd_str)) == NULL) {
		fprintf(stderr, "could not parse meta string '%s'\n", metastring);
		return -1;
	}

	eat_whitespace(&cmd_str);

	if (!memcmp(command, "info-", 5))
		return handle_info(wav, command+5, cmd_str);
	if (!strcmp(command, "loop"))
		return handle_loop(wav, cmd_str);
	if (!strcmp(command, "cue"))
		return handle_cue(wav, cmd_str);
	if (!strcmp(command, "smpl-pitch"))
		return handle_smplpitch(wav, cmd_str);

	fprintf(stderr, "Unknown set command: '%s'\n", command);
	return -1;
}

int main(int argc, char *argv[])
{
	struct wavauth_options opts;
	int err;
	size_t sz;
	unsigned char *buf;
	struct wav wav;
	unsigned i;

	if (argc < 2) {
		print_usage(stdout, argv[0]);
		return 0;
	}

	if ((err = handle_options(&opts, argv + 1, argc - 1)) != 0)
		return err;

	if ((err = read_entire_file(opts.input_filename, &sz, &buf)) != 0)
		return err;

	err = load_wave_sample(&wav, buf, sz, opts.input_filename, opts.flags);

	sort_and_reassign_ids(&wav.sample);

	if (err == 0 && (opts.flags & FLAG_INPUT_METADATA)) {
		
		abort();
	}

	for (i = 0; err == 0 && i < opts.nb_set_items; i++) {
		err = handle_metastring(&wav, opts.set_items[i]);
	}

	sort_and_reassign_ids(&wav.sample);

	if (err == 0 && (opts.flags & FLAG_OUTPUT_METADATA))
		dump_metadata(&wav.sample);

	if (err == 0 && opts.output_filename != NULL)
		err = dump_sample(&wav, opts.output_filename, (opts.flags & FLAG_WRITE_CUE_LOOPS) == FLAG_WRITE_CUE_LOOPS);

	free(buf);

	return err;
}
