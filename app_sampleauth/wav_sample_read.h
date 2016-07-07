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

/* The data buffer supplied could not be identified as being waveform audio.
 * The load is aborted and wav is uninitialised. */
#define WSR_ERROR_NOT_A_WAVE              (1u)

/* The format chunk in the wave was corrupt. The load is aborted and wav is
 * uninitialised. */
#define WSR_ERROR_FMT_INVALID             (2u)

/* The format chunk is valid, but does not contain waveform audio which we can
 * use. The load is aborted and wav is uninitialised. */
#define WSR_ERROR_FMT_UNSUPPORTED         (3u)

/* The data chunk is corrupt. This happens if there is not a whole number of
 * sample frames in the data chunk. The load is aborted and wav is
 * uninitialised. */
#define WSR_ERROR_DATA_INVALID            (4u)

/* The waveform contained metadata in the INFO chunk which was not defined in
 * the RIFF spec. The load is aborted and wav is uninitialised. */
#define WSR_ERROR_INFO_UNSUPPORTED        (5u)

/* The adtl metadata chunk was invalid. This can happen if the adtl chunk has
 * been truncated or if a labelled text chunk is encountered which contained
 * unsupported data. The load is aborted and wav is uninitialised. */
#define WSR_ERROR_ADTL_INVALID            (6u)

/* The adtl metadata chunk contained duplicate note or labl entries for a
 * given cue chunk ID. The load is aborted and wav is uninitialised. */
#define WSR_ERROR_ADTL_DUPLICATES         (7u)

/* The cue chunk is invalid. This can happen if the cue chunk has been
 * truncated. The load is aborted and wav is uninitialised. */
#define WSR_ERROR_CUE_INVALID             (8u)

/* The cue chunk contained cue points which shared the same identifier. The
 * load is aborted and wav is uninitialised. */
#define WSR_ERROR_CUE_DUPLICATE_IDS       (9u)

/* The smpl chunk is invalid. This can happen if the smpl chunk has been
 * truncated. The load is aborted and wav is uninitialised. */
#define WSR_ERROR_SMPL_INVALID            (10u)

/* The wave file contained too many unsupported chunks (more than
 * WAV_SAMPLE_MAX_UNSUPPORTED_CHUNKS) to store in wav. The load is aborted and
 * wav is uninitialised. */
#define WSR_ERROR_TOO_MANY_CHUNKS         (11u)

/* The wave file contained unexpected duplicate chunks (as an example, more
 * than one format or data chunk). The load is aborted and wav is
 * uninitialised. */
#define WSR_ERROR_DUPLICATE_CHUNKS        (12u)

/* The wave file contained more than WAV_SAMPLE_MAX_MARKERS positional
 * metadata items. The load is aborted and wav is uninitialised. */
#define WSR_ERROR_TOO_MANY_MARKERS        (13u)

/* The wave file contained markers which contained samples outside the range
 * of the file. The load is aborted and wav is uninitialised. */
#define WSR_ERROR_MARKER_RANGE            (14u)

/* If the error code is this, there were loops that were in conflict within
 * the cue and smpl chunks. This usually indicates that audio editing software
 * updated one chunk without updating another (this has been verified to
 * happen with Adobe Audition which will not update loops stored in the
 * smpl chunk). This is the only error message where the markers in the output
 * can be examined - and this is only for diagnostic purposes. If a marker
 * has:
 *   in_cue && in_smpl && has_length && (length > 0):
 *     The loop has been reconciled and is the same in both chunks.
 *   !in_cue && in_smpl && has_length && (length > 0):
 *     This loop is in the sampler chunk but not in the cue chunk.
 *   in_cue && !in_smpl && has_length && (length > 0):
 *     This loop is in the cue chunk but not in the smpl chunk.
 * All other markers should be ignored. The FLAG_PREFER options will permit
 * the load to continue selecting which items to preserve. */
#define WSR_ERROR_SMPL_CUE_LOOP_CONFLICTS (15u)

/* This warning happens when the RIFF chunk was shortened because the supplied
 * memory was not long enough to contain it. This can happen when a broken
 * wave writing implementation is used to create the audio data. */
#define WSR_WARNING_FILE_TRUNCATION           (0x100u)

/* The adtl chunk contained strings which were not null-terminated. The
 * strings will be ignored. */
#define WSR_WARNING_ADTL_UNTERMINATED_STRINGS (0x200u)

/* The info chunk contained strings which were not null-terminated. The
 * strings will be ignored. */
#define WSR_WARNING_INFO_UNTERMINATED_STRINGS (0x400u)

/* Use this macro to extract the error code from a return value. */
#define WSR_ERROR_CODE(x) ((x) & 0xFFu)

unsigned wav_sample_mount(struct wav_sample *wav, unsigned char *buf, size_t bufsz, unsigned flags);

void sort_and_reassign_ids(struct wav_sample *wav);

#endif /* WAV_SAMPLE_READ_H */
