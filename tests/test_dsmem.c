/*
 * tests/test_dsmem.c -- the DS address space (sdk/dsmem.c).
 *
 * Every expected value is the hardware rule written out by hand: which bank
 * GBATEK's VRAMCNT table puts at an address, which window mirrors where,
 * which hook runs with which old and new value.  No game data is needed.
 *
 * `test_dsmem --overlap` is a second test (test_dsmem_overlap in
 * CMakeLists.txt): it registers two overlapping ranges, which must panic,
 * and passes only if the panic message appears.
 */

#include <stdio.h>
#include <string.h>

#include "port.h"
#include "dsmem.h"
#include "port_test.h"

/* A game global sdk/game_globals.c registers at its DS address. */
extern s32 g_020A7820;

#define PTR_EQ(label, got, want) \
    BW_REQUIRE(label, (const void *)(got) == (const void *)(want))

static u8 *bank(int b)
{
    return bw_vram_bank(b, NULL);
}

static u32 bank_size(int b)
{
    u32 size = 0;

    return bw_vram_bank(b, &size) != NULL ? size : 0;
}

/* Store a VRAMCNT byte the way matched code does: through bw_ds_ptr, with
 * no synchronisation point after it. */
static void vramcnt(int b, u8 v)
{
    static const u32 reg[BW_VRAM_BANKS] = {
        0x04000240, 0x04000241, 0x04000242, 0x04000243, 0x04000244,
        0x04000245, 0x04000246, 0x04000248, 0x04000249
    };
    *(volatile u8 *)bw_ds_ptr_w(reg[b], 1) = v;
}

static void vram_all_off(void)
{
    int b;

    for (b = 0; b < BW_VRAM_BANKS; b++) {
        vramcnt(b, 0);
    }
}

/* ------------------------------------------------------------------ */

static int test_regions(void)
{
    u8 *p;
    u8 *q;

    /* main RAM: flat, no NTR mirroring, contiguous across 64 KiB pieces */
    p = (u8 *)bw_ds_ptr(0x02300000u);
    p[0] = 0x5A;
    BW_CHECK_EQ("main read back", *(u8 *)bw_ds_ptr(0x02300000u), 0x5A);
    BW_CHECK_EQ("main other byte zero", *(u8 *)bw_ds_ptr(0x02300001u), 0);
    p = (u8 *)bw_ds_ptr(0x027FF000u);
    q = (u8 *)bw_ds_ptr(0x02FFF000u);
    BW_REQUIRE("main 027FF000 != 02FFF000", p != q);
    p = (u8 *)bw_ds_ptr(0x0230FFFCu);
    memset(p, 0xA5, 8);   /* straddles a piece boundary */
    PTR_EQ("main contiguous over a piece", bw_ds_ptr(0x02310000u), p + 4);
    BW_CHECK_EQ("main straddle byte", *(u8 *)bw_ds_ptr(0x02310003u), 0xA5);
    PTR_EQ("main last byte", bw_ds_ptr(0x02FFFFFFu),
           (u8 *)bw_ds_ptr(0x02000000u) + 0xFFFFFF);
    BW_CHECK_EQ("main addr_of", bw_ds_addr_of(p + 1), 0x0230FFFD);

    /* shared WRAM, 256 KiB mirrored over 0x03xxxxxx */
    p = (u8 *)bw_ds_ptr(0x03000010u);
    PTR_EQ("wram mirror 1", bw_ds_ptr(0x03040010u), p);
    PTR_EQ("wram mirror 2", bw_ds_ptr(0x03FC0010u), p);
    BW_REQUIRE("wram distinct", bw_ds_ptr(0x03000011u) == p + 1);
    BW_CHECK_EQ("wram addr_of", bw_ds_addr_of(p), 0x03000010);

    /* palette and OAM, 2 KiB each, mirrored */
    p = (u8 *)bw_ds_ptr(0x05000200u);
    PTR_EQ("pal mirror", bw_ds_ptr(0x05000A00u), p);
    BW_CHECK_EQ("pal addr_of", bw_ds_addr_of(p), 0x05000200);
    q = (u8 *)bw_ds_ptr(0x07000200u);
    PTR_EQ("oam mirror", bw_ds_ptr(0x07001200u), q);
    BW_REQUIRE("pal != oam", p != q);
    BW_CHECK_EQ("oam addr_of", bw_ds_addr_of(q + 6), 0x07000206);

    /* I/O */
    PTR_EQ("io ptr == bw_io", bw_ds_ptr_w(0x04000208u, 1), bw_io(0x04000208u));
    *(volatile u16 *)bw_ds_ptr_w(0x04000208u, sizeof(*(volatile u16 *)0)) = 1;
    BW_CHECK_EQ("io literal store read16", bw_io_read16(0x04000208u), 1);
    BW_CHECK_EQ("KEYINPUT released", bw_io_read16(0x04000130u), 0x03FF);
    BW_REQUIRE("io ipc fifo backed", bw_ds_try_ptr(0x04100000u) != NULL);
    BW_REQUIRE("io card data backed", bw_ds_try_ptr(0x04100010u) != NULL);
    BW_REQUIRE("io past window", bw_ds_try_ptr(0x04010000u) == NULL);
    BW_CHECK_EQ("io addr_of", bw_ds_addr_of(bw_ds_ptr(0x04001008u)), 0x04001008);
    BW_CHECK_EQ("io hi addr_of", bw_ds_addr_of(bw_ds_ptr(0x04100004u)),
                0x04100004);

    /* nothing else is backed */
    BW_REQUIRE("unbacked 0x01", bw_ds_try_ptr(0x01FF8000u) == NULL);
    BW_REQUIRE("unbacked 0x08", bw_ds_try_ptr(0x08000000u) == NULL);
    BW_REQUIRE("unbacked 0xFF", bw_ds_try_ptr(0xFFFF0000u) == NULL);
    BW_CHECK_EQ("addr_of foreign", bw_ds_addr_of(&p), 0);
    BW_CHECK_EQ("addr_of NULL", bw_ds_addr_of(NULL), 0);
    return 0;
}

