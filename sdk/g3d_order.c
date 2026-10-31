/*
 * sdk/g3d_order.c -- NITRO 3D files in the host's byte order, and back (the
 * model is in sdk/g3d_order.h).
 *
 * The layouts are the SDK's, as the game reads them: the file and block
 * lookups in src/game/arm9/nns_g3d_res.c and g3d_system.c, the model's
 * records in nns_g3d_anm_calc_types.h (node, material, shape) and the
 * samplers in nns_g3d_anm_calc.c and nns_g3d_anm_texpat.c, which say how
 * many keys a track holds: one past the highest index any frame reads
 * (keys()).  Every offset is relative to the base those readers add it to.
 *
 * A number is read in the order it is in now: a halfword already turned
 * (the bitset, sdk/byteturn.h) is read in the order it was turned to, so a
 * record is read whether or not something before it turned it.  A file is
 * turned in a copy and put back only when the whole walk closes, so a
 * malformed file, or bytes that only look like an embedded file, are left
 * as they were.
 */

#include <stdlib.h>
#include <string.h>

#include "byteturn.h"
#include "g3d_order.h"

/* Tags as the game compares them: the stored bytes read as a little-endian
 * u32 (the bytes "BMD0"). */
#define TAG_BMD0 0x30444D42u
#define TAG_BTX0 0x30585442u
#define TAG_BCA0 0x30414342u
#define TAG_BTP0 0x30505442u
#define TAG_BTA0 0x30415442u
#define TAG_BMA0 0x30414D42u
#define TAG_BVA0 0x30415642u
#define TAG_MDL0 0x304C444Du
#define TAG_TEX0 0x30584554u
#define TAG_JNT0 0x30544E4Au
#define TAG_MAT0 0x3054414Du
#define TAG_SRT0 0x30545253u
#define TAG_PAT0 0x30544150u
#define TAG_VIS0 0x30534956u

#define MAX_DICT 256u           /* a dictionary's count is a u8 */

/* ---- reading ------------------------------------------------------- */

static u32 rd16(const Turn *t, u32 at)
{
    if (t->n < 2u || at > t->n - 2u) {
        return 0;
    }
    if (is_done(t, at)) {
        return t->to_ds ? (u32)(t->p[at] | (t->p[at + 1] << 8)) : bw_mem_rd16(t->p + at);
    }
    return le16(t, at);
}

static u32 rd32(const Turn *t, u32 at)
{
    if (t->n < 4u || at > t->n - 4u) {
        return 0;
    }
    if (is_done(t, at)) {
        return t->to_ds ? (u32)t->p[at] | ((u32)t->p[at + 1] << 8) | ((u32)t->p[at + 2] << 16)
                              | ((u32)t->p[at + 3] << 24)
                        : bw_mem_rd32(t->p + at);
    }
    return le32(t, at);
}

static u32 rd8(const Turn *t, u32 at)
{
    return at < t->n ? t->p[at] : 0u;
}

static void fix16s(Turn *t, u32 at, u32 count)
{
    u32 i;

    for (i = 0; i < count && !t->bad; i++) {
        fix16(t, at + 2u * i);
    }
}

static void fix32s(Turn *t, u32 at, u32 count)
{
    u32 i;

    for (i = 0; i < count && !t->bad; i++) {
        fix32(t, at + 4u * i);
    }
}

/* ---- dictionaries -------------------------------------------------- */

enum { UNIT_U32, UNIT_TEX, UNIT_PLTT, UNIT_TOMAT, UNIT_RAW };

/* The dictionary at `at`: its header turned, its entries turned by `kind`
 * (UNIT_RAW: by the caller), their offsets in `ents`; the count, or -1. */
