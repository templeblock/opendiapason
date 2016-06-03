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
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include "cop/cop_thread.h"
#include <math.h>

#include "fftset/fftset.h"
#include "opendiapason/src/wavldr.h"
#include "cop/cop_alloc.h"
#include "portaudio.h"
#include "portmidi.h"
#include "opendiapason/src/playeng.h"

/* This is high not because I am a deluded "audiophile". It is high, because
 * it gives the playback system heaps of frequency headroom before aliasing
 * occurs. The playback rate of a sample can be doubled before aliasing
 * starts getting folded back down the spectrum.
 *
 * I am relying on the sound driver low-pass filtering this... which is
 * probably a bad assumption. */
#define PLAYBACK_SAMPLE_RATE (96000)

struct simple_pipe {
	struct pipe_v1 data;
	unsigned       rate;
};

struct pipe_executor {
	struct simple_pipe       pd;
	struct playeng_instance *instance;
	int                      nb_insts;
	int                      enabled;
};

struct test_load_entry {
	const char     *directory_name;
	unsigned        first_midi;
	unsigned        nb_pipes;
	unsigned        midi_channel_mask;
	unsigned        harmonic16;
	int             shortcut;
};

#define GT_MIDICH  (0)
#define SW_MIDICH  (1)
#define PED_MIDICH (2)

#define GT  (1 << GT_MIDICH)
#define SW  (1 << SW_MIDICH)
#define PED (1 << PED_MIDICH)

/* This defines the playback rate of the whole organ. It is the pitch of
 * bottom C of a 16-foot rank. Everything will be tuned to this. */
#define ORGAN_PITCH16 (32.5)

/* I have been using samples from Pribac for testing. Each name corresponds to
 * a sub-directory of where the build is made containing samples named in the
 * usual way. The first number is the first MIDI note, the second is the
 * number of pipe samples, the third is a mask of which MIDI channels activate
 * samples, the final is the harmonic base pitch relative to 16. */
static const struct test_load_entry TEST_ENTRY_LIST[] =
{//	{"Bourdon8",       36, 53, GT | PED, 2}
//,	{"Montre8",        36, 53, GT | PED, 2}
//,	{"Salicional8_go", 36, 53, GT | PED, 2}
//,	{"Prestant4",      36, 53, GT | PED, 4}
//,	{"Doublette2",     36, 53, GT | PED, 6}
//,	{"Pleinjeu3",      36, 53, GT | PED, 2}
	{"I Bordun 16",         36, 53, PED | GT,  1, 'a'}
,	{"I Principal 8",       36, 53, PED | GT,  2, 's'}
,	{"I Octave 4",          36, 53, PED | GT,  4, 'd'}
,	{"I Quinte 2 23",       36, 53, GT,        6, 'f'}
,	{"I Octave 2",          36, 53, PED | GT,  8, 'g'}
,	{"I Mixtur 3f",         36, 53, PED | GT,  2, 'h'}
,	{"P Posaune 16",        36, 27, PED,       1, 'z'}
,	{"P Violon 16",         36, 27, PED,       1, 'x'}
,	{"P Subbass 16",        36, 27, PED,       1, 'c'}
,	{"P Octavbass 8",       36, 27, PED,       2, 'v'}
,	{"P Bassflote 8",       36, 27, PED,       2, 'b'}
,	{"II Viola di Gamba 8", 36, 53, SW,        2, '1'}
,	{"II Gedact 8",         36, 53, SW,        2, '2'}
,	{"II Geigen-principal 8", 36, 53, SW,      2, '3'}
,	{"II Praestant 4",      36, 53, SW,        4, '4'}
};

#define NUM_TEST_ENTRY_LIST (sizeof(TEST_ENTRY_LIST) / sizeof(TEST_ENTRY_LIST[0]))

static struct pipe_executor *loaded_ranks[NUM_TEST_ENTRY_LIST];

