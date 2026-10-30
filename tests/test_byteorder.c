/*
 * tests/test_byteorder.c -- install/byteorder.c on hand-built containers.
 *
 * A NARC and a message bank are written little-endian, as on the cartridge,
 * turned with bw_narc_to_host and bw_msg_to_host, and read back through the
 * data root's readers (sdk/types.h, bw_dr_rd16): every number must read as
 * written on either host, the four-byte tags and the bytes that are not
 * numbers must be untouched, text two entries share is turned once, and a
 * malformed container is refused.  On a little-endian host the turn
 * changes nothing and the same checks hold.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "install.h"
#include "port_test.h"

/* ---- a NARC: one root directory, three members ---------------------- */

static u32 make_narc(u8 *p)
{
    static const u32 len[3] = { 6, 0, 10 };
    u32 btaf = 16, btnf = btaf + 12 + 8 * 3, gmif = btnf + 16, pos = 0, i;

    memset(p, 0xEE, 256);
    memcpy(p, "NARC", 4);
    bw_wr16(p + 4, 0xFFFE);
    bw_wr16(p + 6, 0x0100);
    bw_wr16(p + 12, 16);
    bw_wr16(p + 14, 3);
    memcpy(p + btaf, "BTAF", 4);
    bw_wr32(p + btaf + 4, 12 + 8 * 3);
    bw_wr16(p + btaf + 8, 3);
    bw_wr16(p + btaf + 10, 0);
    for (i = 0; i < 3; i++) {
        bw_wr32(p + btaf + 12 + 8 * i, pos);
        bw_wr32(p + btaf + 16 + 8 * i, pos + len[i]);
        pos += len[i];
    }
    memcpy(p + btnf, "BTNF", 4);
    bw_wr32(p + btnf + 4, 16);
    bw_wr32(p + btnf + 8, 4);
    bw_wr16(p + btnf + 12, 0);
    bw_wr16(p + btnf + 14, 1);
    memcpy(p + gmif, "GMIF", 4);
    bw_wr32(p + gmif + 4, 8 + pos);
    for (i = 0; i < pos; i++) {
        p[gmif + 8 + i] = (u8)(0x40 + i);
    }
    bw_wr32(p + 8, gmif + 8 + pos);
    return gmif + 8 + pos;
}

static void test_narc(void)
{
    u8 p[256];
    u32 n = make_narc(p), btaf = 16, btnf = 16 + 12 + 24, gmif = btnf + 16;

    BW_CHECK_EQ("narc: turned", bw_narc_to_host(p, n), 1);
    BW_CHECK_EQ("narc: tag", memcmp(p, "NARC", 4), 0);
    BW_CHECK_EQ("narc: bom", bw_dr_rd16(p + 4), 0xFFFE);
    BW_CHECK_EQ("narc: size", bw_dr_rd32(p + 8), n);
    BW_CHECK_EQ("narc: header size", bw_dr_rd16(p + 12), 16);
    BW_CHECK_EQ("narc: sections", bw_dr_rd16(p + 14), 3);
    BW_CHECK_EQ("narc: BTAF tag", memcmp(p + btaf, "BTAF", 4), 0);
    BW_CHECK_EQ("narc: BTAF size", bw_dr_rd32(p + btaf + 4), 36);
    BW_CHECK_EQ("narc: count", bw_dr_rd16(p + btaf + 8), 3);
    BW_CHECK_EQ("narc: member 2 start", bw_dr_rd32(p + btaf + 28), 6);
    BW_CHECK_EQ("narc: member 2 end", bw_dr_rd32(p + btaf + 32), 16);
    BW_CHECK_EQ("narc: BTNF offset", bw_dr_rd32(p + btnf + 8), 4);
    BW_CHECK_EQ("narc: BTNF directories", bw_dr_rd16(p + btnf + 14), 1);
    BW_CHECK_EQ("narc: GMIF size", bw_dr_rd32(p + gmif + 4), 24);
    BW_CHECK_EQ("narc: members' bytes untouched", p[gmif + 8 + 15], 0x40 + 15);

    make_narc(p);
    memcpy(p, "RCNA", 4);
    BW_CHECK_EQ("narc: not a NARC", bw_narc_to_host(p, n), 0);
    memcpy(p, "NARC", 4);
    bw_wr32(p + btaf + 4, 0x1000);
    BW_CHECK_EQ("narc: a chunk past the end", bw_narc_to_host(p, n), -1);
}

