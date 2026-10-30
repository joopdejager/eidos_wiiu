/*
 * tests/test_vfs_msg.c -- the `.msg` grammar and the text bank line merge.
 *
 * Synthetic half: every construct of the grammar parses to the units the
 * kenon display form defines, every error case is refused with a message that
 * names the line, and a merge on a synthetic bank keeps untouched lines
 * byte for byte, grows with gaps, and applies mod load order per line.
 *
 * Data half (argv[1] is the data root; skipped without one, SKIP and exit
 * 0): read member 70 (species names) of a/0/0/2, replace line 1, append line
 * 700, and check that line 1 and line 700 carry the new text and every other
 * line is byte-identical to the original (compared as ciphertext and, for
 * the original range, as decoded text).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vfs_msg.h"
#include "port_test.h"

static u32 rd32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u32 rd16(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8);
}

/* ---- grammar ------------------------------------------------------ */

static int parse1(const char *text, BwMsgLines *l, char *err)
{
    return bw_msg_parse(text, strlen(text), l, err, 128);
}

/* Parse one entry and compare its units with `want` (n units). */
static void expect_units(const char *label, const char *text, const u16 *want, u32 n)
{
    BwMsgLines l;
    char err[128];
    int rc = parse1(text, &l, err);

    BW_CHECK_EQ(label, rc, 0);
    if (rc != 0) {
        printf("    %s\n", err);
        return;
    }
    BW_CHECK_EQ(label, l.count, 1);
    if (l.count == 1) {
        BW_CHECK_EQ(label, l.lines[0].len, n);
        if (l.lines[0].len == n && n > 0) {
            BW_CHECK_EQ(label, memcmp(l.lines[0].units, want, n * 2), 0);
        }
    }
    bw_msg_lines_free(&l);
}

static void expect_error(const char *label, const char *text, const char *needle)
{
    BwMsgLines l;
    char err[128];
    int rc = parse1(text, &l, err);

    BW_CHECK_EQ(label, rc, -1);
    BW_CHECK_EQ(label, l.count, 0);
    if (rc == -1) {
        BW_CHECK_EQ(label, strstr(err, needle) != NULL, 1);
        if (strstr(err, needle) == NULL) {
            printf("    message was: %s\n", err);
        }
    }
}

static void test_grammar(void)
{
    BwMsgLines l;
    char err[128];
    static const u16 plain[] = { 'A', 'b', ' ' };
    static const u16 nl[] = { 'x', 0xFFFE, 'y' };
    static const u16 ctl0[] = { 0xF000, 0xBE01, 0 };
    static const u16 ctlf[] = { 0xF000, 0xF000, 0 };
    static const u16 ctl2[] = { 0xF000, 0x0100, 2, 0x0A, 0xFFFE };
    static const u16 raw[] = { 0xD800, 'z' };
    static const u16 esc[] = { '\\', '{', '}' };
    static const u16 uni[] = { 0x00E9, 0x246D, 0xD83D, 0xDE00 };
    static const u16 one[] = { 'q' };

    expect_units("plain, trailing space kept", "5 = Ab ", plain, 3);
    expect_units("no space after =", "5=q", one, 1);
    expect_units("spaces before =", "5   = q", one, 1);
    expect_units("newline", "5 = x\\ny", nl, 3);
    expect_units("control zero args", "5 = {BE01}", ctl0, 3);
    expect_units("control lower case", "5 = {be01}", ctl0, 3);
    expect_units("control empty arg list", "5 = {F000:}", ctlf, 3);
    expect_units("control args", "5 = {0100:a,fffe}", ctl2, 5);
    expect_units("raw unit", "5 = {U+D800}z", raw, 2);
    expect_units("escapes", "5 = \\\\\\{\\}", esc, 3);
    expect_units("utf-8", "5 = \xC3\xA9\xE2\x91\xAD\xF0\x9F\x98\x80", uni, 4);
    expect_units("CRLF", "5 = q\r\n", one, 1);
    expect_units("bom, comment, blank", "\xEF\xBB\xBF# c\n\n  \n5 = q\n", one, 1);
    expect_units("empty text", "5 =", NULL, 0);

    /* order and duplicates are kept in file order */
    BW_CHECK_EQ("two lines", parse1("3 = a\n1 = b\n3 = c", &l, err), 0);
    BW_CHECK_EQ("two lines count", l.count, 3);
    if (l.count == 3) {
        BW_CHECK_EQ("order 0", l.lines[0].index, 3);
        BW_CHECK_EQ("order 1", l.lines[1].index, 1);
        BW_CHECK_EQ("order 2", l.lines[2].index, 3);
    }
    bw_msg_lines_free(&l);

    expect_error("no index", "= a", "line 1");
    expect_error("letters as index", "x = a", "line 1");
    expect_error("no equals", "5 a", "'='");
    expect_error("index too big", "65535 = a", "index");
    expect_error("unknown escape", "5 = a\\q", "unknown escape");
    expect_error("escape at end", "5 = a\\", "unknown escape");
    expect_error("unclosed brace", "5 = {BE01", "closing brace");
    expect_error("bad kind", "5 = {XYZ}", "kind");
    expect_error("empty kind", "5 = {}", "kind");
    expect_error("kind too long", "5 = {12345}", "kind");
    expect_error("bad argument", "5 = {BE01:1,zz}", "argument");
    expect_error("empty argument", "5 = {BE01:1,,2}", "argument");
    expect_error("bad raw unit", "5 = {U+12G4}", "U+");
    expect_error("raw end unit", "5 = {U+FFFF}", "FFFF");
    expect_error("bad utf-8", "5 = \xC3", "UTF-8");
    expect_error("overlong utf-8", "5 = \xC0\x80", "UTF-8");
    expect_error("packed", "5 = {packed}abc", "packed");
    expect_error("error names the later line", "5 = a\n\n7 = \\q", "line 3");
}

