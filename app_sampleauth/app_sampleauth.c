#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include "cop/cop_conversions.h"

#define FLAG_RESET                (1)
#define FLAG_PRESERVE_UNKNOWN     (2)
#define FLAG_PREFER_SMPL_LOOPS    (4)
#define FLAG_PREFER_CUE_LOOPS     (8)
#define FLAG_STRIP_EVENT_METADATA (16)
#define FLAG_WRITE_CUE_LOOPS      (32)
#define FLAG_OUTPUT_METADATA      (64)
#define FLAG_INPUT_METADATA       (128)

#define MAX_MARKERS (64)
#define MAX_CHUNKS  (32)

struct wav_marker {
	/* Cue ID */
	uint_fast32_t         id;

	/* From labl */
	char                 *name;

	/* From note */
	char                 *desc;

	/* From ltxt or smpl. */
	uint_fast32_t         length;
	int                   has_length;

	/* Is this marker in smpl. */
	int                   in_cue;
	int                   in_smpl;
	uint_fast32_t         position;
};

struct wav_chunk {
	uint_fast32_t  id;
	uint_fast32_t  size;
	int            needs_free;
	unsigned char *data;
};

struct wav {
	uint_fast32_t         data_frames;
	int                   has_pitch_info;
	uint_fast64_t         pitch_info;
	unsigned              nb_marker;
	struct wav_marker     markers[MAX_MARKERS];
	unsigned              nb_chunks;
	struct wav_chunk      chunks[MAX_CHUNKS];

	struct wav_chunk     *fmt;
	struct wav_chunk     *fact;
	struct wav_chunk     *data;
	struct wav_chunk     *adtl;
	struct wav_chunk     *cue;
	struct wav_chunk     *smpl;
};

#define RIFF_ID(c1, c2, c3, c4) \
	(   ((uint_fast32_t)(c1)) \
	|   (((uint_fast32_t)(c2)) << 8) \
	|   (((uint_fast32_t)(c3)) << 16) \
	|   (((uint_fast32_t)(c4)) << 24) \
	)

