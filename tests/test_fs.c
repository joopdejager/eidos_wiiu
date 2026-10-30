/*
 * tests/test_fs.c -- the FS shim (sdk/fs.c): the NitroFS tables, every way
 * of opening a file, and reading.
 *
 * The synthetic half needs no game data.  It writes a four-file tree, a FAT
 * and an FNT laid out by hand from the format (sdk/fs.c, "FNT layout"), and
 * checks:
 *
 *   - id -> path and path -> id over the tree, the SDK's path spellings
 *     ("rom:", leading slash, ASCII case, "." and ".."), and the refusals
 *     (a directory, an overlay id with no name, an id past the FAT);
 *   - FS_OpenFile / FS_OpenFileEx by a host string, with and without tables,
 *     and a write mode refused;
 *   - FS_ConvertPathToFileID + FS_OpenFileFast, and a foreign archive tag
 *     refused;
 *   - FS_OpenFileDirect, by id and by searching the FAT, as a window;
 *   - FS_CreateFileFromMemory;
 *   - seek, read, length and position, including a short read at the end;
 *   - the mod overlay winning for a file opened by id;
 *   - FS_LoadArchiveTables' size formula and its copy into a misaligned
 *     block, and the tables loaded from fat.bin / fnt.bin beside the root;
 *   - FS_Init and FS_SetDefaultDMA.
 *
 * The data-gated half runs when argv[1] is a data root whose parent holds
 * the extract's fat.bin and fnt.bin: it round-trips every named file id,
 * opens real archives by path and checks they are NARCs of their FAT
 * length, reads one through FS_OpenFileDirect and compares, and checks the
 * table cache against the two files.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port.h"
#include "fs.h"
#include "port_test.h"

#ifdef _WIN32
#include <direct.h>
#define BW_MKDIR(p) _mkdir(p)
#define BW_RMDIR(p) _rmdir(p)
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define BW_MKDIR(p) mkdir((p), 0777)
#define BW_RMDIR(p) rmdir(p)
#endif

/* The SDK surface, as the shim defines it. */
typedef struct { u32 arc; u32 file_id; } FileID;

extern void FS_Init(u32 dma);
extern BOOL FS_IsAvailable(void);
extern u32  FS_SetDefaultDMA(u32 dma);
extern void FS_InitFile(void *file);
extern BOOL FS_OpenFile(void *file, const char *path);
extern BOOL FS_OpenFileEx(void *file, const char *path, u32 mode);
extern BOOL FS_ConvertPathToFileID(FileID *out, const char *path);
extern BOOL FS_OpenFileFast(void *file, FileID id);
extern BOOL FS_OpenFileDirect(void *file, void *arc, u32 top, u32 bottom, u32 id);
extern BOOL FS_CreateFileFromMemory(void *file, void *src, u32 size);
extern BOOL FS_SeekFile(void *file, s32 offset, u32 origin);
extern s32  FS_ReadFile(void *file, void *dst, s32 len);
extern u32  FS_GetFileLength(void *file);
extern u32  FS_GetLength(void *file);
extern u32  FS_GetFilePosition(void *file);
extern BOOL FS_CloseFile(void *file);
extern u32  FS_LoadArchiveTables(void *arc, void *mem, u32 max_size);
extern u32  func_02078C28(void *file);
extern s32  func_02078B70(void *file, void *dst, s32 len);

#define T_ROOT "fs_test_tmp"
#define T_BASE T_ROOT "/data"
#define T_MOD  T_ROOT "/mod"

static const char TOP[] = "TOP-FILE:0123456789";
static const char XB[]  = "x.bin contents, forty bytes of them....";
static const char YD[]  = "Y";
static const char ZZ[]  = "zz-in-a-subdirectory";
static const char XMOD[] = "the mod's x.bin";

static u8  g_fnt[128];
static u32 g_fnt_len;
static u8  g_fat[7 * 8];

typedef struct { u32 words[18]; } File;   /* the game's 0x48 bytes */

static int write_bytes(const char *path, const void *data, size_t n)
{
    FILE *fp = fopen(path, "wb");

    if (fp == NULL) {
        return -1;
    }
    fwrite(data, 1, n, fp);
    fclose(fp);
    return 0;
}

static void put(const void *p, u32 n)
{
    memcpy(g_fnt + g_fnt_len, p, n);
    g_fnt_len += n;
}

