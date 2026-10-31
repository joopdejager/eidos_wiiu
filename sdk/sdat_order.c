/*
 * sdk/sdat_order.c -- the sound archive in the host's byte order (the model
 * and what is turned are in sdk/sdat_order.h).
 *
 * The layouts are the SDK's (src/game/arm9/nns_snd_arc_types.h,
 * snd_bank.h) and aletheia's tools/assets/sdat.py; this ROM's archive has
 * SSEQ, SBNK and SWAR files only, and no sequence archive, stream or
 * PLAYER2 records, but those are turned too where their layout is plain.
 *
 * Every number is read from the cartridge's little-endian bytes before it
 * is turned (sdk/byteturn.h): a record, file, instrument or wave two
 * offsets share is turned by the first and skipped by the second.
 */

#include <stdlib.h>
#include <string.h>

#include "byteturn.h"
#include "sdat_order.h"

/* ---- files ---------------------------------------------------------- */

static int sbnk_to_host(Turn *t, u32 f, u32 size)
{
    u32 count = le32(t, f + 0x38), i;

    if (size < 0x3C || (u64)count * 4u > size - 0x3C) {
        return -1;
    }
    for (i = 0; i < count; i++) {
        u32 e = le32(t, f + 0x3C + 4 * i), type = e & 0xFFu, off = e >> 8, j;

        if (type == 0) {
            continue;
        }
        if (type >= 1 && type <= 5) {
            if (!inside(f + off, 10, f, size)) {
                return -1;
            }
            fix16(t, f + off);                  /* wave */
            fix16(t, f + off + 2);              /* wave archive */
        } else if (type == 16) {
            u32 lo, hi;

            if (!inside(f + off, 2, f, size)) {
                return -1;
            }
            lo = t->p[f + off];
            hi = t->p[f + off + 1];
            if (hi < lo || !inside(f + off + 2, 12 * (hi - lo + 1), f, size)) {
                return -1;
            }
            for (j = 0; j <= hi - lo; j++) {
                fix16(t, f + off + 2 + 12 * j + 2);
                fix16(t, f + off + 2 + 12 * j + 4);
            }
        } else if (type == 17) {
            u32 used = 0;

            if (!inside(f + off, 8, f, size)) {
                return -1;
            }
            while (used < 8 && t->p[f + off + used] != 0) {
                used++;
            }
            if (!inside(f + off + 8, 12 * used, f, size)) {
                return -1;
            }
            for (j = 0; j < used; j++) {
                fix16(t, f + off + 8 + 12 * j + 2);
                fix16(t, f + off + 8 + 12 * j + 4);
            }
        } else {
            return -1;
        }
    }
    for (i = 0; i < count; i++) {
        fix32(t, f + 0x3C + 4 * i);
    }
    fix32(t, f + 0x38);
    return 0;
}

static int swar_to_host(Turn *t, u32 f, u32 size)
{
    u32 count = le32(t, f + 0x38), i;

    if (size < 0x3C || (u64)count * 4u > size - 0x3C) {
        return -1;
    }
    for (i = 0; i < count; i++) {
        u32 w = le32(t, f + 0x3C + 4 * i);

        if (w == 0) {
            continue;
        }
        if (!inside(f + w, 12, f, size)) {
            return -1;
        }
        fix16(t, f + w + 2);                    /* rate */
        fix16(t, f + w + 4);                    /* timer */
        fix16(t, f + w + 6);                    /* loop start, in words */
        fix32(t, f + w + 8);                    /* loop length, in words */
    }
    for (i = 0; i < count; i++) {
        fix32(t, f + 0x3C + 4 * i);
    }
    fix32(t, f + 0x38);
    return 0;
}

