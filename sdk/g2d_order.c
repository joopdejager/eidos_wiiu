/*
 * sdk/g2d_order.c -- NITRO 2D graphics files in the host's byte order, and
 * NNS_G2dFindBinaryBlock, which turns a file before it walks it (the model
 * is in sdk/g2d_order.h).
 *
 * The layouts are the SDK's, as the game's own unpack code reads them
 * (src/game/arm9/nns_g2d_anim_bank.c, nns_g2d_load.c and
 * nns_g2d_anim_types.h): which base each offset is relative to comes from
 * NNSi_G2dUnpackCellBank, NNSi_G2dUnpackAnimBank and
 * NNSi_G2dUnpackMultiCellBank.  Nothing in this ROM's files uses a cell
 * bank's user-extended attributes or an animation bank's sub-bank, and no
 * reader looks at a label block's body: those are left as they are.
 */

#include <stdio.h>
#include <string.h>

#include "byteturn.h"
#include "g2d_order.h"

/* Block tags as the game compares them: the stored bytes read as a
 * little-endian u32 (the bytes "TTLP" are PLTT). */
#define TAG_PLTT 0x504C5454u
#define TAG_PCMP 0x50434D50u
#define TAG_CHAR 0x43484152u
#define TAG_CPOS 0x43504F53u
#define TAG_SCRN 0x5343524Eu
#define TAG_CEBK 0x4345424Bu
#define TAG_ABNK 0x41424E4Bu
#define TAG_MCBK 0x4D43424Bu
#define TAG_UEXT 0x55455854u

/* Each turns one block's body, at `b`, `size` bytes. */

static int pltt(Turn *t, u32 b, u32 size)
{
    u32 bytes = le32(t, b + 8), off = le32(t, b + 12), i;

    if (size < 16 || off > size || bytes > size - off) {
        return -1;
    }
    for (i = 0; i + 2 <= bytes; i += 2) {
        fix16(t, b + off + i);              /* the colours, BGR555 */
    }
    for (i = 0; i < 16; i += 4) {
        fix32(t, b + i);                    /* format, extended, size, offset */
    }
    return 0;
}

static int pcmp(Turn *t, u32 b, u32 size)
{
    u32 n = le16(t, b), off = le32(t, b + 4), i;

    if (size < 8 || off > size || (u64)n * 2u > size - off) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        fix16(t, b + off + 2 * i);          /* the palette numbers */
    }
    fix16(t, b);
    fix16(t, b + 2);
    fix32(t, b + 4);
    return 0;
}

static int chr(Turn *t, u32 b, u32 size)
{
    u32 i;

    if (size < 24) {
        return -1;
    }
    fix16(t, b);                            /* height, width in tiles */
    fix16(t, b + 2);
    for (i = 4; i < 24; i += 4) {
        fix32(t, b + i);                    /* format, mapping, kind, size, offset */
    }
    return 0;                               /* the pixels are bytes */
}

static int scrn(Turn *t, u32 b, u32 size)
{
    u32 format = le16(t, b + 6), bytes = le32(t, b + 8), i;

    if (size < 12 || bytes > size - 12) {
        return -1;
    }
    if (format != 1) {                      /* 1: affine, one byte an entry */
        for (i = 0; i + 2 <= bytes; i += 2) {
            fix16(t, b + 12 + i);
        }
    }
    for (i = 0; i < 8; i += 2) {
        fix16(t, b + i);                    /* width, height, colour mode, format */
    }
    fix32(t, b + 8);
    return 0;
}