static int dict(Turn *t, u32 at, int kind, u32 *ents)
{
    u32 n = rd8(t, at + 1), size = rd16(t, at + 2), ofs = rd16(t, at + 6);
    u32 eh = at + ofs, unit = rd16(t, eh), ofsname = rd16(t, eh + 2), i;

    if (!inside(at, 8, 0, t->n) || ofs < 8u + 4u * (n + 1u) || ofs > size
        || !inside(at, size, 0, t->n) || 4u + n * unit > ofsname
        || ofs + ofsname + 16u * n > size) {
        return -1;
    }
    switch (kind) {
    case UNIT_U32:
    case UNIT_PLTT:
    case UNIT_TOMAT:
        if (unit != 4u) {
            return -1;
        }
        break;
    case UNIT_TEX:
        if (unit != 8u) {
            return -1;
        }
        break;
    default:
        break;
    }
    for (i = 0; i < n; i++) {
        u32 e = eh + 4u + i * unit;

        ents[i] = e;
        switch (kind) {
        case UNIT_U32:   fix32(t, e); break;
        case UNIT_TEX:   fix32s(t, e, 2); break;    /* TEXIMAGE_PARAM, extra */
        case UNIT_PLTT:  fix16s(t, e, 2); break;    /* offset, flag */
        case UNIT_TOMAT: fix16(t, e); break;        /* offset; count, flag bytes */
        default: break;
        }
    }
    fix16s(t, at + 2, 3);                           /* size, dummy, ofsEntry */
    fix16s(t, eh, 2);                               /* sizeUnit, ofsName */
    return t->bad ? -1 : (int)n;
}

/* ---- models -------------------------------------------------------- */

/* One node record (NNSG3dResNodeData): its end, or 0. */
static u32 node(Turn *t, u32 at, u32 end)
{
    u32 flag = rd16(t, at), p = at + 4;

    fix16s(t, at, 2);                               /* flag, rotation m00 */
    if (!(flag & 1u)) {                             /* translation */
        fix32s(t, p, 3);
        p += 12;
    }
    if (!(flag & 2u)) {                             /* rotation */
        if (flag & 8u) {                            /* pivot-compressed: two fx16 */
            fix16s(t, p, 2);
            p += 4;
        } else {                                    /* the rest of the matrix */
            fix16s(t, p, 8);
            p += 16;
        }
    }
    if (!(flag & 4u)) {                             /* scale and its inverse */
        fix32s(t, p, 6);
        p += 24;
    }
    return p <= end ? p : 0;
}

static void material(Turn *t, u32 at)
{
    u32 flag = rd16(t, at + 30), p = at + 44;

    fix16s(t, at, 2);                               /* tag, size */
    fix32s(t, at + 4, 6);                           /* colours, attributes, image */
    fix16s(t, at + 28, 4);                          /* palette base, flag, width, height */
    fix32s(t, at + 36, 2);                          /* magW, magH */
    if (flag & 0x0001u) {                           /* the texture matrix */
        if (!(flag & 0x0002u)) {
            fix32s(t, p, 2);
            p += 8;
        }
        if (!(flag & 0x0004u)) {
            fix16s(t, p, 2);
            p += 4;
        }
        if (!(flag & 0x0008u)) {
            fix32s(t, p, 2);
            p += 8;
        }
    }
    if (flag & 0x2000u) {                           /* the effect matrix */
        fix32s(t, p, 16);
    }
}

