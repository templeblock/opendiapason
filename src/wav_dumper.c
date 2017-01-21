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

#include "wav_dumper.h"
#include "cop/cop_conversions.h"
#include <assert.h>

#define RIFF_CHUNK_HEADER_SIZE (8)
#define PCM_HEADER_SIZE        (44)
#define RIFF_CHUNK_SIZE_OFFSET (4)
#define DATA_CHUNK_SIZE_OFFSET (40)

#define INTIAL_RIFF_SIZE       (PCM_HEADER_SIZE - RIFF_CHUNK_HEADER_SIZE)

static COP_ATTR_UNUSED COP_ATTR_ALWAYSINLINE uint_fast32_t update_rnd(uint_fast32_t rnd)
{
	return rnd * 1103515245 + 12345;
}

static int write_interleaved_buffer(struct wav_dumper *dump, struct wav_dumper_buffer *buf)
{
	int            err        = 0;
	unsigned       nb_frames  = buf->nb_frames;
	float         *data       = buf->buf;
	unsigned char *wbp        = dump->write_buffer;
	unsigned       nb_samples = nb_frames * dump->channels;
	size_t         block_size = nb_frames * dump->block_align;
	uint_fast32_t  rseed      = dump->rseed;

	assert(nb_frames);

	if (dump->bits_per_sample == 24) {
		while (nb_samples--) {
			float         sample = *data++;
			uint_fast32_t d1     = update_rnd(rseed);
			uint_fast32_t d2     = update_rnd(d1);
			int_fast64_t  iq;
			rseed  = d2;
			iq     = (int_fast64_t)(d1 & 0xFFFFFFFFu);
			iq    += (int_fast64_t)(d2 & 0xFFFFFFFFu);
			iq    += (int_fast64_t)(sample * (float)(((uint_fast64_t)1) << (33 + 23)));
			iq     = iq >> 33; /* Implementation defined... anything care? */
			if (iq < -(int_fast64_t)0x800000)
				iq = -(int_fast64_t)0x800000;
			else if (iq > (int_fast64_t)0x7FFFFF)
				iq = (int_fast64_t)0x7FFFFF;
			cop_st_sle24(wbp, (int_fast32_t)iq);
			wbp += 3;
		}
		if (fwrite(dump->write_buffer, block_size, 1, dump->f) != 1)
			err = -1;
	} else {
		while (nb_samples--) {
			float         sample = *data++;
			uint_fast32_t d1     = update_rnd(rseed);
			uint_fast32_t d2     = update_rnd(d1);
			int_fast64_t  iq;
			rseed  = d2;
			iq     = (int_fast64_t)(d1 & 0xFFFFFFFFu);
			iq    += (int_fast64_t)(d2 & 0xFFFFFFFFu);
			iq    += (int_fast64_t)(sample * (float)(((uint_fast64_t)1) << (33 + 15)));
			iq     = iq >> 33; /* Implementation defined... anything care? */
			if (iq < -(int_fast64_t)0x8000)
				iq = -(int_fast64_t)0x8000;
			else if (iq > (int_fast64_t)0x7FFF)
				iq = (int_fast64_t)0x7FFF;
			cop_st_sle16(wbp, (int_fast32_t)iq);
			wbp += 2;
		}
		if (fwrite(dump->write_buffer, block_size, 1, dump->f) != 1)
			err = -1;
	}

	dump->rseed = rseed;
	buf->nb_frames = 0;

	return err;
}

static void *wav_dumper_threadproc(void *argument)
{
	struct wav_dumper *dump = argument;

	do {
		struct wav_dumper_buffer  *outbuf;
		int err;

		/* Obtain buffer list. */
		cop_mutex_lock(&dump->thread_lock);
		while (dump->length == 0 && !dump->end_thread) {
			cop_cond_wait(&dump->thread_cond, &dump->thread_lock);
		}
		if (dump->end_thread)
			outbuf = NULL;
		else
			outbuf = dump->buffers + dump->out_pos;
		cop_mutex_unlock(&dump->thread_lock);

		/* Quitting? */
		if (outbuf == NULL)
			return NULL;

		/* Write samples to file. */
		err = write_interleaved_buffer(dump, outbuf);
		
		/* Modify queue. */
		cop_mutex_lock(&dump->thread_lock);
		dump->length  = dump->length - 1;
		dump->out_pos = (dump->out_pos + 1) % dump->nb_buffers;
		if (err)
			dump->write_error = err;
		cop_mutex_unlock(&dump->thread_lock);
	} while (1);

	return NULL;
}