static int file_to_host(Turn *t, u32 f, u32 size)
{
    u32 hsize, i;
    int rc;

    if (size < 0x10 || !inside(f, size, 0, t->n)) {
        return -1;
    }
    if (is_done(t, f + 8)) {
        return 0;                               /* a file two entries share */
    }
    hsize = le16(t, f + 12);
    if (le16(t, f + 4) != 0xFEFFu || le32(t, f + 8) != size || hsize < 0x10
        || !inside(f + hsize, 8, f, size) || memcmp(t->p + f + hsize, "DATA", 4) != 0) {
        return -1;
    }
    if (memcmp(t->p + f, "SSEQ", 4) == 0) {
        if (size < 0x1C || le32(t, f + 0x18) > size) {
            return -1;
        }
        fix32(t, f + 0x18);
        rc = 0;
    } else if (memcmp(t->p + f, "SSAR", 4) == 0) {
        u32 count = le32(t, f + 0x1C);

        if (size < 0x20 || le32(t, f + 0x18) > size || (u64)count * 12u > size - 0x20) {
            return -1;
        }
        for (i = 0; i < count; i++) {
            fix32(t, f + 0x20 + 12 * i);        /* the sequence's offset */
            fix16(t, f + 0x20 + 12 * i + 4);    /* its bank */
        }
        fix32(t, f + 0x18);
        fix32(t, f + 0x1C);
        rc = 0;
    } else if (memcmp(t->p + f, "SBNK", 4) == 0) {
        rc = sbnk_to_host(t, f, size);
    } else if (memcmp(t->p + f, "SWAR", 4) == 0) {
        rc = swar_to_host(t, f, size);
    } else {
        rc = -1;
    }
    if (rc != 0) {
        return -1;
    }
    fix32(t, f + hsize + 4);                    /* the DATA block's size */
    fix16(t, f + 4);
    fix16(t, f + 6);
    fix32(t, f + 8);
    fix16(t, f + 12);
    fix16(t, f + 14);
    return 0;
}

/* ---- the archive's blocks ------------------------------------------ */

/* A table of `count` u32 offsets at `tab` (relative to `base`). */
static int table(Turn *t, u32 base, u32 size, u32 off, u32 *count)
{
    if (!inside(base + off, 4, base, size)) {
        return -1;
    }
    *count = le32(t, base + off);
    if ((u64)*count * 4u > size - off - 4u) {
        return -1;
    }
    return 0;
}

static int symb_to_host(Turn *t, u32 s, u32 size)
{
    u32 k, i;

    for (k = 0; k < 8; k++) {
        u32 off = le32(t, s + 8 + 4 * k), count;

        if (off == 0) {
            continue;
        }
        if (table(t, s, size, off, &count) != 0) {
            return -1;
        }
        if (k == 1) {
            /* Sequence archives: {name, sub-table} pairs, a sub-table being
             * the archive's sequence names. */
            if ((u64)count * 8u > size - off - 4u) {
                return -1;
            }
            for (i = 0; i < count; i++) {
                u32 sub = le32(t, s + off + 4 + 8 * i + 4), sc, j;

                if (sub != 0) {
                    if (table(t, s, size, sub, &sc) != 0) {
                        return -1;
                    }
                    for (j = 0; j < sc; j++) {
                        fix32(t, s + sub + 4 + 4 * j);
                    }
                    fix32(t, s + sub);
                }
                fix32(t, s + off + 4 + 8 * i);
                fix32(t, s + off + 4 + 8 * i + 4);
            }
        } else {
            for (i = 0; i < count; i++) {
                fix32(t, s + off + 4 + 4 * i);
            }
        }
        fix32(t, s + off);
    }
    for (k = 0; k < 8; k++) {
        fix32(t, s + 8 + 4 * k);
    }
    fix32(t, s + 4);
    return 0;
}

/* The record sizes of INFO's eight kinds (GROUP's is its header). */
static const u32 k_record_size[8] = { 12, 4, 12, 4, 8, 4, 0x18, 12 };

static int record_to_host(Turn *t, u32 r, u32 kind, u32 info, u32 size)
{
    u32 i;

    if (!inside(r, k_record_size[kind], info, size)) {
        return -1;
    }
    switch (kind) {
    case 0:                                     /* SEQ: file, bank, 6 bytes */
        fix32(t, r);
        fix16(t, r + 4);
        break;
    case 2:                                     /* BANK: file, 4 wave archives */
        fix32(t, r);
        for (i = 0; i < 4; i++) {
            fix16(t, r + 4 + 2 * i);
        }
        break;
    case 1:                                     /* SEQARC: file */
    case 3:                                     /* WAVARC: file | flags << 24 */
    case 7:                                     /* STRM: file, then bytes */
        fix32(t, r);
        break;
    case 4:                                     /* PLAYER: u8, u8, channels, heap */
        fix16(t, r + 2);
        fix32(t, r + 4);
        break;
    case 5: {                                   /* GROUP: count, 8-byte items */
        u32 count;

        if (is_done(t, r)) {
            break;                              /* a record two entries share */
        }
        count = le32(t, r);
        if ((u64)count * 8u > size - (r - info) - 4u) {
            return -1;
        }
        for (i = 0; i < count; i++) {
            fix16(t, r + 4 + 8 * i + 2);
            fix32(t, r + 4 + 8 * i + 4);
        }
        fix32(t, r);
        break;
    }
    default:                                    /* PLAYER2: bytes */
        break;
    }
    return 0;
}

