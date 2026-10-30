/*
 * sdk/vfs_narc.c -- member-level NARC overrides: the merged archive.
 *
 * Why this exists.  Every asset the game loads is a member of a NARC, and a
 * mod that adds one Pokemon or edits one map should not have to repack and
 * ship a whole archive (and then conflict with every other mod touching the
 * same archive).  So a mod may hold a *directory* at an archive's NitroFS
 * path, and each file in it named `<index>` or `<index>.<ext>` replaces that
 * member, or adds it past the archive's end (sdk/vfs.h has the whole model).
 *
 * Why a synthesised archive rather than a lookup on the read path.  The
 * readers that matter are matched code: func_02048E34, func_02049578 and the
 * rest read the NARC's own header, BTAF and GMIF through FS_SeekFile and
 * FS_ReadFile and do the arithmetic themselves, and the BwArcFile fields
 * (imageBase, fatOffset, count) they trust come from the same bytes.  The
 * one place that can make all of them agree without touching any of them is
 * the bytes: this file builds the NARC the mods describe, laid out exactly
 * the way the cartridge's archives are, and the FS shim (sdk/fs.c) serves it
 * as a memory file whenever the archive's path is opened, by path, by file
 * id, by ROM range or through the archive id table.  Nothing above the FS
 * shim can tell a merged archive from a shipped one.
 *
 * Merged images are cached per path, with a reference count held by the
 * open files reading them (the FS record stores the cache *index*, never a
 * pointer, per the 0x48-byte FSFile rule), and dropped on any change to the
 * roots or the mod list.
 *
 * Framing only: no member's contents are interpreted, and no game bytes are
 * in this file.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port.h"
#include "vfs.h"
#include "plat_dir.h"
#include "vfs_msg.h"
#include "vfs_sdat.h"

#define FOURCC(a, b, c, d) \
    ((u32)(a) | ((u32)(b) << 8) | ((u32)(c) << 16) | ((u32)(d) << 24))

#define NARC_MAGIC FOURCC('N', 'A', 'R', 'C')
#define BTAF_MAGIC FOURCC('B', 'T', 'A', 'F')
#define BTNF_MAGIC FOURCC('B', 'T', 'N', 'F')
#define GMIF_MAGIC FOURCC('G', 'M', 'I', 'F')

#define NARC_HEADER_SIZE 0x10u
#define NARC_ALIGN       4u
#define NARC_PAD         0xFFu

/* The merged archive's numbers, in the data root's order (sdk/types.h,
 * bw_dr_wr16): the same order as the archives it stands in for. */
static void put16(u8 *p, u32 v)
{
    bw_dr_wr16(p, (u16)v);
}

static void put32(u8 *p, u32 v)
{
    bw_dr_wr32(p, v);
}

/* ------------------------------------------------------------------ */
/* Packing                                                             */
/* ------------------------------------------------------------------ */

/* The nameless BTNF every archive of this game but one carries: magic,
 * size 0x10, the root's sub-table at +4, first file id 0, one directory;
 * its numbers in the data root's order like the rest of the framing. */
#define MINIMAL_BTNF_SIZE 16u

static void minimal_btnf(u8 out[MINIMAL_BTNF_SIZE])
{
    bw_wr32(out, BTNF_MAGIC);
    bw_dr_wr32(out + 4, MINIMAL_BTNF_SIZE);
    bw_dr_wr32(out + 8, 4);
    bw_dr_wr16(out + 12, 0);
    bw_dr_wr16(out + 14, 1);
}

