#ifndef STRSET_H
#define STRSET_H

#include <stddef.h>

struct strset_buf {
	size_t             size;
	size_t             pos;
	struct strset_buf *next;
};

/* TODO: Might be worth turning this into a "proper" set. */
struct strset {
	struct strset_buf *mem;
};

/* Initialize an empty string set. */
void strset_init(struct strset *s);
void strset_free(struct strset *s);

const char *strset_sprintf(struct strset *s, const char *fmt, ...);

#endif /* STRSET_H */