static struct wav_marker *get_new_marker(struct wav *wav)
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
	(struct wav     *wav
	,uint_fast32_t   id
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

static
int
load_markers
	(struct wav *wav
	,const char *filename
	,unsigned    flags
	)
{
	unsigned i;

	/* Reset the marker count to zero. */
	wav->nb_marker = 0;
	wav->has_pitch_info = 0;

	/* Load metadata strings and labelled text durations first. */
	if (wav->adtl != NULL) {
		size_t adtl_len        = wav->adtl->size;
		unsigned char *adtl    = wav->adtl->data;
		unsigned char *zero_me = NULL;

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

			/* Zero me (if it ever gets set) will likely end up pointing to
			 * the first character in the next chunk (i.e part of its header).
			 * We always zero it after we have read the chunk ID. */
			if (zero_me != NULL) {
				zero_me[0] = 0;
				zero_me = NULL;
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

				/* If the text metadata chunk did not end with a NULL
				 * terminator (which is required by the spec) - ensure that we
				 * insert one at the end of the string. This may write into
				 * the ID of the next chunk, so we delay writing it until
				 * after we have read the next chunk ID. */
				if (!meta_size || meta_base[meta_size-1] != 0) {
					zero_me = meta_base + meta_size;
				}
			}
		}

		if (zero_me != NULL)
			zero_me[0] = '\0';
	}

	/* Next we read the cue points into the marker list. */
	if (wav->cue != NULL) {
		size_t         cue_len = wav->cue->size;
		unsigned char *cue     = wav->cue->data;
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

	if (wav->smpl != NULL) {
		size_t         smpl_len      = wav->smpl->size;
		unsigned char *smpl          = wav->smpl->data;
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
			if (!wav->markers[i].in_smpl && !wav->markers[i].in_cue)
				continue;
			if (wav->markers[i].in_smpl && !wav->markers[i].in_cue)
				nb_smpl_only_loops++;
			if (!wav->markers[i].in_smpl && wav->markers[i].in_cue)
				nb_cue_only_loops++;
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
	}

	/* Reassign IDs and strip text metadata. */
	for (i = 0; i < wav->nb_marker; i++) {
		wav->markers[i].id = i + 1;
		if (flags & FLAG_STRIP_EVENT_METADATA) {
			wav->markers[i].name = NULL;
			wav->markers[i].desc = NULL;
		}
	}

	return 0;
}

/*
 * info.icop
 * smpl.midi_pitch_frac
 * smpl.midi_unity_note
 */



/* -k keep loops in cue chunk */
/* -m discard textual metadata */
/* -i discard RIFF INFO chunk */
/* -p copy pitch information from file into the sample */

/* -c/-s if there is conflicting loop information, prefer the data in the cue
 * or smpl chunk respectively. if neither specified, the program will not
 * update the sample. */

/* -o specify a different output file - otherwise the operation happens on
 * the source wave file. */

void print_usage(FILE *f, const char *pname)
{
	fprintf(f, "Usage:\n  %s\n", pname);
	fprintf(f, "    [ -o ( output filename ) ]\n");
	fprintf(f, "    [ -p ( source pitch filename ) ]\n");
	fprintf(f, "    [ -i ] [ -k ] [ -m ] [ -c | -s ] ( sample filename )\n");



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

static int load_wave_sample(struct wav *wav, unsigned char *buf, size_t bufsz, const char *filename, unsigned flags)
{
	uint_fast32_t riff_sz;

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
	wav->fmt  = NULL;
	wav->fact = NULL;
	wav->data = NULL;
	wav->adtl = NULL;
	wav->cue  = NULL;
	wav->smpl = NULL;

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

		if (ckid == RIFF_ID('L', 'I', 'S', 'T')) {
			if (cksz >= 4 && cop_ld_ule32(ckbase) == RIFF_ID('a', 'd', 't', 'l')) {
				known_ptr = &wav->adtl;
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
				if (*known_ptr != NULL) {
					fprintf(stderr, "%s contained duplicate wave chunks\n", filename);
					return -1;
				}
				*known_ptr = ck;
			}
			ck->id         = ckid;
			ck->size       = cksz;
			ck->needs_free = 0;
			ck->data       = ckbase;
		}
	}

	return load_markers(wav, filename, flags);
}

struct wavauth_options {
	const char  *input_filename;
	const char  *output_filename;
	unsigned     flags;
	unsigned     nb_set_items;
	int         *set_items;
};

static void free_options(struct wavauth_options *opts)
{
	if (opts->set_items != NULL)
		free(opts->set_items);
}

static int handle_options(struct wavauth_options *opts, char **argv, unsigned argc)
{
	int output_inplace = 0;

	opts->input_filename  = NULL;
	opts->output_filename = NULL;
	opts->flags           = 0;
	opts->set_items       = (argc / 3 <= 0) ? NULL : malloc(sizeof(int) * (argc / 3));
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


/*
 * Sample Parsing Options
 *
 * --reset
 *   Removes all chunks are unnecessary for wave operation. i.e. any chunk
 *   which is not "fmt", "fact" or "data" will be removed.
 *
 * --preserve-unknown-chunks
 *   Chunks which were not parsable will be retained in the wave file. This
 *   is normally a bad idea. This flag has no effect when --reset is used.
 *
 * --prefer-smpl-loops
 * --prefer-cue-loops
 *   Specify whether loops should be read from smpl or cue when loops are
 *   found in both and the loops do not match. If neither is specified, the
 *   program will exit with an error message when this condition occurs unless
 *   the loops are all identical. This flag has no effect when --reset is
 *   used. Only has an effect if loops are found in both the cue and smpl
 *   chunks.
 *
 * --strip-event-metadata
 *   Forces text metadata related to markers and loops to be stripped from the
 *   sample. This flag has no effect when --reset is used.
 *
 * --set [ metadata item ] [ value ]
 *
 *   metadata item   value format
 *   loop            start-duration name description
 *   cue             position name description
 *   smpl-pitch      pitch integer
 *   info-icop       string
 *
 * --input-metadata
 *   This flag will read metadata from stdin and overwrite singular metadata
 *   elements (e.g. copyrights, midi pitch information). For non-singular
 *   metadata elements (loops, markers), the behavior is to append to the
 *   metadata (which is useful for merging loops).
 *
 * --output-metadata
 *   This flag can be used to cause the final metadata to be output to stdout.
 *
 * --write-cue-loops
 *   Only has effect when writing an output file. The default behavior is to
 *   strip loops out of the cue chunk and only write them in the sampler chunk.
 *   This flag can be used to add them back in.
 *
 * --output-inplace
 * --output [ filename ]
 *   This flag can be used to save the processed file over the input file or
 *   to write it to a new file.
 *
 */

void printstr(const char *s)
{
	printf("\"");
	if (s != NULL) {
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
	}
	printf("\"");
}

static void dump_metadata(const struct wav *wav)
{
	unsigned i;
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

static void dump_sample(const struct wav *wav, const char *filename)
{
	abort();
}

int main(int argc, char *argv[])
{
	struct wavauth_options opts;
	int err;

	if (argc < 2) {
		print_usage(stdout, argv[0]);
		return 0;
	}
	
	err = handle_options(&opts, argv + 1, argc - 1);
	if (err == 0) {
		size_t sz;
		unsigned char *buf;

		err = read_entire_file(opts.input_filename, &sz, &buf);
		if (err == 0) {
			struct wav wav;

			err = load_wave_sample(&wav, buf, sz, opts.input_filename, opts.flags);
			if (err == 0) {


				if (opts.flags & FLAG_OUTPUT_METADATA)
					dump_metadata(&wav);

				if (opts.output_filename != NULL)
					dump_sample(&wav, opts.output_filename);
			}

			free(buf);
		}
	}
	free_options(&opts);

	return err;
}
