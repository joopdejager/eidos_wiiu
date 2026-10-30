/*
 * tests/test_ovlink.c -- overlays compiled into one executable
 * (tools/ovlink.py, sdk/overlay_image.c; docs/overlays.md), against the
 * synthetic aletheia tree in tests/ovl_synth/.  No game data: the overlay
 * files are byte patterns made here.
 *
 * The tree has two overlays, 0 and 1, sharing the window 0x02200000 and
 * both defining func_02200000 and a table at 0x02200100, a third overlay at
 * 0x02300000 that calls into the shared window, and an ARM9 function that
 * does the same.  What is checked:
 *
 *   - both overlays link side by side (ov000_func_02200000 and
 *     ov001_func_02200000), and a call inside an overlay stays in it;
 *   - the ARM9 caller and overlay 2 reach whichever of 0 and 1 is resident,
 *     through the generated trampoline, loading them alternately;
 *   - each load copies the overlay's file into the window (the table word
 *     each overlay's function reads), zeroes the .bss (a counter starts over
 *     on every load) and, where pointers are four bytes, relocates the
 *     listed pointer words: to the overlay's own Thumb function, to its own
 *     data, to an ARM9 function;
 *   - a call with no declaration in scope (C89 implicit int) links, and so
 *     does a file carrying a second copy of a function another file owns;
 *   - a Thumb function's address written as a literal is that function;
 *   - a static initialiser list (the shape of ov230's, the only one in the
 *     game) runs at load, in order;
 *
 * and, through tests/expect_panic.cmake:
 *
 *   test_ovlink --stub   reaching a function no source defines panics with
 *                        the overlay and the address;
 *   test_ovlink --none   calling into the window with nothing resident
 *                        panics naming the address.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "port.h"
#include "dsmem.h"
#include "armdata.h"
#include "overlay.h"
#include "overlay_image.h"
#include "port_test.h"

/* sdk/overlay.c's SDK names. */
extern BOOL FS_LoadOverlay(u32 target, u32 id);
extern BOOL FS_UnloadOverlay(u32 target, u32 id);

/* The set cmake/overlays.cmake built from tests/ovl_synth/. */
extern const BwOvlImageSet bw_ovl_set_synth;

/* The renamed functions, and the plain names (ARM9's own, a trampoline). */
int   ov000_func_02200000(int x);
int   ov000_func_02200010(int x);
void  ov000_func_02200020(void);
int   ov000_func_02200040(void);
int   ov000_func_02200050(int x);
int   ov001_func_02200000(int x);
int   ov001_func_02200010(int x);
void *ov001_func_02200040(void);
int   ov002_func_02300000(int x);
int   func_02100000(int x);
int   func_02200000(int x);

#define WIN     0x02200000u
#define WIN_B   0x02300000u

#define WIN_SI  0x02188400u    /* ov230's: its five initialisers' addresses */

/* The registry indexes by id, so the table runs to 230; the ids between
 * are empty windows. */
static BwOverlayDesc  k_descs[231];
static BwOverlayTable k_table = { k_descs, 231, NULL, 0, NULL, 0 };

static void desc(u32 id, u32 ram, u32 code, u32 bss)
{
    BwOverlayDesc *d = &k_descs[id];

    memset(d, 0, sizeof(*d));
    d->id = id;
    d->ram_address = ram;
    d->ram_size = code;
    d->bss_size = bss;
    d->file_id = id;
}

static u8 g_file0[0x200];
static u8 g_file1[0x200];
static u8 g_file2[0x100];
static u8 g_file230[0x1600];