/* ---- synthetic bank ----------------------------------------------- */

typedef struct Bank {
    u8  *data;
    u32  size;
} Bank;

/* Build a bank of `n` lines; line i is the two units 'a'+i, 'A'+i. */
static Bank make_bank(u32 n, u32 flags)
{
    Bank b;
    u32 total = 16 + 4 + 8 * n + 6 * n;
    u32 i;
    u32 off = 4 + 8 * n;

    b.data = (u8 *)calloc(total, 1);
    b.size = total;
    b.data[0] = 1;
    b.data[2] = (u8)n;
    b.data[3] = (u8)(n >> 8);
    b.data[4] = (u8)(total - 16);
    b.data[5] = (u8)((total - 16) >> 8);
    b.data[12] = 16;
    b.data[16] = (u8)(total - 16);
    b.data[17] = (u8)((total - 16) >> 8);
    for (i = 0; i < n; i++) {
        u16 s[3];
        u8 *e = b.data + 20 + 8 * i;
        u32 j;

        s[0] = (u16)('a' + i);
        s[1] = (u16)('A' + i);
        s[2] = 0xFFFF;
        bw_msg_cipher(s, 3, i);
        for (j = 0; j < 3; j++) {
            b.data[16 + off + 2 * j] = (u8)s[j];
            b.data[16 + off + 2 * j + 1] = (u8)(s[j] >> 8);
        }
        e[0] = (u8)off;
        e[1] = (u8)(off >> 8);
        e[4] = 3;
        e[6] = (u8)flags;
        off += 6;
    }
    return b;
}

/* Decode line `i` of a bank into `out` (no terminator); returns its length. */
static u32 decode_line(const u8 *bank, u32 i, u16 *out)
{
    u32 sect = rd32(bank + 12);
    const u8 *e = bank + sect + 4 + 8 * i;
    u32 len = rd16(e + 4);
    u32 j;

    for (j = 0; j < len; j++) {
        out[j] = (u16)rd16(bank + sect + rd32(e) + 2 * j);
    }
    bw_msg_cipher(out, len, i);
    return len - 1;
}

static void parse_ok(const char *text, BwMsgLines *l)
{
    char err[128];

    if (parse1(text, l, err) != 0) {
        printf("  FAIL fixture: %s\n", err);
        bw_failures++;
    }
}