static void put8(u32 v)
{
    g_fnt[g_fnt_len++] = (u8)v;
}

static void name(u32 type, const char *s)
{
    put8(type | (u32)strlen(s));
    put(s, (u32)strlen(s));
}

/*
 * root (0xF000, first id 3): top.bin, a/
 * a    (0xF001, first id 4): x.bin, Y.dat, sub/
 * sub  (0xF002, first id 6): z
 * ids 0..2 are overlay files, in the FAT but not in the FNT.
 */
static void build_tables(void)
{
    u32 off_root;
    u32 off_a;
    u32 off_sub;
    u32 i;

    g_fnt_len = 24;
    off_root = g_fnt_len;
    name(0x00, "top.bin");
    name(0x80, "a");
    put8(0x01); put8(0xF0);
    put8(0);
    off_a = g_fnt_len;
    name(0x00, "x.bin");
    name(0x00, "Y.dat");
    name(0x80, "sub");
    put8(0x02); put8(0xF0);
    put8(0);
    off_sub = g_fnt_len;
    name(0x00, "z");
    put8(0);

    bw_wr32(g_fnt + 0, off_root); bw_wr16(g_fnt + 4, 3); bw_wr16(g_fnt + 6, 3);
    bw_wr32(g_fnt + 8, off_a);    bw_wr16(g_fnt + 12, 4); bw_wr16(g_fnt + 14, 0xF000);
    bw_wr32(g_fnt + 16, off_sub); bw_wr16(g_fnt + 20, 6); bw_wr16(g_fnt + 22, 0xF001);

    for (i = 0; i < 3; i++) {                       /* overlay files */
        bw_wr32(g_fat + i * 8, 0x100 + i * 0x20);
        bw_wr32(g_fat + i * 8 + 4, 0x110 + i * 0x20);
    }
    bw_wr32(g_fat + 24, 0x1000); bw_wr32(g_fat + 28, 0x1000 + sizeof(TOP) - 1);
    bw_wr32(g_fat + 32, 0x2000); bw_wr32(g_fat + 36, 0x2000 + sizeof(XB) - 1);
    bw_wr32(g_fat + 40, 0x3000); bw_wr32(g_fat + 44, 0x3000 + sizeof(YD) - 1);
    bw_wr32(g_fat + 48, 0x4000); bw_wr32(g_fat + 52, 0x4000 + sizeof(ZZ) - 1);
}

static void build_tree(void)
{
    BW_MKDIR(T_ROOT);
    BW_MKDIR(T_BASE);
    BW_MKDIR(T_BASE "/a");
    BW_MKDIR(T_BASE "/a/sub");
    BW_MKDIR(T_MOD);
    BW_MKDIR(T_MOD "/a");
    write_bytes(T_BASE "/top.bin", TOP, sizeof(TOP) - 1);
    write_bytes(T_BASE "/a/x.bin", XB, sizeof(XB) - 1);
    write_bytes(T_BASE "/a/Y.dat", YD, sizeof(YD) - 1);
    write_bytes(T_BASE "/a/sub/z", ZZ, sizeof(ZZ) - 1);
    write_bytes(T_MOD "/a/x.bin", XMOD, sizeof(XMOD) - 1);
}

static void clean_tree(void)
{
    remove(T_BASE "/top.bin");
    remove(T_BASE "/a/x.bin");
    remove(T_BASE "/a/Y.dat");
    remove(T_BASE "/a/sub/z");
    remove(T_MOD "/a/x.bin");
    remove(T_ROOT "/fat.bin");
    remove(T_ROOT "/fnt.bin");
    BW_RMDIR(T_BASE "/a/sub");
    BW_RMDIR(T_BASE "/a");
    BW_RMDIR(T_BASE);
    BW_RMDIR(T_MOD "/a");
    BW_RMDIR(T_MOD);
    BW_RMDIR(T_ROOT);
}

/* 1 if `file` reads back exactly `want` from its current position. */
static int reads(void *file, const char *want)
{
    char buf[128];
    s32 n = (s32)strlen(want);

    memset(buf, 0, sizeof(buf));
    return FS_ReadFile(file, buf, n) == n && memcmp(buf, want, (size_t)n) == 0;
}