unsigned wav_dumper_write_from_floats(struct wav_dumper *dump, const float *data, unsigned num_samples, unsigned sample_stride, unsigned channel_stride)
{
	unsigned num_written = 0;
	while (num_written < num_samples) {
		unsigned nb_can_write_into_buffer;
		uint_fast32_t nb_can_write_into_wave;
		unsigned can_write;
		unsigned remain;
		unsigned i, ch;
		float    *wb;

		struct wav_dumper_buffer *buf;

		if (dump->nb_buffers > 1) {
			cop_mutex_lock(&dump->thread_lock);
			if (dump->length < dump->nb_buffers) {
				buf = dump->buffers + dump->in_pos;
			}
			else {
				buf = NULL;
			}
			cop_mutex_unlock(&dump->thread_lock);
		} else {
			buf = dump->buffers;
		}

		if (buf == NULL)
			return num_written;

		/* How much is left to write? */
		remain                   = num_samples - num_written;

		/* Limit the write so that it does not overflow the current buffer. */
		nb_can_write_into_buffer = dump->buffer_frames - buf->nb_frames;
		can_write                = (remain < nb_can_write_into_buffer) ? remain : nb_can_write_into_buffer;

		/* Limit the write so that it won't result in an invalid wave file. */
		nb_can_write_into_wave   = dump->max_frames - dump->nb_frames;
		can_write                = (can_write < nb_can_write_into_wave) ? can_write : nb_can_write_into_wave;

		/* Bomb out if we can't write anything. */
		if (can_write == 0)
			return num_written;

		/* Store the input data in the write buffer. */
		wb = buf->buf + (buf->nb_frames * dump->channels);
		for (i = 0; i < can_write; i++) {
			for (ch = 0; ch < dump->channels; ch++) {
				*wb++ = data[(num_written + i) * sample_stride + ch * channel_stride];
			}
		}

		/* Increment the number of frames in the buffer we just wrote into,
		 * the entire wave file and the number of frames we have written this
		 * call. */
		buf->nb_frames    += can_write;
		dump->nb_frames   += can_write;
		num_written       += can_write;

		/* If we did not write the amount remaining, this indicates that
		 * either the wave file is full or the current buffer is now full.
		 * Flush the current buffer to the output file. */
		if (can_write != remain || buf->nb_frames == dump->buffer_frames) {
			if (dump->nb_buffers > 1) {
				cop_mutex_lock(&dump->thread_lock);
				dump->length = dump->length + 1;
				dump->in_pos = (dump->in_pos + 1) % dump->nb_buffers;
				cop_cond_signal(&dump->thread_cond);
				cop_mutex_unlock(&dump->thread_lock);
			} else {
				write_interleaved_buffer(dump, buf);
			}
		}
	}

	return num_written;
}

static size_t align_pad(size_t val, unsigned next_align_mask)
{
	return (1u + next_align_mask - (((unsigned)val) & next_align_mask)) & next_align_mask;
}

static size_t align_first(unsigned first_align_mask)
{
	return first_align_mask;
}

static void *align_buf(void *buf, unsigned align_mask)
{
	unsigned char *b = buf;
	return b + align_pad((size_t)b, align_mask);
}