static int info_to_host(Turn *t, u32 info, u32 size)
{
    u32 k, i;

    for (k = 0; k < 8; k++) {
        u32 off = le32(t, info + 8 + 4 * k), count;

        if (off == 0) {
            continue;
        }
        if (table(t, info, size, off, &count) != 0) {
            return -1;
        }
        for (i = 0; i < count; i++) {
            u32 r = le32(t, info + off + 4 + 4 * i);

            if (r != 0 && record_to_host(t, info + r, k, info, size) != 0) {
                return -1;
            }
        }
        for (i = 0; i < count; i++) {
            fix32(t, info + off + 4 + 4 * i);
        }
        fix32(t, info + off);
    }
    for (k = 0; k < 8; k++) {
        fix32(t, info + 8 + 4 * k);
    }
    fix32(t, info + 4);
    return 0;
}

static int fat_to_host(Turn *t, u32 fat, u32 size)
{
    u32 count = le32(t, fat + 8), i, k;

    if (size < 12 || (u64)count * 16u > size - 12u) {
        return -1;
    }
    for (i = 0; i < count; i++) {
        u32 e = fat + 12 + 16 * i;

        if (le32(t, e + 4) != 0 && file_to_host(t, le32(t, e), le32(t, e + 4)) != 0) {
            return -1;
        }
    }
    for (i = 0; i < count; i++) {
        for (k = 0; k < 4; k++) {
            fix32(t, fat + 12 + 16 * i + 4 * k);
        }
    }
    fix32(t, fat + 8);
    fix32(t, fat + 4);
    return 0;
}

int bw_sdat_to_host(u8 *p, u32 n)
{
    static const char *const tags[3] = { "SYMB", "INFO", "FAT " };
    u32 off[4], size[4], k;
    Turn t;
    int rc = 0;

    if (n < 0x40 || memcmp(p, "SDAT", 4) != 0) {
        return 0;
    }
    if (turn_begin(&t, p, n) != 0) {
        return -1;
    }
    for (k = 0; k < 4; k++) {
        off[k] = le32(&t, 0x10 + 8 * k);
        size[k] = le32(&t, 0x14 + 8 * k);
        if (k == 0 && off[k] == 0) {
            continue;                           /* no symbols */
        }
        if (off[k] < 0x40 || size[k] < 0x10 || !inside(off[k], size[k], 0, n)
            || memcmp(p + off[k], k < 3 ? tags[k] : "FILE", 4) != 0) {
            return turn_end(&t, -1);
        }
    }
    if (le16(&t, 4) != 0xFEFFu || le32(&t, 8) != n) {
        return turn_end(&t, -1);
    }
    if (off[0] != 0) {
        rc = symb_to_host(&t, off[0], size[0]);
    }
    if (rc == 0) {
        rc = info_to_host(&t, off[1], size[1]);
    }
    if (rc == 0) {
        rc = fat_to_host(&t, off[2], size[2]);  /* and the files it lists */
    }
    if (rc == 0) {
        fix32(&t, off[3] + 4);                  /* FILE: size, count */
        fix32(&t, off[3] + 8);
        fix16(&t, 4);
        fix16(&t, 6);
        fix32(&t, 8);
        fix16(&t, 12);
        fix16(&t, 14);
        for (k = 0; k < 8; k++) {
            fix32(&t, 0x10 + 4 * k);
        }
    }
    return turn_end(&t, rc);
}

int bw_sdat_file_to_host(u8 *p, u32 n)
{
    Turn t;

    if (turn_begin(&t, p, n) != 0) {
        return -1;
    }
    return turn_end(&t, file_to_host(&t, 0, n));
}