int bw_narc_pack(const u8 *const *data, const u32 *sizes, u32 count,
                 const u8 *btnf, u32 btnf_size, u8 **out, u32 *out_size)
{
    u64 gmif_data = 0;
    u64 total;
    u32 btaf_size;
    u32 i;
    u32 at;
    u32 pos;
    u8 *img;
    u8 minimal[MINIMAL_BTNF_SIZE];

    *out = NULL;
    *out_size = 0;
    if (count > BW_NARC_MAX_MEMBERS || (count != 0 && sizes == NULL)) {
        return -1;
    }
    if (btnf == NULL) {
        minimal_btnf(minimal);
        btnf = minimal;
        btnf_size = MINIMAL_BTNF_SIZE;
    }
    if (btnf_size < 8 || (btnf_size % NARC_ALIGN) != 0 ||
        bw_rd32(btnf) != BTNF_MAGIC || bw_dr_rd32(btnf + 4) != btnf_size) {
        return -1;
    }
    for (i = 0; i < count; i++) {
        if (sizes[i] != 0 && (data == NULL || data[i] == NULL)) {
            return -1;
        }
        gmif_data += ((u64)sizes[i] + NARC_ALIGN - 1) & ~(u64)(NARC_ALIGN - 1);
    }
    btaf_size = 12u + 8u * count;
    total = (u64)NARC_HEADER_SIZE + btaf_size + btnf_size + 8u + gmif_data;
    if (total > 0xFFFFFFFFu) {
        return -1;
    }
    img = (u8 *)malloc((size_t)total);
    if (img == NULL) {
        return -1;
    }

    bw_wr32(img + 0x00, NARC_MAGIC);       /* tags stay bytes          */
    put16(img + 0x04, 0xFFFEu);           /* byte order mark          */
    put16(img + 0x06, 0x0100u);           /* version                  */
    put32(img + 0x08, (u32)total);
    put16(img + 0x0C, NARC_HEADER_SIZE);
    put16(img + 0x0E, 3);                 /* chunk count              */

    at = NARC_HEADER_SIZE;
    bw_wr32(img + at + 0, BTAF_MAGIC);
    put32(img + at + 4, btaf_size);
    put16(img + at + 8, count);
    put16(img + at + 10, 0);
    pos = 0;
    for (i = 0; i < count; i++) {
        put32(img + at + 12 + i * 8, pos);
        put32(img + at + 16 + i * 8, pos + sizes[i]);
        pos += (sizes[i] + NARC_ALIGN - 1) & ~(NARC_ALIGN - 1);
    }
    at += btaf_size;

    memcpy(img + at, btnf, btnf_size);
    at += btnf_size;

    bw_wr32(img + at + 0, GMIF_MAGIC);
    put32(img + at + 4, (u32)(8u + gmif_data));
    at += 8;
    for (i = 0; i < count; i++) {
        u32 padded = (sizes[i] + NARC_ALIGN - 1) & ~(NARC_ALIGN - 1);

        if (sizes[i] != 0) {
            memcpy(img + at, data[i], sizes[i]);
        }
        memset(img + at + sizes[i], NARC_PAD, padded - sizes[i]);
        at += padded;
    }

    *out = img;
    *out_size = (u32)total;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Reading the base archive                                            */
/* ------------------------------------------------------------------ */

typedef struct Narc {
    const u8 *img;
    u32 size;
    u32 count;
    u32 fat;        /* offset of BTAF                     */
    u32 btnf;       /* offset of BTNF, or 0 when absent   */
    u32 btnf_size;
    u32 gmif;       /* offset of GMIF                     */
    u32 gmif_size;
} Narc;

static int parse_narc(const u8 *img, u32 size, Narc *n)
{
    u32 pos;
    int have_fat = 0;
    int have_img = 0;
    u32 i;

    memset(n, 0, sizeof(*n));
    if (size < NARC_HEADER_SIZE || bw_rd32(img) != NARC_MAGIC) {
        return -1;
    }
    n->img = img;
    n->size = size;
    pos = bw_dr_rd16(img + 12);
    while (pos <= size && size - pos >= 8) {
        u32 magic = bw_rd32(img + pos);
        u32 csize = bw_dr_rd32(img + pos + 4);

        if (csize < 8 || csize > size - pos) {
            break;
        }
        if (magic == BTAF_MAGIC && csize >= 12) {
            n->fat = pos;
            n->count = bw_dr_rd16(img + pos + 8);
            if ((u64)12 + (u64)8 * n->count > csize) {
                return -1;
            }
            have_fat = 1;
        } else if (magic == BTNF_MAGIC) {
            n->btnf = pos;
            n->btnf_size = csize;
        } else if (magic == GMIF_MAGIC) {
            n->gmif = pos;
            n->gmif_size = csize;
            have_img = 1;
        }
        pos += csize;
    }
    if (!have_fat || !have_img) {
        return -1;
    }
    for (i = 0; i < n->count; i++) {
        u32 start = bw_dr_rd32(img + n->fat + 12 + i * 8);
        u32 end = bw_dr_rd32(img + n->fat + 16 + i * 8);

        if (end < start || end > n->gmif_size - 8) {
            return -1;
        }
    }
    return 0;
}

static u8 *read_whole(const char *path, u32 *size_out)
{
    FILE *fp = fopen(path, "rb");
    long end;
    u8 *buf;

    *size_out = 0;
    if (fp == NULL) {
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0 || (end = ftell(fp)) < 0 ||
        (unsigned long)end > 0xFFFFFFFFul || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }
    buf = (u8 *)malloc(end == 0 ? 1 : (size_t)end);
    if (buf == NULL) {
        fclose(fp);
        return NULL;
    }
    if (end != 0 && fread(buf, 1, (size_t)end, fp) != (size_t)end) {
        free(buf);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    *size_out = (u32)end;
    return buf;
}

/* ------------------------------------------------------------------ */
/* Collecting the mods' members                                        */
/* ------------------------------------------------------------------ */

typedef struct Override {
    u32   index;
    u32   mod;      /* position in the load order */
    char *path;
} Override;

typedef struct Collect {
    Override   *list;
    u32         count;
    u32         cap;
    Override   *msg;        /* `<bank>.msg` line files, every mod's, in order */
    u32         msg_count;
    u32         msg_cap;
    u32         mod;
    const char *dir;
    int         error;
} Collect;

/*
 * `<digits>` or `<digits>.<ext>` with no further dot: 12, 0012, 12.bin,
 * 12.nclr.  Anything else (12.bin.bak, readme.txt, .DS_Store) is not a
 * member and is ignored.  Returns 1 for a member name, 0 for another name,
 * -1 for a member name whose index the format cannot hold.
 */
static int member_index(const char *name, u32 *index)
{
    const char *p = name;
    u32 v = 0;

    if (*p < '0' || *p > '9') {
        return 0;
    }
    while (*p >= '0' && *p <= '9') {
        if (v < BW_NARC_MAX_MEMBERS) {
            v = v * 10u + (u32)(*p - '0');
        }
        p++;
    }
    if (*p == '.') {
        if (p[1] == '\0' || strchr(p + 1, '.') != NULL) {
            return 0;
        }
    } else if (*p != '\0') {
        return 0;
    }
    if (v >= BW_NARC_MAX_MEMBERS) {
        return -1;
    }
    *index = v;
    return 1;
}

/* `<digits>.msg`: lines for text bank `<digits>` (sdk/vfs_msg.h). */
static int msg_index(const char *name, u32 *index)
{
    size_t n = strlen(name);
    u32 v = 0;
    size_t i;

    if (n < 5 || strcmp(name + n - 4, ".msg") != 0) {
        return 0;
    }
    for (i = 0; i < n - 4; i++) {
        if (name[i] < '0' || name[i] > '9') {
            return 0;
        }
        if (v < BW_NARC_MAX_MEMBERS) {
            v = v * 10u + (u32)(name[i] - '0');
        }
    }
    if (v >= BW_NARC_MAX_MEMBERS) {
        return -1;
    }
    *index = v;
    return 1;
}

static int add_entry(Override **list, u32 *count, u32 *cap, u32 index,
                     u32 mod, char *path)
{
    if (*count == *cap) {
        u32 grow = *cap == 0 ? 16 : *cap * 2;
        Override *grown = (Override *)realloc(*list, grow * sizeof(*grown));

        if (grown == NULL) {
            return -1;
        }
        *list = grown;
        *cap = grow;
    }
    (*list)[*count].index = index;
    (*list)[*count].mod = mod;
    (*list)[*count].path = path;
    (*count)++;
    return 0;
}

static int collect_one(void *ctx, const char *name)
{
    Collect *c = (Collect *)ctx;
    u32 index;
    int kind;
    size_t need;
    char *path;
    int is_msg;

    /* A line file is not a member: `70.msg` holds lines for bank 70, it is
     * not member 70 itself. */
    kind = msg_index(name, &index);
    is_msg = kind != 0;
    if (!is_msg) {
        kind = member_index(name, &index);
    }
    if (kind == 0) {
        return 0;
    }
    if (kind < 0) {
        fprintf(stderr, "vfs: %s/%s: member index past the NARC limit of %u\n",
                c->dir, name, BW_NARC_MAX_MEMBERS);
        c->error = 1;
        return 1;
    }
    need = strlen(c->dir) + 1 + strlen(name) + 1;
    path = (char *)malloc(need);
    if (path == NULL) {
        c->error = 1;
        return 1;
    }
    sprintf(path, "%s/%s", c->dir, name);
    if (bw_plat_path_kind(path) != BW_PATH_FILE) {
        free(path);      /* a subdirectory named like a member: not one */
        return 0;
    }
    if ((is_msg ? add_entry(&c->msg, &c->msg_count, &c->msg_cap, index, c->mod, path)
                : add_entry(&c->list, &c->count, &c->cap, index, c->mod, path)) != 0) {
        free(path);
        c->error = 1;
        return 1;
    }
    return 0;
}

static int cmp_override(const void *a, const void *b)
{
    const Override *x = (const Override *)a;
    const Override *y = (const Override *)b;

    if (x->index != y->index) {
        return x->index < y->index ? -1 : 1;
    }
    if (x->mod != y->mod) {
        return x->mod < y->mod ? -1 : 1;
    }
    return 0;
}

static void free_collect(Collect *c)
{
    u32 i;

    for (i = 0; i < c->count; i++) {
        free(c->list[i].path);
    }
    free(c->list);
    for (i = 0; i < c->msg_count; i++) {
        free(c->msg[i].path);
    }
    free(c->msg);
    memset(c, 0, sizeof(*c));
}

/*
 * Gather the member files of every mod later than the file-granular winner
 * `layer`, then reduce them to one per index, the latest mod's.  Two files
 * for one index in the same mod is an error: neither is "later".
 * Returns 0 (c->count may be 0) or -1.
 */
static int collect(const char *rel, int layer, Collect *c)
{
    char dir[1200];
    u32 n = bw_mod_count();
    u32 i;
    u32 kept;

    memset(c, 0, sizeof(*c));
    for (i = (u32)(layer + 1); i < n; i++) {
        const char *root = bw_mod_root_at(i);

        if (strlen(root) + 1 + strlen(rel) + 1 > sizeof(dir)) {
            continue;
        }
        if (snprintf(dir, sizeof(dir), "%s/%s", root, rel) >= (int)sizeof(dir)) {
            continue;
        }
        if (bw_plat_path_kind(dir) != BW_PATH_DIR) {
            continue;
        }
        c->mod = i;
        c->dir = dir;
        (void)bw_plat_list_dir(dir, collect_one, c);
        if (c->error) {
            free_collect(c);
            return -1;
        }
    }
    if (c->count == 0) {
        return 0;
    }
    /* The line files stay in load order (the merge applies them in it). */
    qsort(c->list, c->count, sizeof(*c->list), cmp_override);
    kept = 0;
    for (i = 0; i < c->count; i++) {
        if (i + 1 < c->count && c->list[i + 1].index == c->list[i].index) {
            if (c->list[i + 1].mod == c->list[i].mod) {
                fprintf(stderr, "vfs: %s and %s name the same member of %s\n",
                        c->list[i].path, c->list[i + 1].path, rel);
                free_collect(c);
                return -1;
            }
            free(c->list[i].path);        /* a later mod has this index */
            continue;
        }
        c->list[kept++] = c->list[i];
    }
    c->count = kept;
    return 0;
}

int bw_vfs_member_overrides(const char *rel)
{
    Collect c;
    int layer = bw_vfs_file_layer(rel);
    int n;

    if (layer == BW_LAYER_NONE || bw_mod_count() == 0) {
        return 0;
    }
    if (collect(rel, layer, &c) != 0) {
        return -1;
    }
    n = (int)(c.count + c.msg_count);
    free_collect(&c);
    return n;
}

/* ------------------------------------------------------------------ */
/* Merging                                                             */
/* ------------------------------------------------------------------ */

/*
 * The text bank a `.msg` merge starts from when the bank has no bytes yet
 * (an index past the archive's end): one language section, no lines.
 */
static const u8 k_empty_bank[20] = {
    1, 0, 0, 0,   4, 0, 0, 0,   0, 0, 0, 0,   16, 0, 0, 0,   4, 0, 0, 0
};

/*
 * Apply the `.msg` line files to the members in data[]/sizes[] (count of
 * them, sized for every bank index a line file names).  Each bank's lines
 * apply in load order on top of whatever the whole-member layers left.
 * The merged banks are kept in merged[] (count entries, NULL where none)
 * for the caller to free.  Returns 0 or -1.
 */
static int apply_msg(const char *rel, const Collect *c, const u8 **data,
                     u32 *sizes, u32 count, u8 **merged)
{
    u32 i;

    for (i = 0; i < c->msg_count; i++) {
        u32 idx = c->msg[i].index;
        BwMsgLines *lists[64];
        const BwMsgLines *const_lists[64];
        u32 n = 0, j;
        int rc;
        const u8 *base;
        u32 base_size;

        if (merged[idx] != NULL) {
            continue;                       /* done with an earlier entry */
        }
        /* every line file for this bank, in load order */
        for (j = i; j < c->msg_count; j++) {
            char err[256];
            u8 *text;
            u32 text_size;

            if (c->msg[j].index != idx) {
                continue;
            }
            if (n == sizeof(lists) / sizeof(lists[0])) {
                fprintf(stderr, "vfs: %s bank %u: too many line files\n", rel, idx);
                break;
            }
            lists[n] = (BwMsgLines *)calloc(1, sizeof(BwMsgLines));
            text = read_whole(c->msg[j].path, &text_size);
            if (lists[n] == NULL || text == NULL ||
                bw_msg_parse((const char *)text, text_size, lists[n], err,
                             sizeof(err)) != 0) {
                fprintf(stderr, "vfs: %s: %s\n", c->msg[j].path,
                        text == NULL ? "cannot read" : err);
                free(text);
                if (lists[n] != NULL) {
                    bw_msg_lines_free(lists[n]);
                    free(lists[n]);
                }
                for (j = 0; j < n; j++) {
                    bw_msg_lines_free(lists[j]);
                    free(lists[j]);
                }
                return -1;
            }
            free(text);
            const_lists[n] = lists[n];
            n++;
        }
        base = idx < count ? data[idx] : NULL;
        base_size = idx < count ? sizes[idx] : 0;
        if (base == NULL || base_size < sizeof(k_empty_bank)) {
            base = k_empty_bank;
            base_size = sizeof(k_empty_bank);
        }
        rc = bw_msg_merge(base, base_size, const_lists, n, &merged[idx], &sizes[idx]);
        for (j = 0; j < n; j++) {
            bw_msg_lines_free(lists[j]);
            free(lists[j]);
        }
        if (rc != 0) {
            fprintf(stderr, "vfs: %s: bank %u: the text lines do not merge\n", rel, idx);
            merged[idx] = NULL;
            return -1;
        }
        data[idx] = merged[idx];
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* The sound archive                                                   */
/* ------------------------------------------------------------------ */

#define SDAT_MODS_MAX 32

/*
 * An SDAT is not a NARC.  A mod holding a directory at its path adds
 * sequences, banks and wave archives through `sdat.txt` there
 * (sdk/vfs_sdat.h); the merge keeps every original byte and appends.
 * Returns 1 with *out set, 0 when no mod adds anything, or -1.
 */
static int build_sdat(const char *rel, int layer, u8 **out, u32 *out_size)
{
    char dir[1200], path[1400], base_path[1200];
    u32 n = bw_mod_count();
    u32 i, k, nadd = 0;
    BwSdatAdd adds[SDAT_MODS_MAX];
    BwSdatDecl decls[SDAT_MODS_MAX];
    BwSdatFile *files[SDAT_MODS_MAX];
    u8 *base = NULL;
    u32 base_size;
    int rc = -1;

    memset(decls, 0, sizeof(decls));
    memset(files, 0, sizeof(files));
    for (i = (u32)(layer + 1); i < n && nadd < SDAT_MODS_MAX; i++) {
        const char *root = bw_mod_root_at(i);
        u8 *text;
        u32 text_size;
        char err[256];

        if (strlen(root) + 1 + strlen(rel) + 16 > sizeof(dir)) {
            continue;
        }
        if (snprintf(dir, sizeof(dir), "%s/%s", root, rel) >= (int)sizeof(dir) ||
            snprintf(path, sizeof(path), "%s/sdat.txt", dir) >= (int)sizeof(path)) {
            continue;
        }
        if (bw_plat_path_kind(path) != BW_PATH_FILE) {
            continue;
        }
        text = read_whole(path, &text_size);
        if (text == NULL || bw_sdat_parse_decl((const char *)text, &decls[nadd],
                                               err, sizeof(err)) != 0) {
            fprintf(stderr, "vfs: %s: %s\n", path, text == NULL ? "cannot read" : err);
            free(text);
            goto done;
        }
        free(text);
        files[nadd] = (BwSdatFile *)calloc(decls[nadd].count + 1, sizeof(BwSdatFile));
        if (files[nadd] == NULL) {
            nadd++;
            goto done;
        }
        for (k = 0; k < decls[nadd].count; k++) {
            u32 sz = 0;
            char fp[1400];

            snprintf(fp, sizeof(fp), "%s/%s", dir, decls[nadd].entries[k].file);
            files[nadd][k].name = decls[nadd].entries[k].file;
            files[nadd][k].data = read_whole(fp, &sz);
            files[nadd][k].size = sz;
            if (files[nadd][k].data == NULL) {
                fprintf(stderr, "vfs: %s: cannot read\n", fp);
                nadd++;
                goto done;
            }
        }
        adds[nadd].mod_name = bw_mod_name_at(i);
        adds[nadd].decl = &decls[nadd];
        adds[nadd].files = files[nadd];
        adds[nadd].file_count = decls[nadd].count;
        nadd++;
    }
    if (nadd == 0) {
        return 0;
    }
    if (bw_data_path(base_path, sizeof(base_path), rel) != 0 ||
        (base = read_whole(base_path, &base_size)) == NULL) {
        fprintf(stderr, "vfs: %s: cannot read the sound archive\n", rel);
        goto done;
    }
    if (bw_vfs_sdat_merge(base, base_size, adds, nadd, out, out_size) != 0) {
        goto done;
    }
    rc = 1;

done:
    for (i = 0; i < nadd; i++) {
        if (files[i] != NULL) {
            for (k = 0; k < decls[i].count; k++) {
                free((void *)files[i][k].data);
            }
            free(files[i]);
        }
        bw_sdat_decl_free(&decls[i]);
    }
    free(base);
    return rc;
}

int bw_vfs_build_merged(const char *rel, u8 **out, u32 *out_size)
{
    char base_path[1200];
    Collect c;
    Narc narc;
    int layer;
    u8 *base = NULL;
    u32 base_size;
    const u8 **data = NULL;
    u32 *sizes = NULL;
    u8 **owned = NULL;
    u8 **msg_merged = NULL;
    u32 count = 0;
    u32 i;
    int rc = -1;

    *out = NULL;
    *out_size = 0;
    if (bw_mod_count() == 0) {
        return 0;
    }
    /*
     * Members apply on top of a file: with no layer holding one, a mod
     * directory at this path is just a directory (a/0/1 holds the archives
     * a/0/1/0..9, whose names look like member indices).
     */
    layer = bw_vfs_file_layer(rel);
    if (layer == BW_LAYER_NONE) {
        return 0;
    }
    {
        size_t rl = strlen(rel);

        if (rl > 5 && strcmp(rel + rl - 5, ".sdat") == 0) {
            return build_sdat(rel, layer, out, out_size);
        }
    }
    if (collect(rel, layer, &c) != 0) {
        return -1;
    }
    if (c.count == 0 && c.msg_count == 0) {
        free_collect(&c);
        return 0;
    }

    if (bw_data_path(base_path, sizeof(base_path), rel) != 0 ||
        (base = read_whole(base_path, &base_size)) == NULL) {
        fprintf(stderr, "vfs: %s: cannot read the archive the members apply to\n", rel);
        goto done;
    }
    if (parse_narc(base, base_size, &narc) != 0) {
        fprintf(stderr, "vfs: %s: member overrides given, but %s is not a NARC\n",
                rel, base_path);
        goto done;
    }

    count = narc.count;
    if (c.count > 0 && c.list[c.count - 1].index + 1 > count) {
        count = c.list[c.count - 1].index + 1;
    }
    for (i = 0; i < c.msg_count; i++) {
        if (c.msg[i].index + 1 > count) {
            count = c.msg[i].index + 1;
        }
    }
    data = (const u8 **)calloc(count == 0 ? 1 : count, sizeof(*data));
    sizes = (u32 *)calloc(count == 0 ? 1 : count, sizeof(*sizes));
    owned = (u8 **)calloc(c.count == 0 ? 1 : c.count, sizeof(*owned));
    msg_merged = (u8 **)calloc(count == 0 ? 1 : count, sizeof(*msg_merged));
    if (data == NULL || sizes == NULL || owned == NULL || msg_merged == NULL) {
        goto done;
    }
    for (i = 0; i < narc.count; i++) {
        u32 start = bw_dr_rd32(base + narc.fat + 12 + i * 8);
        u32 end = bw_dr_rd32(base + narc.fat + 16 + i * 8);

        data[i] = base + narc.gmif + 8 + start;
        sizes[i] = end - start;
    }
    /* Indices past the base count that no mod fills stay empty members. */
    for (i = 0; i < c.count; i++) {
        u32 sz;

        owned[i] = read_whole(c.list[i].path, &sz);
        if (owned[i] == NULL) {
            fprintf(stderr, "vfs: %s: cannot read member file\n", c.list[i].path);
            goto done;
        }
        data[c.list[i].index] = owned[i];
        sizes[c.list[i].index] = sz;
    }
    /* The text lines, on top of every whole-member layer. */
    if (c.msg_count > 0 && apply_msg(rel, &c, data, sizes, count, msg_merged) != 0) {
        goto done;
    }
    if (bw_narc_pack(data, sizes, count,
                     narc.btnf != 0 ? base + narc.btnf : NULL, narc.btnf_size,
                     out, out_size) != 0) {
        fprintf(stderr, "vfs: %s: the merged archive does not fit the NARC format\n", rel);
        goto done;
    }
    rc = 1;

done:
    if (owned != NULL) {
        for (i = 0; i < c.count; i++) {
            free(owned[i]);
        }
    }
    free(owned);
    if (msg_merged != NULL) {
        for (i = 0; i < count; i++) {
            bw_msg_free(msg_merged[i]);
        }
    }
    free(msg_merged);
    free(sizes);
    free((void *)data);
    free(base);
    free_collect(&c);
    return rc;
}

/* ------------------------------------------------------------------ */
/* The cache the FS shim reads                                         */
/* ------------------------------------------------------------------ */

typedef struct Merged {
    char *rel;     /* NULL: a free entry                         */
    u8   *data;
    u32   size;
    u32   refs;    /* open files reading it                       */
    int   stale;   /* dropped by a flush, freed at the last close */
} Merged;

static Merged *g_merged;
static u32     g_merged_cap;

static void free_entry(Merged *m)
{
    free(m->rel);
    free(m->data);
    memset(m, 0, sizeof(*m));
}

void bw_vfs_flush(void)
{
    u32 i;

    for (i = 0; i < g_merged_cap; i++) {
        if (g_merged[i].rel == NULL) {
            continue;
        }
        if (g_merged[i].refs == 0) {
            free_entry(&g_merged[i]);
        } else {
            g_merged[i].stale = 1;
        }
    }
}

static int new_entry(u32 *index)
{
    u32 i;
    u32 cap;
    Merged *grown;

    for (i = 0; i < g_merged_cap; i++) {
        if (g_merged[i].rel == NULL) {
            *index = i;
            return 0;
        }
    }
    cap = g_merged_cap == 0 ? 8 : g_merged_cap * 2;
    grown = (Merged *)realloc(g_merged, cap * sizeof(*grown));
    if (grown == NULL) {
        return -1;
    }
    memset(grown + g_merged_cap, 0, (cap - g_merged_cap) * sizeof(*grown));
    *index = g_merged_cap;
    g_merged = grown;
    g_merged_cap = cap;
    return 0;
}

int bw_vfs_acquire(const char *rel, const u8 **data, u32 *size, u32 *handle)
{
    u32 i;
    u8 *img;
    u32 img_size;
    char *key;
    int rc;

    *data = NULL;
    *size = 0;
    *handle = 0;
    if (rel == NULL || bw_mod_count() == 0) {
        return 0;
    }
    for (i = 0; i < g_merged_cap; i++) {
        Merged *m = &g_merged[i];

        if (m->rel != NULL && !m->stale && strcmp(m->rel, rel) == 0) {
            m->refs++;
            *data = m->data;
            *size = m->size;
            *handle = i + 1;
            return 1;
        }
    }
    rc = bw_vfs_build_merged(rel, &img, &img_size);
    if (rc <= 0) {
        return rc;
    }
    key = (char *)malloc(strlen(rel) + 1);
    if (key == NULL || new_entry(&i) != 0) {
        free(key);
        free(img);
        return -1;
    }
    strcpy(key, rel);
    g_merged[i].rel = key;
    g_merged[i].data = img;
    g_merged[i].size = img_size;
    g_merged[i].refs = 1;
    g_merged[i].stale = 0;
    *data = img;
    *size = img_size;
    *handle = i + 1;
    return 1;
}

void bw_vfs_release(u32 handle)
{
    Merged *m;

    if (handle == 0 || handle > g_merged_cap) {
        return;
    }
    m = &g_merged[handle - 1];
    if (m->rel == NULL || m->refs == 0) {
        return;
    }
    m->refs--;
    if (m->refs == 0 && m->stale) {
        free_entry(m);
    }
}
