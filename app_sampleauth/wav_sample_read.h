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

#ifndef WAV_SAMPLE_READ_H
#define WAV_SAMPLE_READ_H

#include "wav_sample.h"

#define FLAG_RESET                (1)
#define FLAG_PRESERVE_UNKNOWN     (2)
#define FLAG_PREFER_SMPL_LOOPS    (4)
#define FLAG_PREFER_CUE_LOOPS     (8)

#define WSR_ERROR_NOT_A_WAVE        (1u)

#define WSR_ERROR_INFO_UNSUPPORTED  (14u)
#define WSR_ERROR_FMT_INVALID       (12u)
#define WSR_ERROR_FMT_UNSUPPORTED   (13u)
#define WSR_ERROR_DATA_INVALID      (4u)
#define WSR_ERROR_ADTL_DUPLICATES   (6u)
#define WSR_ERROR_ADTL_INVALID      (7u)
#define WSR_ERROR_CUE_INVALID       (9u)
#define WSR_ERROR_CUE_DUPLICATE_IDS (10u)
#define WSR_ERROR_SMPL_INVALID      (11u)

#define WSR_ERROR_TOO_MANY_CHUNKS   (2u)
#define WSR_ERROR_DUPLICATE_CHUNKS  (3u)
#define WSR_ERROR_TOO_MANY_MARKERS  (8u)


#define WSR_WARNING_FILE_TRUNCATION           (0x100u)
#define WSR_WARNING_ADTL_UNTERMINATED_STRINGS (0x200u)
#define WSR_WARNING_INFO_UNTERMINATED_STRINGS (0x400u)

#define WSR_ERROR_CODE(x) (x & 0xFFu)

unsigned load_wave_sample(struct wav_sample *wav, unsigned char *buf, size_t bufsz, const char *filename, unsigned flags);

void sort_and_reassign_ids(struct wav_sample *wav);

#endif /* WAV_SAMPLE_READ_H */