/* ------------------------------------------------------------------ */

static u8 g_objs[64][8];

static int test_registry(void)
{
    static u8 obj[16];
    static u8 low[4];
    int i;

    bw_ds_register(0x02300100u, obj, sizeof(obj), "obj");
    PTR_EQ("reg base", bw_ds_ptr(0x02300100u), obj);
    PTR_EQ("reg offset", bw_ds_ptr(0x0230010Fu), obj + 15);
    BW_REQUIRE("reg end falls through",
               bw_ds_ptr(0x02300110u) != (void *)(obj + 16));
    BW_REQUIRE("reg before falls through", bw_ds_ptr(0x023000FFu) != NULL);
    BW_CHECK_EQ("reg addr_of", bw_ds_addr_of(obj + 3), 0x02300103);

    /* adjacent ranges are not an overlap */
    bw_ds_register(0x023000FCu, low, sizeof(low), "low");
    PTR_EQ("reg adjacent below", bw_ds_ptr(0x023000FFu), low + 3);
    PTR_EQ("reg adjacent above", bw_ds_ptr(0x02300100u), obj);

    /* many entries, registered out of order: the search stays exact */
    for (i = 63; i >= 0; i -= 2) {
        bw_ds_register(0x02400000u + (u32)i * 0x10u, g_objs[i], 8, "even");
    }
    for (i = 62; i >= 0; i -= 2) {
        bw_ds_register(0x02400000u + (u32)i * 0x10u, g_objs[i], 8, "odd");
    }
    for (i = 0; i < 64; i++) {
        u32 a = 0x02400000u + (u32)i * 0x10u;
        if (bw_ds_ptr(a + 5u) != (void *)(g_objs[i] + 5)
            || bw_ds_ptr(a + 8u) == (void *)(g_objs[i] + 8)
            || bw_ds_addr_of(g_objs[i] + 7) != a + 7u) {
            printf("  FAIL registry entry %d\n", i);
            bw_failures++;
        }
        bw_checks++;
    }

    /* registration outside main RAM works too */
    bw_ds_register(0x01FF8000u, low, sizeof(low), "itcm");
    PTR_EQ("reg itcm", bw_ds_ptr(0x01FF8002u), low + 2);

    /* a game global: literal cast and extern name are the same bytes */
    PTR_EQ("game global", bw_ds_ptr(0x020A7820u), &g_020A7820);
    BW_CHECK_EQ("game global value", *(s32 *)bw_ds_ptr(0x020A7820u), -1);
    BW_CHECK_EQ("game global addr_of", bw_ds_addr_of(&g_020A7820), 0x020A7820);

    /* an overlay's objects come and go: withdraw one, the region answers */
    BW_REQUIRE("registered covers", bw_ds_registered(0x02300108u, 4) != 0);
    BW_REQUIRE("registered straddles", bw_ds_registered(0x023000F0u, 0x20) != 0);
    BW_REQUIRE("registered misses", bw_ds_registered(0x02300110u, 0x10) == 0);
    BW_REQUIRE("unregister", bw_ds_unregister(0x02300100u) == 1);
    BW_REQUIRE("unregister twice", bw_ds_unregister(0x02300100u) == 0);
    BW_REQUIRE("unregistered falls through",
               bw_ds_ptr(0x02300100u) != (void *)obj);
    BW_REQUIRE("unregistered gone", bw_ds_registered(0x02300100u, 16) == 0);
    PTR_EQ("neighbour kept", bw_ds_ptr(0x023000FFu), low + 3);
    bw_ds_register(0x02300100u, obj, sizeof(obj), "obj again");
    PTR_EQ("registered again", bw_ds_ptr(0x02300104u), obj + 4);

    /* identity mapping: where it is on, main RAM is at its own address */
    if (bw_ds_identity()) {
        PTR_EQ("identity main", bw_ds_ptr(0x02200000u), (void *)(uintptr_t)0x02200000u);
        BW_CHECK_EQ("identity addr_of", bw_ds_addr_of((void *)(uintptr_t)0x02200010u),
                    0x02200010);
    } else {
        BW_REQUIRE("movable main", bw_ds_ptr(0x02200000u) != NULL);
    }

    /* reset forgets the test's registrations, keeps the game's */
    bw_ds_reset();
    BW_REQUIRE("reset drops obj", bw_ds_ptr(0x02300100u) != (void *)obj);
    BW_REQUIRE("reset drops itcm", bw_ds_try_ptr(0x01FF8000u) == NULL);
    PTR_EQ("reset keeps game global", bw_ds_ptr(0x020A7820u), &g_020A7820);
    return 0;
}

