/*
 * install/font_order.c -- the game's fonts in the host's byte order.
 *
 * The fonts are NITRO font files (NFTR, tag "RTFN") in a/0/2/3, which
 * GF_FONT_LoadResource (src/game/arm9/gf_font.c) reads in pieces, straight
 * from the archive, into structs it reads natively: the info block at
 * +0x18 (struct FontInfo), the glyph header (struct GlyphInfo), the width
 * table (struct GlyphWidthTable, the game's own layout: three default
 * widths, then the class, hash and chain offsets) and the character maps
 * from the first map block to the end of the file (struct CMap, chained by
 * file offset).  Read in pieces, they cannot be turned when loaded, so the
 * extractor turns them (install/byteorder.c, k_formats).
 *
 * Turned: the file header's numbers, the sizes of the blocks the game
 * reads, FontInfo's null
 * glyph index and three block offsets, the glyph header's cell size, the
 * width table's three offsets, and each map's header, link and u16 map
 * data.  Left as bytes: the block tags (nothing compares them), the glyph
 * bitmaps and the width table's class, hash and chain bytes (the chain's
 * keys are big-endian bytes the code assembles itself).
 */

#include <string.h>

#include "byteturn.h"
#include "install.h"

/* A font's fields may sit at odd offsets (a glyph block of odd size pads
 * the next one; the game copies each piece into an aligned struct), and
 * nothing in a font is shared, so they are turned in place without
 * byteturn.h's alignment rule and halfword bitset. */
static void fix16u(Turn *t, u32 at)
{
    if (t->n < 2u || at > t->n - 2u) {
        t->bad = 1;
        return;
    }
    bw_mem_wr16(t->p + at, (u16)le16(t, at));
}

static void fix32u(Turn *t, u32 at)
{
    if (t->n < 4u || at > t->n - 4u) {
        t->bad = 1;
        return;
    }
    bw_mem_wr32(t->p + at, le32(t, at));
}

#define fix16 fix16u
#define fix32 fix32u

/* One character map at `b`, `size` bytes left in the file after it. */
static int cmap(Turn *t, u32 b, u32 size)
{
    u32 begin = le16(t, b), end = le16(t, b + 2), method = le16(t, b + 4), units, i;

    if (size < 12) {
        return -1;
    }
    switch (method) {
    case 0:                                 /* direct: the first glyph index */
        units = 1;
        break;
    case 1:                                 /* table: an index per code */
        if (end < begin) {
            return -1;
        }
        units = end - begin + 1;
        break;
    case 2:                                 /* scan: a count, then (code, index) */
        units = 1 + 2 * le16(t, b + 12);
        break;
    default:
        return -1;
    }
    if ((u64)units * 2u > size - 12u) {
        return -1;
    }
    for (i = 0; i < units; i++) {
        fix16(t, b + 12 + 2 * i);
    }
    for (i = 0; i < 8; i += 2) {
        fix16(t, b + i);                    /* begin, end, method, reserved */
    }
    fix32(t, b + 8);                        /* the next map's file offset */
    return 0;
}

/* The size word of the block whose body starts at `body`, if one fits. */
static void block_size(Turn *t, u32 body)
{
    if (body >= 24) {
        fix32(t, body - 4);
    }
}

int bw_nftr_to_host(u8 *p, u32 n)
{
    Turn t;
    u32 glyph, width, map, k;
    int rc = 0;

    if (n == 0 || n < 4 || memcmp(p, "RTFN", 4) != 0) {
        return 0;                           /* a member that is not a font */
    }
    if (n < 0x2C || memcmp(p + 0x10, "FNIF", 4) != 0 || turn_begin(&t, p, n) != 0) {
        return -1;
    }
    /* Where the game goes: FontInfo's offsets (each a block's body), then
     * the maps' chain.  The blocks are not walked by size, as the game does
     * not: a glyph block may carry an odd size with the next block padded. */
    glyph = le32(&t, 0x18 + 8);
    width = le32(&t, 0x18 + 12);
    map = le32(&t, 0x18 + 16);
    if (!inside(glyph, 8, 0, n) || !inside(width, 16, 0, n)) {
        return turn_end(&t, -1);
    }
    fix16(&t, glyph + 2);                   /* the cell size; the cells are bytes */
    block_size(&t, glyph);
    fix32(&t, width + 4);                   /* class, hash and chain offsets */
    fix32(&t, width + 8);
    fix32(&t, width + 12);
    block_size(&t, width);
    for (k = 0; map != 0 && rc == 0; k++) {
        u32 next;

        if (k >= 256 || !inside(map, 12, 0, n)) {
            rc = -1;
            break;
        }
        next = le32(&t, map + 8);
        if (next != 0 && next <= map) {
            rc = -1;                        /* the chain only goes forward */
            break;
        }
        if (next >= n) {
            next = 0;                       /* the last map's link points past
                                             * the file; its scan map covers
                                             * 0..0xFFFF, so it is never taken */
        }
        rc = cmap(&t, map, n - map);
        block_size(&t, map);
        map = next;
    }
    fix16(&t, 0x18 + 2);                    /* the null glyph's index */
    fix32(&t, 0x18 + 8);
    fix32(&t, 0x18 + 12);
    fix32(&t, 0x18 + 16);
    fix32(&t, 0x14);                        /* the info block's size */
    fix16(&t, 4);
    fix16(&t, 6);
    fix32(&t, 8);
    fix16(&t, 12);
    fix16(&t, 14);
    return turn_end(&t, rc);
}