static int cebk(Turn *t, u32 b, u32 size)
{
    u32 n = le16(t, b), attr = le16(t, b + 2), cells = le32(t, b + 4);
    u32 stride = (attr & 1u) ? 16u : 8u, oam, vram = le32(t, b + 12), i, k;

    if (size < 24 || cells > size || (u64)n * stride > size - cells) {
        return -1;
    }
    oam = cells + n * stride;               /* NNSi_G2dGetCellDataBankOamBase */
    for (i = 0; i < n; i++) {
        u32 c = b + cells + i * stride, count = le16(t, c), off = le32(t, c + 4);

        if (oam + off > size || (u64)count * 6u > size - oam - off) {
            return -1;
        }
        for (k = 0; k < count * 3u; k++) {
            fix16(t, b + oam + off + 2 * k);    /* OAM attributes 0..2 */
        }
        fix16(t, c);
        fix16(t, c + 2);
        fix32(t, c + 4);
        if (stride == 16u) {
            for (k = 8; k < 16; k += 2) {
                fix16(t, c + k);            /* the bounding rectangle */
            }
        }
    }
    if (vram != 0) {                        /* {max size, offset} + n {offset, size} */
        u32 arr = le32(t, b + vram + 4);

        if (vram > size - 8u || vram + arr > size || (u64)n * 8u > size - vram - arr) {
            return -1;
        }
        for (i = 0; i < n * 2u; i++) {
            fix32(t, b + vram + arr + 4 * i);
        }
        fix32(t, b + vram);
        fix32(t, b + vram + 4);
    }
    if (le32(t, b + 20) != 0) {
        static int said;
        if (!said) {
            said = 1;
            fprintf(stderr, "bw-port: a cell bank's user-extended attributes are "
                            "left in the DS's order (sdk/g2d_order.c)\n");
        }
    }
    fix16(t, b);
    fix16(t, b + 2);
    for (i = 4; i < 24; i += 4) {
        fix32(t, b + i);
    }
    return 0;
}

/* One frame's content, by the sequence's element kind (animType's low half). */
static void content(Turn *t, u32 c, u32 kind)
{
    switch (kind) {
    case 0:                                 /* index */
        fix16(t, c);
        break;
    case 1:                                 /* index, rotZ, scale x/y, translate x/y */
        fix16(t, c);
        fix16(t, c + 2);
        fix32(t, c + 4);
        fix32(t, c + 8);
        fix16(t, c + 12);
        fix16(t, c + 14);
        break;
    case 2:                                 /* index, pad, translate x/y */
        fix16(t, c);
        fix16(t, c + 2);
        fix16(t, c + 4);
        fix16(t, c + 6);
        break;
    default:
        t->bad = 1;
        break;
    }
}

static const u32 k_content_size[3] = { 2, 16, 8 };

static int abnk(Turn *t, u32 b, u32 size)
{
    u32 ns = le16(t, b), seqs = le32(t, b + 4), frames = le32(t, b + 8), pool = le32(t, b + 12);
    u32 i, j;

    if (size < 24 || seqs > size || (u64)ns * 16u > size - seqs || frames > size || pool > size) {
        return -1;
    }
    for (i = 0; i < ns; i++) {
        u32 s = b + seqs + 16 * i, nf = le16(t, s), kind = le32(t, s + 4) & 0xFFFFu;
        u32 fa = frames + le32(t, s + 12);

        if (kind > 2 || fa > size || (u64)nf * 8u > size - fa) {
            return -1;
        }
        for (j = 0; j < nf; j++) {
            u32 f = b + fa + 8 * j, c = pool + le32(t, f);

            if (c > size || k_content_size[kind] > size - c) {
                return -1;
            }
            content(t, b + c, kind);
            fix32(t, f);
            fix16(t, f + 4);
            fix16(t, f + 6);
        }
        fix16(t, s);
        fix16(t, s + 2);
        fix32(t, s + 4);
        fix32(t, s + 8);
        fix32(t, s + 12);
    }
    if (le32(t, b + 20) != 0) {
        static int said;
        if (!said) {
            said = 1;
            fprintf(stderr, "bw-port: an animation bank's sub-bank is left in the "
                            "DS's order (sdk/g2d_order.c)\n");
        }
    }
    fix16(t, b);
    fix16(t, b + 2);
    for (i = 4; i < 24; i += 4) {
        fix32(t, b + i);
    }
    return 0;
}