/* ------------------------------------------------------------------ */

static int g_hook_runs;
static u32 g_hook_addr;
static u32 g_hook_old;
static u32 g_hook_new;

static void record_hook(u32 addr, u32 old_value, u32 new_value)
{
    g_hook_runs++;
    g_hook_addr = addr;
    g_hook_old = old_value;
    g_hook_new = new_value;
}

/* IF is write-one-to-clear: the bits written are the bits removed. */
static void if_hook(u32 addr, u32 old_value, u32 new_value)
{
    volatile u8 *r = bw_io(addr);
    u32 v = old_value & ~new_value;

    g_hook_runs++;
    r[0] = (u8)v;
    r[1] = (u8)(v >> 8);
    r[2] = (u8)(v >> 16);
    r[3] = (u8)(v >> 24);
}

static int test_hooks(void)
{
    /* immediate hooks from the port's writers */
    bw_io_hook(0x04000208u, 2, record_hook);
    g_hook_runs = 0;
    bw_io_write16(0x04000208u, 1);
    BW_CHECK_EQ("write16 hook ran", g_hook_runs, 1);
    BW_CHECK_EQ("write16 hook addr", g_hook_addr, 0x04000208);
    BW_CHECK_EQ("write16 hook old", g_hook_old, 0);
    BW_CHECK_EQ("write16 hook new", g_hook_new, 1);
    bw_io_write16(0x04000208u, 1);
    BW_CHECK_EQ("write16 same value still runs", g_hook_runs, 2);
    bw_io_write32(0x04000208u, 0x00010000u);   /* covers 0x208 too */
    BW_CHECK_EQ("write32 over hook runs", g_hook_runs, 3);
    BW_CHECK_EQ("write32 over hook new", g_hook_new, 0);
    bw_io_sync();
    BW_CHECK_EQ("sync after writes is quiet", g_hook_runs, 3);

    /* matched-code stores are seen at sync, once, with old and new */
    *(volatile u16 *)bw_ds_ptr_w(0x04000208u, sizeof(*(volatile u16 *)0)) = 1;
    BW_CHECK_EQ("store alone runs nothing", g_hook_runs, 3);
    bw_io_sync();
    BW_CHECK_EQ("sync ran hook", g_hook_runs, 4);
    BW_CHECK_EQ("sync hook old", g_hook_old, 0);
    BW_CHECK_EQ("sync hook new", g_hook_new, 1);
    bw_io_sync();
    BW_CHECK_EQ("second sync quiet", g_hook_runs, 4);
    *(volatile u16 *)bw_ds_ptr_w(0x0400020Au, sizeof(*(volatile u16 *)0)) = 7;   /* unhooked neighbour */
    bw_io_sync();
    BW_CHECK_EQ("unhooked change quiet", g_hook_runs, 4);

    /* a hook may change the register it fired for */
    bw_io_hook(0x04000214u, 4, if_hook);
    *(volatile u32 *)bw_ds_ptr(0x04000214u) = 5;   /* as the IRQ source would */
    bw_io_sync();                                  /* if_hook: 0 & ~5 = 0 */
    *(volatile u32 *)bw_ds_ptr(0x04000214u) = 5;
    bw_io_hook(0x04000214u, 4, if_hook);           /* re-hook: now is the base */
    g_hook_runs = 0;
    bw_io_write32(0x04000214u, 1);                 /* acknowledge bit 0 */
    BW_CHECK_EQ("IF hook ran", g_hook_runs, 1);
    BW_CHECK_EQ("IF write-one-to-clear", bw_io_read32(0x04000214u), 4);
    bw_io_sync();
    BW_CHECK_EQ("IF sync quiet after hook", g_hook_runs, 1);

    /* removal */
    bw_io_hook(0x04000208u, 2, NULL);
    g_hook_runs = 0;
    bw_io_write16(0x04000208u, 9);
    BW_CHECK_EQ("removed hook silent", g_hook_runs, 0);
    BW_CHECK_EQ("value still stored", bw_io_read16(0x04000208u), 9);

    /* the VRAMCNT hooks bw_ds_reset installed remap on a port write */
    vram_all_off();
    bw_io_sync();
    bw_io_write16(0x04000240u, 0x8980u);   /* A LCDC, B main BG OFS 1 */
    PTR_EQ("hook remap A", bw_ds_ptr(0x06800000u), bank(BW_VRAM_A));
    PTR_EQ("hook remap B", bw_ds_ptr(0x06020000u), bank(BW_VRAM_B));
    return 0;
}

