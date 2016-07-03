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

#ifndef WAV_SAMPLE_WRITE_H
#define WAV_SAMPLE_WRITE_H

#include "wav_sample.h"

/* Serialises the data in wav into the given buffer. If buf is NULL, no data
 * will be written. The size argument will always be updated to reflect how
 * much data will be/was written into the buffer.
 *
 * If store_cue_loops is non-zero, cue points and labeled text chunks for the
 * loops will be written.
 *
 * The unsupported chunks in the wav structure will always be written - set
 * the list to NULL to suppress writing them.
 *
 * It is undefined for any of the values in the wav_sample structure to be
 * invalid. */
void wav_sample_serialise(const struct wav_sample *wav, unsigned char *buf, size_t *size, int store_cue_loops);

#endif /* WAV_SAMPLE_WRITE_H */
