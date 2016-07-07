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
#include "wav_sample.h"
#include "wav_sample_write.h"
#include "wav_sample_read.h"

#define MAX_SET_ITEMS             (32)

#define FLAG_STRIP_EVENT_METADATA (16)
#define FLAG_WRITE_CUE_LOOPS      (32)
#define FLAG_OUTPUT_METADATA      (64)
#define FLAG_INPUT_METADATA       (128)

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

	if (opts.flags & FLAG_STRIP_EVENT_METADATA) {
		for (i = 0; i < wav.sample.nb_marker; i++) {
			wav.sample.markers[i].name = NULL;
			wav.sample.markers[i].desc = NULL;
		}
	}

	sort_and_reassign_ids(&wav.sample);

	if (err == 0 && (opts.flags & FLAG_INPUT_METADATA)) {
		char c;
		char linebuf[1024];
		unsigned llen = 0;
		printf("STDIN:");
		while ((c = getchar()) != EOF) {
			if (c == '\r' || c == '\n') {
				linebuf[llen] = '\0';

				llen = 0;
			}
//			printf("%c", c);
		}
		printf("\n");
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