static int synthetic(void)
{
    File f;
    FileID fid;
    char path[256];
    u32 id = 0;
    u8 block[256 + 64];
    u8 *mem;
    u32 total;
    char buf[64];

    build_tables();
    bw_set_data_root(T_BASE);
    bw_set_mod_root("");

    printf("no tables\n");
    bw_fs_set_tables(NULL, 0, NULL, 0);       /* and none beside the root */
    BW_CHECK_EQ("no tables on disk", bw_fs_tables_available(), 0);
    BW_CHECK_EQ("archive tables size without tables",
                FS_LoadArchiveTables(NULL, NULL, 0), 0);
    BW_CHECK_EQ("open by plain path", FS_OpenFile(&f, "a/x.bin"), 1);
    BW_CHECK_EQ("  its length", FS_GetFileLength(&f), sizeof(XB) - 1);
    BW_CHECK_EQ("  its bytes", reads(&f, XB), 1);
    FS_CloseFile(&f);
    BW_CHECK_EQ("open with rom: prefix", FS_OpenFile(&f, "rom:/top.bin"), 1);
    FS_CloseFile(&f);
    BW_CHECK_EQ("open a missing file", FS_OpenFile(&f, "nope"), 0);
    BW_CHECK_EQ("path -> id without tables", bw_fs_id_of_path("a/x.bin", &id), -1);

    printf("tables installed\n");
    BW_REQUIRE("install", bw_fs_set_tables(g_fat, sizeof(g_fat), g_fnt, g_fnt_len) == 0);
    BW_CHECK_EQ("file count", bw_fs_file_count(), 7);
    BW_CHECK_EQ("path of 3", bw_fs_path_of_id(3, path, sizeof(path)), 0);
    BW_CHECK_EQ("  is top.bin", strcmp(path, "top.bin"), 0);
    BW_CHECK_EQ("path of 5", bw_fs_path_of_id(5, path, sizeof(path)), 0);
    BW_CHECK_EQ("  is a/Y.dat", strcmp(path, "a/Y.dat"), 0);
    BW_CHECK_EQ("path of 6", bw_fs_path_of_id(6, path, sizeof(path)), 0);
    BW_CHECK_EQ("  is a/sub/z", strcmp(path, "a/sub/z"), 0);
    BW_CHECK_EQ("an overlay id has no name", bw_fs_path_of_id(1, path, sizeof(path)), -1);
    BW_CHECK_EQ("an id past the FAT", bw_fs_path_of_id(7, path, sizeof(path)), -1);
    BW_CHECK_EQ("a buffer too small", bw_fs_path_of_id(6, path, 4), -1);

    BW_CHECK_EQ("id of a/x.bin", bw_fs_id_of_path("a/x.bin", &id), 0);
    BW_CHECK_EQ("  is 4", id, 4);
    id = 0;
    BW_CHECK_EQ("id of /a/sub/z", bw_fs_id_of_path("/a/sub/z", &id) == 0 && id == 6, 1);
    BW_CHECK_EQ("id of rom:/top.bin", bw_fs_id_of_path("rom:/top.bin", &id) == 0 && id == 3, 1);
    BW_CHECK_EQ("id of A/y.DAT", bw_fs_id_of_path("A/y.DAT", &id) == 0 && id == 5, 1);
    BW_CHECK_EQ("id of a/sub/../x.bin", bw_fs_id_of_path("a/sub/../x.bin", &id) == 0 && id == 4, 1);
    BW_CHECK_EQ("id of ./a//x.bin", bw_fs_id_of_path("./a//x.bin", &id) == 0 && id == 4, 1);
    BW_CHECK_EQ("a directory is not a file", bw_fs_id_of_path("a/sub", &id), -1);
    BW_CHECK_EQ("a file is not a directory", bw_fs_id_of_path("top.bin/x", &id), -1);
    BW_CHECK_EQ("a missing name", bw_fs_id_of_path("a/q.bin", &id), -1);
    BW_CHECK_EQ("an empty path", bw_fs_id_of_path("", &id), -1);

    printf("opening\n");
    BW_CHECK_EQ("FS_OpenFile through the FNT", FS_OpenFile(&f, "/A/SUB/z"), 1);
    BW_CHECK_EQ("  length", FS_GetLength(&f), sizeof(ZZ) - 1);
    BW_CHECK_EQ("  the wrapper spelling", func_02078C28(&f), sizeof(ZZ) - 1);
    BW_CHECK_EQ("  bytes", reads(&f, ZZ), 1);
    BW_CHECK_EQ("  position at the end", FS_GetFilePosition(&f), sizeof(ZZ) - 1);
    BW_CHECK_EQ("  a read at the end is 0", FS_ReadFile(&f, buf, 4), 0);
    BW_CHECK_EQ("  seek set", FS_SeekFile(&f, 3, 0), 1);
    BW_CHECK_EQ("  read after seek", reads(&f, ZZ + 3), 1);
    BW_CHECK_EQ("  seek end -2", FS_SeekFile(&f, -2, 2), 1);
    BW_CHECK_EQ("  short read", func_02078B70(&f, buf, 10), 2);
    BW_CHECK_EQ("  seek cur -4", FS_SeekFile(&f, -4, 1), 1);
    BW_CHECK_EQ("  position", FS_GetFilePosition(&f), sizeof(ZZ) - 1 - 4);
    BW_CHECK_EQ("  seek before the start fails", FS_SeekFile(&f, -1, 0), 0);
    BW_CHECK_EQ("  seek past the end fails", FS_SeekFile(&f, 1, 2), 0);
    BW_CHECK_EQ("  a bad origin fails", FS_SeekFile(&f, 0, 7), 0);
    BW_CHECK_EQ("  position unchanged", FS_GetFilePosition(&f), sizeof(ZZ) - 1 - 4);
    BW_CHECK_EQ("  close", FS_CloseFile(&f), 1);
    BW_CHECK_EQ("  close again", FS_CloseFile(&f), 0);
    BW_CHECK_EQ("write mode refused", FS_OpenFileEx(&f, "top.bin", 3), 0);
    BW_CHECK_EQ("read mode", FS_OpenFileEx(&f, "top.bin", 1), 1);
    FS_CloseFile(&f);

    BW_CHECK_EQ("convert path", FS_ConvertPathToFileID(&fid, "a/Y.dat"), 1);
    BW_CHECK_EQ("  archive tag", fid.arc, 0x02150DA8);
    BW_CHECK_EQ("  file id", fid.file_id, 5);
    BW_CHECK_EQ("open fast", FS_OpenFileFast(&f, fid), 1);
    BW_CHECK_EQ("  bytes", reads(&f, YD), 1);
    FS_CloseFile(&f);
    fid.arc = 0x02150000;
    BW_CHECK_EQ("open fast, foreign archive", FS_OpenFileFast(&f, fid), 0);
    BW_CHECK_EQ("convert a missing path", FS_ConvertPathToFileID(&fid, "zz"), 0);

    BW_CHECK_EQ("direct, by id, a window", FS_OpenFileDirect(&f, NULL, 0x2002, 0x2008, 4), 1);
    BW_CHECK_EQ("  window length", FS_GetFileLength(&f), 6);
    BW_CHECK_EQ("  window bytes", reads(&f, "bin co"), 1);
    BW_CHECK_EQ("  nothing past the window", FS_ReadFile(&f, buf, 1), 0);
    FS_CloseFile(&f);
    BW_CHECK_EQ("direct, by search", FS_OpenFileDirect(&f, NULL, 0x1004, 0x1009, 0xFFFFFFFFu), 1);
    BW_CHECK_EQ("  bytes", reads(&f, "FILE:"), 1);
    FS_CloseFile(&f);
    BW_CHECK_EQ("direct, outside the id's extent",
                FS_OpenFileDirect(&f, NULL, 0x1004, 0x2004, 3), 0);
    BW_CHECK_EQ("direct, inside no file",
                FS_OpenFileDirect(&f, NULL, 0x5000, 0x5004, 0xFFFFFFFFu), 0);
    BW_CHECK_EQ("direct, an overlay file has no host copy",
                FS_OpenFileDirect(&f, NULL, 0x100, 0x110, 0), 0);

    BW_CHECK_EQ("memory file", FS_CreateFileFromMemory(&f, (void *)TOP, 8), 1);
    BW_CHECK_EQ("  length", FS_GetFileLength(&f), 8);
    BW_CHECK_EQ("  seek", FS_SeekFile(&f, 4, 0), 1);
    BW_CHECK_EQ("  short read", FS_ReadFile(&f, buf, 10), 4);
    BW_CHECK_EQ("  bytes", memcmp(buf, "FILE", 4), 0);
    FS_CloseFile(&f);

    printf("mod overlay\n");
    bw_set_mod_root(T_MOD);
    BW_CHECK_EQ("by id, the mod copy wins", bw_fs_open_id(&f, 4), 0);
    BW_CHECK_EQ("  bytes", reads(&f, XMOD), 1);
    FS_CloseFile(&f);
    BW_CHECK_EQ("by path, base falls through", FS_OpenFile(&f, "a/Y.dat"), 1);
    BW_CHECK_EQ("  bytes", reads(&f, YD), 1);
    FS_CloseFile(&f);
    bw_set_mod_root("");

    printf("archive tables\n");
    total = (sizeof(g_fat) + g_fnt_len + 32 + 31) & ~31u;
    BW_CHECK_EQ("size query", FS_LoadArchiveTables(NULL, NULL, 0), total);
    memset(block, 0xEE, sizeof(block));
    mem = block + 4;
    while (((uintptr_t)mem & 31u) == 0) {
        mem++;                                /* make sure it is misaligned */
    }
    BW_CHECK_EQ("too small a block", FS_LoadArchiveTables(NULL, mem, total - 1), total);
    BW_CHECK_EQ("  left alone", block[40], 0xEE);
    BW_CHECK_EQ("fill", FS_LoadArchiveTables(NULL, mem, total), total);
    {
        u8 *p = mem;

        while (((uintptr_t)p & 31u) != 0) {
            p++;
        }
        BW_CHECK_EQ("  FAT at the first 32-byte boundary", memcmp(p, g_fat, sizeof(g_fat)), 0);
        BW_CHECK_EQ("  FNT after it", memcmp(p + sizeof(g_fat), g_fnt, g_fnt_len), 0);
        BW_CHECK_EQ("  inside the block", (p + sizeof(g_fat) + g_fnt_len) <= mem + total, 1);
    }

    printf("tables from disk\n");
    write_bytes(T_ROOT "/fat.bin", g_fat, sizeof(g_fat));
    write_bytes(T_ROOT "/fnt.bin", g_fnt, g_fnt_len);
    bw_fs_set_tables(NULL, 0, NULL, 0);
    BW_CHECK_EQ("loaded beside the data root", bw_fs_tables_available(), 1);
    BW_CHECK_EQ("  file count", bw_fs_file_count(), 7);
    BW_CHECK_EQ("  resolves", bw_fs_id_of_path("a/sub/z", &id) == 0 && id == 6, 1);

    printf("init\n");
    BW_CHECK_EQ("not initialised", FS_IsAvailable(), 0);
    FS_Init(1);
    BW_CHECK_EQ("initialised", FS_IsAvailable(), 1);
    BW_CHECK_EQ("default DMA", bw_fs_default_dma(), 1);
    BW_CHECK_EQ("set returns the old", FS_SetDefaultDMA(0xFFFFFFFFu), 1);
    BW_CHECK_EQ("and the new is kept", FS_SetDefaultDMA(1), 0xFFFFFFFFu);
    FS_Init(3);
    BW_CHECK_EQ("a second init is a no-op", bw_fs_default_dma(), 1);
    return 0;
}

