#include "strset.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#define STR_BUFFER_SIZE (128 * 1024)

/* Initialize an empty string set. */
void strset_init(struct strset *s)
{
	s->mem = NULL;
}

void strset_free(struct strset *s)
{
	while (s->mem != NULL) {
		struct strset_buf *b = s->mem;
		s->mem = b->next;
		free(b);
	}
}

static const char *strset_try_sprintf(struct strset_buf *b, size_t *ssz, const char *fmt, va_list ap)
{
	char   *wptr   = (char *)(b + 1) + b->pos;
	size_t  wptrsz = b->size - b->pos;
	int     len_sz = vsnprintf(wptr, wptrsz, fmt, ap);
	size_t  wbsz;

	assert(len_sz >= 0);
	wbsz = ((size_t)len_sz) + 1;

	if (ssz != NULL)
		*ssz = wbsz;

	if (wbsz > wptrsz)
		return NULL;

	b->pos += wbsz;
	return wptr;
}

const char *strset_sprintf(struct strset *s, const char *fmt, ...)
{
	va_list ap;
	size_t  wptrsz;
	const char *ret = NULL;

	va_start(ap, fmt);
	if (s->mem != NULL && s->mem->pos < s->mem->size) {
		ret = strset_try_sprintf(s->mem, &wptrsz, fmt, ap);
	} else {
		int len_sz = vsnprintf(NULL, 0, fmt, ap);
		assert(len_sz >= 0);
		wptrsz = ((size_t)len_sz) + 1;
	}
	va_end(ap);

	if (ret == NULL) {
		size_t             alloc_sz = (wptrsz * 20 > STR_BUFFER_SIZE) ? (wptrsz * 20) : STR_BUFFER_SIZE;
		struct strset_buf *nb       = malloc(sizeof(*nb) + alloc_sz);

		if (nb == NULL)
			return NULL;

		nb->pos  = 0;
		nb->size = alloc_sz;
		nb->next = s->mem;
		s->mem   = nb;

		va_start(ap, fmt);
		ret = strset_try_sprintf(nb, &wptrsz, fmt, ap);
		va_end(ap);

		assert(ret != NULL);
	}

	return ret;
}

