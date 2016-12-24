#ifdef _MSC_VER
#define _USE_MATH_DEFINES
#endif

#include <stdlib.h>
#include <stdio.h>
#include "cop/cop_thread.h"
#include "portaudio.h"
#include "opendiapason/src/wav_dumper.h"
#include "opendiapason/src/wavldr.h"
#include "opendiapason/src/playeng.h"
#include "opendiapason/src/strset.h"
#include <math.h>

#define DUMP_TUNING_SESSION "out.wav"

#define PLAYBACK_SAMPLE_RATE (48000)

struct pipe_executor {
	struct pipe_v1           data;

	struct playeng_instance *instance;
	int                      nb_insts;
	double                   target_freq;
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

static COP_ATTR_UNUSED int get_target_note(float target_freq, unsigned rank_harmonic64)
{
	return 12.0f * log2f(target_freq * 8.0f / (440.0f * rank_harmonic64)) + 69.0f;
}

static struct pipe_executor *
load_executors
	(struct sample_load_set     *load_set
	,struct strset              *sset
	,const char                 *path
	,struct cop_salloc_iface    *mem
	,struct fftset              *fftset
	,const struct odfilter      *prefilter
	,unsigned                    first_midi
	,unsigned                    nb_pipes
	,unsigned                    rank_harmonic64
	)
{
	struct pipe_executor *pipes = malloc(sizeof(*pipes) * nb_pipes);
	if (pipes != NULL) {
		unsigned i;
		for (i = 0; i < nb_pipes; i++) {
			static const char *NAMES[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

			struct sample_load_info *sli = sample_load_set_push(load_set);

			if (sli == NULL) {
				printf("out of memory\n");
				abort();
			}
		
			pipes[i].instance     = NULL;
			pipes[i].nb_insts     = 0;
			pipes[i].target_freq  = get_target_frequency(i + first_midi, rank_harmonic64);
			sli->filenames[0]     = strset_sprintf(sset, "%s/A0/%03d-%s.wav", path, i + first_midi, NAMES[(i+first_midi)%12]);
			sli->load_flags[0]    = SMPL_COMP_LOADFLAG_AS;
			sli->filenames[1]     = strset_sprintf(sset, "%s/R0/%03d-%s.wav", path, i + first_midi, NAMES[(i+first_midi)%12]);
			sli->load_flags[1]    = SMPL_COMP_LOADFLAG_R;
			sli->filenames[2]     = strset_sprintf(sset, "%s/R1/%03d-%s.wav", path, i + first_midi, NAMES[(i+first_midi)%12]);
			sli->load_flags[2]    = SMPL_COMP_LOADFLAG_R;
			sli->filenames[3]     = strset_sprintf(sset, "%s/R2/%03d-%s.wav", path, i + first_midi, NAMES[(i+first_midi)%12]);
			sli->load_flags[3]    = SMPL_COMP_LOADFLAG_R;
			sli->num_files        = 4;
			sli->harmonic_number  = rank_harmonic64;
			sli->load_format      = 16;
			sli->dest             = &(pipes[i].data);
			sli->ctx              = pipes + i;
			sli->on_loaded        = NULL;

			if  (   sli->filenames[0] == NULL || sli->filenames[1] == NULL
			    ||  sli->filenames[2] == NULL || sli->filenames[3] == NULL) {
				abort();
			}
		}
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
	struct pipe_executor *pd = userdata;

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
		struct reltable_data rtd;

		reltable_find(&pd->data.reltable, &rtd, states[0]->ipos, states[0]->fpos);

#if OPENDIAPASON_VERBOSE_DEBUG
		{
			int mn = get_target_note(pd->target_freq, at_rank_harmonic64);
			printf("%03d-%s RELEASED pos=(%u,%u),rgain=%f,xfade=%d,id=%d\n", mn, NAMES[mn%12], rtd.pos_int, rtd.pos_frac, rtd.gain, rtd.crossfade, rtd.id);
		}
#endif

		pd->data.releases[rtd.id].instantiate(states[1], &pd->data.releases[rtd.id], rtd.pos_int, rtd.pos_frac);
		states[1]->rate = states[0]->rate;
		states[1]->setfade(states[1], 0, 0.0f);
		states[1]->setfade(states[1], rtd.crossfade, rtd.gain);
		states[0]->setfade(states[0], rtd.crossfade, 0.0f);

		/* Both state 0 and state 1 are enabled.
		 * State 0 terminates on fade completion.
		 * State 1 terminates on entering of loop. */
		old_flags = PLAYENG_PACK_CALLBACK_STATUS(0, 0x3, 0x1, 0x2);
	}

	return old_flags;
}

struct wav_dumper dump;

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
	const float rate = 2.0f * ((float)M_PI) / at_tuning_period;

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
					at_pipes[i-at_first_midi].instance = playeng_insert(engine, 2, 0x01, engine_callback, &(at_pipes[i-at_first_midi].data));
				}
				if (at_pipes[i-at_first_midi].instance != NULL && !pen) {
					playeng_signal_instance(engine, at_pipes[i-at_first_midi].instance, 0x02);
					at_pipes[i-at_first_midi].instance = NULL;
				}
			}
		} else if (at_pipes[current_midi-at_first_midi].instance != NULL && pipe_frequencies[current_midi-at_first_midi] != at_pipes[current_midi-at_first_midi].data.frequency) {
			/* Update frequency. */
			at_pipes[current_midi-at_first_midi].data.frequency = pipe_frequencies[current_midi-at_first_midi];
			playeng_signal_instance(engine, at_pipes[current_midi-at_first_midi].instance, 0x04);
		}
		cop_mutex_unlock(&at_param_lock);
	}

	/* Run audio engine. */
	playeng_process(engine, ob, 2, frameCount);

	for (samp = 0; samp < frameCount; samp++) {
		ob[2*samp+0] *= 0.5f;
		ob[2*samp+1] *= 0.5f;
	}

