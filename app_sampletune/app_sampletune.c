#include <stdlib.h>
#include <stdio.h>
#include "cop/cop_thread.h"
#include "portaudio.h"
#include "opendiapason/src/wavldr.h"
#include "opendiapason/src/playeng.h"
#include <math.h>

#define PLAYBACK_SAMPLE_RATE (48000)

struct simple_pipe {
	struct pipe_v1 data;
	double         target_freq;
};

struct pipe_executor {
	struct simple_pipe       pd;
	struct playeng_instance *instance;
	int                      nb_insts;
};

static struct playeng *engine;

static cop_mutex       at_param_lock;

/* Run-time constants. */
static unsigned        at_rank_harmonic64;
static unsigned        at_first_midi;
static unsigned        at_last_midi;

/* Run-time variables. */
static int             tuning_signal_enabled = 1;
static int             tuning_signal_octave;
static unsigned        current_midi;
static double         *pipe_frequencies;
static double         *old_pipe_frequencies;
static unsigned        at_tuning_signal_components;
static int             at_tuning_signal_enabled;
static int             at_tuning_signal_octave;
static unsigned        at_current_midi;
static struct pipe_executor *at_pipes;

static uint_fast32_t  at_tuning_period = (PLAYBACK_SAMPLE_RATE / 1000.0f) * SMPL_POSITION_SCALE + 0.5f;
static uint_fast32_t  at_time = 0;

static double get_target_frequency(int midi_note, unsigned rank_harmonic64)
{
	return 440.0f * powf(2.0f, (midi_note - 69.0f) / 12.0f) * rank_harmonic64 / 8.0f;
}

static int get_target_note(float target_freq, unsigned rank_harmonic64)
{
	return 12.0f * log2f(target_freq * 8.0f / (440.0f * rank_harmonic64)) + 69.0f;
}