static struct pipe_executor *
load_executors
	(const char              *path
	,struct aalloc           *mem
	,struct fftset           *fftset
	,const float             *prefilter_data
	,unsigned                 prefilter_conv_len
	,const struct fftset_fft *prefilter_conv
	,unsigned                 first_midi
	,unsigned                 nb_pipes
	,double                   organ_pitch16
	,unsigned                 harmonic16
	)
{
	struct pipe_executor *pipes = malloc(sizeof(*pipes) * nb_pipes);
	unsigned i;
	for (i = 0; i < nb_pipes; i++) {
		float pipe_freq;
		float target_freq;
		static const char *NAMES[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
		const char * err;
		char namebuf[1024];
		sprintf(namebuf, "%s/%03d-%s.wav", path, i + first_midi, NAMES[(i+first_midi)%12]);
		err = load_smpl_f(&(pipes[i].pd.data), namebuf, mem, fftset, prefilter_data, SMPL_INVERSE_FILTER_LEN, prefilter_conv_len, prefilter_conv);
		if (err != NULL) {
			printf("WAVE ERR: %s-%s\n", namebuf, err);
			abort();
		}
		target_freq = organ_pitch16 * harmonic16 * pow(2.0, (i + first_midi - 36) / 12.0);
		pipe_freq   = pipes[i].pd.data.frequency;
		pipes[i].pd.rate = (target_freq * pipes[i].pd.data.sample_rate) * SMPL_POSITION_SCALE / (PLAYBACK_SAMPLE_RATE * pipe_freq) + 0.5;
		pipes[i].instance = NULL;
		pipes[i].nb_insts = 0;
		pipes[i].enabled = 0;
#if OPENDIAPASON_VERBOSE_DEBUG
		printf("%s:%d FREQUENCIES: %f,%f,%u\n", path, i, target_freq, pipe_freq, pipes[i].pd.rate);
#endif
	}
	return pipes;
}

struct playeng *engine;

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
		states[0]->rate = pd->rate;

		/* Only state 0 is enabled and there are no termination conditions. */
		old_flags = PLAYENG_PACK_CALLBACK_STATUS(0, 0x1, 0x0, 0x0);
	}

	/* End sample */
	if (sigmask & 0x2) {
		double   np;
		unsigned newi;
		unsigned newf;
		float    f;
		unsigned d = 128;
		float err;
		unsigned derror;

		np   = reltable_find(&pd->data.reltable, states[0]->ipos + states[0]->fpos * (1.0 / SMPL_POSITION_SCALE), &f, &err);
		err  = -10.0 * log10(err + 1e-18);
		newi = floor(np);
		newf = (unsigned)((np - newi) * SMPL_POSITION_SCALE);
		pd->data.release.instantiate(states[1], &pd->data.release, newi, newf);
		
		if (f < 0.8) {
			d = (8192 * (0.8 - f) + 128 + 0.5f);
		} if (f > 1.1) {
			d = (8192 * fmin((f - 1.1) / (1.3 - 1.1), 1.0) + 128 + 0.5f);
		}

		f = (f > 1.05) ? 1.05 : f;

		derror = (unsigned)((err > 0.5f) ? (err - 0.5f) * (16384.0f / 20.0f) : 0.0f);
		d = (derror > d) ? derror : d;
		
		states[1]->rate = pd->rate;
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

	playeng_process(engine, ob, 2, frameCount);

	for (samp = 0; samp < frameCount; samp++) {
		ob[2*samp+0] *= 1; /* 0.25 */
		ob[2*samp+1] *= 1; /* 0.25 */
	}

	return paContinue;
}

struct midi_stream_data {
	PortMidiStream *pms;
	cop_thread      th;
	void           *ud;
	cop_mutex       abort_signal;
};

#define NEVENTREAD (64)

