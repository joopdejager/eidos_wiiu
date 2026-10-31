/*
 * install/byteorder.c -- the extracted tree in the host's byte order.
 *
 * The cartridge's files are little-endian, and the matched code reads the
 * containers it opens with native loads: ARC_TOOL_ReadHandleHeader reads a
 * NARC's header size, file count and FAT words straight into its own
 * variables.  On a big-endian host (the Wii U's PowerPC) those reads are
 * wrong unless the file itself is in the host's order, so the extractor
 * turns the framing of every NARC under data/ into the host's order once,
 * when the tree is made (bw_tree_to_host_order), and leaves a marker
 * (BW_BYTE_ORDER_FILE) the runtime checks.  The port's own NARC readers read
 * the same fields in that order (sdk/types.h, bw_dr_rd16..bw_dr_wr32).
 *
 * What is turned: the NARC header's numbers, the BTAF chunk (count and
 * every start/end word), the BTNF chunk (the directory table and the
 * directory ids in its name lists) and every chunk's size.  The four-byte
 * tags stay bytes.  The members' own contents (GMIF) are other formats:
 * those of the archives in k_formats are turned by their format's function
 * first, while the framing still says where they are; the rest are left as
 * they are.  A little-endian host does nothing.
 *
 * The formats:
 *
 *   wb_sound_data.sdat, and any SDAT: the sound archive's structure
 *   (sdk/sdat_order.h, bw_sdat_to_host).
 *
 *   a/0/2/3: the fonts (install/font_order.c, bw_nftr_to_host).
 *
 *   Every archive: the NITRO 3D files (models, textures, animations) its
 *   members are or hold (sdk/g3d_order.h, bw_g3d_member_to_host), and the
 *   camera animations (ICA, src/game/arm9/ica_anime.c): a u32 frame count,
 *   three u8 channel flags (scale, rotation, translation) and a pad byte,
 *   then for every frame three float32 per channel present, which
 *   ICA_ANIME_LoadHeader and ICA_ANIME_LoadFrames read straight from the
 *   archive with native loads (bw_ica_to_host).  They have no tag: a member
 *   is one when its size is that shape's and every value a plausible float.
 *
 *   a/0/0/2, a/0/0/3, and member 1 of a/1/1/1..a/1/1/4 (the musicals'
 *   stages): message banks (src/game/arm9/msgdata.c), read by
 *   MSGDATA_LoadFileHeader, MSGDATA_LoadBank and MSGDATA_ReadUnits with
 *   native loads, the text included: MSGDATA_DecryptUnits XORs each u16
 *   unit as a number, so the units are turned before the cipher sees them
 *   (bw_msg_to_host).
 */

#if !defined(_WIN32)
#  ifndef _DEFAULT_SOURCE
#    define _DEFAULT_SOURCE 1
#  endif
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "install.h"
#include "g3d_order.h"
#include "sdat_order.h"

#if !defined(_WIN32)
#include <dirent.h>
#include <sys/stat.h>
#endif

static u16 le16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 le32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static void fix16(u8 *p) { bw_mem_wr16(p, le16(p)); }
static void fix32(u8 *p) { bw_mem_wr32(p, le32(p)); }

/* The BTNF body at `fnt`, `len` bytes: the directory table (u32 offset,
 * u16 first file id, u16 parent or, for the root, the directory count) and
 * the directory ids after each subdirectory's name.  -1 if it runs out. */
static int fnt_to_host(u8 *fnt, u32 len)
{
    u32 dirs, d;

    if (len < 8) {
        return len == 0 ? 0 : -1;
    }
    dirs = le16(fnt + 6);
    if (dirs == 0 || (u64)dirs * 8u > len) {
        return -1;
    }
    for (d = 0; d < dirs; d++) {
        u8 *e = fnt + 8u * d;
        u32 at = le32(e);

        while (at < len && fnt[at] != 0) {
            u32 n = fnt[at] & 0x7Fu;
            int is_dir = (fnt[at] & 0x80u) != 0;

            at += 1u + n;
            if (is_dir) {
                if (at + 2u > len) {
                    return -1;
                }
                fix16(fnt + at);
                at += 2u;
            }
        }
        fix32(e);
        fix16(e + 4);
        fix16(e + 6);
    }
    return 0;
}