/* ------------------------------------------------------------------ */

static u8 *slurp(const char *path, u32 *size)
{
    FILE *fp = fopen(path, "rb");
    long end;
    u8 *b;

    *size = 0;
    if (fp == NULL) {
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    end = ftell(fp);
    rewind(fp);
    b = (u8 *)malloc((size_t)end + 1);
    if (b != NULL && fread(b, 1, (size_t)end, fp) != (size_t)end) {
        free(b);
        b = NULL;
    }
    fclose(fp);
    *size = (u32)end;
    return b;
}

static int real_data(const char *root)
{
    char fatp[1200];
    char fntp[1200];
    char path[512];
    size_t n = strlen(root);
    u8 *fat;
    u8 *fnt;
    u32 fat_size;
    u32 fnt_size;
    u32 i;
    u32 named = 0;
    u32 round = 0;
    File f;
    File g;
    u8 hdr[16];
    u8 a[64];
    u8 b[64];
    u32 top;
    u32 bottom;
    u32 id;
    u8 *cache;
    u32 total;

    while (n > 0 && root[n - 1] != '/' && root[n - 1] != '\\') {
        n--;
    }
    snprintf(fatp, sizeof(fatp), "%.*sfat.bin", (int)n, root);
    snprintf(fntp, sizeof(fntp), "%.*sfnt.bin", (int)n, root);
    fat = slurp(fatp, &fat_size);
    fnt = slurp(fntp, &fnt_size);
    if (fat == NULL || fnt == NULL) {
        printf("real data: no fat.bin / fnt.bin beside %s, skipped\n", root);
        free(fat);
        free(fnt);
        return 0;
    }
    printf("real data: %s\n", root);
    bw_set_data_root(root);
    bw_set_mod_root("");
    bw_fs_set_tables(NULL, 0, NULL, 0);
    BW_REQUIRE("tables load", bw_fs_tables_available());
    BW_CHECK_EQ("file count is the FAT's", bw_fs_file_count(), fat_size / 8);

    for (i = 0; i < fat_size / 8; i++) {
        if (bw_fs_path_of_id(i, path, sizeof(path)) == 0) {
            named++;
            if (bw_fs_id_of_path(path, &id) == 0 && id == i) {
                round++;
            }
        }
    }
    printf("  %lu of %lu ids named\n", (unsigned long)named, (unsigned long)(fat_size / 8));
    BW_CHECK_EQ("every named id round-trips", round, named);
    BW_CHECK_EQ("most ids are named (overlays are not)", named + 237 >= fat_size / 8, 1);

    BW_REQUIRE("open a/0/0/2 by path", FS_OpenFile(&f, "a/0/0/2"));
    BW_CHECK_EQ("  it is a NARC", FS_ReadFile(&f, hdr, 16) == 16 && memcmp(hdr, "NARC", 4) == 0, 1);
    BW_CHECK_EQ("  path -> id", bw_fs_id_of_path("rom:/a/0/0/2", &id), 0);
    BW_CHECK_EQ("  its FAT extent", bw_fs_file_extent(id, &top, &bottom), 0);
    BW_CHECK_EQ("  length is the FAT's", FS_GetFileLength(&f), bottom - top);
    BW_CHECK_EQ("  the NARC's own size agrees", bw_dr_rd32(hdr + 8), bottom - top);

    BW_CHECK_EQ("seek 0x40", FS_SeekFile(&f, 0x40, 0), 1);
    BW_CHECK_EQ("read 64", FS_ReadFile(&f, a, 64), 64);
    BW_REQUIRE("direct over the same range",
               FS_OpenFileDirect(&g, NULL, top + 0x40, top + 0x80, 0xFFFFFFFFu));
    BW_CHECK_EQ("  read 64", FS_ReadFile(&g, b, 64), 64);
    BW_CHECK_EQ("  same bytes", memcmp(a, b, 64), 0);
    FS_CloseFile(&g);
    FS_CloseFile(&f);

    BW_CHECK_EQ("open a/2/3/4", FS_OpenFile(&f, "a/2/3/4"), 1);
    FS_CloseFile(&f);
    BW_CHECK_EQ("a/2/3/5 does not exist", FS_OpenFile(&f, "a/2/3/5"), 0);

    total = FS_LoadArchiveTables(NULL, NULL, 0);
    BW_CHECK_EQ("table cache size", total, (fat_size + fnt_size + 32 + 31) & ~31u);
    cache = (u8 *)malloc(total + 32);
    if (cache != NULL) {
        u8 *p = cache;

        FS_LoadArchiveTables(NULL, cache, total);
        while (((uintptr_t)p & 31u) != 0) {
            p++;
        }
        BW_CHECK_EQ("  holds fat.bin", memcmp(p, fat, fat_size), 0);
        BW_CHECK_EQ("  then fnt.bin", memcmp(p + fat_size, fnt, fnt_size), 0);
        free(cache);
    }
    free(fat);
    free(fnt);
    return 0;
}

int main(int argc, char **argv)
{
    clean_tree();
    build_tree();
    if (synthetic() != 0) {
        BW_REPORT("test_fs");
    }
    clean_tree();
    if (argc > 1 && argv[1] != NULL && argv[1][0] != '\0') {
        (void)real_data(argv[1]);
    } else {
        printf("real data: no data root given, skipped\n");
    }
    BW_REPORT("test_fs");
}