static int model(Turn *t, u32 m)
{
    u32 ents[MAX_DICT];
    u32 size = rd32(t, m), sbc = rd32(t, m + 4), mat = rd32(t, m + 8), shp = rd32(t, m + 12);
    u32 evp = rd32(t, m + 16), info = m + 0x14, nd = m + 0x40, ma, t2m, p2m, sa, i;
    int n;

    if (!inside(m, size, 0, t->n) || size < 0x48u || sbc > size || mat > size || shp > size
        || evp > size) {
        return -1;
    }
    fix32s(t, m, 5);
    fix32s(t, info + 8, 2);                         /* posScale, invPosScale */
    fix16s(t, info + 16, 4);                        /* vertex, polygon, triangle, quad counts */
    fix16s(t, info + 24, 6);                        /* the bounding box */
    fix32s(t, info + 36, 2);
    if ((n = dict(t, nd, UNIT_U32, ents)) < 0) {
        return -1;
    }
    for (i = 0; i < (u32)n; i++) {
        if (node(t, nd + rd32(t, ents[i]), m + sbc) == 0) {
            return -1;
        }
    }
    /* Materials: offsets from the material block. */
    ma = m + mat;
    t2m = rd16(t, ma);
    p2m = rd16(t, ma + 2);
    fix16s(t, ma, 2);
    if ((n = dict(t, ma + 4, UNIT_U32, ents)) < 0) {
        return -1;
    }
    for (i = 0; i < (u32)n; i++) {
        material(t, ma + rd32(t, ents[i]));
    }
    if ((t2m != 0 && dict(t, ma + t2m, UNIT_TOMAT, ents) < 0)
        || (p2m != 0 && dict(t, ma + p2m, UNIT_TOMAT, ents) < 0)) {
        return -1;
    }
    /* Shapes: offsets from the shape block; each display list from its shape. */
    sa = m + shp;
    if ((n = dict(t, sa, UNIT_U32, ents)) < 0) {
        return -1;
    }
    for (i = 0; i < (u32)n; i++) {
        u32 s = sa + rd32(t, ents[i]), dl = rd32(t, s + 8), dsz = rd32(t, s + 12);

        if ((dsz & 3u) != 0 || dl > size || !inside(s + dl, dsz, m, size)) {
            return -1;
        }
        fix16s(t, s, 2);
        fix32s(t, s + 4, 3);
        fix32s(t, s + dl, dsz / 4u);                /* the geometry commands, a word each */
    }
    /* The envelope matrices run to the model's end. */
    if (evp != 0 && evp < size) {
        fix32s(t, m + evp, (size - evp) / 4u);
    }
    return t->bad ? -1 : 0;
}

static int mdl0(Turn *t, u32 b)
{
    u32 ents[MAX_DICT], i;
    int n = dict(t, b + 8, UNIT_U32, ents);

    for (i = 0; n > 0 && i < (u32)n; i++) {
        if (model(t, b + rd32(t, ents[i])) != 0) {
            return -1;
        }
    }
    return n < 0 ? -1 : 0;
}

/* The texture block: its three headers and two dictionaries.  The texels,
 * the 4x4 palette indices and the palettes stay the DS's bytes. */
static int tex0(Turn *t, u32 b)
{
    u32 ents[MAX_DICT], pd = rd16(t, b + 52);

    fix32(t, b + 8);                                /* texInfo: vramKey */
    fix16s(t, b + 12, 4);                           /* sizeTex, ofsDict, flag, dummy */
    fix32(t, b + 20);                               /* ofsTex */
    fix32(t, b + 24);                               /* tex4x4Info */
    fix16s(t, b + 28, 4);
    fix32s(t, b + 36, 2);
    fix32(t, b + 44);                               /* plttInfo */
    fix16s(t, b + 48, 4);
    fix32(t, b + 56);
    if (dict(t, b + 0x3C, UNIT_TEX, ents) < 0 || dict(t, b + pd, UNIT_PLTT, ents) < 0) {
        return -1;
    }
    return t->bad ? -1 : 0;
}

/* ---- animations ---------------------------------------------------- */

/* Keys a track holds: one past the highest index the samplers read for a
 * frame in [0, frames), for a track whose step bits are those of `info`
 * and whose last interpolated frame is `last`. */
static u32 keys(u32 info, u32 last, u32 frames)
{
    u32 f, hi = 0;

    for (f = 0; f < frames; f++) {
        u32 idx;

        if ((info & 0xC0000000u) == 0) {
            idx = f;
        } else if (info & 0x40000000u) {            /* every second frame */
            idx = !(f & 1u) ? f >> 1 : f > last ? (last >> 1) + 1u : (f >> 1) + 1u;
        } else {                                    /* every fourth */
            u32 r = f & 3u;

            idx = r == 0 ? f >> 2 : f > last ? (last >> 2) + r : (f >> 2) + 1u;
        }
        if (idx > hi) {
            hi = idx;
        }
    }
    return hi + 1u;
}

