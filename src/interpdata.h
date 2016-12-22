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

#ifndef INTERPDATA_H
#define INTERPDATA_H

/* The filter is symmetric and of odd order and introduces a latency of
 * (SMPL_INVERSE_FILTER_LEN-1)/2. */
#define SMPL_INVERSE_FILTER_LEN (191u)

#define SMPL_POSITION_SCALE     (16384u)
#define SMPL_INTERP_TAPS        (8u)

extern const float SMPL_INVERSE_COEFS[SMPL_INVERSE_FILTER_LEN];
extern const float SMPL_INTERP[SMPL_POSITION_SCALE][SMPL_INTERP_TAPS];

#include "cop/cop_attributes.h"
#include "opendiapason/odfilter.h"

int odfilter_interp_prefilter_init(struct odfilter *pf, struct cop_salloc_iface *allocobj, struct fftset *fftset);

#endif /* INTERPDATA_H */