int
wav_dumper_begin
	(struct wav_dumper *dump
	,const char        *filename
	,unsigned           channels
	,unsigned           bits_per_sample
	,uint_fast32_t      rate
	,unsigned           nb_buffers
	,unsigned           buffer_length
	)
{
	unsigned char header[PCM_HEADER_SIZE];
	size_t memsz;
	unsigned i;
	void *x;
	float *bufdata;

	assert(nb_buffers);

	if (nb_buffers > 1) {
		if (cop_mutex_create(&dump->thread_lock))
			return -1;
		if (cop_cond_create(&dump->thread_cond)) {
			cop_mutex_destroy(&dump->thread_lock);
			return -1;
		}
	}
	dump->rseed             = 0x1EA7F00Du;
	dump->buffer_frames     = buffer_length;
	dump->in_pos            = 0;
	dump->out_pos           = 0;
	dump->length            = 0;
	dump->nb_buffers        = nb_buffers;
	dump->write_error       = 0;
	dump->end_thread        = 0;
	dump->channels          = channels;
	dump->bits_per_sample   = bits_per_sample;
	dump->block_align       = channels * ((bits_per_sample + 7) / 8);
	dump->max_frames        = (0xFFFFFFFFu - INTIAL_RIFF_SIZE) / dump->block_align;
	dump->nb_frames         = 0;

	memsz                   = align_first(31)      + sizeof(unsigned char) * buffer_length * dump->block_align;
	memsz                  += align_pad(memsz, 31) + sizeof(struct wav_dumper_buffer) * nb_buffers;
	memsz                  += align_pad(memsz, 31) + sizeof(float) * nb_buffers * buffer_length * channels;
	dump->mem_base          = (x = malloc(memsz));

	if (dump->mem_base == NULL) {
		if (dump->nb_buffers > 1) {
			cop_cond_destroy(&dump->thread_cond);
			cop_mutex_destroy(&dump->thread_lock);
		}
		return -1;
	}

	dump->write_buffer      = (x = align_buf(x, 31));
	dump->buffers           = (x = align_buf((char *)x + sizeof(unsigned char) * buffer_length * dump->block_align, 31));
	bufdata                 = (x = align_buf((char *)x + sizeof(struct wav_dumper_buffer) * nb_buffers, 31));

	for (i = 0; i < nb_buffers; i++) {
		dump->buffers[i].nb_frames = 0;
		dump->buffers[i].buf = bufdata;
		bufdata += buffer_length * channels;
	}

	dump->f = fopen(filename, "wb");
	if (dump->f == NULL) {
		free(dump->mem_base);
		if (dump->nb_buffers > 1) {
			cop_cond_destroy(&dump->thread_cond);
			cop_mutex_destroy(&dump->thread_lock);
		}
		return -1;
	}

	header[0] = 'R';
	header[1] = 'I';
	header[2] = 'F';
	header[3] = 'F';
	cop_st_ule32(header + 4, INTIAL_RIFF_SIZE);
	header[8] = 'W';
	header[9] = 'A';
	header[10] = 'V';
	header[11] = 'E';
	header[12] = 'f';
	header[13] = 'm';
	header[14] = 't';
	header[15] = ' ';
	cop_st_ule32(header + 16, 16);
	cop_st_ule16(header + 20, 1);
	cop_st_ule16(header + 22, dump->channels);
	cop_st_ule32(header + 24, rate);
	cop_st_ule32(header + 28, rate * dump->block_align);
	cop_st_ule16(header + 32, dump->block_align);
	cop_st_ule16(header + 34, dump->bits_per_sample);
	header[36] = 'd';
	header[37] = 'a';
	header[38] = 't';
	header[39] = 'a';
	cop_st_ule32(header + DATA_CHUNK_SIZE_OFFSET, 0);

	if (fwrite(header, PCM_HEADER_SIZE, 1, dump->f) != 1) {
		free(dump->mem_base);
		if (dump->nb_buffers > 1) {
			cop_cond_destroy(&dump->thread_cond);
			cop_mutex_destroy(&dump->thread_lock);
		}
		fclose(dump->f);
		return -1;
	}

	if (dump->nb_buffers > 1 && cop_thread_create(&dump->thread, wav_dumper_threadproc, dump, 0, 0)) {
		free(dump->mem_base);
		fclose(dump->f);
		cop_cond_destroy(&dump->thread_cond);
		cop_mutex_destroy(&dump->thread_lock);
		return -1;
	}

	return 0;
}

int wav_dumper_end(struct wav_dumper *dump)
{
	int ret = 0;

	if (dump->nb_buffers > 1) {
		cop_mutex_lock(&dump->thread_lock);
		dump->end_thread = 1;
		cop_cond_signal(&dump->thread_cond);
		cop_mutex_unlock(&dump->thread_lock);
		cop_thread_join(dump->thread, NULL);
		cop_cond_destroy(&dump->thread_cond);
		cop_mutex_destroy(&dump->thread_lock);
		ret = dump->write_error;
	} else {
		while (dump->length--) {
			ret = write_interleaved_buffer(dump, dump->buffers + dump->out_pos);
			dump->out_pos = (dump->out_pos + 1) % dump->nb_buffers;
		}
	}

	assert(dump->f != NULL);

	if (!ret && dump->nb_frames) {
		unsigned char riff_sz_buf[4];
		unsigned char data_sz_buf[4];
		uint_fast32_t written = dump->nb_frames * dump->block_align;
		cop_st_ule32(riff_sz_buf, INTIAL_RIFF_SIZE + written);
		cop_st_ule32(data_sz_buf, written);
		ret =
			(   fseek(dump->f, RIFF_CHUNK_SIZE_OFFSET, SEEK_SET) != 0
			||  fwrite(riff_sz_buf, 4, 1, dump->f) != 1
			||  fseek(dump->f, DATA_CHUNK_SIZE_OFFSET, SEEK_SET) != 0
			||  fwrite(data_sz_buf, 4, 1, dump->f) != 1
			);
	}

	free(dump->mem_base);
	fclose(dump->f);
	dump->f = NULL;
	return ret;
}