typedef struct Jnt {
    u32 a, frames;
    s32 rot3, rot9;             /* the highest index each table is read at */
} Jnt;

static void rot_key(Jnt *j, u32 v)
{
    s32 idx = (s32)(v & 0x7FFFu);

    if (v & 0x8000u) {
        j->rot3 = idx > j->rot3 ? idx : j->rot3;
    } else {
        j->rot9 = idx > j->rot9 ? idx : j->rot9;
    }
}

/* One key track (NNSG3dJntAnmTInfo) at `p`: 't' translation, 'r' rotation,
 * 's' scale (a scale and its inverse per key). */
static void jnt_track(Turn *t, Jnt *j, u32 p, char kind)
{
    u32 info = rd32(t, p), ofs = rd32(t, p + 4);
    u32 n = keys(info, (info & 0x1FFF0000u) >> 16, j->frames), k;

    fix32s(t, p, 2);
    if (kind == 'r') {
        for (k = 0; k < n && !t->bad; k++) {
            rot_key(j, rd16(t, j->a + ofs + 2u * k));
            fix16(t, j->a + ofs + 2u * k);
        }
    } else if (info & 0x20000000u) {
        fix16s(t, j->a + ofs, kind == 's' ? 2u * n : n);
    } else {
        fix32s(t, j->a + ofs, kind == 's' ? 2u * n : n);
    }
}

static int jnt(Turn *t, u32 a)
{
    Jnt j;
    u32 nodes = rd16(t, a + 6), r3 = rd32(t, a + 12), r9 = rd32(t, a + 16), i;

    j.a = a;
    j.frames = rd16(t, a + 4);
    j.rot3 = j.rot9 = -1;
    fix16(t, a + 6);
    fix32s(t, a + 8, 3);                            /* flag, ofsRot3, ofsRot9 */
    for (i = 0; i < nodes && !t->bad; i++) {
        u32 r = a + rd16(t, a + 20 + 2u * i), flag = rd32(t, r), p = r + 4, k;

        fix16(t, a + 20 + 2u * i);
        fix32(t, r);
        if (flag & 1u) {
            continue;
        }
        if (!(flag & 6u)) {
            for (k = 0; k < 3; k++) {
                if (flag & (8u << k)) {             /* a constant */
                    fix32(t, p);
                    p += 4;
                } else {
                    jnt_track(t, &j, p, 't');
                    p += 8;
                }
            }
        }
        if (!(flag & 0xC0u)) {
            if (flag & 0x100u) {
                rot_key(&j, rd32(t, p));
                fix32(t, p);
                p += 4;
            } else {
                jnt_track(t, &j, p, 'r');
                p += 8;
            }
        }
        if (!(flag & 0x600u)) {
            for (k = 0; k < 3; k++) {
                if (flag & (0x800u << k)) {
                    fix32s(t, p, 2);
                } else {
                    jnt_track(t, &j, p, 's');
                }
                p += 8;
            }
        }
    }
    if (j.rot3 >= 0) {
        fix16s(t, a + r3, 3u * (u32)(j.rot3 + 1));  /* compressed rows */
    }
    if (j.rot9 >= 0) {
        fix16s(t, a + r9, 5u * (u32)(j.rot9 + 1));  /* full matrices */
    }
    return t->bad ? -1 : 0;
}

/* Material colours: five track words per material; the colour keys are
 * u16 (RGB555), the alpha keys bytes. */
