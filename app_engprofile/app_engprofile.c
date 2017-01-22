#include <time.h>
#include <math.h>
#include <stdio.h>
#include "opendiapason/src/playeng.h"
#include "opendiapason/src/decode_least16x2.h"
#include "cop/cop_alloc.h"
#ifndef WIN32
#include <sys/time.h>
#else
#include <windows.h>
#endif

/* Current performance measurements

53*4 consecutive samples

1734229536 TICKS
1766864469 TICKS

2 threads
1481126398 TICKS
1471865085 TICKS

3 threads
1317146429 TICKS
1306609837 TICKS

4 threads
1208621317 TICKS
1204731602 TICKS

*/

/* We will play back this many samples at the same time. */
#define NB_SAMPLES (256)

struct pipe_executor {
	struct dec_smpl          attack;
	struct dec_smpl          release;
	unsigned                 rate;
	struct playeng_instance *instance;
	int                      nb_insts;
};

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
		pd->attack.instantiate(states[0], &pd->attack, 0, 0);
		states[0]->rate = pd->rate;

		/* Only state 0 is enabled and there are no termination conditions. */
		old_flags = PLAYENG_PACK_CALLBACK_STATUS(0, 0x1, 0x0, 0x0);
	}

	/* End sample */
	if (sigmask & 0x2) {
		pd->release.instantiate(states[1], &pd->release, 0, 0);

		states[1]->rate = states[0]->rate;
		states[1]->setfade(states[1], 0, 0.0f);
		states[1]->setfade(states[1], 1024, 1.0f);
		states[0]->setfade(states[0], 1024, 0.0f);

		/* Both state 0 and state 1 are enabled.
		 * State 0 terminates on fade completion.
		 * State 1 terminates on entering of loop. */
		old_flags = PLAYENG_PACK_CALLBACK_STATUS(0, 0x3, 0x1, 0x2);
	}

	return old_flags;
}

#define RNG_A0 (1u+4u*899809363u)
#define PROCESS_ITERATIONS (7500)
#define PROCESS_BUFFER_SIZE (64)


int main(int argc, char *argv[])
{
	float VEC_ALIGN_BEST        buf[PROCESS_BUFFER_SIZE*2];
	unsigned                    i;
	struct pipe_executor        samples[NB_SAMPLES];
	struct cop_salloc_iface     mem;
	struct cop_alloc_virtual    mem_impl;
	struct playeng             *eng;

	uint_fast32_t               rval = 1;

	if ((eng = playeng_init(2048, 2, 8)) == NULL) {
		fprintf(stderr, "could not create instance of playback engine.\n");
		return -1;
	}

	if (cop_alloc_virtual_init(&mem_impl, &mem, 512*1024*1024, 32, 0)) {
		playeng_destroy(eng);
		return -1;
	}

	for (i = 0; i < NB_SAMPLES; i++) {
		unsigned        attack_len  = 48000;
		unsigned        release_len = 48128;
		uint_least16_t *data;
		unsigned        j;

		if ((data = cop_salloc(&mem, sizeof(int_least16_t) * (attack_len + release_len) * 2, 0)) == NULL) {
			fprintf(stderr, "out of memory.\n");
			return -1;
		}

		for (j = 0; j < 2 * (attack_len + release_len); j++) {
			data[j] = (int_least16_t)((uint_least16_t)(rval >> 16));
			rval = (rval * RNG_A0 + 1) & 0xFFFFFFFF;
		}

		samples[i].attack.gain                       = 1.0 / (32768.0 * sqrt(NB_SAMPLES));
		samples[i].attack.nloop                      = 1;
		samples[i].attack.starts[0].start_smpl       = 1999;
		samples[i].attack.starts[0].first_valid_end  = 0;
		samples[i].attack.ends[0].end_smpl           = attack_len - 1;
		samples[i].attack.ends[0].start_idx          = 0;
		samples[i].attack.data                       = data;
		samples[i].attack.instantiate                = u16c2_instantiate;

		samples[i].release.gain                      = 1.0 / (32768.0 * sqrt(NB_SAMPLES));
		samples[i].release.nloop                     = 1;
		samples[i].release.starts[0].start_smpl      = release_len - 128;
		samples[i].release.starts[0].first_valid_end = 0;
		samples[i].release.ends[0].end_smpl          = release_len - 1;
		samples[i].release.ends[0].start_idx         = 0;
		samples[i].release.data                      = data + (attack_len * 2);
		samples[i].release.instantiate               = u16c2_instantiate;

		samples[i].rate                              = ((3*SMPL_POSITION_SCALE)/4) + (rval & (SMPL_POSITION_SCALE/2-1));
		rval                                         = (rval * RNG_A0 + 1) & 0xFFFFFFFF;

		samples[i].instance                          = playeng_insert(eng, 2, 1, engine_callback, &(samples[i]));
	}

#if WIN32
	ULONGLONG spec1 = GetTickCount64();
	ULONGLONG spec2;
	unsigned long long t0 = 0;
#else
	struct timeval spec1;
	struct timeval spec2;
	gettimeofday(&spec1, NULL);
	unsigned long long t0 = __builtin_readcyclecounter();
#endif

	vlf zero = vlf_broadcast(0.0f);
	for (i = 0; i < PROCESS_ITERATIONS; i++) {
		unsigned j;

		for (j = 0; j < (2 * PROCESS_BUFFER_SIZE) / VLF_WIDTH; j++)
			vlf_st(buf + VLF_WIDTH * j, zero);

		playeng_process(eng, buf, 2, PROCESS_BUFFER_SIZE);
	}

#if WIN32
	spec2 = GetTickCount64();
	double ms1 = spec1;
	double ms2 = spec2;
	unsigned long long t1 = 0;
#else
	unsigned long long t1 = __builtin_readcyclecounter();
	gettimeofday(&spec2, NULL);
	double ms1 = spec1.tv_sec * 1000.0 + spec1.tv_usec / 1000.0;
	double ms2 = spec2.tv_sec * 1000.0 + spec2.tv_usec / 1000.0;
#endif

	double execution_time_seconds                 = (ms2 - ms1) / 1000.0;

	/* This is how many stereo output samples were created. */
	double samples_generated                      = PROCESS_ITERATIONS * PROCESS_BUFFER_SIZE;

	/* This is how many stereo samples can be produced per output sample. */
	double samples_generated_per_execution_second = samples_generated / execution_time_seconds;

	/* In one second of execution, we can generate this many output samples. */
	double seconds_generated_per_execution_second = samples_generated_per_execution_second / 44100.0;

	/* In one second of execution, we can generate this many seconds if we
	 * only have one sample playing back. */
	double seconds_generated_per_execution_second_one_sample = seconds_generated_per_execution_second * NB_SAMPLES;




	printf("%llu TICKS (%f ms %f max poly @44.1k)\n", t1 - t0, ms2 - ms1, seconds_generated_per_execution_second_one_sample);
}