static int test_merge_synthetic(void)
{
    Bank b = make_bank(5, 60);
    BwMsgLines m1, m2;
    const BwMsgLines *mods[2];
    u8 *out;
    u32 out_size;
    u16 u[64];
    u32 i, n;
    u32 sect;

    /* mod 1 changes 1 and appends 8; mod 2 changes 8 and 1 again, adds 9 */
    parse_ok("1 = one\n8 = eight\n", &m1);
    parse_ok("# later\n8 = EIGHT\\n!\n9 = {BE01}\n", &m2);
    mods[0] = &m1;
    mods[1] = &m2;
    BW_CHECK_EQ("merge rc", bw_msg_merge(b.data, b.size, mods, 2, &out, &out_size), 0);
    BW_REQUIRE("merge output", out != NULL);
    BW_CHECK_EQ("count", rd16(out + 2), 10);
    BW_CHECK_EQ("size field", rd32(out + 4), out_size - 16);
    BW_CHECK_EQ("section size", rd32(out + 16), out_size - 16);
    BW_CHECK_EQ("lang offset", rd32(out + 12), 16);
    sect = 16;

    /* untouched original lines: same ciphertext bytes, same flags */
    for (i = 0; i < 5; i++) {
        const u8 *oe = out + sect + 4 + 8 * i;
        const u8 *be = b.data + 20 + 8 * i;

        if (i == 1) {
            continue;
        }
        BW_CHECK_EQ("kept length", rd16(oe + 4), 3);
        BW_CHECK_EQ("kept flags", rd16(oe + 6), 60);
        BW_CHECK_EQ("kept bytes", memcmp(out + sect + rd32(oe),
                                         b.data + 16 + rd32(be), 6), 0);
    }
    n = decode_line(out, 1, u);
    BW_CHECK_EQ("line 1 len", n, 3);
    BW_CHECK_EQ("line 1 text", u[0] == 'o' && u[1] == 'n' && u[2] == 'e', 1);
    BW_CHECK_EQ("line 1 flags", rd16(out + sect + 4 + 8 + 6), 60);
    /* gaps 5, 6, 7 are empty strings */
    for (i = 5; i < 8; i++) {
        BW_CHECK_EQ("gap len", decode_line(out, i, u), 0);
        BW_CHECK_EQ("gap flags", rd16(out + sect + 4 + 8 * i + 6), 60);
    }
    /* load order: the later mod's line 8 wins */
    n = decode_line(out, 8, u);
    BW_CHECK_EQ("line 8 len", n, 7);
    BW_CHECK_EQ("line 8 text", u[0] == 'E' && u[4] == 'T' && u[5] == 0xFFFE && u[6] == '!', 1);
    n = decode_line(out, 9, u);
    BW_CHECK_EQ("line 9 len", n, 3);
    BW_CHECK_EQ("line 9 text", u[0] == 0xF000 && u[1] == 0xBE01 && u[2] == 0, 1);

    /* swapped load order: mod 1's line 8 now wins */
    bw_msg_free(out);
    mods[0] = &m2;
    mods[1] = &m1;
    BW_CHECK_EQ("swap rc", bw_msg_merge(b.data, b.size, mods, 2, &out, &out_size), 0);
    n = decode_line(out, 8, u);
    BW_CHECK_EQ("swapped line 8 len", n, 5);
    BW_CHECK_EQ("swapped line 8 text", u[0] == 'e' && u[4] == 't', 1);
    bw_msg_free(out);

    /* no mods: the rebuilt bank equals the original bytes */
    BW_CHECK_EQ("none rc", bw_msg_merge(b.data, b.size, mods, 0, &out, &out_size), 0);
    BW_CHECK_EQ("none size", out_size, b.size);
    BW_CHECK_EQ("none bytes", out_size == b.size && memcmp(out, b.data, b.size) == 0, 1);
    bw_msg_free(out);

    /* duplicate index inside one file: the later line wins */
    bw_msg_lines_free(&m1);
    parse_ok("2 = first\n2 = second\n", &m1);
    mods[0] = &m1;
    BW_CHECK_EQ("dup rc", bw_msg_merge(b.data, b.size, mods, 1, &out, &out_size), 0);
    BW_CHECK_EQ("dup len", decode_line(out, 2, u), 6);
    bw_msg_free(out);

    /* a malformed bank is refused */
    b.data[0] = 2;
    BW_CHECK_EQ("bad bank", bw_msg_merge(b.data, b.size, mods, 1, &out, &out_size), -1);

    bw_msg_lines_free(&m1);
    bw_msg_lines_free(&m2);
    free(b.data);
    return 0;
}