/* ------------------------------------------------------------------ */

static int test_vram(void)
{
    u32 size;
    u32 contig;
    u8 *p;

    BW_CHECK_EQ("bank A size", bank_size(BW_VRAM_A), 0x20000);
    BW_CHECK_EQ("bank E size", bank_size(BW_VRAM_E), 0x10000);
    BW_CHECK_EQ("bank F size", bank_size(BW_VRAM_F), 0x4000);
    BW_CHECK_EQ("bank H size", bank_size(BW_VRAM_H), 0x8000);
    BW_CHECK_EQ("bank I size", bank_size(BW_VRAM_I), 0x4000);
    BW_REQUIRE("bank out of range", bw_vram_bank(BW_VRAM_BANKS, &size) == NULL);

    vram_all_off();
    BW_REQUIRE("off: BG unmapped", bw_ds_try_ptr(0x06000000u) == NULL);
    BW_REQUIRE("off: LCDC unmapped", bw_ds_try_ptr(0x06800000u) == NULL);

    /* LCDC: every bank at its fixed address, in A..I order */
    vramcnt(BW_VRAM_A, 0x80);
    vramcnt(BW_VRAM_G, 0x80);
    vramcnt(BW_VRAM_I, 0x80);
    PTR_EQ("LCDC A", bw_ds_ptr(0x06800000u), bank(BW_VRAM_A));
    PTR_EQ("LCDC A end", bw_ds_ptr(0x0681FFFFu), bank(BW_VRAM_A) + 0x1FFFF);
    BW_REQUIRE("LCDC B off", bw_ds_try_ptr(0x06820000u) == NULL);
    PTR_EQ("LCDC G", bw_ds_ptr(0x06894000u), bank(BW_VRAM_G));
    PTR_EQ("LCDC I", bw_ds_ptr(0x068A0000u), bank(BW_VRAM_I));
    BW_REQUIRE("LCDC past I", bw_ds_try_ptr(0x068A4000u) == NULL);
    BW_REQUIRE("enable bit off", (vramcnt(BW_VRAM_A, 0x00),
               bw_ds_try_ptr(0x06800000u) == NULL));

    /* main BG: A then B contiguous, mirrored every 512 KiB */
    vram_all_off();
    vramcnt(BW_VRAM_A, 0x81);
    vramcnt(BW_VRAM_B, 0x89);
    p = (u8 *)bw_ds_ptr(0x06000000u);
    PTR_EQ("BG A", p, bank(BW_VRAM_A));
    PTR_EQ("BG B", bw_ds_ptr(0x06020000u), bank(BW_VRAM_B));
    PTR_EQ("BG A..B contiguous", bw_ds_ptr(0x06020000u), p + 0x20000);
    PTR_EQ("BG mirror", bw_ds_ptr(0x060A0010u), bank(BW_VRAM_B) + 0x10);
    PTR_EQ("BG mirror 3", bw_ds_ptr(0x06180000u), bank(BW_VRAM_A));
    BW_REQUIRE("BG gap", bw_ds_try_ptr(0x06040000u) == NULL);
    BW_CHECK_EQ("BG addr_of", bw_ds_addr_of(bank(BW_VRAM_B) + 4), 0x06020004);

    /* A with OFS 1: the 0x06000000 page is empty */
    vram_all_off();
    vramcnt(BW_VRAM_A, 0x89);
    BW_REQUIRE("A OFS1 base empty", bw_ds_try_ptr(0x06000000u) == NULL);
    PTR_EQ("A OFS1", bw_ds_ptr(0x06020000u), bank(BW_VRAM_A));

    /* main OBJ: E at 0x06400000, 64 KiB, mirrored every 256 KiB; G with
     * OFS 1 at +0x4000; A as OBJ with OFS 1 at +0x20000 */
    vram_all_off();
    vramcnt(BW_VRAM_E, 0x82);
    vramcnt(BW_VRAM_G, 0x8A);
    PTR_EQ("OBJ E", bw_ds_ptr(0x06400000u), bank(BW_VRAM_E));
    PTR_EQ("OBJ E mirror", bw_ds_ptr(0x06440000u), bank(BW_VRAM_E));
    BW_REQUIRE("OBJ G under E", bw_ds_ptr(0x06404000u) == bank(BW_VRAM_E) + 0x4000);
    vramcnt(BW_VRAM_E, 0);
    PTR_EQ("OBJ G OFS1", bw_ds_ptr(0x06404000u), bank(BW_VRAM_G));
    vramcnt(BW_VRAM_A, 0x8A);
    PTR_EQ("OBJ A OFS1", bw_ds_ptr(0x06420000u), bank(BW_VRAM_A));

    /* F in main BG with OFS 3: 0x06000000 + 0x4000 + 0x10000 */
    vram_all_off();
    vramcnt(BW_VRAM_F, 0x99);
    PTR_EQ("BG F OFS3", bw_ds_ptr(0x06014000u), bank(BW_VRAM_F));
    BW_REQUIRE("BG F OFS3 only", bw_ds_try_ptr(0x06010000u) == NULL);

    /* sub engines: C sub BG, H sub BG, I sub BG at +0x8000, D sub OBJ */
    vram_all_off();
    vramcnt(BW_VRAM_H, 0x81);
    vramcnt(BW_VRAM_I, 0x81);
    vramcnt(BW_VRAM_D, 0x84);
    PTR_EQ("sub BG H", bw_ds_ptr(0x06200000u), bank(BW_VRAM_H));
    PTR_EQ("sub BG I", bw_ds_ptr(0x06208000u), bank(BW_VRAM_I));
    PTR_EQ("sub BG H..I contiguous", bw_ds_ptr(0x06208000u),
           bank(BW_VRAM_H) + 0x8000);
    PTR_EQ("sub BG mirror", bw_ds_ptr(0x06220000u), bank(BW_VRAM_H));
    PTR_EQ("sub OBJ D", bw_ds_ptr(0x06600000u), bank(BW_VRAM_D));
    PTR_EQ("sub OBJ mirror", bw_ds_ptr(0x06620000u), bank(BW_VRAM_D));
    vram_all_off();
    vramcnt(BW_VRAM_C, 0x84);
    vramcnt(BW_VRAM_I, 0x82);
    PTR_EQ("sub BG C", bw_ds_ptr(0x06200000u), bank(BW_VRAM_C));
    PTR_EQ("sub OBJ I", bw_ds_ptr(0x06600000u), bank(BW_VRAM_I));

    /* overlap: A and C both main BG at 0x06000000, A owns the page */
    vram_all_off();
    vramcnt(BW_VRAM_A, 0x81);
    vramcnt(BW_VRAM_C, 0x81);
    PTR_EQ("overlap first bank wins", bw_ds_ptr(0x06000000u), bank(BW_VRAM_A));

    /* engine-only spaces: invisible to the CPU, visible to the renderer */
    vram_all_off();
    vramcnt(BW_VRAM_A, 0x8B);   /* texture slot 1 */
    vramcnt(BW_VRAM_C, 0x8A);   /* ARM7 slot 1 */
    vramcnt(BW_VRAM_E, 0x84);   /* main BG ext palette slots 0-3 */
    vramcnt(BW_VRAM_F, 0x85);   /* main OBJ ext palette */
    vramcnt(BW_VRAM_G, 0x93);   /* texture palette slot 4 */
    vramcnt(BW_VRAM_H, 0x82);   /* sub BG ext palette */
    vramcnt(BW_VRAM_I, 0x83);   /* sub OBJ ext palette */
    BW_REQUIRE("tex not CPU", bw_ds_try_ptr(0x06000000u) == NULL);
    PTR_EQ("tex slot 1", bw_vram_slot(BW_VSLOT_TEX, 0x20000, &contig),
           bank(BW_VRAM_A));
    BW_CHECK_EQ("tex slot 1 contig", contig, 0x20000);
    BW_REQUIRE("tex slot 0 empty", bw_vram_slot(BW_VSLOT_TEX, 0, NULL) == NULL);
    PTR_EQ("arm7 slot 1", bw_vram_slot(BW_VSLOT_ARM7, 0x20004, NULL),
           bank(BW_VRAM_C) + 4);
    PTR_EQ("bgext main", bw_vram_slot(BW_VSLOT_BGEXT_MAIN, 0x6000, &contig),
           bank(BW_VRAM_E) + 0x6000);
    BW_CHECK_EQ("bgext main contig", contig, 0x2000);
    PTR_EQ("objext main", bw_vram_slot(BW_VSLOT_OBJEXT_MAIN, 0, NULL),
           bank(BW_VRAM_F));
    PTR_EQ("texpal slot 4", bw_vram_slot(BW_VSLOT_TEXPAL, 0x10000, NULL),
           bank(BW_VRAM_G));
    PTR_EQ("bgext sub", bw_vram_slot(BW_VSLOT_BGEXT_SUB, 0x4000, NULL),
           bank(BW_VRAM_H) + 0x4000);
    PTR_EQ("objext sub", bw_vram_slot(BW_VSLOT_OBJEXT_SUB, 0x1FFF, NULL),
           bank(BW_VRAM_I) + 0x1FFF);
    BW_REQUIRE("slot out of range",
               bw_vram_slot(BW_VSLOT_OBJEXT_SUB, 0x2000, NULL) == NULL);
    BW_CHECK_EQ("addr_of unmapped bank", bw_ds_addr_of(bank(BW_VRAM_A)), 0);

    vram_all_off();
    vramcnt(BW_VRAM_E, 0x83);   /* texture palette slots 0-3 */
    vramcnt(BW_VRAM_F, 0x8C);   /* main BG ext palette slots 2-3 */
    PTR_EQ("texpal E", bw_vram_slot(BW_VSLOT_TEXPAL, 0xC000, NULL),
           bank(BW_VRAM_E) + 0xC000);
    PTR_EQ("bgext F OFS1", bw_vram_slot(BW_VSLOT_BGEXT_MAIN, 0x4000, NULL),
           bank(BW_VRAM_F));

    /* data written through one mapping is there through another */
    vram_all_off();
    vramcnt(BW_VRAM_B, 0x81);
    *(volatile u16 *)bw_ds_ptr(0x06000100u) = 0x7C1F;
    vramcnt(BW_VRAM_B, 0x80);
    BW_CHECK_EQ("bank keeps data across remap",
                *(volatile u16 *)bw_ds_ptr(0x06820100u), 0x7C1F);
    return 0;
}

