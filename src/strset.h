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

#ifndef STRSET_H
#define STRSET_H

#include <stddef.h>

/* The intention of this component is to hold what is likely to be many
 * thousands of strings without fragmenting memory too much. Strings will
 * never be removed from the set and if a string already exists in the set,
 * it *may* re-use the string pointer (doesn't at the moment because I hacked
 * this together quickly). */
struct strset;

/* Initialize an empty string set. */
void strset_init(struct strset *s);

/* Free all memory associated with a string set. */
void strset_free(struct strset *s);

/* All the following functions add strings into the set. The return value is
 * a pointer to the string. If the return value is NULL, memory was
 * exhausted. */

/* Add a new string into the set using a printf() style format string. */
const char *strset_sprintf(struct strset *s, const char *fmt, ...);

/* ---------------------------------------------------------------------------
 * Private parts. These are defined here so you can put them on the stack -
 * not so you can meddle with their bits. */
struct strset_buf {
	size_t             size;
	size_t             pos;
	struct strset_buf *next;
};
struct strset {
	struct strset_buf *mem;
};

#endif /* STRSET_H */