/* ---- real data ---------------------------------------------------- */

static u8 *read_file(const char *path, u32 *size)
{
    FILE *f = fopen(path, "rb");
    u8 *buf;
    long n;

    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (u8 *)malloc((size_t)n + 1);
    if (buf == NULL || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    *size = (u32)n;
    return buf;
}

/* Locate member `idx` of a NARC image; returns 0 on success. */
static int narc_member(const u8 *n, u32 size, u32 idx, const u8 **p, u32 *len)
{
    u32 off = bw_dr_rd16(n + 12);
    const u8 *fat = NULL;
    u32 count = 0;

    while (off + 8 <= size) {
        u32 sz = bw_dr_rd32(n + off + 4);

        if (memcmp(n + off, "BTAF", 4) == 0) {
            fat = n + off;
            count = bw_dr_rd16(n + off + 8);
        } else if (memcmp(n + off, "GMIF", 4) == 0 && fat != NULL && idx < count) {
            u32 s = bw_dr_rd32(fat + 12 + 8 * idx);
            u32 e = bw_dr_rd32(fat + 16 + 8 * idx);

            *p = n + off + 8 + s;
            *len = e - s;
            return 0;
        }
        off += sz;
    }
    return -1;
}

static int test_merge_real(const char *root)
{
    char path[1024];
    u32 nsize, blen;
    u8 *narc;
    const u8 *bank;
    BwMsgLines l;
    const BwMsgLines *mods[1];
    u8 *out;
    u32 out_size;
    u16 a[256], b[256];
    u32 i, count;
    u32 sect_old, sect_new;

    snprintf(path, sizeof(path), "%s/a/0/0/2", root);
    narc = read_file(path, &nsize);
    if (narc == NULL) {
        printf("SKIP real data: cannot read %s\n", path);
        return 0;
    }
    BW_REQUIRE("member 70", narc_member(narc, nsize, 70, &bank, &blen) == 0);
    count = rd16(bank + 2);
    BW_CHECK_EQ("species count", count, 652);

    parse_ok("1 = Ivy\\nsaur{0100:1,2}\n700 = Newmon\n", &l);
    mods[0] = &l;
    BW_REQUIRE("merge", bw_msg_merge(bank, blen, mods, 1, &out, &out_size) == 0);
    BW_CHECK_EQ("new count", rd16(out + 2), 701);
    sect_old = rd32(bank + 12);
    sect_new = rd32(out + 12);

    for (i = 0; i < 701; i++) {
        u32 n = decode_line(out, i, a);

        if (i == 1) {
            BW_CHECK_EQ("line 1 len", n, 13);
            BW_CHECK_EQ("line 1 text", a[0] == 'I' && a[3] == 0xFFFE && a[4] == 's' &&
                        a[8] == 0xF000 && a[9] == 0x0100 && a[10] == 2 && a[11] == 1 &&
                        a[12] == 2, 1);
        } else if (i == 700) {
            BW_CHECK_EQ("line 700 len", n, 6);
            BW_CHECK_EQ("line 700 text", a[0] == 'N' && a[5] == 'n', 1);
        } else if (i < count) {
            const u8 *oe = bank + sect_old + 4 + 8 * i;
            const u8 *ne = out + sect_new + 4 + 8 * i;
            u32 on = decode_line(bank, i, b);

            BW_CHECK_EQ("kept len", n, on);
            BW_CHECK_EQ("kept text", memcmp(a, b, on * 2), 0);
            BW_CHECK_EQ("kept cipher", memcmp(bank + sect_old + rd32(oe),
                                              out + sect_new + rd32(ne),
                                              2 * rd16(oe + 4)), 0);
            BW_CHECK_EQ("kept flags", rd16(ne + 6), rd16(oe + 6));
        } else {
            BW_CHECK_EQ("gap empty", n, 0);
        }
    }
    bw_msg_free(out);
    bw_msg_lines_free(&l);
    free(narc);
    return 0;
}

int main(int argc, char **argv)
{
    test_grammar();
    test_merge_synthetic();
    if (argc > 1) {
        test_merge_real(argv[1]);
    } else {
        printf("SKIP real data: no data root given\n");
    }
    printf("%d checks, %d failures\n", bw_checks, bw_failures);
    return bw_failures != 0;
}