/* ------------------------------------------------------------------ */

static int test_reset(void)
{
    *(u8 *)bw_ds_ptr(0x02200000u) = 1;
    *(u8 *)bw_ds_ptr(0x03000000u) = 1;
    *(u8 *)bw_ds_ptr(0x05000000u) = 1;
    *(u8 *)bw_ds_ptr(0x07000000u) = 1;
    *(volatile u8 *)bw_ds_ptr_w(0x04000240u, sizeof(*(volatile u8 *)0)) = 0x80;
    *(u8 *)bw_ds_ptr(0x06800000u) = 1;
    bw_ds_reset();
    BW_CHECK_EQ("reset main", *(u8 *)bw_ds_ptr(0x02200000u), 0);
    BW_CHECK_EQ("reset wram", *(u8 *)bw_ds_ptr(0x03000000u), 0);
    BW_CHECK_EQ("reset pal", *(u8 *)bw_ds_ptr(0x05000000u), 0);
    BW_CHECK_EQ("reset oam", *(u8 *)bw_ds_ptr(0x07000000u), 0);
    BW_CHECK_EQ("reset VRAMCNT", bw_io_read16(0x04000240u), 0);
    BW_REQUIRE("reset unmaps VRAM", bw_ds_try_ptr(0x06800000u) == NULL);
    BW_CHECK_EQ("reset bank data", bank(BW_VRAM_A)[0], 0);
    return 0;
}