static void *midi_thread_proc(void *argument)
{
	struct midi_stream_data *msd;
	PortMidiStream *pms;
	PmEvent events[NEVENTREAD];
	int nread = 0;
	int terminated = 0;

	msd = argument;
	pms = msd->pms;
	do {
		int note_locked = 0;

		terminated = cop_mutex_trylock(&msd->abort_signal);
		if (terminated)
			cop_mutex_unlock(&msd->abort_signal);

		while ((nread = Pm_Read(pms, events, NEVENTREAD)) > 0) {
			int i;
			for (i = 0; i < nread; i++) {
				unsigned j;
				long     msg      = events[i].message;
				unsigned channel  = Pm_MessageStatus(msg) & 0x0F;
				unsigned evtid    = Pm_MessageStatus(msg) & 0xF0;
				unsigned idx      = Pm_MessageData1(msg);
				unsigned velocity = Pm_MessageData2(msg);

				for (j = 0; j < NUM_TEST_ENTRY_LIST; j++) {
					unsigned midx = idx;

					if ((TEST_ENTRY_LIST[j].midi_channel_mask & (1ul << channel)) == 0)
						continue;

					if (midx < TEST_ENTRY_LIST[j].first_midi)
						continue;

					midx -= TEST_ENTRY_LIST[j].first_midi;

					if (midx >= TEST_ENTRY_LIST[j].nb_pipes)
						continue;

					if (!loaded_ranks[j][midx].enabled)
						continue;

					if (evtid == 0x80 || (evtid == 0x90 && velocity == 0x00)) {
						if (loaded_ranks[j][midx].nb_insts < 1)
							continue;

						loaded_ranks[j][midx].nb_insts--;

						if (loaded_ranks[j][midx].nb_insts != 0 || loaded_ranks[j][midx].instance == NULL)
							continue;

						if (!note_locked) {
							playeng_push_block_insertion(engine);
							playeng_signal_block(engine, 0x3);
							note_locked = 1;
						}

						playeng_signal_instance(engine, loaded_ranks[j][midx].instance, 0x02);
						loaded_ranks[j][midx].instance = NULL;
					} else if (evtid == 0x90) {
						loaded_ranks[j][midx].nb_insts++;

						if (loaded_ranks[j][midx].instance != NULL)
							continue;

						if (!note_locked) {
							playeng_push_block_insertion(engine);
							playeng_signal_block(engine, 0x3);
							note_locked = 1;
						}

						loaded_ranks[j][midx].instance = playeng_insert(engine, 2, 0x01, engine_callback, &(loaded_ranks[j][midx].pd));
						if (loaded_ranks[j][midx].instance == NULL)
							printf("Polyphony exceeded!\n");
					}
				}
			}
		}

		if (note_locked) {
			playeng_signal_unblock(engine, 0x3);
			playeng_pop_block_insertion(engine);
		}

		Pa_Sleep(10);
	} while (nread != 0 || !terminated);

	(void)Pm_Close(pms);
	free(msd);

	return NULL;
}

static struct midi_stream_data *start_midi(PmDeviceID midi_in_id, void *ud)
{
	int threrr;
	PmError merr;
	struct midi_stream_data *msd;

	if ((msd = malloc(sizeof(*msd))) == NULL)
		return NULL;

	msd->ud = ud;

	if (cop_mutex_create(&msd->abort_signal)) {
		free(msd);
		return NULL;
	}

	if ((merr = Pm_OpenInput(&msd->pms, midi_in_id, NULL, 128, NULL, NULL)) != pmNoError) {
		cop_mutex_destroy(&msd->abort_signal);
		free(msd);
		return NULL;
	}

	cop_mutex_lock(&msd->abort_signal);

	if ((threrr = cop_thread_create(&msd->th, midi_thread_proc, msd, 0, 0)) != 0) {
		(void)Pm_Close(msd->pms);
		cop_mutex_unlock(&msd->abort_signal);
		cop_mutex_destroy(&msd->abort_signal);
		free(msd);
		return NULL;
	}