static int mat(Turn *t, u32 a)
{
    u32 ents[MAX_DICT], frames = rd16(t, a + 4), i, k;
    int n;

    fix16(t, a + 6);
    if ((n = dict(t, a + 8, UNIT_RAW, ents)) < 0) {
        return -1;
    }
    for (i = 0; i < (u32)n; i++) {
        for (k = 0; k < 5; k++) {
            u32 info = rd32(t, ents[i] + 4u * k);

            fix32(t, ents[i] + 4u * k);
            if (!(info & 0x20000000u) && k != 4) {
                fix16s(t, a + (info & 0xFFFFu), keys(info, (info & 0x1FFF0000u) >> 16, frames));
            }
        }
    }
    return t->bad ? -1 : 0;
}

/* Texture SRT: five (info, offset) tracks per material, scale S and T,
 * rotation, translation S and T.  A rotation key is a (sin, cos) pair of
 * halfwords. */
static int srt(Turn *t, u32 a)
{
    u32 ents[MAX_DICT], frames = rd16(t, a + 4), i, k;
    int n;

    fix16(t, a + 6);
    if ((n = dict(t, a + 8, UNIT_RAW, ents)) < 0) {
        return -1;
    }
    for (i = 0; i < (u32)n; i++) {
        for (k = 0; k < 5; k++) {
            u32 p = ents[i] + 8u * k, info = rd32(t, p), ofs = rd32(t, p + 4), m;

            fix32s(t, p, 2);
            if (info & 0x20000000u) {
                continue;                           /* the value is the offset word */
            }
            m = keys(info, info & 0xFFFFu, frames);
            if (k == 2) {
                fix16s(t, a + ofs, 2u * m);
            } else if (info & 0x10000000u) {
                fix16s(t, a + ofs, m);
            } else {
                fix32s(t, a + ofs, m);
            }
        }
    }
    return t->bad ? -1 : 0;
}

/* Texture patterns: per material a track (u16 count, u16, s16 ratio, u16
 * offset) of (u16 frame, u8 texture, u8 palette) values. */
static int pat(Turn *t, u32 a)
{
    u32 ents[MAX_DICT], i, k;
    int n;

    fix16s(t, a + 8, 2);                            /* ofsTexName, ofsPlttName */
    if ((n = dict(t, a + 12, UNIT_RAW, ents)) < 0) {
        return -1;
    }
    for (i = 0; i < (u32)n; i++) {
        u32 count = rd16(t, ents[i]), ofs = rd16(t, ents[i] + 6);

        fix16s(t, ents[i], 4);
        for (k = 0; k < count && !t->bad; k++) {
            fix16(t, a + ofs + 4u * k);
        }
    }
    return t->bad ? -1 : 0;
}

/* Visibility: a bit per node per frame, in words from +12. */
static int vis(Turn *t, u32 a)
{
    u32 frames = rd16(t, a + 4), nodes = rd16(t, a + 6);

    fix16(t, a + 6);
    fix32s(t, a + 12, (frames * nodes + 31u) / 32u);
    return t->bad ? -1 : 0;
}

static int anm(Turn *t, u32 b, int (*fn)(Turn *, u32))
{
    u32 ents[MAX_DICT], i;
    int n = dict(t, b + 8, UNIT_U32, ents);

    for (i = 0; n > 0 && i < (u32)n; i++) {
        u32 a = b + rd32(t, ents[i]);

        if (!inside(a, 8, 0, t->n)) {
            return -1;
        }
        fix16s(t, a + 2, 2);                        /* category1, numFrame */
        if (fn(t, a) != 0) {
            return -1;
        }
    }
    return n < 0 ? -1 : 0;
}

/* ---- files --------------------------------------------------------- */

static int is_g3d_tag(u32 tag)
{
    return tag == TAG_BMD0 || tag == TAG_BTX0 || tag == TAG_BCA0 || tag == TAG_BTP0
           || tag == TAG_BTA0 || tag == TAG_BMA0 || tag == TAG_BVA0;
}

/* The file's numbers as they are now: little-endian, or the host's order
 * when turning back. */
static u32 now16(const u8 *p, int to_ds)
{
    return to_ds ? bw_mem_rd16(p) : (u32)(p[0] | (p[1] << 8));
}