/*
 * The contracts other layers build on (sdk/port_reset.c): bw_ds_reset
 * leaves IF unhooked, because the OS layer owns it, and bw_io_write32
 * updates the copy bw_io_sync compares against, so a port write is never
 * seen a second time as a matched store.
 */
static int test_contracts(void)
{
    bw_ds_reset();

    /* No IF hook: a store to IF is plain memory until the OS layer hooks it. */
    *(volatile u32 *)bw_ds_ptr(0x04000214u) = 5;
    bw_io_sync();
    BW_CHECK_EQ("reset leaves IF unhooked", bw_io_read32(0x04000214u), 5);
    bw_io_write32(0x04000214u, 1);
    BW_CHECK_EQ("IF write is a plain store", bw_io_read32(0x04000214u), 1);

    /* bw_io_write32 on a four-byte hooked register: one run, sync quiet. */
    bw_io_hook(0x04000188u, 4, record_hook);
    g_hook_runs = 0;
    bw_io_write32(0x04000188u, 0x12345678u);
    BW_CHECK_EQ("write32 hook ran once", g_hook_runs, 1);
    BW_CHECK_EQ("write32 hook new", g_hook_new, 0x12345678u);
    bw_io_sync();
    BW_CHECK_EQ("write32 updated the sync copy", g_hook_runs, 1);
    *(volatile u32 *)bw_ds_ptr(0x04000188u) = 0x12345678u;
    bw_io_sync();
    BW_CHECK_EQ("same value stored again is invisible", g_hook_runs, 1);
    *(volatile u32 *)bw_ds_ptr(0x04000188u) = 0x9ABCDEF0u;
    bw_io_sync();
    BW_CHECK_EQ("a changed store runs at sync", g_hook_runs, 2);
    BW_CHECK_EQ("sync hook old is the write32 value", g_hook_old, 0x12345678u);

    /* An unhooked register written by bw_io_write32, hooked afterwards:
     * the write is not replayed at the next sync.                           */
    bw_io_write32(0x04000180u, 0x0300u);
    bw_io_hook(0x04000180u, 4, record_hook);
    bw_io_sync();
    BW_CHECK_EQ("no replay of an earlier write32", g_hook_runs, 2);
    bw_io_hook(0x04000180u, 4, NULL);
    bw_io_hook(0x04000188u, 4, NULL);
    return 0;
}

static int run_overlap(void)
{
    static u8 a[0x20];
    static u8 b[4];

    bw_ds_reset();
    bw_ds_register(0x02300000u, a, sizeof(a), "a");
    bw_ds_register(0x02300010u, b, sizeof(b), "b");   /* must panic */
    printf("overlap was not detected\n");
    return 1;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--overlap") == 0) {
        return run_overlap();
    }

    BW_REQUIRE("no translation before reset",
               bw_ds_try_ptr(0x04000000u) == NULL);
    bw_ds_reset();

    if (test_regions() != 0 || test_registry() != 0 || test_hooks() != 0
        || test_vram() != 0 || test_reset() != 0 || test_contracts() != 0) {
        BW_REPORT("test_dsmem");
    }
    BW_REPORT("test_dsmem");
}
