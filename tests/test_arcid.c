/*
 * tests/test_arcid.c -- opening archives by id, the way the game does.
 *
 * No game data.  The test writes its own small world under the current
 * directory (the ctest build directory):
 *
 *   - three NARC archives, a/0/0/0, a/0/0/1 and a/0/0/2, whose members are
 *     byte patterns the test chose, including a zero-length member and one
 *     that is not a multiple of four;
 *   - a mod root holding a different a/0/0/1;
 *   - a synthetic ARM9 image, installed with bw_arm9_set_image, holding the
 *     three path strings and a three-entry table of their ARM9 addresses,
 *     which is the shape of the game's own table at 0x020A6C58.
 *
 * Then it drives the matched readers: src/matched/unit_02048DDC.c (open by
 * id, resolve a member), ARC_TOOL_ReadMemberRangeImpl.c and ARC_TOOL_ReadMember.c (read a member
 * by id), and checks every byte against what it wrote and, independently,
 * against the port's own NARC reader in sdk/narc.c, which shares no code
 * with the matched one.  What is checked:
 *
 *   - every member of every archive, by id, reads back exactly;
 *   - the resolved size is the member's own, zero included;
 *   - D00's `mode` argument is a byte offset added to the member's start;
 *   - a member index past the count reaches the game's assert sink, with the
 *     port's handler swapped for a counter;
 *   - the mod root wins for a/0/0/1 and falls through for the others, and
 *     clearing it restores the base copy;
 *   - the open-by-path shim refuses an address outside the image, an empty
 *     string and a missing file, and a table outside the image is refused by
 *     bw_arc_install_table.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port.h"
#include "port_test.h"

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#include <sys/types.h>
#define MKDIR(p) mkdir((p), 0777)
#endif

struct ArcHandle {
    u8 raw[BW_FSFILE_SIZE];
};

extern void ARC_TOOL_OpenArchive(struct ArcHandle *arc, u32 id);
extern u32  ARC_TOOL_SeekMember(struct ArcHandle *arc, u32 member, u32 base, u32 size);
extern void ARC_TOOL_ReadMemberRangeImpl(void *dst, u32 archive_id, u32 member, u32 mode,
                          u32 *size_out);
extern void ARC_TOOL_ReadMember(void *dst, u32 archive_id, u32 member);
extern s32  FS_ReadFile(void *file, void *dst, s32 len);
extern s32  FS_CloseFile(void *file);
extern u32  CRYPTOi__BN_lshift1(void *file, u32 path_address);

/* The port's g_Sink (sdk/game_globals.c), so the test can count asserts. */
struct Sink {
    u32 unk0;
    u32 unk4;
    u32 unk8;
    void (*handler)(void);
};
extern struct Sink g_Sink;

static int g_asserts;
static void count_assert(void)
{
    g_asserts++;
}

/* ------------------------------------------------------------------ */
/* A NARC writer                                                       */
/* ------------------------------------------------------------------ */

#define MAX_MEMBERS 8

struct Member {
    u32 len;
    u8  seed;
};

/* The numbers of the container's framing, as the extractor leaves them in
 * the data root (sdk/types.h, bw_dr_wr16: the host's order). */
static void put16(u8 *p, u32 v) { bw_dr_wr16(p, (u16)v); }
static void put32(u8 *p, u32 v) { bw_dr_wr32(p, v); }

static u8 member_byte(u8 seed, u32 i)
{
    return (u8)(seed * 31u + i * 7u + (i >> 3));
}

/*
 * The container as the DS tools lay it out: a 0x10-byte header, BTAF (count,
 * then one {start, end} pair per member, relative to GMIF's data), a minimal
 * BTNF, and GMIF with every member four-byte aligned.  Returns the byte
 * count written to `out`, which must be big enough.
 */
static u32 build_narc(u8 *out, const struct Member *m, u32 n)
{
    u32 btaf = 0x10;
    u32 btaf_size = 12 + 8 * n;
    u32 btnf = btaf + btaf_size;
    u32 btnf_size = 0x10;
    u32 gmif = btnf + btnf_size;
    u32 data = gmif + 8;
    u32 pos = 0;
    u32 i;
    u32 k;

    for (i = 0; i < n; i++) {
        put32(out + btaf + 12 + 8 * i, pos);
        put32(out + btaf + 16 + 8 * i, pos + m[i].len);
        for (k = 0; k < m[i].len; k++) {
            out[data + pos + k] = member_byte(m[i].seed, k);
        }
        pos += m[i].len;
        while (pos & 3u) {
            out[data + pos++] = 0xFF;
        }
    }

    memcpy(out, "NARC", 4);
    put16(out + 4, 0xFFFE);
    put16(out + 6, 0x0100);
    put32(out + 8, data + pos);
    put16(out + 12, 0x10);
    put16(out + 14, 3);

    memcpy(out + btaf, "BTAF", 4);
    put32(out + btaf + 4, btaf_size);
    put16(out + btaf + 8, n);
    put16(out + btaf + 10, 0);

    memcpy(out + btnf, "BTNF", 4);
    put32(out + btnf + 4, btnf_size);
    put32(out + btnf + 8, 4);       /* one root directory, no names */
    put16(out + btnf + 12, 0);
    put16(out + btnf + 14, 1);

    memcpy(out + gmif, "GMIF", 4);
    put32(out + gmif + 4, 8 + pos);
    return data + pos;
}