	return msd;
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

static int setup_sound(PmDeviceID midi_devid)
{
	PaHostApiIndex def_api;
	const PaHostApiInfo *def_api_info;
	const PaDeviceInfo *def_device_info;
	PaStreamParameters stream_params;
	PaError pa_err;
	PaStream *stream;
	int input;

	printf("attempting to a default device... ");

	def_api = Pa_GetDefaultHostApi();
	if (def_api < 0) {
		fprintf(stderr, "Pa_GetDefaultHostApi failed (%s)\n", Pa_GetErrorText(def_api));
		return -1;
	}

	def_api_info = Pa_GetHostApiInfo(def_api);
	if (def_api_info == NULL || def_api_info->defaultOutputDevice == paNoDevice) {
		fprintf(stderr, "Pa_GetHostApiInfo failed or no output devices are available\n");
		return -1;
	}

	def_device_info = Pa_GetDeviceInfo(def_api_info->defaultOutputDevice);
	if (def_device_info == NULL) {
		fprintf(stderr, "Pa_GetDeviceInfo returned null\n");
		return -1;
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
		return -1;
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
		return -1;
	}
	printf("ok\n");

	printf("initializing default midi device... ");
	struct midi_stream_data *midi_acs = start_midi(midi_devid, NULL);
	if (midi_acs == NULL) {
		fprintf(stderr, "Failed to start midi thread.\n");
		Pa_CloseStream(stream);
		return -3;
	}
	printf("ok\n");

	Pa_StartStream(stream);

	while ((input = immediate_getchar()) != 'q') {
		unsigned i;
		for (i = 0; i < NUM_TEST_ENTRY_LIST; i++) {
			if (TEST_ENTRY_LIST[i].shortcut == input) {
				if (loaded_ranks[i][0].enabled) {
					unsigned j;
					for (j = 0; j < TEST_ENTRY_LIST[i].nb_pipes; j++) {
						if (loaded_ranks[i][j].nb_insts && loaded_ranks[i][j].instance) {
							playeng_signal_instance(engine, loaded_ranks[i][j].instance, 0x02);
						}
						loaded_ranks[i][j].instance = NULL;
						loaded_ranks[i][j].nb_insts = 0;
						loaded_ranks[i][j].enabled = 0;
					}
				} else {
					unsigned j;
					for (j = 0; j < TEST_ENTRY_LIST[i].nb_pipes; j++) {
						loaded_ranks[i][j].enabled = 1;
					}
				}
			}
		}
	}

	Pa_StopStream(stream);

	cop_mutex_unlock(&midi_acs->abort_signal);
	cop_thread_join(midi_acs->th, NULL);
	cop_mutex_destroy(&midi_acs->abort_signal);

	Pa_CloseStream(stream);

	return 0;
}

#if 0
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <semaphore.h>

static sem_t sigint_sem;

void my_handler(int s)
{
	sem_post(&sigint_sem);
}

int register_sigint()
{
	struct sigaction sigIntHandler;

	sem_init(&sigint_sem, 0, 0);
	sigIntHandler.sa_handler = my_handler;
	sigemptyset(&sigIntHandler.sa_mask);
	sigIntHandler.sa_flags = 0;

	sigaction(SIGINT, &sigIntHandler, NULL);

	pause();

	return 0;
}

int should_terminate()
{
	if (sem_trywait(&sigint_sem) == 0) {
		sem_post(&sigint_sem);
		return 1;
	}
	return 0;
}
#endif


int main(int argc, char *argv[])
{
	PaError ec;
	PmError merr;
	int rv = -1;
	int start_audio = 1;
	PmDeviceID midi_devid = -1;

	printf("OpenDiapason terminal frontend\n");
	printf("----------------------------------\n");
	printf("initializing PortAudio... ");
	ec = Pa_Initialize();
	if (ec != paNoError) {
		fprintf(stderr, "Pa_Initialize() failed: '%s'\n", Pa_GetErrorText(ec));
		return -1;
	}
	printf("ok\n");

	printf("initializing PortMidi... ");
	merr = Pm_Initialize();
	if (merr != pmNoError) {
		fprintf(stderr, "Pm_Initialize() failed: '%s'\n", Pm_GetErrorText(merr));
		Pa_Terminate();
		return -2;
	}
	printf("ok\n");

	engine = playeng_init(4096, 2, 4);
	if (engine == NULL) {
		Pm_Terminate();
		Pa_Terminate();
		fprintf(stderr, "couldn't create playback engine. out of memory.\n");
		return -1;
	}

	if (argc > 1 && strcmp(argv[1], "--midilist") == 0) {
		int i;
		int nd = Pm_CountDevices();
		for (i = 0; i < nd; i++) {
			const PmDeviceInfo* d = Pm_GetDeviceInfo(i);
			if (!d->input)
				continue;

			printf("%d) %s/%s\n", i, d->interf, d->name);
		}
		if (i == 0) {
			printf("no midi devices!\n");
		}
		start_audio = 0;
	}
	else if (argc > 2 && strcmp(argv[1], "--midiname") == 0)
	{
		int i;
		int nd = Pm_CountDevices();
		for (i = 0; i < nd; i++) {
			const PmDeviceInfo* d = Pm_GetDeviceInfo(i);
			if (!d->input)
				continue;
			if (strstr(d->interf, argv[2]) != NULL || strstr(d->name, argv[2]) != NULL) {
				if (midi_devid >= 0) {
					fprintf(stderr, "could not choose between midi devices\n");
					start_audio = 0;
				} else {
					midi_devid = i;
				}
			}
		}
		if (midi_devid < 0) {
			fprintf(stderr, "could not find midi device containing '%s'\n", argv[2]);
			start_audio = 0;
		}
	}

	if (start_audio) {
		unsigned i;
		struct aalloc               mem;
		struct fftset               fftset;
		const struct fftset_fft    *prefilter_conv;
		unsigned                    prefilter_conv_len;
		float                      *prefilter_data;
		float                      *prefilter_workbuf;

		/* Build the interpolation pre-filter */
		aalloc_init(&mem, 32, 512*1024);
		fftset_init(&fftset);
		prefilter_conv_len = fftset_recommend_conv_length(SMPL_INVERSE_FILTER_LEN, 4*SMPL_INVERSE_FILTER_LEN) * 2;
		prefilter_conv     = fftset_create_fft(&fftset, FFTSET_MODULATION_FREQ_OFFSET_REAL, prefilter_conv_len / 2);
		prefilter_data     = aalloc_align_alloc(&mem, prefilter_conv_len * sizeof(float), 64);
		aalloc_push(&mem);
		prefilter_workbuf  = aalloc_align_alloc(&mem, prefilter_conv_len * sizeof(float), 64);
		for (i = 0; i < SMPL_INVERSE_FILTER_LEN; i++) {
			prefilter_workbuf[i] = SMPL_INVERSE_COEFS[i] * (2.0 / prefilter_conv_len);
		}
		for (; i < prefilter_conv_len; i++) {
			prefilter_workbuf[i] = 0.0f;
		}
		fftset_fft_conv_get_kernel(prefilter_conv, prefilter_data, prefilter_workbuf);
		aalloc_pop(&mem);

		for (i = 0; i < NUM_TEST_ENTRY_LIST; i++) {
			printf("loading '%s'\n", TEST_ENTRY_LIST[i].directory_name);
			loaded_ranks[i] = load_executors(TEST_ENTRY_LIST[i].directory_name, &mem, &fftset, prefilter_data, prefilter_conv_len, prefilter_conv, TEST_ENTRY_LIST[i].first_midi, TEST_ENTRY_LIST[i].nb_pipes, ORGAN_PITCH16, TEST_ENTRY_LIST[i].harmonic16);
		}

		rv = setup_sound((midi_devid < 0) ? Pm_GetDefaultInputDeviceID() : midi_devid);

		fftset_destroy(&fftset);
		aalloc_free(&mem);
	}

	/* Who cares? */
	playeng_destroy(engine);
	(void)Pm_Terminate();
	(void)Pa_Terminate();

	return rv;
}