static u32 now32(const u8 *p, int to_ds)
{
    return to_ds ? bw_mem_rd32(p)
                 : (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

/* The size of the 3D file at `p` (`n` bytes left), 0 if there is none. */
static u32 file_size(const u8 *p, u32 n, int to_ds)
{
    u32 size;

    if (n < 16 || !is_g3d_tag(now32(p, to_ds)) || now16(p + 4, to_ds) != 0xFEFFu) {
        return 0;
    }
    size = now32(p + 8, to_ds);
    return size >= 16 && size <= n ? size : 0;
}

static int walk(Turn *t)
{
    u32 hs = rd16(t, 12), blocks = rd16(t, 14), k;

    if (hs < 16 || blocks == 0 || blocks > 16 || !inside(hs, 4u * blocks, 0, t->n)) {
        return -1;
    }
    for (k = 0; k < blocks; k++) {
        u32 b = rd32(t, hs + 4u * k), tag = rd32(t, b), size = rd32(t, b + 4);
        int rc;

        if (b < hs + 4u * blocks || size < 8 || !inside(b, size, 0, t->n)) {
            return -1;
        }
        switch (tag) {
        case TAG_MDL0: rc = mdl0(t, b); break;
        case TAG_TEX0: rc = tex0(t, b); break;
        case TAG_JNT0: rc = anm(t, b, jnt); break;
        case TAG_MAT0: rc = anm(t, b, mat); break;
        case TAG_SRT0: rc = anm(t, b, srt); break;
        case TAG_PAT0: rc = anm(t, b, pat); break;
        case TAG_VIS0: rc = anm(t, b, vis); break;
        default: rc = -1; break;
        }
        if (rc != 0) {
            return -1;
        }
        fix32s(t, b, 2);                            /* the tag, compared as a number */
        fix32(t, hs + 4u * k);
    }
    fix32(t, 0);
    fix16s(t, 4, 2);
    fix32(t, 8);
    fix16s(t, 12, 2);
    return t->bad ? -1 : 0;
}

/* One file, `size` bytes, turned in a copy that replaces it when the walk
 * closes. */
static int turn_file(u8 *p, u32 size, int to_ds)
{
    Turn t;
    u8 *copy = (u8 *)malloc(size);
    int rc;

    if (copy == NULL) {
        return -1;
    }
    memcpy(copy, p, size);
    if (turn_begin(&t, copy, size) != 0) {
        free(copy);
        return -1;
    }
    t.to_ds = to_ds;
    rc = turn_end(&t, walk(&t));
    if (rc == 1) {
        memcpy(p, copy, size);
    }
    free(copy);
    return rc;
}

static int one(u8 *p, u32 n, int to_ds)
{
    u32 size = p != NULL ? file_size(p, n, to_ds) : 0;

    return size != 0 ? turn_file(p, size, to_ds) : 0;
}

int bw_g3d_to_host(u8 *p, u32 n)
{
    return one(p, n, 0);
}

int bw_g3d_to_ds(u8 *p, u32 n)
{
    return one(p, n, 1);
}

static int member(u8 *p, u32 n, int to_ds)
{
    u32 o = 0;
    int turned = 0;

    if (!BW_HOST_BIG_ENDIAN || p == NULL) {
        return 0;
    }
    while (n >= 16 && o <= n - 16u) {
        u32 size = file_size(p + o, n - o, to_ds);

        if (size != 0 && turn_file(p + o, size, to_ds) == 1) {
            turned++;
            o += (size + 3u) & ~3u;
            continue;
        }
        if (size != 0 && o == 0) {
            return -1;                              /* the member is one, and malformed */
        }
        o += 4;
    }
    return turned;
}

int bw_g3d_member_to_host(u8 *p, u32 n)
{
    return member(p, n, 0);
}

int bw_g3d_member_to_ds(u8 *p, u32 n)
{
    return member(p, n, 1);
}