static const char *NAMES[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

static struct pipe_executor *
load_executors
	(const char                 *path
	,struct aalloc              *mem
	,struct fastconv_fftset     *fftset
	,const float                *prefilter_data
	,unsigned                    prefilter_conv_len
	,const struct fastconv_pass *prefilter_conv
	,unsigned                    first_midi
	,unsigned                    nb_pipes
	,unsigned                    rank_harmonic64
	)
{
	struct pipe_executor *pipes = malloc(sizeof(*pipes) * nb_pipes);
	unsigned i;
	for (i = 0; i < nb_pipes; i++) {
		const char * err;
		char namebuf[128];
		sprintf(namebuf, "%s/%03d-%s.wav", path, i + first_midi, NAMES[(i+first_midi)%12]);
		err = load_smpl_f(&(pipes[i].pd.data), namebuf, mem, fftset, prefilter_data, SMPL_INVERSE_FILTER_LEN, prefilter_conv_len, prefilter_conv);
		if (err != NULL) {
			printf("WAVE ERR: %s-%s\n", namebuf, err);
			abort();
		}
		pipes[i].pd.target_freq = get_target_frequency(i + first_midi, rank_harmonic64);
		pipes[i].instance = NULL;
		pipes[i].nb_insts = 0;
	}
	return pipes;
}

static
unsigned
engine_callback
	(void              *userdata
	,struct dec_state **states
	,unsigned           sigmask
	,unsigned           old_flags
	,unsigned           sampler_time
	)
{
	struct simple_pipe *pd = userdata;

	/* Initialize sample */
	if (sigmask & 0x1) {
		pd->data.attack.instantiate(states[0], &pd->data.attack, 0, 0);

		/* Only state 0 is enabled and there are no termination conditions. */
		old_flags = PLAYENG_PACK_CALLBACK_STATUS(0, 0x1, 0x0, 0x0);
	}

	if (sigmask & (0x4|0x1)) {
		/* Update rate */
		states[0]->rate = (pd->target_freq * pd->data.sample_rate) * SMPL_POSITION_SCALE / (PLAYBACK_SAMPLE_RATE * pd->data.frequency) + 0.5;
	}

	/* End sample */
	if (sigmask & 0x2) {
		double   np;
		unsigned newi;
		unsigned newf;
		float    f;
		unsigned d = 128;
		int      mn = get_target_note(pd->target_freq, at_rank_harmonic64);
		double   op = states[0]->ipos + states[0]->fpos * (1.0 / SMPL_POSITION_SCALE);

		np   = reltable_find(&pd->data.reltable, op, &f);
		newi = floor(np);
		newf = (unsigned)((np - newi) * SMPL_POSITION_SCALE);
		pd->data.release.instantiate(states[1], &pd->data.release, newi, newf);
		
		printf("%03d-%s RELEASED ipos=%f,rpos=%f,rgain=%f\n", mn, NAMES[mn%12], op, np, (double)f);

		if (f < 0.8) {
			d = (8192 * (0.8 - f) + 128 + 0.5f);
		}

		states[1]->rate = states[0]->rate;
		states[1]->setfade(states[1], 0, 0.0f);
		states[1]->setfade(states[1], d, f);
		states[0]->setfade(states[0], d, 0.0f);
		/* Both state 0 and state 1 are enabled.
		 * State 0 terminates on fade completion.
		 * State 1 terminates on entering of loop. */
		old_flags = PLAYENG_PACK_CALLBACK_STATUS(0, 0x3, 0x1, 0x2);
	}

	return old_flags;
}

static
int
pa_callback
	(const void                     *input
	,void                           *output
	,unsigned long                   frameCount
	,const PaStreamCallbackTimeInfo *timeInfo
	,PaStreamCallbackFlags           statusFlags
	,void                           *userData
	)
{
	unsigned long samp;
	float *ob = output;

	if (cop_mutex_trylock(&at_param_lock)) {
		/* Copy parameters from control thread. */
		at_tuning_signal_enabled = tuning_signal_enabled;

		if (current_midi != at_current_midi || tuning_signal_octave != at_tuning_signal_octave) {
			unsigned i;
			double freq                 = get_target_frequency((int)current_midi + tuning_signal_octave*12, at_rank_harmonic64);
			at_tuning_period            = ((PLAYBACK_SAMPLE_RATE / freq) * SMPL_POSITION_SCALE + 0.5f);
			at_tuning_signal_components = fmin((PLAYBACK_SAMPLE_RATE*0.5) / freq, 5);
			at_tuning_signal_octave     = tuning_signal_octave;
			at_current_midi             = current_midi;

			for (i = at_first_midi; i <= at_last_midi; i++) {
				int pen = at_current_midi == i;
				if (at_pipes[i-at_first_midi].instance == NULL && pen) {
					at_pipes[i-at_first_midi].instance = playeng_insert(engine, 2, 0x01, engine_callback, &(at_pipes[i-at_first_midi].pd));
				}
				if (at_pipes[i-at_first_midi].instance != NULL && !pen) {
					playeng_signal_instance(engine, at_pipes[i-at_first_midi].instance, 0x02);
					at_pipes[i-at_first_midi].instance = NULL;
				}
			}
		} else if (at_pipes[current_midi-at_first_midi].instance != NULL && pipe_frequencies[current_midi-at_first_midi] != at_pipes[current_midi-at_first_midi].pd.data.frequency) {
			/* Update frequency. */
			at_pipes[current_midi-at_first_midi].pd.data.frequency = pipe_frequencies[current_midi-at_first_midi];
			playeng_signal_instance(engine, at_pipes[current_midi-at_first_midi].instance, 0x04);
		}
		cop_mutex_unlock(&at_param_lock);
	}

	if (frameCount % OUTPUT_SAMPLES == 0) {
		float VEC_ALIGN_BEST ipbuf[128];
		float *buffers[2];
		unsigned subframe = frameCount / OUTPUT_SAMPLES;

		buffers[0] = ipbuf + 0;
		buffers[1] = ipbuf + OUTPUT_SAMPLES;

		while (subframe--) {

			float rate = 2.0f * ((float)M_PI) / at_tuning_period;

			/* Synthesis the tuning signal */
			if (at_tuning_signal_enabled) {
				for (samp = 0; samp < frameCount; samp++) {
					float tunesig = 0.0f;
					tunesig += (at_tuning_signal_components > 0) ? sinf(at_time * 1 * rate) : 0.0f;
					tunesig += (at_tuning_signal_components > 1) ? (cosf(at_time * 2 * rate) * 0.5f) : 0.0f;
					tunesig += (at_tuning_signal_components > 2) ? (sinf(at_time * 3 * rate) * 0.25f) : 0.0f;
					tunesig += (at_tuning_signal_components > 3) ? (cosf(at_time * 4 * rate) * 0.125f) : 0.0f;
					tunesig += (at_tuning_signal_components > 4) ? (sinf(at_time * 5 * rate) * 0.0625f) : 0.0f;
					at_time += SMPL_POSITION_SCALE;
					while (at_time >= at_tuning_period) {
						at_time -= at_tuning_period;
					}
					buffers[0][samp] = tunesig * 0.125f;
					buffers[1][samp] = tunesig * 0.125f;
				}
			} else {
				for (samp = 0; samp < frameCount; samp++) {
					buffers[0][samp] = 0.0f;
					buffers[1][samp] = 0.0f;
				}
			}

			/* Run audio engine. */
			playeng_process(engine, buffers, 2, OUTPUT_SAMPLES);

			/* Copy to audio buffers. */
			for (samp = 0; samp < frameCount; samp++) {
				*ob++ = buffers[0][samp] * 0.5;
				*ob++ = buffers[1][samp] * 0.5;
			}
		}
	}
	else
	{
		for (samp = 0; samp < frameCount; samp++) {
			*ob++ = ((rand() / (double)RAND_MAX) - 0.5) * 0.125;
			*ob++ = ((rand() / (double)RAND_MAX) - 0.5) * 0.125;
		}
	}

	return paContinue;
}

static PaStream *setup_sound(void)
{
	PaHostApiIndex def_api;
	const PaHostApiInfo *def_api_info;
	const PaDeviceInfo *def_device_info;
	PaStreamParameters stream_params;
	PaError pa_err;
	PaStream *stream;

	printf("attempting to a default device... ");

	def_api = Pa_GetDefaultHostApi();
	if (def_api < 0) {
		fprintf(stderr, "Pa_GetDefaultHostApi failed (%s)\n", Pa_GetErrorText(def_api));
		return NULL;
	}

	def_api_info = Pa_GetHostApiInfo(def_api);
	if (def_api_info == NULL || def_api_info->defaultOutputDevice == paNoDevice) {
		fprintf(stderr, "Pa_GetHostApiInfo failed or no output devices are available\n");
		return NULL;
	}

	def_device_info = Pa_GetDeviceInfo(def_api_info->defaultOutputDevice);
	if (def_device_info == NULL) {
		fprintf(stderr, "Pa_GetDeviceInfo returned null\n");
		return NULL;
	}

	printf("using %s on %s API\n", def_device_info->name, def_api_info->name);

	printf("opening a %u Hz stream... ", PLAYBACK_SAMPLE_RATE);
	stream_params.device                    = def_api_info->defaultOutputDevice;
	stream_params.channelCount              = 2;
	stream_params.sampleFormat              = paFloat32;
	stream_params.suggestedLatency          = def_device_info->defaultLowOutputLatency * 1.5;
	stream_params.hostApiSpecificStreamInfo = NULL;
	if (Pa_IsFormatSupported(NULL, &stream_params, PLAYBACK_SAMPLE_RATE) != paFormatIsSupported) {
		fprintf(stderr, "the required stream format is not supported\n");
		return NULL;
	}
	pa_err =
		Pa_OpenStream
			(&stream
			,NULL
			,&stream_params
			,PLAYBACK_SAMPLE_RATE
			,64
			,0
			,&pa_callback
			,NULL /* user data */
			);
	if (pa_err != paNoError) {
		fprintf(stderr, "failed to open an output stream (%s)\n", Pa_GetErrorText(pa_err));
		return NULL;
	}
	printf("ok\n");

	Pa_StartStream(stream);

	return stream;
}

#ifdef _WIN32

#include <windows.h>

static int immediate_getchar()
{
	/* TODO - disable echo... */
	return getch();
}

#else

#include <termios.h>
#include <unistd.h>

static int immediate_getchar()
{
	static struct termios oldt, newt;
	int ret;
	tcgetattr(STDIN_FILENO, &oldt);
	newt          = oldt;
	newt.c_lflag &= ~(ECHO | ICANON);
	tcsetattr(STDIN_FILENO, TCSANOW, &newt);
	ret = getchar();
	tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
	return ret;
}

#endif


#define USAGE_STR \
"The tool will load wave files named in the standard way. When the program \n" \
"exits, any modified tuning data will be written back into the input wave \n" \
"file. This is the only field which will be modified in the file.\n" \
"\n" \
"Fundamental pitch specifies the fundamental of the pipe samples and assumes \n" \
"that it exists (i.e. for mixtures you need to be careful with this parameter.\n" \
"\n" \
"',' and '.' move between the previous and next sample.\n" \
"'[' and ']' toggle to octave of the tuning frequency up and down.\n" \
"'+' and '-' turn the volume of the tuning signal up and down.\n" \
"'r'         reset pipe frequency. assume that it is in-tune.\n" \
"'a' and 'f' tune down/up by 100 cents.\n" \
"'s' and 'd' tune down/up by 10 cents.\n" \
"'z' and 'v' tune down/up by 1 Hz.\n" \
"'x' and 'c' tune down/up by 0.1 Hz.\n" \
"'o' and 's' toggle the presence of a sample and octave above or below the \n" \
"            the sample being tuned.\n" \
"'t'         toggle the presence of the tuning signal.\n"

int main_audio(int argc, char *argv[])
{
	PaStream *astream;
	PaError ec;
	int finished = 0;

	printf("initializing PortAudio... ");
	ec = Pa_Initialize();
	if (ec != paNoError) {
		fprintf(stderr, "Pa_Initialize() failed: '%s'\n", Pa_GetErrorText(ec));
		return -1;
	}
	const PaVersionInfo *pav = Pa_GetVersionInfo();
	printf("v%d.%d.%d ok\n", pav->versionMajor, pav->versionMinor, pav->versionSubMinor);

	astream = setup_sound();

	do {
		int c = immediate_getchar();
		if (c == EOF && ferror(stdin)) {
			fprintf(stderr, "error reading from terminal.\n");
			break;
		}

		switch (c) {
		case 'r':
			pipe_frequencies[current_midi-at_first_midi] = at_pipes[current_midi-at_first_midi].pd.target_freq;
			printf("pipe frequency reset: %.3f           \r", pipe_frequencies[current_midi-at_first_midi]);
			break;
		case 'a':
			pipe_frequencies[current_midi-at_first_midi] *= 0.9438743127;
			printf("pipe frequency set at: %.3f           \r", pipe_frequencies[current_midi-at_first_midi]);
			break;
		case 'f':
			pipe_frequencies[current_midi-at_first_midi] *= 1.0594630944;
			printf("pipe frequency set at: %.3f           \r", pipe_frequencies[current_midi-at_first_midi]);
			break;
		case 's':
			pipe_frequencies[current_midi-at_first_midi] *= 0.9942404238;
			printf("pipe frequency set at: %.3f           \r", pipe_frequencies[current_midi-at_first_midi]);
			break;
		case 'd':
			pipe_frequencies[current_midi-at_first_midi] *= 1.0057929411;
			printf("pipe frequency set at: %.3f           \r", pipe_frequencies[current_midi-at_first_midi]);
			break;
		case 'z':
			pipe_frequencies[current_midi-at_first_midi] -= 1.0;
			printf("pipe frequency set at: %.3f           \r", pipe_frequencies[current_midi-at_first_midi]);
			break;
		case 'v':
			pipe_frequencies[current_midi-at_first_midi] += 1.0;
			printf("pipe frequency set at: %.3f           \r", pipe_frequencies[current_midi-at_first_midi]);
			break;
		case 'x':
			pipe_frequencies[current_midi-at_first_midi] -= 0.1;
			printf("pipe frequency set at: %.3f           \r", pipe_frequencies[current_midi-at_first_midi]);
			break;
		case 'c':
			pipe_frequencies[current_midi-at_first_midi] += 0.1;
			printf("pipe frequency set at: %.3f           \r", pipe_frequencies[current_midi-at_first_midi]);
			break;
		case 't':
			tuning_signal_enabled = !tuning_signal_enabled;
			if (tuning_signal_enabled) {
				printf("tuning tone enabled              \r");
			} else {
				printf("tuning tone disabled             \r");
			}
			break;
		case '[':
			if (tuning_signal_octave > -4)
				tuning_signal_octave--;
			printf("tuning signal %d octave adjustment\r", tuning_signal_octave);
			break;
		case ']':
			if (tuning_signal_octave < 4)
				tuning_signal_octave++;
			printf("tuning signal %d octave adjustment\r", tuning_signal_octave);
			break;
		case ',':
			if (current_midi > at_first_midi)
				current_midi--;
			printf("pipe %d frequency is: %.3f           \r", current_midi, pipe_frequencies[current_midi-at_first_midi]);
			break;
		case '.':
			if (current_midi < at_last_midi)
				current_midi++;
			printf("pipe %d frequency is: %.3f           \r", current_midi, pipe_frequencies[current_midi-at_first_midi]);
			break;
		case 'q':
		case EOF:
			finished = 1;
			break;
		default:
			printf("unknown command '%c'\r", c);
		}
	} while (!finished);

	Pa_StopStream(astream);
	Pa_CloseStream(astream);

	return 0;
}

const char *read_entire_file(struct aalloc *a, const char *filename, size_t *sz, void **buf)
{
	const char *err = NULL;
	FILE *f = fopen(filename, "rb");
	if (f != NULL) {
		if (fseek(f, 0, SEEK_END) == 0) {
			long fsz = ftell(f);
			if (fsz >= 0) {
				if (fseek(f, 0, SEEK_SET) == 0) {
					void *fbuf;
					aalloc_push(a);
					fbuf = aalloc_alloc(a, fsz+1);
					if (fbuf != NULL) {
						if (fread(fbuf, 1, fsz, f) == fsz) {
							*sz = fsz;
							*buf = fbuf;
							aalloc_merge_pop(a);
						} else {
							aalloc_pop(a);
							err = "failed to read file";
						}
					} else {
						aalloc_pop(a);
						err = "out of memory";
					}
				} else {
					err = "failed to seek to start of file";
				}
			} else {
				err = "ftell failed";
			}
		} else {
			err = "failed to seek to eof";
		}
		fclose(f);
	} else {
		err = "failed to open file for reading";
	}
	return err;
}

const char *write_entire_file(const char *filename, size_t sz, void *buf)
{
	const char *err = NULL;
	FILE *f = fopen(filename, "wb");
	if (f != NULL) {
		if (fwrite(buf, 1, sz, f) != sz) {
			err = "failed to write file";
		}
		fclose(f);
	} else {
		err = "failed to open file for writing";
	}
	return err;
}

static unsigned parse_le16(const void *data)
{
	const unsigned char *d = data;
	return d[0] | (((unsigned)(d[1])) << 8);
}

static unsigned long parse_le32(const void *data)
{
	return parse_le16(data) | (((unsigned long)parse_le16((const char *)data + 2)) << 16);
}

static void store_le32(void *data, unsigned long v)
{
	unsigned char *d = data;
	d[0] = v & 0xFF;
	d[1] = (v >> 8) & 0xFF;
	d[2] = (v >> 16) & 0xFF;
	d[3] = (v >> 24) & 0xFF;
}

int main(int argc, char *argv[])
{
	int ret;
	unsigned i;
	struct aalloc               mem;

	if (argc < 4) {
		printf("usage:\n");
		printf("  %s [ first midi ] [ last midi ] [ fundamental pitch ]\n", argv[0]);
		printf("%s", USAGE_STR);
		return -1;
	}

	if (cop_mutex_create(&at_param_lock) != 0) {
		fprintf(stderr, "could not create parameter lock mutex.\n");
		return -1;
	}

	engine = playeng_init(2048, 2, 4);
	if (engine == 0) {
		cop_mutex_destroy(&at_param_lock);
		fprintf(stderr, "failed to initialise sampling engine.\n");
		return -1;
	}

	at_first_midi              = atoi(argv[1]);
	at_last_midi               = atoi(argv[2]);
	at_rank_harmonic64         = atoi(argv[3]);
	at_tuning_signal_enabled   = 1;

	current_midi               = at_first_midi;
	at_current_midi            = at_last_midi;

	tuning_signal_octave       = 0;
	at_tuning_signal_octave    = 0;

	aalloc_init(&mem, 32, 512*1024);

	{
		struct fastconv_fftset      fftset;
		const struct fastconv_pass *prefilter_conv;
		unsigned                    prefilter_conv_len;
		float                      *prefilter_data;
		float                      *prefilter_workbuf;

		/* Build the interpolation pre-filter */
		fastconv_fftset_init(&fftset);
		prefilter_conv_len = fastconv_recommend_length(SMPL_INVERSE_FILTER_LEN, 4*SMPL_INVERSE_FILTER_LEN);
		prefilter_conv     = fastconv_get_real_conv(&fftset, prefilter_conv_len);
		prefilter_data     = aalloc_align_alloc(&mem, prefilter_conv_len * sizeof(float), 64);
		aalloc_push(&mem);
		prefilter_workbuf  = aalloc_align_alloc(&mem, prefilter_conv_len * sizeof(float), 64);
		for (i = 0; i < SMPL_INVERSE_FILTER_LEN; i++) {
			prefilter_workbuf[i] = SMPL_INVERSE_COEFS[i] * (2.0 / prefilter_conv_len);
		}
		for (; i < prefilter_conv_len; i++) {
			prefilter_workbuf[i] = 0.0f;
		}
		fastconv_execute_fwd(prefilter_conv, prefilter_workbuf, prefilter_data);
		aalloc_pop(&mem);
		at_pipes = load_executors(".", &mem, &fftset, prefilter_data, prefilter_conv_len, prefilter_conv, at_first_midi, 1+at_last_midi-at_first_midi, at_rank_harmonic64);
		pipe_frequencies = malloc(sizeof(pipe_frequencies[0]) * (1+at_last_midi-at_first_midi));
		old_pipe_frequencies = malloc(sizeof(pipe_frequencies[0]) * (1+at_last_midi-at_first_midi));
		for (i = 0; i < 1+at_last_midi-at_first_midi; i++) {
			pipe_frequencies[i] = at_pipes[i].pd.data.frequency;
			old_pipe_frequencies[i] = at_pipes[i].pd.data.frequency;
		}
	}

	ret = main_audio(argc, argv);

	printf("\n");
	for (i = 0; i < 1+at_last_midi-at_first_midi; i++) {
		size_t sz;
		unsigned char *buf;
		void *db;

		if (fabs(pipe_frequencies[i] - old_pipe_frequencies[i]) < 0.01)
			continue;

		static const char *NAMES[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
		const char *err;
		char namebuf[128];
		sprintf(namebuf, "%s/%03d-%s.wav", ".", i + at_first_midi, NAMES[(i+at_first_midi)%12]);
		printf("Updating tuning in %s from %f->%f\n", namebuf, old_pipe_frequencies[i], pipe_frequencies[i]);

		aalloc_push(&mem);
		err = read_entire_file(&mem, namebuf, &sz, &db);
		if (err == NULL) {
			size_t riffsz;
			buf = db;
			if ((sz >= 12) && ((riffsz = parse_le32(buf + 4)) >= 4)) {
				unsigned char *smpl = NULL;
				sz     -= 12;
				riffsz -= 4;
				if (riffsz > sz)
					riffsz = sz;
				buf += 12;
				while (riffsz > 8 && smpl == NULL) {
					char   *ckid   = buf;
					size_t  cksz   = parse_le32(buf + 4);
					void   *ckdata = buf + 8;
					size_t  padsz  = cksz + (cksz & 1);
					riffsz -= 8;
					buf    += 8;
					if (cksz >= riffsz) {
						cksz   = riffsz;
						riffsz = 0;
					} else {
						assert(padsz < riffsz && "if this gets hit, I can't do logic.");
						riffsz -= padsz;
						buf    += padsz;
					}
					if (ckid[0] == 's' && ckid[1] == 'm' && ckid[2] == 'p' && ckid[3] == 'l' && cksz >= 36) {
						smpl = ckdata;
					}
				}
				if (smpl != NULL) {
					double note = 12 * log2(pipe_frequencies[i] / 440.0) + 69;
					unsigned long int_part  = note;
					unsigned long frac_part = (note - int_part) * (65536.0 * 65536.0);
					store_le32(smpl + 12, int_part);
					store_le32(smpl + 16, frac_part);

					err = write_entire_file(namebuf, sz, db);
					if (err != NULL) {
						fprintf(stderr, "failed to write %s: %s\n", namebuf, err);
					}
				} else {
					fprintf(stderr, "did not find sampler chunk in %s\n", namebuf);
				}
			} else {
				fprintf(stderr, "not a wave file %s\n", namebuf);
			}
		} else {
			fprintf(stderr, "failed to read %s: %s\n", namebuf, err);
		}
		aalloc_pop(&mem);
	}

	playeng_destroy(engine);
	cop_mutex_destroy(&at_param_lock);

	return 0;
}