static int write_narc(const char *path, const struct Member *m, u32 n)
{
    static u8 buf[0x4000];
    FILE *fp;
    u32 len;

    memset(buf, 0, sizeof(buf));
    len = build_narc(buf, m, n);
    fp = fopen(path, "wb");
    if (fp == NULL) {
        return -1;
    }
    fwrite(buf, 1, len, fp);
    fclose(fp);
    return 0;
}

static void mkdirs(const char *root)
{
    char p[256];
    const char *parts[] = { "", "/a", "/a/0", "/a/0/0" };
    u32 i;

    for (i = 0; i < 4; i++) {
        snprintf(p, sizeof(p), "%s%s", root, parts[i]);
        MKDIR(p);
    }
}

/* ------------------------------------------------------------------ */

#define ROOT  "arcid_tmp"
#define BASE  ROOT "/base"
#define MOD   ROOT "/mod"

static const struct Member ARC0[] = { { 16, 1 }, { 5, 2 }, { 0, 3 }, { 300, 4 } };
static const struct Member ARC1[] = { { 8, 10 }, { 64, 11 } };
static const struct Member ARC2[] = { { 1, 20 } };
static const struct Member MOD1[] = { { 12, 90 }, { 64, 91 }, { 7, 92 } };

static const struct Member *const ARCS[3] = { ARC0, ARC1, ARC2 };
static const u32 COUNTS[3] = { 4, 2, 1 };

/* A synthetic ARM9 image: strings at +0x100, the table at +0x200. */
#define IMAGE_SIZE   0x400u
#define STR_OFF      0x100u
#define TABLE_OFF    0x200u
#define A(off)       (BW_ARM9_LOAD_ADDRESS + (off))

static u8 g_image[IMAGE_SIZE];

static void build_image(void)
{
    static const char *const names[3] = { "a/0/0/0", "a/0/0/1", "a/0/0/2" };
    u32 off = STR_OFF;
    u32 i;

    memset(g_image, 0, sizeof(g_image));
    for (i = 0; i < 3; i++) {
        memcpy(g_image + off, names[i], strlen(names[i]) + 1);
        put32(g_image + TABLE_OFF + 4 * i, A(off));
        off += (u32)strlen(names[i]) + 1;
    }
    /* An empty string at +0x180, and an unterminated run filling the end. */
    g_image[0x180] = '\0';
    memset(g_image + IMAGE_SIZE - 0x10, 'x', 0x10);
}

static int member_matches(const u8 *got, const struct Member *m, u32 skip)
{
    u32 k;

    for (k = 0; k < m->len; k++) {
        if (got[k] != member_byte(m->seed, k + skip)) {
            return 0;
        }
    }
    return 1;
}