static void put32(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

/* A word as the game reads it: the loaded overlay's data is in the host's
 * order (sdk/armdata.c, swap_to_host), the files the test writes are the
 * DS's (put32). */
static u32 ds_word(u32 addr)
{
    return bw_mem_rd32((const u8 *)bw_ds_ptr(addr));
}

static void setup(void)
{
    u32 i;

    memset(g_file0, 0xA5, sizeof(g_file0));      /* code bytes: anything */
    memset(g_file1, 0x5A, sizeof(g_file1));
    memset(g_file2, 0x3C, sizeof(g_file2));
    put32(g_file0 + 0x100, 7);                   /* ov000's table          */
    put32(g_file0 + 0x180, WIN + 0x11u);         /* -> func_02200010 thumb */
    put32(g_file0 + 0x184, WIN + 0x100u);        /* -> its own table       */
    put32(g_file0 + 0x188, 0x02100000u);         /* -> ARM9 func_02100000  */
    put32(g_file1 + 0x100, 3);                   /* ov001's table          */
    put32(g_file1 + 0x180, WIN + 0x01u);         /* -> func_02200000 thumb */

    memset(g_file230, 0x77, sizeof(g_file230));
    for (i = 0; i < 231; i++) {
        desc(i, 0, 0, 0);
    }
    desc(0, WIN, 0x200u, 0x20u);
    desc(1, WIN, 0x200u, 0x20u);
    desc(2, WIN_B, 0x100u, 0);
    desc(230, WIN_SI, 0x1600u, 0x20u);

    bw_ds_reset();
    bw_overlay_use_table(&k_table);
    bw_overlay_sdk_init();
    bw_ovl_set_file(0, g_file0, sizeof(g_file0));
    bw_ovl_set_file(1, g_file1, sizeof(g_file1));
    bw_ovl_set_file(2, g_file2, sizeof(g_file2));
    bw_ovl_set_file(230, g_file230, sizeof(g_file230));
    bw_ovl_install(&bw_ovl_set_synth, BW_DATA_ALLOW_UNRELOCATED);
}

int main(int argc, char **argv)
{
    int reloc = bw_data_relocation_supported();

    setup();

    if (argc > 1 && strcmp(argv[1], "--stub") == 0) {
        FS_LoadOverlay(0, 0);
        ov000_func_02200020();                   /* calls the stub 0x02200030 */
        printf("returned from a stub\n");
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--none") == 0) {
        FS_LoadOverlay(0, 0);
        FS_UnloadOverlay(0, 0);
        (void)func_02100000(1);                  /* nothing resident at 0x02200000 */
        printf("returned from an empty window\n");
        return 0;
    }

    /* Nothing loaded yet: compiled overlays are loadable, the rest not. */
    BW_CHECK_EQ("ov0 is compiled", bw_ovl_image(0) != NULL, 1);
    BW_CHECK_EQ("ov3 is not", bw_ovl_image(3) == NULL, 1);
    BW_CHECK_EQ("nothing resident", bw_ovl_resident_at(WIN), -1);

    /* Overlay 0. */
    BW_REQUIRE("load ov0", FS_LoadOverlay(0, 0));
    BW_CHECK_EQ("ov0 resident", bw_ovl_resident_at(WIN + 0x10u), 0);
    BW_CHECK_EQ("ov0 table copied", ds_word(WIN + 0x100u), 7);
    BW_CHECK_EQ("ov0 .bss zeroed", ds_word(WIN + 0x200u), 0);
    BW_CHECK_EQ("ARM9 -> ov0 through the trampoline", func_02100000(5), 12);
    BW_CHECK_EQ("plain name from a test", func_02200000(1), 8);
    BW_CHECK_EQ("call inside ov0", ov000_func_02200010(5), 112);
    BW_CHECK_EQ("ov0 .bss counter 1", ov000_func_02200040(), 1);
    BW_CHECK_EQ("ov0 .bss counter 2", ov000_func_02200040(), 2);
    BW_CHECK_EQ("ov0 .bss in the window", ds_word(WIN + 0x200u), 2);
    BW_CHECK_EQ("a second copy of a function links", ov000_func_02200050(1), 80);
    if (reloc) {
        BW_CHECK_EQ("reloc: ov0's own thumb function",
                    ds_word(WIN + 0x180u), (u32)(uintptr_t)ov000_func_02200010);
        BW_CHECK_EQ("reloc: ov0's own data",
                    ds_word(WIN + 0x184u), (u32)(uintptr_t)bw_ds_ptr(WIN + 0x100u));
        BW_CHECK_EQ("reloc: an ARM9 function",
                    ds_word(WIN + 0x188u), (u32)(uintptr_t)func_02100000);
    } else {
        BW_CHECK_EQ("LP64: pointer word left as the DS address",
                    ds_word(WIN + 0x180u), WIN + 0x11u);
    }

    /* Overlay 2, in the other window, calls into ov0. */
    BW_REQUIRE("load ov2", FS_LoadOverlay(0, 2));
    BW_CHECK_EQ("ov2 -> ov0", ov002_func_02300000(4), 22);

    /* Overlay 1 takes the window. */
    BW_REQUIRE("load ov1", FS_LoadOverlay(0, 1));
    BW_CHECK_EQ("ov0 evicted", bw_overlay_is_loaded(0), 0);
    BW_CHECK_EQ("ov1 resident", bw_ovl_resident_at(WIN), 1);
    BW_CHECK_EQ("ov1 table copied", ds_word(WIN + 0x100u), 3);
    BW_CHECK_EQ("ARM9 -> ov1 through the trampoline", func_02100000(5), 15);
    BW_CHECK_EQ("ov2 -> ov1", ov002_func_02300000(4), 24);
    BW_CHECK_EQ("implicit declaration inside ov1", ov001_func_02200010(5), 16);
    BW_CHECK_EQ("ov1's own function, by name", ov001_func_02200000(2), 6);
    BW_CHECK_EQ("code literal is the function",
                (long)(ov001_func_02200040() == (void *)(uintptr_t)ov001_func_02200000), 1);
    if (reloc) {
        BW_CHECK_EQ("reloc: ov1's thumb function",
                    ds_word(WIN + 0x180u), (u32)(uintptr_t)ov001_func_02200000);
    }

    /* Back to overlay 0: a fresh copy, the .bss zeroed again. */
    BW_REQUIRE("reload ov0", FS_LoadOverlay(0, 0));
    BW_CHECK_EQ("ov0 .bss counter restarts", ov000_func_02200040(), 1);
    BW_CHECK_EQ("ARM9 -> ov0 again", func_02100000(5), 12);
    BW_CHECK_EQ("by its own name", ov000_func_02200000(0), 7);

    /* Static initialisers: ov230's five, in table order, at load. */
    BW_REQUIRE("load ov230", FS_LoadOverlay(0, 230));
    BW_CHECK_EQ("static initialisers ran in order", ds_word(WIN_SI + 0x1600u), 12345);

    bw_ovl_install(NULL, 0);
    bw_overlay_use_table(NULL);
    BW_REPORT("test_ovlink");
}