/* ---- a message bank: one language, three entries ------------------- */

/* Entries 0 and 2 share their text, so it must be turned once. */
static u32 make_bank(u8 *p)
{
    u32 sec = 16, table = 4 + 8 * 3, i;

    memset(p, 0, 128);
    bw_wr16(p, 1);                       /* languages */
    bw_wr16(p + 2, 3);                   /* entries */
    bw_wr32(p + 4, table + 2 * 5);       /* the section's size */
    bw_wr32(p + 8, 0);
    bw_wr32(p + 12, sec);
    bw_wr32(p + sec, table + 2 * 5);
    bw_wr32(p + sec + 4, table);
    bw_wr16(p + sec + 8, 3);
    bw_wr16(p + sec + 10, 0x0102);
    bw_wr32(p + sec + 12, table + 6);
    bw_wr16(p + sec + 16, 2);
    bw_wr16(p + sec + 18, 0);
    bw_wr32(p + sec + 20, table);        /* entry 2: entry 0's text again */
    bw_wr16(p + sec + 24, 3);
    bw_wr16(p + sec + 26, 0);
    for (i = 0; i < 5; i++) {
        bw_wr16(p + sec + table + 2 * i, (u16)(0x1234 + 0x1111 * i));
    }
    return sec + table + 2 * 5;
}

static void test_msg(void)
{
    u8 p[128];
    u32 n = make_bank(p), sec = 16, table = 28, i;

    BW_CHECK_EQ("msg: turned", bw_msg_to_host(p, n), 1);
    BW_CHECK_EQ("msg: languages", bw_dr_rd16(p), 1);
    BW_CHECK_EQ("msg: entries", bw_dr_rd16(p + 2), 3);
    BW_CHECK_EQ("msg: size", bw_dr_rd32(p + 4), table + 10);
    BW_CHECK_EQ("msg: language offset", bw_dr_rd32(p + 12), sec);
    BW_CHECK_EQ("msg: section size", bw_dr_rd32(p + sec), table + 10);
    BW_CHECK_EQ("msg: entry 1 offset", bw_dr_rd32(p + sec + 12), table + 6);
    BW_CHECK_EQ("msg: entry 1 length", bw_dr_rd16(p + sec + 16), 2);
    BW_CHECK_EQ("msg: entry 0 flags", bw_dr_rd16(p + sec + 10), 0x0102);
    for (i = 0; i < 5; i++) {
        BW_CHECK_EQ("msg: a unit (shared text turned once)",
                    bw_dr_rd16(p + sec + table + 2 * i), 0x1234 + 0x1111 * i);
    }

    BW_CHECK_EQ("msg: an empty member", bw_msg_to_host(p, 0), 0);
    make_bank(p);
    bw_wr32(p + sec + 4, table + 9);     /* odd: text is u16 units */
    BW_CHECK_EQ("msg: odd text offset", bw_msg_to_host(p, n), -1);
    make_bank(p);
    bw_wr16(p + sec + 8, 40);            /* text past the section */
    BW_CHECK_EQ("msg: text past the section", bw_msg_to_host(p, n), -1);
    make_bank(p);
    bw_wr16(p + 2, 400);                 /* entry table past the section */
    BW_CHECK_EQ("msg: table past the section", bw_msg_to_host(p, n), -1);
    make_bank(p);
    bw_wr32(p + 12, 200);                /* language past the file */
    BW_CHECK_EQ("msg: language past the file", bw_msg_to_host(p, n), -1);
}

int main(void)
{
    test_narc();
    test_msg();
    BW_REPORT("test_byteorder");
}