int main(void)
{
    static u8 buf[0x1000];
    struct ArcHandle h;
    u32 id;
    u32 mem;
    u32 size;
    char path[256];

    mkdirs(ROOT);
    mkdirs(BASE);
    mkdirs(MOD);
    for (id = 0; id < 3; id++) {
        snprintf(path, sizeof(path), BASE "/a/0/0/%u", id);
        BW_REQUIRE("write a base archive", write_narc(path, ARCS[id], COUNTS[id]) == 0);
    }
    BW_REQUIRE("write the mod archive", write_narc(MOD "/a/0/0/1", MOD1, 3) == 0);

    bw_set_data_root(BASE);
    bw_set_mod_root(NULL);
    build_image();
    bw_arm9_set_image(g_image, IMAGE_SIZE);

    BW_CHECK_EQ("a table outside the image is refused",
                bw_arc_install_table(A(IMAGE_SIZE - 4), 3), -1);
    BW_CHECK_EQ("install the table", bw_arc_install_table(A(TABLE_OFF), 3), 0);

    /* Every member of every archive, three ways. */
    for (id = 0; id < 3; id++) {
        BwArcFile port;
        char rel[16];

        snprintf(rel, sizeof(rel), "a/0/0/%u", id);
        BW_REQUIRE("the port's own reader opens it", bw_arc_open(&port, rel) == 0);
        for (mem = 0; mem < COUNTS[id]; mem++) {
            const struct Member *m = &ARCS[id][mem];

            /* 1. the two matched halves by hand */
            ARC_TOOL_OpenArchive(&h, id);
            size = ARC_TOOL_SeekMember(&h, mem, 0, 0);
            BW_CHECK_EQ("the resolved size is the member's", size, m->len);
            memset(buf, 0xEE, sizeof(buf));
            BW_CHECK_EQ("the whole member reads",
                        FS_ReadFile(&h, buf, (s32)size), (long)m->len);
            BW_CHECK_EQ("byte for byte", member_matches(buf, m, 0), 1);
            FS_CloseFile(&h);

            /* 2. the game's own read-by-id entry point */
            memset(buf, 0xEE, sizeof(buf));
            ARC_TOOL_ReadMember(buf, id, mem);
            BW_CHECK_EQ("ARC_TOOL_ReadMember reads the same bytes",
                        member_matches(buf, m, 0), 1);
            BW_CHECK_EQ("and stops at the member's end", buf[m->len], 0xEE);

            /* 3. the port's independent reader agrees */
            BW_CHECK_EQ("sdk/narc.c agrees on the size",
                        bw_arc_member_size(&port, mem), (long)m->len);
            memset(buf, 0xEE, sizeof(buf));
            BW_CHECK_EQ("sdk/narc.c reads the member",
                        bw_arc_read_member(&port, mem, buf, sizeof(buf)), (long)m->len);
            BW_CHECK_EQ("... with the same bytes", member_matches(buf, m, 0), 1);
        }
        bw_arc_close(&port);
    }

    /* D00's mode is a byte offset from the member's start. */
    memset(buf, 0xEE, sizeof(buf));
    ARC_TOOL_ReadMemberRangeImpl(buf, 0, 3, 5, NULL);
    {
        struct Member tail = { ARC0[3].len - 5, ARC0[3].seed };
        BW_CHECK_EQ("mode 5 starts five bytes in", member_matches(buf, &tail, 5), 1);
    }

    /* A member past the count reaches the assert sink.  The matched code
     * carries on after it, as the DS does when the sink returns. */
    {
        void (*saved)(void) = g_Sink.handler;
        g_Sink.handler = count_assert;
        g_asserts = 0;
        ARC_TOOL_OpenArchive(&h, 1);
        (void)ARC_TOOL_SeekMember(&h, 2, 0, 0);
        FS_CloseFile(&h);
        g_Sink.handler = saved;
        BW_CHECK_EQ("member 2 of a 2-member archive asserts", g_asserts, 1);
        g_asserts = 0;
        ARC_TOOL_OpenArchive(&h, 1);
        (void)ARC_TOOL_SeekMember(&h, 1, 0, 0);
        FS_CloseFile(&h);
        BW_CHECK_EQ("member 1 does not", g_asserts, 0);
    }

    /* The mod root: a/0/0/1 is replaced, the others fall through. */
    bw_set_mod_root(MOD);
    for (mem = 0; mem < 3; mem++) {
        memset(buf, 0xEE, sizeof(buf));
        ARC_TOOL_OpenArchive(&h, 1);
        size = ARC_TOOL_SeekMember(&h, mem, 0, 0);
        BW_CHECK_EQ("the mod's member size", size, MOD1[mem].len);
        FS_ReadFile(&h, buf, (s32)size);
        FS_CloseFile(&h);
        BW_CHECK_EQ("the mod's bytes", member_matches(buf, &MOD1[mem], 0), 1);
    }
    memset(buf, 0xEE, sizeof(buf));
    ARC_TOOL_ReadMember(buf, 2, 0);
    BW_CHECK_EQ("an archive the mod lacks falls through",
                member_matches(buf, &ARC2[0], 0), 1);
    bw_set_mod_root(NULL);
    memset(buf, 0xEE, sizeof(buf));
    ARC_TOOL_ReadMember(buf, 1, 1);
    BW_CHECK_EQ("clearing the mod root restores the base",
                member_matches(buf, &ARC1[1], 0), 1);

    /* The open-by-path shim's refusals. */
    BW_CHECK_EQ("a path address below the image",
                CRYPTOi__BN_lshift1(&h, 0x02000000u), 0);
    BW_CHECK_EQ("a path address past the image",
                CRYPTOi__BN_lshift1(&h, A(IMAGE_SIZE)), 0);
    BW_CHECK_EQ("an empty path", CRYPTOi__BN_lshift1(&h, A(0x180)), 0);
    BW_CHECK_EQ("an unterminated path",
                CRYPTOi__BN_lshift1(&h, A(IMAGE_SIZE - 0x10)), 0);
    g_image[STR_OFF + 6] = '9';          /* "a/0/0/9": no such file */
    bw_arm9_set_image(g_image, IMAGE_SIZE);
    BW_CHECK_EQ("a missing file", CRYPTOi__BN_lshift1(&h, A(STR_OFF)), 0);
    BW_CHECK_EQ("an existing one", CRYPTOi__BN_lshift1(&h, A(STR_OFF + 8)), 1);
    FS_CloseFile(&h);

    bw_arm9_set_image(NULL, 0);
    BW_REPORT("test_arcid");
}