static int mcbk(Turn *t, u32 b, u32 size)
{
    u32 n = le16(t, b), arr = le32(t, b + 4), nodes = le32(t, b + 8), i, k;

    if (size < 20 || arr > size || (u64)n * 8u > size - arr || nodes > size) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        u32 m = b + arr + 8 * i, nn = le16(t, m), h = nodes + le32(t, m + 4);

        if (h > size || (u64)nn * 8u > size - h) {
            return -1;
        }
        for (k = 0; k < nn * 4u; k++) {
            fix16(t, b + h + 2 * k);        /* sequence, x, y, attribute */
        }
        fix16(t, m);
        fix16(t, m + 2);
        fix32(t, m + 4);
    }
    fix16(t, b);
    fix16(t, b + 2);
    for (i = 4; i < 20; i += 4) {
        fix32(t, b + i);
    }
    return 0;
}

/* A word of a block that may sit at any byte: a label block of odd size
 * (2470 of the 3274 cell files) leaves the next block, UEXT, unaligned.
 * Nothing else points into those words, so the halfword bitset is not
 * needed for them. */
static void fix32_any(Turn *t, u32 at)
{
    if ((at & 1u) == 0) {
        fix32(t, at);
        return;
    }
    if (t->n < 4u || at > t->n - 4u) {
        t->bad = 1;
        return;
    }
    bw_mem_wr32(t->p + at, le32(t, at));
}

int bw_g2d_to_host(void *file)
{
#if BW_HOST_BIG_ENDIAN
    u8 *p = (u8 *)file;
    u32 n, pos, blocks, k;
    Turn t;
    int rc = 0;

    if (p == NULL || bw_mem_rd16(p + 4) != 0xFFFEu) {
        return 0;                           /* turned already, or not NITRO */
    }
    n = bw_rd32(p + 8);
    if (n < 16 || turn_begin(&t, p, n) != 0) {
        return -1;
    }
    pos = le16(&t, 12);
    blocks = le16(&t, 14);
    for (k = 0; k < blocks && rc == 0; k++) {
        u32 tag = le32(&t, pos), size = le32(&t, pos + 4), b = pos + 8;

        if (pos < 16 || size < 8 || !inside(pos, size, 0, n)) {
            rc = -1;
            break;
        }
        switch (tag) {
        case TAG_PLTT: rc = pltt(&t, b, size - 8); break;
        case TAG_PCMP: rc = pcmp(&t, b, size - 8); break;
        case TAG_CHAR: rc = chr(&t, b, size - 8); break;
        case TAG_CPOS:
            for (b = pos + 8; b < pos + 16 && b + 2 <= pos + size; b += 2) {
                fix16(&t, b);
            }
            break;
        case TAG_SCRN: rc = scrn(&t, b, size - 8); break;
        case TAG_CEBK: rc = cebk(&t, b, size - 8); break;
        case TAG_ABNK: rc = abnk(&t, b, size - 8); break;
        case TAG_MCBK: rc = mcbk(&t, b, size - 8); break;
        case TAG_UEXT:
            for (b = pos + 8; b + 4 <= pos + size; b += 4) {
                fix32_any(&t, b);
            }
            break;
        default:                            /* labels, or a block no reader opens */
            break;
        }
        fix32_any(&t, pos);                 /* the tag, compared as a number */
        fix32_any(&t, pos + 4);
        pos += size;
    }
    fix32(&t, 0);
    fix16(&t, 4);
    fix16(&t, 6);
    fix32(&t, 8);
    fix16(&t, 12);
    fix16(&t, 14);
    return turn_end(&t, rc);
#else
    (void)file;
    return 0;
#endif
}

/* The game's NNS_G2dFindBinaryBlock (src/game/arm9/nns_g2d_load.c), with
 * the file turned first: every reader of a NITRO 2D file comes here before
 * it reads a block. */
void *NNS_G2dFindBinaryBlock(void *file, u32 signature)
{
    u8 *p = (u8 *)file;
    u8 *block;
    u16 i, blocks;

    if (bw_g2d_to_host(file) < 0) {
        static int said;
        if (!said) {
            said = 1;
            fprintf(stderr, "bw-port: a malformed NITRO 2D file (tag %.4s) is left "
                            "partly in the DS's order\n", (const char *)p);
        }
    }
    block = p + bw_mem_rd16(p + 12);
    blocks = bw_mem_rd16(p + 14);
    for (i = 0; i < blocks; i++) {
        if (bw_mem_rd32(block) == signature) {
            return block;
        }
        block += bw_mem_rd32(block + 4);
    }
    return NULL;
}