#ifdef DUMP_TUNING_SESSION
	(void)wav_dumper_write_from_floats(&dump, ob, frameCount, 2, 1);
#endif

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
			*ob++ += tunesig * 0.125f;
			*ob++ += tunesig * 0.125f;
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
			,paFramesPerBufferUnspecified
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

#if 0
	pav = Pa_GetVersionInfo();
	printf("v%d.%d.%d ok\n", pav->versionMajor, pav->versionMinor, pav->versionSubMinor);
#endif

#ifdef DUMP_TUNING_SESSION
	printf("dumping output to " DUMP_TUNING_SESSION "\n");
	if (wav_dumper_begin(&dump, DUMP_TUNING_SESSION, 2, 16, PLAYBACK_SAMPLE_RATE, 6, PLAYBACK_SAMPLE_RATE)) {
		Pa_Terminate();
		fprintf(stderr, "could not open " DUMP_TUNING_SESSION " for writing\n");
		return -1;
	}
#endif

	astream = setup_sound();

	do {
		int c = immediate_getchar();
		if (c == EOF && ferror(stdin)) {
			fprintf(stderr, "error reading from terminal.\n");
			break;
		}

		switch (c) {
		case 'r':
			pipe_frequencies[current_midi-at_first_midi] = at_pipes[current_midi-at_first_midi].target_freq;
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
			pipe_frequencies[current_midi-at_first_midi] -= 0.05;
			printf("pipe frequency set at: %.3f           \r", pipe_frequencies[current_midi-at_first_midi]);
			break;
		case 'c':
			pipe_frequencies[current_midi-at_first_midi] += 0.05;
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

#ifdef DUMP_TUNING_SESSION
	if (wav_dumper_end(&dump))
		fprintf(stderr, "there were issues with the dump file\n");
#endif

	Pa_StopStream(astream);
	Pa_CloseStream(astream);
	Pa_Terminate();

	return 0;
}

const char *read_entire_file(struct cop_salloc_iface *a, const char *filename, size_t *sz, void **buf)
{
	const char *err = NULL;
	FILE *f = fopen(filename, "rb");
	if (f != NULL) {
		if (fseek(f, 0, SEEK_END) == 0) {
			long fsz = ftell(f);
			if (fsz >= 0) {
				if (fseek(f, 0, SEEK_SET) == 0) {
					void *fbuf;
					size_t save = cop_salloc_save(a);
					fbuf = cop_salloc(a, fsz+1, 0);
					if (fbuf != NULL) {
						if (fread(fbuf, 1, fsz, f) == fsz) {
							*sz = fsz;
							*buf = fbuf;
						} else {
							cop_salloc_restore(a, save);
							err = "failed to read file";
						}
					} else {
						cop_salloc_restore(a, save);
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
	struct cop_alloc_virtual mem_impl;
	struct cop_salloc_iface  mem;
	size_t sysmem = cop_memory_query_system_memory();

	if (sysmem == 0) {
		fprintf(stderr, "could not get system memory\n");
		return -1;
	}

	if (argc < 4) {
		printf("usage:\n");
		printf("  %s [ first midi ] [ last midi ] [ fundamental pitch ]\n", argv[0]);
		printf("%s", USAGE_STR);
		return -1;
	}

	{
		size_t lockable = cop_memory_query_current_lockable();
		printf("page size:     %lu\n", (unsigned long)cop_memory_query_page_size());
		printf("system memory: %lu\n", (unsigned long)sysmem);
		if (lockable == SIZE_MAX)
			printf("max lockable:  not limited\n");
		else
			printf("max lockable:  %lu\n", (unsigned long)lockable);
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

	if (at_first_midi > at_last_midi) {
		cop_mutex_destroy(&at_param_lock);
		fprintf(stderr, "last midi index must be greater than or equal to the first midi index.\n");
		return -1;
	}

	current_midi               = at_first_midi;
	at_current_midi            = at_last_midi;

	tuning_signal_octave       = 0;
	at_tuning_signal_octave    = 0;

	if (sysmem > 1024*(size_t)1024*1024) {
		sysmem -= 256*(size_t)1024*1024;
	} else {
		sysmem = 3 * (sysmem / 4);
	}
	cop_alloc_virtual_init(&mem_impl, &mem, sysmem, 32, 16*1024*1024);

	{
		struct fftset          fftset;
		struct odfilter        prefilter;
		struct sample_load_set ls;
		struct strset          sset;
		const char            *err;

		/* Build the interpolation pre-filter */
		fftset_init(&fftset);
		strset_init(&sset);
		ret = sample_load_set_init(&ls);
		if (ret)
			return ret;

		/*TODO */
		(void)odfilter_interp_prefilter_init(&prefilter, &mem, &fftset);

		at_pipes = load_executors(&ls, &sset, ".", &mem, &fftset, &prefilter, at_first_midi, 1+at_last_midi-at_first_midi, at_rank_harmonic64);

		err = load_samples(&ls, &(mem.iface), &fftset, &prefilter);
		if (err != NULL) {
			fprintf(stderr, "load error: %s\n", err);
			abort();
		}

		pipe_frequencies = malloc(sizeof(pipe_frequencies[0]) * (1+at_last_midi-at_first_midi));
		old_pipe_frequencies = malloc(sizeof(pipe_frequencies[0]) * (1+at_last_midi-at_first_midi));
		for (i = 0; i < 1+at_last_midi-at_first_midi; i++) {
			pipe_frequencies[i] = at_pipes[i].data.frequency;
			old_pipe_frequencies[i] = at_pipes[i].data.frequency;
		}
	}

	ret = main_audio(argc, argv);
	if (ret) {
		playeng_destroy(engine);
		cop_mutex_destroy(&at_param_lock);
		return ret;
	}

	printf("\n");
	for (i = 0; i < 1+at_last_midi-at_first_midi; i++) {
		static const char *NAMES[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
		static const char *FMTS[4] = {"%s/A0/%03d-%s.wav", "%s/R0/%03d-%s.wav", "%s/R1/%03d-%s.wav", "%s/R2/%03d-%s.wav"};
		const char *err;
		size_t sz;
		unsigned char *buf;
		void *db;
		char namebuf[128];
		unsigned fidx;

		if (fabs(pipe_frequencies[i] - old_pipe_frequencies[i]) < 0.01)
			continue;

		for (fidx = 0; fidx < 4; fidx++) {
			size_t save;

			sprintf(namebuf, FMTS[fidx], ".", i + at_first_midi, NAMES[(i+at_first_midi)%12]);
			printf("Updating tuning in %s from %f->%f\n", namebuf, old_pipe_frequencies[i], pipe_frequencies[i]);

			save = cop_salloc_save(&mem);
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
						char   *ckid   = (char *)buf;
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
						err = write_entire_file(namebuf, sz + 12, db);
						if (err != NULL) {
							ret = -1;
							fprintf(stderr, "failed to write %s: %s\n", namebuf, err);
						}
					} else {
						ret = -1;
						fprintf(stderr, "did not find sampler chunk in %s\n", namebuf);
					}
				} else {
					ret = -1;
					fprintf(stderr, "not a wave file %s\n", namebuf);
				}
			} else {
				ret = -1;
				fprintf(stderr, "failed to read %s: %s\n", namebuf, err);
			}
			cop_salloc_restore(&mem, save);
		}


	}

	playeng_destroy(engine);
	cop_mutex_destroy(&at_param_lock);

	return ret;
}