int bw_msg_to_host(u8 *p, u32 n)
{
    u32 langs, count, l;

    if (n == 0) {
        return 0;
    }
    if (n < 12) {
        return -1;
    }
    langs = le16(p);
    count = le16(p + 2);
    if (langs == 0 || 12u + 4u * langs > n) {
        return -1;
    }
    for (l = 0; l < langs; l++) {
        u32 at = le32(p + 12 + 4u * l), size, i;
        u8 *sec, *done;

        if (at > n - 4u) {
            return -1;
        }
        sec = p + at;
        size = le32(sec);
        if (size > n - at || 4u + 8ull * count > size) {
            return -1;
        }
        /* One flag per unit: two entries sharing text turn it once. */
        done = (u8 *)calloc(size / 2u + 1u, 1);
        if (done == NULL) {
            return -1;
        }
        for (i = 0; i < count; i++) {
            u8 *e = sec + 4 + 8u * i;
            u32 off = le32(e), units = le16(e + 4), k;

            if ((off & 1u) != 0 || (u64)off + 2ull * units > size) {
                free(done);
                return -1;
            }
            for (k = 0; k < units; k++) {
                u32 at2 = off / 2u + k;

                if (!done[at2]) {
                    fix16(sec + 2u * at2);
                    done[at2] = 1;
                }
            }
            fix32(e);
            fix16(e + 4);
            fix16(e + 6);
        }
        free(done);
        fix32(sec);
    }
    for (l = 0; l < langs; l++) {
        fix32(p + 12 + 4u * l);
    }
    fix16(p);
    fix16(p + 2);
    fix32(p + 4);
    fix32(p + 8);
    return 1;
}

/* A camera animation (ICA) in the host's order: 1 turned, 0 not one. */
int bw_ica_to_host(u8 *p, u32 n)
{
    u32 frames, ch, at;

    if (n < 20 || p[4] > 1 || p[5] > 1 || p[6] > 1 || p[7] != 0) {
        return 0;
    }
    frames = le32(p);
    ch = (u32)p[4] + p[5] + p[6];
    if (frames == 0 || frames > 100000 || ch == 0 || n != 8u + frames * 12u * ch) {
        return 0;
    }
    for (at = 8; at < n; at += 4) {         /* zero, or between 1e-8 and 1e7 */
        u32 w = le32(p + at), e = (w >> 23) & 0xFFu;

        if ((w & 0x7FFFFFFFu) != 0 && (e < 100 || e > 150)) {
            return 0;
        }
    }
    for (at = 0; at < n; at += 4) {
        if (at != 4) {
            bw_mem_wr32(p + at, le32(p + at));
        }
    }
    return 1;
}

int bw_narc_to_host(u8 *p, u32 n)
{
    u32 pos, sections, s;

    if (n < 16 || memcmp(p, "NARC", 4) != 0) {
        return 0;
    }
    pos = le16(p + 12);
    sections = le16(p + 14);
    if (pos < 16 || pos > n) {
        return -1;
    }
    for (s = 0; s < sections && pos + 8u <= n; s++) {
        u8 *c = p + pos;
        u32 size = le32(c + 4);

        if (size < 8 || size > n - pos) {
            return -1;
        }
        if (memcmp(c, "BTAF", 4) == 0) {
            u32 count = le16(c + 8), i;

            if (size < 12 || (u64)count * 8u > size - 12u) {
                return -1;
            }
            for (i = 0; i < count; i++) {
                fix32(c + 12 + 8u * i);
                fix32(c + 16 + 8u * i);
            }
            fix16(c + 8);
            fix16(c + 10);
        } else if (memcmp(c, "BTNF", 4) == 0) {
            if (fnt_to_host(c + 8, size - 8u) != 0) {
                return -1;
            }
        }
        fix32(c + 4);
        pos += size;
    }
    fix16(p + 4);
    fix16(p + 6);
    fix32(p + 8);
    fix16(p + 12);
    fix16(p + 14);
    return 1;
}

