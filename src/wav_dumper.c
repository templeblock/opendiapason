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

int wav_dumper_begin(struct wav_dumper *dump, const char *filename, size_t channels, unsigned bits_per_sample, uint_fast32_t rate)
{
	unsigned char header[PCM_HEADER_SIZE];

	dump->f = fopen(filename, "wb");
	if (dump->f == NULL)
		return -1;

	dump->riff_size       = PCM_HEADER_SIZE - RIFF_CHUNK_HEADER_SIZE;
	dump->channels        = channels;
	dump->bits_per_sample = bits_per_sample;
	dump->block_align     = channels * ((bits_per_sample + 7) / 8);
	dump->buffer_size     = 0;

	header[0] = 'R';
	header[1] = 'I';
	header[2] = 'F';
	header[3] = 'F';
	cop_st_ule32(header + 4, dump->riff_size);
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
	cop_st_ule32(header + DATA_CHUNK_SIZE_OFFSET, dump->riff_size - (PCM_HEADER_SIZE - RIFF_CHUNK_HEADER_SIZE));

	if (fwrite(header, 44, 1, dump->f) != 1) {
		fclose(dump->f);
		dump->f = NULL;
		return -1;
	}

	return 0;
}

unsigned wav_dumper_write_from_floats(struct wav_dumper *dump, const float *data, unsigned num_samples, unsigned sample_stride, unsigned channel_stride)
{
	uint_fast32_t block_size;
	unsigned num_written;
	unsigned num_can_write_into_buffer;
	unsigned num_can_write_into_empty_buffer;

	block_size = num_samples * dump->block_align;
	if (block_size + dump->riff_size > 0xFFFFFFFFu) {
		block_size = 0xFFFFFFFFu - dump->riff_size;
		num_samples = block_size / num_samples;
	}

	num_can_write_into_empty_buffer = WAV_DUMPER_WRITE_BUFFER_SIZE / dump->block_align;
	assert(num_can_write_into_empty_buffer);
	num_can_write_into_buffer = (WAV_DUMPER_WRITE_BUFFER_SIZE - dump->buffer_size) / dump->block_align;
	num_written = 0;

	while (num_written < num_samples) {
		unsigned can_write = num_samples < num_can_write_into_buffer ? num_samples : num_can_write_into_buffer;
		unsigned i, ch;

		if (dump->bits_per_sample == 24) {
			for (i = 0; i < can_write; i++) {
				for (ch = 0; ch < dump->channels; ch++) {
					float sample = data[(num_written + i) * sample_stride + ch * channel_stride];
					int_fast32_t quant = (int_fast32_t)(sample * (float)0x7FFFFF + (float)(0x800000 + 0.5)) - (int_fast32_t)0x800000;
					uint_fast32_t uquant;
					if (quant < -(int_fast32_t)0x800000)
						quant = -(int_fast32_t)0x800000;
					else if (quant > (int_fast32_t)0x7FFFFF)
						quant = (int_fast32_t)0x7FFFFF;
					uquant = quant;
					cop_st_ule24(dump->buffer + dump->buffer_size, uquant & 0xFFFFFF);
					dump->buffer_size += 3;
				}
			}
		} else {
			assert(dump->bits_per_sample == 16);


		}

		if (num_written + can_write != num_samples) {
			fwrite(dump->buffer, dump->buffer_size, 1, dump->f);
			dump->riff_size   += dump->buffer_size;
			dump->buffer_size  = 0;
		}

		num_written += can_write;
		num_can_write_into_buffer = num_can_write_into_empty_buffer;
	}

	return num_written;
}

int wav_dumper_end(struct wav_dumper *dump)
{
	int ret = 0;

	assert(dump->f != NULL);

	if (dump->buffer_size) {
		ret = (fwrite(dump->buffer, dump->buffer_size, 1, dump->f) == 1) ? 0 : -1;
		dump->riff_size += dump->buffer_size;
	}

	if (!ret) {
		unsigned char riff_sz_buf[4];
		unsigned char data_sz_buf[4];

		cop_st_ule32(riff_sz_buf, dump->riff_size);
		cop_st_ule32(data_sz_buf, dump->riff_size - (PCM_HEADER_SIZE - RIFF_CHUNK_HEADER_SIZE));

		ret =
			(   fseek(dump->f, RIFF_CHUNK_SIZE_OFFSET, SEEK_SET) != 0
			||  fwrite(riff_sz_buf, 4, 1, dump->f) != 1
			||  fseek(dump->f, DATA_CHUNK_SIZE_OFFSET, SEEK_SET) != 0
			||  fwrite(data_sz_buf, 4, 1, dump->f) != 1
			);
	}

	fclose(dump->f);
	dump->f = NULL;
	return ret;
}
