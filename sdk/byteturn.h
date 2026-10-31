/*
 * sdk/byteturn.h -- turning a little-endian file's numbers into the host's
 * order, in place, once each: the helpers sdk/sdat_order.c (the sound
 * archive) and sdk/g2d_order.c (the 2D graphics files) share.
 *
 * Every number is read from the file's little-endian bytes before it is
 * turned, and a bit per halfword remembers what is turned, so a record two
 * offsets share is turned by the first and skipped by the second.  A turn
 * outside the image, or at an odd address, marks the image malformed
 * (Turn.bad).  On a little-endian host a turn writes the value it read.
 */

#ifndef BW_BYTETURN_H
#define BW_BYTETURN_H

#include <stdlib.h>

#include "types.h"

typedef struct Turn {
    u8  *p;
    u32  n;
    u8  *done;          /* one bit per halfword */
    int  bad;
} Turn;

static inline u32 le16(const Turn *t, u32 at)
{
    if (t->n < 2u || at > t->n - 2u) {
        return 0;
    }
    return (u32)t->p[at] | ((u32)t->p[at + 1] << 8);
}

static inline u32 le32(const Turn *t, u32 at)
{
    if (t->n < 4u || at > t->n - 4u) {
        return 0;
    }
    return (u32)t->p[at] | ((u32)t->p[at + 1] << 8) | ((u32)t->p[at + 2] << 16)
           | ((u32)t->p[at + 3] << 24);
}

static inline int is_done(const Turn *t, u32 at)
{
    return (t->done[at >> 4] >> ((at >> 1) & 7u)) & 1u;
}

static inline void mark(Turn *t, u32 at)
{
    t->done[at >> 4] |= (u8)(1u << ((at >> 1) & 7u));
}

static inline void fix16(Turn *t, u32 at)
{
    if (t->n < 2u || at > t->n - 2u || (at & 1u) != 0) {
        t->bad = 1;
        return;
    }
    if (!is_done(t, at)) {
        bw_mem_wr16(t->p + at, (u16)le16(t, at));
        mark(t, at);
    }
}

static inline void fix32(Turn *t, u32 at)
{
    if (t->n < 4u || at > t->n - 4u || (at & 1u) != 0) {
        t->bad = 1;
        return;
    }
    if (!is_done(t, at)) {
        bw_mem_wr32(t->p + at, le32(t, at));
        mark(t, at);
        mark(t, at + 2);
    }
}

/* [at, at + len) inside [lo, lo + size): no overflow. */
static inline int inside(u32 at, u32 len, u32 lo, u32 size)
{
    return at >= lo && len <= size && at - lo <= size - len;
}

static inline int turn_begin(Turn *t, u8 *p, u32 n)
{
    t->p = p;
    t->n = n;
    t->bad = 0;
    t->done = (u8 *)calloc(n / 16u + 1u, 1);
    return t->done != NULL ? 0 : -1;
}

static inline int turn_end(Turn *t, int rc)
{
    free(t->done);
    return rc != 0 || t->bad ? -1 : 1;
}

#endif