#if BW_HOST_BIG_ENDIAN && !defined(_WIN32)

/* The members of the little-endian NARC at `p` given to `fn`: every one,
 * or only member `only` when it is not -1. */
static int narc_members_to_host(u8 *p, u32 n, int (*fn)(u8 *, u32), int only)
{
    u32 pos, sections, s, btaf = 0, gmif = 0;

    if (n < 16 || memcmp(p, "NARC", 4) != 0) {
        return -1;
    }
    pos = le16(p + 12);
    sections = le16(p + 14);
    for (s = 0; s < sections && pos >= 16 && pos <= n - 8u; s++) {
        u32 size = le32(p + pos + 4);

        if (size < 8 || size > n - pos) {
            return -1;
        }
        if (memcmp(p + pos, "BTAF", 4) == 0) {
            btaf = pos;
        } else if (memcmp(p + pos, "GMIF", 4) == 0) {
            gmif = pos;
        }
        pos += size;
    }
    if (btaf == 0 || gmif == 0) {
        return -1;
    }
    {
        u32 count = le16(p + btaf + 8), i;
        u32 gsize = le32(p + gmif + 4) - 8u;

        if ((u64)count * 8u > le32(p + btaf + 4) - 12u) {
            return -1;
        }
        for (i = 0; i < count; i++) {
            u32 a = le32(p + btaf + 12 + 8u * i), b = le32(p + btaf + 16 + 8u * i);

            if (only >= 0 && i != (u32)only) {
                continue;
            }
            if (a > b || b > gsize || fn(p + gmif + 8 + a, b - a) < 0) {
                return -1;
            }
        }
        if (only >= 0 && (u32)only >= count) {
            return -1;
        }
    }
    return 0;
}

static const struct {
    const char *path;                   /* under data/ */
    int member;                         /* -1: every member */
    int (*fn)(u8 *, u32);
    const char *what;
} k_formats[] = {
    /* The text archives (arc ids 2 and 3). */
    { "a/0/0/2", -1, bw_msg_to_host, "malformed message bank" },
    { "a/0/0/3", -1, bw_msg_to_host, "malformed message bank" },
    /* The musicals' stage archives (arc ids 0x6F..0x72): member 1 is the
     * stage's message bank (MUSICAL_MCSS_LoadDistSave, ov112). */
    { "a/1/1/1", 1, bw_msg_to_host, "malformed message bank" },
    { "a/1/1/2", 1, bw_msg_to_host, "malformed message bank" },
    { "a/1/1/3", 1, bw_msg_to_host, "malformed message bank" },
    { "a/1/1/4", 1, bw_msg_to_host, "malformed message bank" },
    /* The fonts (arc id 0x17), which GF_FONT reads in pieces. */
    { "a/0/2/3", -1, bw_nftr_to_host, "malformed font" },
};


static void say(char *err, size_t err_len, const char *what, const char *path)
{
    if (err != NULL && err_len != 0) {
        snprintf(err, err_len, "%s: %s", what, path);
    }
}

/* One file of the tree, `rel` under data/, read into `buf`: NULL when it is
 * turned, else what is wrong with it. */
static const char *buffer_to_host(const char *rel, u8 *buf, u32 size)
{
    size_t k;

    if (memcmp(buf, "SDAT", 4) == 0) {
        return bw_sdat_to_host(buf, size) == 1 ? NULL : "malformed sound archive";
    }
    for (k = 0; k < sizeof(k_formats) / sizeof(k_formats[0]); k++) {
        if (strcmp(rel, k_formats[k].path) == 0
            && narc_members_to_host(buf, size, k_formats[k].fn, k_formats[k].member) != 0) {
            return k_formats[k].what;
        }
    }
    if (memcmp(buf, "NARC", 4) == 0
        && narc_members_to_host(buf, size, bw_g3d_member_to_host, -1) != 0) {
        return "malformed 3D file";
    }
    if (memcmp(buf, "NARC", 4) == 0) {
        narc_members_to_host(buf, size, bw_ica_to_host, -1);
    }
    return bw_narc_to_host(buf, size) < 0 ? "malformed NARC" : NULL;
}

static int file_to_host(const char *path, const char *rel, u32 *converted,
                        char *err, size_t err_len)
{
    FILE *fp = fopen(path, "rb");
    u8 head[4];
    u8 *buf;
    long size;
    const char *what;

    if (fp == NULL) {
        say(err, err_len, "cannot read", path);
        return -1;
    }
    if (fread(head, 1, 4, fp) != 4
        || (memcmp(head, "NARC", 4) != 0 && memcmp(head, "SDAT", 4) != 0)) {
        fclose(fp);
        return 0;
    }
    if (fseek(fp, 0, SEEK_END) != 0 || (size = ftell(fp)) < 16 || fseek(fp, 0, SEEK_SET) != 0
        || (buf = (u8 *)malloc((size_t)size)) == NULL) {
        fclose(fp);
        say(err, err_len, "cannot read", path);
        return -1;
    }
    if (fread(buf, 1, (size_t)size, fp) != (size_t)size) {
        fclose(fp);
        free(buf);
        say(err, err_len, "cannot read", path);
        return -1;
    }
    fclose(fp);
    if ((what = buffer_to_host(rel, buf, (u32)size)) != NULL) {
        free(buf);
        say(err, err_len, what, path);
        return -1;
    }
    if ((fp = fopen(path, "wb")) == NULL
        || fwrite(buf, 1, (size_t)size, fp) != (size_t)size) {
        if (fp != NULL) {
            fclose(fp);
        }
        free(buf);
        say(err, err_len, "cannot write", path);
        return -1;
    }
    free(buf);
    if (fclose(fp) != 0) {
        say(err, err_len, "cannot write", path);
        return -1;
    }
    (*converted)++;
    return 0;
}

static int walk(const char *dir, const char *rel, u32 *converted, char *err, size_t err_len)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int rc = 0;

    if (d == NULL) {
        say(err, err_len, "cannot list", dir);
        return -1;
    }
    while (rc == 0 && (e = readdir(d)) != NULL) {
        char path[1024], sub[1024];
        struct stat st;

        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        if ((size_t)snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) >= sizeof(path)
            || (size_t)snprintf(sub, sizeof(sub), "%s%s%s", rel, *rel ? "/" : "",
                                e->d_name) >= sizeof(sub)
            || stat(path, &st) != 0) {
            say(err, err_len, "cannot read", path);
            rc = -1;
        } else if (S_ISDIR(st.st_mode)) {
            rc = walk(path, sub, converted, err, err_len);
        } else if (S_ISREG(st.st_mode)) {
            rc = file_to_host(path, sub, converted, err, err_len);
        }
    }
    closedir(d);
    return rc;
}

#endif

int bw_tree_to_host_order(const char *extract_dir, u32 *converted, char *err, size_t err_len)
{
    u32 n = 0;
#if BW_HOST_BIG_ENDIAN && !defined(_WIN32)
    char path[1024];
    FILE *fp;

    snprintf(path, sizeof(path), "%s/%s", extract_dir, BW_BYTE_ORDER_FILE);
    if ((fp = fopen(path, "r")) != NULL) {
        fclose(fp);
        say(err, err_len, "already in the host's byte order", extract_dir);
        return -1;
    }
    snprintf(path, sizeof(path), "%s/data", extract_dir);
    if (walk(path, "", &n, err, err_len) != 0) {
        return -1;
    }
    snprintf(path, sizeof(path), "%s/%s", extract_dir, BW_BYTE_ORDER_FILE);
    if ((fp = fopen(path, "w")) == NULL
        || fputs(BW_BYTE_ORDER_MARK "\n", fp) < 0 || fclose(fp) != 0) {
        say(err, err_len, "cannot write", path);
        return -1;
    }
#elif BW_HOST_BIG_ENDIAN
    (void)extract_dir;
    if (err != NULL && err_len != 0) {
        snprintf(err, err_len, "a big-endian Windows host is not supported");
    }
    return -1;
#else
    (void)extract_dir;
    (void)err;
    (void)err_len;
#endif
    if (converted != NULL) {
        *converted = n;
    }
    return 0;
}
