/*
 * tests/test_hwcompose.c -- engine/g2d/hwcompose.c against hand-computed
 * pixels and against the asset path.
 *
 *     ./test_hwcompose [data root]
 *
 * Two halves.
 *
 *   - Synthetic hardware states: registers, palette, OAM and VRAM written
 *     byte by byte, and every expected pixel computed on paper from the
 *     hardware rule under test (priority, scroll, OBJ-against-OBJ, flips,
 *     affine OBJ, the windows, the OBJ window, alpha and brightness, the
 *     semi-transparent OBJ, affine, extended and bitmap BGs, extended
 *     palettes, the 3D layer with its alpha and BG0HOFS, master brightness
 *     in 6 bits, display modes, the screen swap, the bank mapping, BG and
 *     OBJ mosaic, the display capture with the game's own DISPCAPCNT
 *     words).
 *
 *   - With game data: the title screen the BG oracle test and eidos_view
 *     use (titledemo.narc 0/1/2) and the species 25 rig pose eidos_view
 *     shows, put into VRAM, palette RAM and OAM the way the SDK would, and
 *     required to come out pixel-identical to the asset path drawing the
 *     same content from the parsed files, alone and together under a
 *     window and an alpha blend.  Without game data this half reports
 *     itself skipped.
 *
 * The renderer reads the live address space through sdk/dsmem.h, and so
 * does this test: every state below is written into the real sdk/dsmem.c
 * (register file, palette RAM, OAM, the VRAM banks through VRAMCNT) after a
 * bw_ds_reset, so the bank mapping the renderer sees is the port's own.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsmem.h"
#include "g2d.h"
#include "port.h"
#include "port_test.h"

/* ================================================================== */
/* The address space (sdk/dsmem.c), as plain host pointers             */
/* ================================================================== */

/* Set by reset(); the regions and banks stay put across bw_ds_reset. */
static u8 *t_io, *t_pal, *t_oam;
static u8 *t_bank_a, *t_bank_e, *t_bank_i;

/* ================================================================== */
/* State helpers                                                       */
/* ================================================================== */

#define W G2D_SCREEN_WIDTH
#define H G2D_SCREEN_HEIGHT

static u8 g_top[W * H * 4], g_bottom[W * H * 4];
static u32 g_flags[2];

static void io16(u32 addr, u16 v) { bw_io_poke(addr, 2, v); }
static void io32(u32 addr, u32 v) { bw_io_poke(addr, 4, v); }
static void io8(u32 addr, u8 v)   { bw_io_poke(addr, 1, v); }

static void vram_put(u32 addr, const u8 *src, u32 n)
{
    u32 i;

    for (i = 0; i < n; i++) {
        u8 *p = (u8 *)bw_ds_try_ptr(addr + i);

        if (p != NULL) {
            *p = src[i];
        }
    }
}

static void vram_fill(u32 addr, u8 v, u32 n)
{
    u32 i;

    for (i = 0; i < n; i++) {
        vram_put(addr + i, &v, 1);
    }
}

static void vram16(u32 addr, u16 v)
{
    u8 b[2];

    bw_mem_wr16(b, v);
    vram_put(addr, b, 2);
}

/* engine: 0 main, 1 sub; which: 0 BG, 1 OBJ. */
static void pal_set(int engine, int which, u32 i, u16 c)
{
    bw_mem_wr16(t_pal + (u32)engine * 0x400u + (u32)which * 0x200u + i * 2u, c);
}

static void oam_set(int engine, u32 i, u16 a0, u16 a1, u16 a2)
{
    u8 *e = t_oam + (u32)engine * 0x400u + i * 8u;

    bw_wr16(e, a0);                     /* OAM holds the DS's order */
    bw_wr16(e + 2, a1);
    bw_wr16(e + 4, a2);
}

static void oam_affine(int engine, u32 group, s16 pa, s16 pb, s16 pc, s16 pd)
{
    u8 *g = t_oam + (u32)engine * 0x400u + group * 32u;

    bw_wr16(g + 6, (u16)pa);
    bw_wr16(g + 14, (u16)pb);
    bw_wr16(g + 22, (u16)pc);
    bw_wr16(g + 30, (u16)pd);
}

/*
 * Power-on, then the default mapping: A and C the main BG window (512 KB
 * needs four banks; 256 KB is plenty here), B the main OBJ window, H the
 * sub BG window, D the sub OBJ window.  Main engine on top.  Every OAM
 * entry is hidden (attr0 bit 9 without bit 8), as the SDK leaves OAM.
 */
static void reset(void)
{
    u32 i;

    bw_ds_reset();                    /* every region zero, KEYINPUT up */
    t_io = (u8 *)(void *)bw_io(BW_DS_IO_BASE);
    t_pal = (u8 *)bw_ds_ptr(BW_DS_PAL_BASE);
    t_oam = (u8 *)bw_ds_ptr(BW_DS_OAM_BASE);
    t_bank_a = bw_vram_bank(BW_VRAM_A, NULL);
    t_bank_e = bw_vram_bank(BW_VRAM_E, NULL);
    t_bank_i = bw_vram_bank(BW_VRAM_I, NULL);
    io8(0x04000240u, 0x81);           /* A: main BG, ofs 0 */
    io8(0x04000242u, 0x89);           /* C: main BG, ofs 1 */
    io8(0x04000241u, 0x82);           /* B: main OBJ        */
    io8(0x04000243u, 0x84);           /* D: sub OBJ         */
    io8(0x04000248u, 0x81);           /* H: sub BG          */
    io16(0x04000304u, 0x8000);        /* POWCNT1: main on top */
    for (i = 0; i < 128u; i++) {
        oam_set(0, i, 0x0200, 0, 0);
        oam_set(1, i, 0x0200, 0, 0);
    }
}

static int render(const u8 *rgba3d)
{
    return g2d_hw_render_dsmem(rgba3d, g_top, g_bottom, g_flags);
}

static const u8 *px(const u8 *screen, u32 x, u32 y)
{
    return screen + ((size_t)y * W + x) * 4u;
}

static u32 rgb_of(const u8 *screen, u32 x, u32 y)
{
    const u8 *p = px(screen, x, y);

    return ((u32)p[0] << 16) | ((u32)p[1] << 8) | p[2];
}

/* 8-bit expansion of a 5-bit channel, as the palette is expanded. */
static u32 e8(u32 c5)
{
    return c5 * 255u / 31u;
}

static u32 rgb5(u32 r, u32 g, u32 b)
{
    return (e8(r) << 16) | (e8(g) << 8) | e8(b);
}

#define RED     0x001Fu
#define GREEN   0x03E0u
#define BLUE    0x7C00u
#define WHITE   0x7FFFu
#define YELLOW  0x03FFu

#define C_RED    rgb5(31, 0, 0)
#define C_GREEN  rgb5(0, 31, 0)
#define C_BLUE   rgb5(0, 0, 31)
#define C_WHITE  rgb5(31, 31, 31)
#define C_YELLOW rgb5(31, 31, 0)

/* A 4bpp tile of one colour index at a byte address. */
static void tile4(u32 addr, u8 v)
{
    vram_fill(addr, (u8)(v | (v << 4)), 32);
}

static void tile8(u32 addr, u8 v)
{
    vram_fill(addr, v, 64);
}

/* Fill a 32x32 text map with one entry. */
static void map_fill(u32 addr, u16 entry)
{
    u32 i;

    for (i = 0; i < 1024u; i++) {
        vram16(addr + i * 2u, entry);
    }
}

/* ================================================================== */
/* Synthetic cases                                                     */
/* ================================================================== */

static void test_display(void)
{
    reset();
    pal_set(0, 0, 0, RED);
    io32(0x04000000u, 0x10000u);          /* graphics, nothing enabled */
    BW_CHECK_EQ("render ok", render(NULL), 0);
    BW_CHECK_EQ("backdrop on top", rgb_of(g_top, 100, 100), C_RED);
    BW_CHECK_EQ("sub display off is white", rgb_of(g_bottom, 5, 5), 0xFFFFFFu);
    BW_CHECK_EQ("opaque", px(g_top, 0, 0)[3], 255);

    io16(0x04000304u, 0x0000);            /* main engine on the bottom */
    render(NULL);
    BW_CHECK_EQ("swap: main on bottom", rgb_of(g_bottom, 1, 1), C_RED);
    BW_CHECK_EQ("swap: sub on top", rgb_of(g_top, 1, 1), 0xFFFFFFu);

    io16(0x04000304u, 0x8000);
    io32(0x04000000u, 0x10080u);          /* forced blank */
    render(NULL);
    BW_CHECK_EQ("forced blank is white", rgb_of(g_top, 7, 7), 0xFFFFFFu);

    /* Master brightness, both directions, at full factor. */
    io32(0x04000000u, 0x10000u);
    io16(0x0400006Cu, 0x4000 | 16);
    render(NULL);
    BW_CHECK_EQ("bright up 16 is white", rgb_of(g_top, 0, 0), 0xFFFFFFu);
    io16(0x0400006Cu, 0x8000 | 16);
    render(NULL);
    BW_CHECK_EQ("bright down 16 is black", rgb_of(g_top, 0, 0), 0u);
    io16(0x0400006Cu, 0x8000 | 8);
    render(NULL);
    /* In the 6-bit stage: red 31 is 63, 63 - (63 * 8 >> 4) = 32, and
     * 32 * 255 / 63 = 129 (the 5-bit arithmetic gave 16, 131). */
    BW_CHECK_EQ("bright down 8, 6-bit", rgb_of(g_top, 0, 0), 129u << 16);
    pal_set(0, 0, 0, (u16)(10u | (20u << 5)));
    io16(0x0400006Cu, 0x4000 | 5);
    render(NULL);
    /* 10 -> 21, 21 + ((63 - 21) * 5 >> 4) = 34, 34 * 255 / 63 = 137;
     * 20 -> 41, 41 + (22 * 5 >> 4) = 47, 190; 0 -> 0, 0 + (63 * 5 >> 4)
     * = 19, 76. */
    BW_CHECK_EQ("bright up 5, 6-bit", rgb_of(g_top, 0, 0),
                (137u << 16) | (190u << 8) | 76u);
    io16(0x0400006Cu, 0xC000 | 8);
    render(NULL);
    BW_CHECK_EQ("mode 3 does nothing", rgb_of(g_top, 0, 0), rgb5(10, 20, 0));
    pal_set(0, 0, 0, RED);
    io16(0x0400006Cu, 0);
    io32(0x04001000u, 0x10000u);
    pal_set(1, 0, 0, BLUE);
    io16(0x0400106Cu, 0x8000 | 16);
    render(NULL);
    BW_CHECK_EQ("sub master bright is its own", rgb_of(g_bottom, 0, 0), 0u);
    BW_CHECK_EQ("main unaffected", rgb_of(g_top, 0, 0), C_RED);

    /* Display mode 2: bank A shown directly. */
    reset();
    io8(0x04000240u, 0x80);               /* A in LCDC mode */
    bw_mem_wr16(t_bank_a + (10u * 256u + 3u) * 2u, GREEN);
    io32(0x04000000u, 0x20000u);
    render(NULL);
    BW_CHECK_EQ("VRAM display", rgb_of(g_top, 3, 10), C_GREEN);
    BW_CHECK_EQ("VRAM display zero", rgb_of(g_top, 4, 10), 0u);
    io16(0x0400006Cu, 0x8000 | 8);
    render(NULL);
    /* green 31 -> 63 -> 32 -> 129, as on the graphics screen */
    BW_CHECK_EQ("VRAM display faded", rgb_of(g_top, 3, 10), 129u << 8);
}

/* BG0 and BG1, tile 1 and tile 2, char base 0, maps at 0x4000 / 0x4800. */
static void two_text_bgs(u16 bg0_prio, u16 bg1_prio)
{
    tile4(0x06000000u + 1u * 32u, 1);
    tile4(0x06000000u + 2u * 32u, 2);
    pal_set(0, 0, 0, BLUE);
    pal_set(0, 0, 1, RED);
    pal_set(0, 0, 16 + 2, GREEN);
    vram16(0x06004000u, 0x0001);                  /* BG0 (0,0): tile 1 */
    vram16(0x06004800u, 0x1002);                  /* BG1 (0,0): tile 2, row 1 */
    io16(0x04000008u, (u16)((8u << 8) | bg0_prio));
    io16(0x0400000Au, (u16)((9u << 8) | bg1_prio));
    io32(0x04000000u, 0x10000u | 0x0300u);
}

static void test_text_priority(void)
{
    reset();
    two_text_bgs(0, 0);
    render(NULL);
    BW_CHECK_EQ("tie: BG0 in front of BG1", rgb_of(g_top, 0, 0), C_RED);
    BW_CHECK_EQ("palette row 1 behind", rgb_of(g_top, 8, 0), C_BLUE);

    io16(0x04000008u, (u16)((8u << 8) | 1u));
    render(NULL);
    BW_CHECK_EQ("BG1 priority 0 wins", rgb_of(g_top, 0, 0), C_GREEN);

    /* Scroll BG0 by 4: screen x 4 samples map x 8, an empty tile. */
    io16(0x04000008u, (u16)(8u << 8));
    io16(0x04000010u, 4);
    render(NULL);
    BW_CHECK_EQ("scrolled BG0 still at x=3", rgb_of(g_top, 3, 0), C_RED);
    BW_CHECK_EQ("scrolled BG0 gone at x=4", rgb_of(g_top, 4, 0), C_GREEN);
    /* The map wraps at 256: screen x 252 samples map x 0. */
    BW_CHECK_EQ("scroll wraps", rgb_of(g_top, 252, 0), C_RED);

    /* Disabled in DISPCNT. */
    io32(0x04000000u, 0x10000u | 0x0200u);
    render(NULL);
    BW_CHECK_EQ("BG0 off in DISPCNT", rgb_of(g_top, 0, 0), C_GREEN);
}

static void test_objs(void)
{
    u32 i;

    reset();
    /* BG0 covers the screen with red at priority 0. */
    tile4(0x06000000u + 32u, 1);
    pal_set(0, 0, 1, RED);
    map_fill(0x06004000u, 0x0001);
    io16(0x04000008u, (u16)(8u << 8));
    io32(0x04000000u, 0x10000u | 0x0100u | 0x1000u | 0x10u);   /* 1D */

    tile4(0x06400000u + 0u * 32u, 1);
    tile4(0x06400000u + 1u * 32u, 2);
    pal_set(0, 1, 1, WHITE);
    pal_set(0, 1, 2, YELLOW);
    oam_set(0, 0, 0, 0, (u16)(0u | (1u << 10)));           /* 8x8 at 0,0 prio 1 */
    oam_set(0, 1, 0, 4, (u16)(1u | (0u << 10)));           /* 8x8 at 4,0 prio 0 */
    render(NULL);
    BW_CHECK_EQ("OBJ prio 1 behind BG prio 0", rgb_of(g_top, 0, 0), C_RED);
    BW_CHECK_EQ("OBJ prio 0 wins a tie with a BG", rgb_of(g_top, 4, 0), C_YELLOW);
    BW_CHECK_EQ("OBJ 1 right part", rgb_of(g_top, 11, 0), C_YELLOW);
    BW_CHECK_EQ("past the OBJs", rgb_of(g_top, 12, 0), C_RED);

    io16(0x04000008u, (u16)((8u << 8) | 2u));
    render(NULL);
    BW_CHECK_EQ("OBJ prio 1 over BG prio 2", rgb_of(g_top, 0, 0), C_WHITE);
    BW_CHECK_EQ("lower priority value wins among OBJs", rgb_of(g_top, 5, 0), C_YELLOW);

    oam_set(0, 1, 0, 4, (u16)(1u | (1u << 10)));
    render(NULL);
    BW_CHECK_EQ("equal priority: lower OAM index wins", rgb_of(g_top, 5, 0), C_WHITE);

    /* Hidden entry. */
    oam_set(0, 0, 0x0200, 0, 0);
    render(NULL);
    BW_CHECK_EQ("hidden OBJ draws nothing", rgb_of(g_top, 0, 0), C_RED);
    BW_CHECK_EQ("the other one still does", rgb_of(g_top, 5, 0), C_YELLOW);

    /* Negative X (9-bit) and wrapped Y. */
    oam_set(0, 1, 0, 0x1FC, 1);           /* x = -4 */
    render(NULL);
    BW_CHECK_EQ("x=-4 shows its right half", rgb_of(g_top, 3, 0), C_YELLOW);
    BW_CHECK_EQ("x=-4 ends at 4", rgb_of(g_top, 4, 0), C_RED);
    oam_set(0, 1, 252, 20, 1);            /* y = 252: rows 0..3 */
    render(NULL);
    BW_CHECK_EQ("y wraps to the top", rgb_of(g_top, 20, 3), C_YELLOW);
    BW_CHECK_EQ("y wrap ends", rgb_of(g_top, 20, 4), C_RED);

    /* Flip: a 16x8 OBJ (shape 1 size 0) whose left tile is 1 and right 2. */
    oam_set(0, 1, 0x0200, 0, 0);
    oam_set(0, 2, (u16)(0x4000u | 50u), (u16)(50u | 0x1000u), 0);
    render(NULL);
    BW_CHECK_EQ("hflip: right tile drawn left", rgb_of(g_top, 50, 50), C_YELLOW);
    BW_CHECK_EQ("hflip: left tile drawn right", rgb_of(g_top, 58, 50), C_WHITE);

    /* 2D mapping: a 16x16 OBJ, second row of tiles is 32 tiles on. */
    io32(0x04000000u, 0x10000u | 0x0100u | 0x1000u);
    tile4(0x06400000u + 32u * 32u, 2);
    tile4(0x06400000u + 33u * 32u, 2);
    oam_set(0, 2, (u16)(60u), (u16)(60u | 0x4000u), 0);   /* square size 1 */
    render(NULL);
    BW_CHECK_EQ("2D map: top-left tile 0", rgb_of(g_top, 60, 60), C_WHITE);
    BW_CHECK_EQ("2D map: bottom-left tile 32", rgb_of(g_top, 60, 68), C_YELLOW);
    /* 1D mapping: the same OBJ's second row is tiles 2 and 3. */
    io32(0x04000000u, 0x10000u | 0x0100u | 0x1000u | 0x10u);
    tile4(0x06400000u + 2u * 32u, 1);
    render(NULL);
    BW_CHECK_EQ("1D map: bottom-left tile 2", rgb_of(g_top, 60, 68), C_WHITE);

    /* 1D boundary 64: tile number 1 is byte 64, tile 2. */
    io32(0x04000000u, 0x10000u | 0x0100u | 0x1000u | 0x10u | (1u << 20));
    oam_set(0, 2, (u16)70u, (u16)70u, 1);
    tile4(0x06400000u + 2u * 32u, 2);
    render(NULL);
    BW_CHECK_EQ("1D boundary 64", rgb_of(g_top, 70, 70), C_YELLOW);

    /* Affine, double size, identity: the 8x8 lands at +4,+4 of 16x16. */
    io32(0x04000000u, 0x10000u | 0x0100u | 0x1000u | 0x10u);
    for (i = 0; i < 8u; i++) {
        /* tile 4: left half index 1, right half index 2 */
        vram_fill(0x06400000u + 4u * 32u + i * 4u, 0x11, 2);
        vram_fill(0x06400000u + 4u * 32u + i * 4u + 2u, 0x22, 2);
    }
    oam_set(0, 2, 0x0200, 0, 0);
    oam_affine(0, 3, 256, 0, 0, 256);
    oam_set(0, 3, (u16)(0x0300u | 100u), (u16)(100u | (3u << 9)), (u16)(4u | (2u << 10)));
    render(NULL);
    BW_CHECK_EQ("affine double: margin", rgb_of(g_top, 103, 104), C_RED);
    BW_CHECK_EQ("affine double: left half", rgb_of(g_top, 104, 104), C_WHITE);
    BW_CHECK_EQ("affine double: right half", rgb_of(g_top, 108, 104), C_YELLOW);
    BW_CHECK_EQ("affine double: right edge", rgb_of(g_top, 111, 111), C_YELLOW);
    BW_CHECK_EQ("affine double: past it", rgb_of(g_top, 112, 104), C_RED);
    /* 2x: the source column is floor((cx - 8) / 2) + 4. */
    oam_affine(0, 3, 128, 0, 0, 128);
    render(NULL);
    BW_CHECK_EQ("2x: cx 0 -> texel 0", rgb_of(g_top, 100, 100), C_WHITE);
    BW_CHECK_EQ("2x: cx 7 -> texel 3", rgb_of(g_top, 107, 100), C_WHITE);
    BW_CHECK_EQ("2x: cx 8 -> texel 4", rgb_of(g_top, 108, 100), C_YELLOW);
    BW_CHECK_EQ("2x: cx 15 -> texel 7", rgb_of(g_top, 115, 115), C_YELLOW);

    /* Bitmap OBJ is reported, not drawn. */
    oam_set(0, 3, (u16)(0x0C00u | 30u), 30u, 0);
    render(NULL);
    BW_CHECK_EQ("bitmap OBJ flagged", g_flags[0] & G2D_HW_UNSUP_BITMAP_OBJ,
                G2D_HW_UNSUP_BITMAP_OBJ);
    BW_CHECK_EQ("bitmap OBJ not drawn", rgb_of(g_top, 30, 30), C_RED);
}

/* BG0 red everywhere at priority 0, BG1 green everywhere at priority 1. */
static void red_over_green(void)
{
    tile4(0x06000000u + 32u, 1);
    tile4(0x06000000u + 64u, 2);
    pal_set(0, 0, 0, BLUE);
    pal_set(0, 0, 1, RED);
    pal_set(0, 0, 2, GREEN);
    map_fill(0x06004000u, 0x0001);
    map_fill(0x06004800u, 0x0002);
    io16(0x04000008u, (u16)(8u << 8));
    io16(0x0400000Au, (u16)((9u << 8) | 1u));
}

static void test_windows(void)
{
    reset();
    red_over_green();
    io16(0x04000040u, (u16)((10u << 8) | 20u));    /* WIN0H: 10..20 */
    io16(0x04000044u, (u16)((10u << 8) | 20u));    /* WIN0V */
    io16(0x04000048u, 0x02);                       /* WININ: win0 BG1 */
    io16(0x0400004Au, 0x3F);                       /* WINOUT: all */
    io32(0x04000000u, 0x10000u | 0x0300u | 0x2000u);
    render(NULL);
    BW_CHECK_EQ("inside WIN0 BG0 hidden", rgb_of(g_top, 15, 15), C_GREEN);
    BW_CHECK_EQ("outside WIN0", rgb_of(g_top, 5, 5), C_RED);
    BW_CHECK_EQ("x2 is exclusive", rgb_of(g_top, 20, 15), C_RED);
    BW_CHECK_EQ("y1 is inclusive", rgb_of(g_top, 10, 10), C_GREEN);

    /* WIN1 under WIN0: WIN0 wins where both contain the pixel. */
    io16(0x04000042u, (u16)((0u << 8) | 30u));
    io16(0x04000046u, (u16)((0u << 8) | 30u));
    io16(0x04000048u, (u16)(0x02u | (0x00u << 8)));  /* win1 shows nothing */
    io32(0x04000000u, 0x10000u | 0x0300u | 0x2000u | 0x4000u);
    render(NULL);
    BW_CHECK_EQ("WIN0 over WIN1", rgb_of(g_top, 15, 15), C_GREEN);
    BW_CHECK_EQ("WIN1 shows the backdrop", rgb_of(g_top, 5, 5), C_BLUE);
    BW_CHECK_EQ("outside both", rgb_of(g_top, 40, 40), C_RED);

    /* The OBJ window: an 8x8 OBJ in mode 2 at (0,0). */
    reset();
    red_over_green();
    tile4(0x06400000u, 1);
    oam_set(0, 0, 0x0800, 0, 0);
    io16(0x0400004Au, (u16)(0x01u | (0x02u << 8)));  /* out: BG0, objwin: BG1 */
    io32(0x04000000u, 0x10000u | 0x0300u | 0x1000u | 0x8000u | 0x10u);
    render(NULL);
    BW_CHECK_EQ("OBJ window shows BG1", rgb_of(g_top, 2, 2), C_GREEN);
    BW_CHECK_EQ("outside the OBJ window", rgb_of(g_top, 20, 20), C_RED);
}

static void test_blend(void)
{
    reset();
    red_over_green();
    pal_set(0, 0, 2, BLUE);                 /* BG1 blue for the blend */
    io32(0x04000000u, 0x10000u | 0x0300u);
    io16(0x04000050u, (u16)(0x01u | (1u << 6) | (0x02u << 8)));
    io16(0x04000052u, (u16)(8u | (8u << 8)));
    render(NULL);
    /* (31 * 8 + 0 * 8) >> 4 = 15 red, (0 + 31 * 8) >> 4 = 15 blue */
    BW_CHECK_EQ("alpha 8/8", rgb_of(g_top, 0, 0), rgb5(15, 0, 15));

    io16(0x04000050u, (u16)(0x01u | (1u << 6) | (0x20u << 8)));
    render(NULL);
    BW_CHECK_EQ("second target not beneath: no blend", rgb_of(g_top, 0, 0), C_RED);

    io16(0x04000050u, (u16)(0x01u | (2u << 6)));
    io16(0x04000054u, 16);
    render(NULL);
    BW_CHECK_EQ("brightness up 16", rgb_of(g_top, 0, 0), C_WHITE);
    io16(0x04000050u, (u16)(0x01u | (3u << 6)));
    io16(0x04000054u, 8);
    render(NULL);
    BW_CHECK_EQ("brightness down 8", rgb_of(g_top, 0, 0), rgb5(16, 0, 0));

    /* A window without the effect bit stops it. */
    io16(0x04000040u, (u16)((0u << 8) | 50u));
    io16(0x04000044u, (u16)((0u << 8) | 50u));
    io16(0x04000048u, 0x1F);
    io16(0x0400004Au, 0x3F);
    io32(0x04000000u, 0x10000u | 0x0300u | 0x2000u);
    render(NULL);
    BW_CHECK_EQ("no effect inside WIN0", rgb_of(g_top, 0, 0), C_RED);
    BW_CHECK_EQ("effect outside", rgb_of(g_top, 60, 60), rgb5(16, 0, 0));

    /* A semi-transparent OBJ blends with BG0 although BLDCNT names no
     * first target and no effect. */
    reset();
    red_over_green();
    tile4(0x06400000u, 1);
    pal_set(0, 1, 1, WHITE);
    oam_set(0, 0, 0x0400, 0, 0);                 /* mode 1 at (0,0) */
    io16(0x04000050u, (u16)(0x01u << 8));        /* second: BG0 */
    io16(0x04000052u, (u16)(8u | (8u << 8)));
    io32(0x04000000u, 0x10000u | 0x0100u | 0x1000u | 0x10u);
    render(NULL);
    /* (31 * 8 + 31 * 8) >> 4 = 31, (31 * 8 + 0) >> 4 = 15 */
    BW_CHECK_EQ("semi-transparent OBJ", rgb_of(g_top, 0, 0), rgb5(31, 15, 15));
    BW_CHECK_EQ("BG0 elsewhere", rgb_of(g_top, 9, 0), C_RED);
    io16(0x04000050u, (u16)(0x02u << 8));        /* second: BG1, not beneath */
    render(NULL);
    BW_CHECK_EQ("semi OBJ over a non-target", rgb_of(g_top, 0, 0), C_WHITE);
}

static void test_affine_bg(void)
{
    reset();
    pal_set(0, 0, 0, BLUE);
    pal_set(0, 0, 5, GREEN);
    tile8(0x06000000u + 64u, 5);                  /* 8bpp tile 1 */
    vram_fill(0x06004000u, 1, 1);                 /* map (0,0) = tile 1 */
    io16(0x0400000Cu, (u16)(8u << 8));            /* BG2: size 128, no wrap */
    io16(0x04000020u, 256);
    io16(0x04000026u, 256);
    io32(0x04000028u, (u32)-2048);                /* X = -8.0 */
    io32(0x04000000u, 0x10000u | 2u | 0x0400u);
    render(NULL);
    BW_CHECK_EQ("affine: left of the map", rgb_of(g_top, 7, 0), C_BLUE);
    BW_CHECK_EQ("affine: tile at x 8", rgb_of(g_top, 8, 0), C_GREEN);
    BW_CHECK_EQ("affine: tile ends", rgb_of(g_top, 16, 0), C_BLUE);
    BW_CHECK_EQ("affine: past the 128 map", rgb_of(g_top, 140, 0), C_BLUE);
    io16(0x0400000Cu, (u16)((8u << 8) | 0x2000u));   /* wrap */
    render(NULL);
    BW_CHECK_EQ("affine wrap: x 136 is map x 0", rgb_of(g_top, 136, 0), C_GREEN);
    BW_CHECK_EQ("affine wrap: x 0 is map x 120", rgb_of(g_top, 0, 0), C_BLUE);

    /* Extended rotation/scaling BG with 16-bit entries (mode 5, BG3). */
    reset();
    pal_set(0, 0, 0, BLUE);
    pal_set(0, 0, 3, RED);
    pal_set(0, 0, 4, GREEN);
    {
        u32 r;

        for (r = 0; r < 8u; r++) {
            vram_fill(0x06000000u + 64u + r * 8u, 3, 4);
            vram_fill(0x06000000u + 64u + r * 8u + 4u, 4, 4);
        }
    }
    vram16(0x06004000u, 0x0401);                  /* tile 1, hflip */
    io16(0x0400000Eu, (u16)(8u << 8));
    io16(0x04000030u, 256);
    io16(0x04000036u, 256);
    io32(0x04000000u, 0x10000u | 5u | 0x0800u);
    render(NULL);
    BW_CHECK_EQ("ext affine hflip: x 0", rgb_of(g_top, 0, 0), C_GREEN);
    BW_CHECK_EQ("ext affine hflip: x 7", rgb_of(g_top, 7, 0), C_RED);
    BW_CHECK_EQ("ext affine: empty entry", rgb_of(g_top, 8, 0), C_BLUE);

    /* 256-colour bitmap, BG2, 256x256, base block 2 (32 KB). */
    reset();
    pal_set(0, 0, 0, BLUE);
    pal_set(0, 0, 9, YELLOW);
    vram_fill(0x06008000u + 5u * 256u + 10u, 9, 1);
    io16(0x0400000Cu, (u16)(0x80u | (2u << 8) | (1u << 14)));
    io16(0x04000020u, 256);
    io16(0x04000026u, 256);
    io32(0x04000000u, 0x10000u | 5u | 0x0400u);
    render(NULL);
    BW_CHECK_EQ("bitmap256 pixel", rgb_of(g_top, 10, 5), C_YELLOW);
    BW_CHECK_EQ("bitmap256 zero is transparent", rgb_of(g_top, 11, 5), C_BLUE);

    /* Direct colour bitmap: bit 15 is opacity. */
    vram16(0x06008000u + (3u * 256u + 3u) * 2u, 0x8000u | GREEN);
    vram16(0x06008000u + (3u * 256u + 4u) * 2u, GREEN);
    io16(0x0400000Cu, (u16)(0x84u | (2u << 8) | (1u << 14)));
    render(NULL);
    BW_CHECK_EQ("direct colour", rgb_of(g_top, 3, 3), C_GREEN);
    BW_CHECK_EQ("direct colour, bit 15 clear", rgb_of(g_top, 4, 3), C_BLUE);
}

static void test_extpal(void)
{
    reset();
    io8(0x04000244u, 0x84);                       /* E: BG ext palettes */
    pal_set(0, 0, 0, BLUE);
    pal_set(0, 0, 7, RED);                        /* the plain palette */
    bw_mem_wr16(t_bank_e + 2u * 0x2000u + (3u * 256u + 7u) * 2u, GREEN);
    tile8(0x06000000u + 64u, 7);
    vram16(0x06004000u, (u16)(1u | (3u << 12)));
    io16(0x04000008u, (u16)(0x80u | (8u << 8) | 0x2000u));   /* slot 2 */
    io32(0x04000000u, 0x10000u | 0x0100u | 0x40000000u);
    render(NULL);
    BW_CHECK_EQ("text ext palette slot 2 row 3", rgb_of(g_top, 0, 0), C_GREEN);
    BW_CHECK_EQ("ext text transparent", rgb_of(g_top, 8, 0), C_BLUE);
    io32(0x04000000u, 0x10000u | 0x0100u);
    render(NULL);
    BW_CHECK_EQ("ext off: plain 256 palette", rgb_of(g_top, 0, 0), C_RED);

    /* Sub engine OBJ through bank I's extended palette. */
    io8(0x04000249u, 0x83);                       /* I: sub OBJ ext */
    bw_mem_wr16(t_bank_i + (2u * 256u + 5u) * 2u, YELLOW);
    pal_set(1, 0, 0, BLUE);
    tile8(0x06600000u, 5);
    oam_set(1, 0, (u16)(0x2000u | 40u), 40u, (u16)(2u << 12));
    io32(0x04001000u, 0x10000u | 0x1000u | 0x10u | 0x80000000u);
    render(NULL);
    BW_CHECK_EQ("sub OBJ ext palette", rgb_of(g_bottom, 40, 40), C_YELLOW);
    BW_CHECK_EQ("sub backdrop", rgb_of(g_bottom, 39, 40), C_BLUE);
}

static void test_3d(void)
{
    static u8 g3d[W * H * 4];
    u32 i;

    for (i = 0; i < W * H; i++) {
        g3d[i * 4u + 0u] = 10;
        g3d[i * 4u + 1u] = 20;
        g3d[i * 4u + 2u] = 30;
        g3d[i * 4u + 3u] = 255;
    }
    reset();
    tile4(0x06000000u + 32u, 1);
    pal_set(0, 0, 1, RED);
    vram16(0x06004800u, 0x0001);                  /* BG1 (0,0) */
    io16(0x04000008u, 1);                         /* BG0 (3D) priority 1 */
    io16(0x0400000Au, (u16)(9u << 8));            /* BG1 priority 0 */
    io32(0x04000000u, 0x10000u | 0x8u | 0x0300u);
    render(g3d);
    BW_CHECK_EQ("BG1 in front of 3D", rgb_of(g_top, 0, 0), C_RED);
    BW_CHECK_EQ("3D elsewhere", rgb_of(g_top, 20, 20), 0x0A141Eu);
    render(NULL);
    BW_CHECK_EQ("no 3D frame flagged", g_flags[0] & G2D_HW_UNSUP_NO_3D,
                G2D_HW_UNSUP_NO_3D);

    io32(0x04000000u, 0x10000u | 6u | 0x0500u);   /* mode 6, BG2 large */
    render(g3d);
    BW_CHECK_EQ("large screen flagged", g_flags[0] & G2D_HW_UNSUP_BG_MODE,
                G2D_HW_UNSUP_BG_MODE);
}

/* ================================================================== */
/* The 3D layer's alpha and scroll, mosaic, the display capture        */
/* ================================================================== */

static void test_3d_alpha(void)
{
    static u8 g3d[W * H * 4];
    u32 i;

    for (i = 0; i < W * H; i++) {
        g3d[i * 4u + 0u] = 10;
        g3d[i * 4u + 1u] = 20;
        g3d[i * 4u + 2u] = 30;
        g3d[i * 4u + 3u] = 255;
    }
    g3d[(5u * W + 5u) * 4u + 3u] = 0;             /* (5, 5): nothing drawn */
    g3d[(7u * W + 7u) * 4u + 0u] = 252;           /* (7, 7): translucent */
    g3d[(7u * W + 7u) * 4u + 1u] = 0;
    g3d[(7u * W + 7u) * 4u + 2u] = 0;
    g3d[(7u * W + 7u) * 4u + 3u] = 123;           /* folds to alpha 15 */

    reset();
    tile4(0x06000000u + 32u, 1);
    pal_set(0, 0, 0, BLUE);
    pal_set(0, 0, 1, RED);
    map_fill(0x06004800u, 0x0001);                /* BG1 red everywhere */
    io16(0x04000008u, 0);                         /* BG0 (3D) priority 0 */
    io16(0x0400000Au, (u16)((9u << 8) | 1u));     /* BG1 priority 1 */
    io32(0x04000000u, 0x10000u | 0x8u | 0x0300u);
    render(g3d);
    BW_CHECK_EQ("3D in front", rgb_of(g_top, 6, 5), 0x0A141Eu);
    BW_CHECK_EQ("3D alpha 0 shows BG1", rgb_of(g_top, 5, 5), C_RED);
    BW_CHECK_EQ("translucent, no second target: as drawn",
                rgb_of(g_top, 7, 7), 0xFC0000u);

    /*
     * BG1 a second target: the 3D pixel blends by its own alpha 15.
     * 6-bit: 3D red 252 -> 62, BG1 red 31 -> 63;
     * (62 * (15 + 1) + 63 * (31 - 15)) / 32 = 2000 / 32 = 62, and
     * 62 * 255 / 63 = 250.  The opaque pixels are unchanged.
     */
    io16(0x04000050u, 0x0200u);
    render(g3d);
    BW_CHECK_EQ("3D alpha blend with BG1", rgb_of(g_top, 7, 7), 250u << 16);
    BW_CHECK_EQ("opaque 3D unchanged", rgb_of(g_top, 6, 5), 0x0A141Eu);
    io16(0x04000050u, 0);

    /* BG0HOFS 3: screen x 2 is column 5, the transparent pixel. */
    io16(0x04000010u, 3);
    render(g3d);
    BW_CHECK_EQ("BG0HOFS: x 2 shows column 5", rgb_of(g_top, 2, 5), C_RED);
    BW_CHECK_EQ("BG0HOFS: x 5 shows column 8", rgb_of(g_top, 5, 5), 0x0A141Eu);
    BW_CHECK_EQ("BG0HOFS: column 256 and on is empty",
                rgb_of(g_top, 253, 5), C_RED);
    io16(0x04000010u, 0);

    /* The mosaic bit on the 3D BG0 is ignored and flagged. */
    io16(0x04000008u, 0x40u);
    io8(0x0400004Cu, 0x33);
    render(g3d);
    BW_CHECK_EQ("3D BG0 mosaic flagged", g_flags[0] & G2D_HW_UNSUP_MOSAIC,
                G2D_HW_UNSUP_MOSAIC);
    BW_CHECK_EQ("3D BG0 not mosaicked", rgb_of(g_top, 5, 5), C_RED);
}

/* A 4bpp tile: row 0 alternates indices 1 and 2, every other row is 3. */
static void tile_rows(u32 addr)
{
    u32 r;

    vram_fill(addr, 0x21, 4);
    for (r = 1; r < 8u; r++) {
        vram_fill(addr + r * 4u, 0x33, 4);
    }
}

static void test_mosaic(void)
{
    reset();
    tile_rows(0x06000000u + 32u);
    pal_set(0, 0, 0, BLUE);
    pal_set(0, 0, 1, RED);
    pal_set(0, 0, 2, GREEN);
    pal_set(0, 0, 3, YELLOW);
    vram16(0x06004000u, 0x0001);                  /* BG0 (0,0): tile 1 */
    io16(0x04000008u, (u16)(8u << 8));
    io32(0x04000000u, 0x10000u | 0x0100u);
    render(NULL);
    BW_CHECK_EQ("no mosaic: x 1 is index 2", rgb_of(g_top, 1, 0), C_GREEN);
    BW_CHECK_EQ("no mosaic: row 2 is index 3", rgb_of(g_top, 0, 2), C_YELLOW);

    /* H 2, V 4: (x, y) shows (x - x % 2, y - y % 4). */
    io16(0x04000008u, (u16)((8u << 8) | 0x40u));
    io8(0x0400004Cu, 0x31);
    render(NULL);
    BW_CHECK_EQ("mosaic: (1,0) shows (0,0)", rgb_of(g_top, 1, 0), C_RED);
    BW_CHECK_EQ("mosaic: (1,3) shows (0,0)", rgb_of(g_top, 1, 3), C_RED);
    BW_CHECK_EQ("mosaic: (3,0) shows (2,0)", rgb_of(g_top, 3, 0), C_RED);
    BW_CHECK_EQ("mosaic: (3,5) shows (2,4)", rgb_of(g_top, 3, 5), C_YELLOW);
    BW_CHECK_EQ("mosaic: (9,0) shows (8,0), empty", rgb_of(g_top, 9, 0), C_BLUE);
    BW_CHECK_EQ("mosaic layer keeps no flag", g_flags[0] & G2D_HW_UNSUP_MOSAIC, 0u);

    /* The size register at 0 (1x1) changes nothing. */
    io8(0x0400004Cu, 0x00);
    render(NULL);
    BW_CHECK_EQ("mosaic 1x1: x 1 is index 2", rgb_of(g_top, 1, 0), C_GREEN);

    /* A bitmap BG (an existing raster) through the same grid: 4 x 4. */
    reset();
    pal_set(0, 0, 0, BLUE);
    pal_set(0, 0, 9, YELLOW);
    vram_fill(0x06008000u + 4u * 256u + 8u, 9, 1);
    io16(0x0400000Cu, (u16)(0x80u | 0x40u | (2u << 8) | (1u << 14)));
    io16(0x04000020u, 256);
    io16(0x04000026u, 256);
    io8(0x0400004Cu, 0x33);
    io32(0x04000000u, 0x10000u | 5u | 0x0400u);
    render(NULL);
    BW_CHECK_EQ("bitmap mosaic: (11,7) shows (8,4)", rgb_of(g_top, 11, 7), C_YELLOW);
    BW_CHECK_EQ("bitmap mosaic: (12,7) shows (12,4)", rgb_of(g_top, 12, 7), C_BLUE);

    /*
     * OBJ mosaic, H 2 and V 4 in MOSAIC's high byte, an 8x8 OBJ at x 1:
     * screen x 1 is OBJ column 0 (the grid cell starts at x 0, left of the
     * OBJ: its first column is repeated), x 2 and 3 are column 1, x 4 and
     * 5 column 3; screen row 2 is OBJ row 0.
     */
    reset();
    tile_rows(0x06400000u + 32u);
    pal_set(0, 0, 0, BLUE);
    pal_set(0, 1, 1, RED);
    pal_set(0, 1, 2, GREEN);
    pal_set(0, 1, 3, YELLOW);
    oam_set(0, 0, (u16)(0x1000u | 0u), 1u, 1u);
    io8(0x0400004Du, 0x31);
    io32(0x04000000u, 0x10000u | 0x1000u);
    render(NULL);
    BW_CHECK_EQ("OBJ mosaic: x 1", rgb_of(g_top, 1, 0), C_RED);
    BW_CHECK_EQ("OBJ mosaic: x 2", rgb_of(g_top, 2, 0), C_GREEN);
    BW_CHECK_EQ("OBJ mosaic: x 3", rgb_of(g_top, 3, 0), C_GREEN);
    BW_CHECK_EQ("OBJ mosaic: x 4", rgb_of(g_top, 4, 0), C_GREEN);
    BW_CHECK_EQ("OBJ mosaic: row 2 is row 0", rgb_of(g_top, 2, 2), C_GREEN);
    BW_CHECK_EQ("OBJ mosaic: row 4 is row 4", rgb_of(g_top, 2, 4), C_YELLOW);
    oam_set(0, 0, 0u, 1u, 1u);
    render(NULL);
    BW_CHECK_EQ("OBJ without the bit: x 2 is column 1",
                rgb_of(g_top, 2, 0), C_GREEN);
    BW_CHECK_EQ("OBJ without the bit: x 3 is column 2",
                rgb_of(g_top, 3, 0), C_RED);
}

static u16 bank16(const u8 *bank, u32 ofs)
{
    return bw_mem_rd16(bank + ofs);
}

static void test_capture(void)
{
    static u8 g3d[W * H * 4];
    u8 *bank_c = bw_vram_bank(BW_VRAM_C, NULL);
    u8 *bank_d = bw_vram_bank(BW_VRAM_D, NULL);
    u32 i;

    for (i = 0; i < W * H; i++) {
        g3d[i * 4u + 0u] = 10;
        g3d[i * 4u + 1u] = 20;
        g3d[i * 4u + 2u] = 30;
        g3d[i * 4u + 3u] = 255;
    }
    g3d[3] = 0;                                    /* (0, 0) transparent */

    /*
     * The game's own word, 0x80330010 (func_0204EAD4): source A the
     * graphics screen, 256x192 into bank D at 0, EVA 16.  The backdrop is
     * red and the main screen is faded to black: the capture is taken
     * before the master brightness.
     */
    reset();
    io8(0x04000243u, 0x80);                        /* D in LCDC mode */
    pal_set(0, 0, 0, RED);
    io32(0x04000000u, 0x10000u);
    io16(0x0400006Cu, 0x8000 | 16);
    io32(0x04000064u, 0x80330010u);
    render(NULL);
    BW_CHECK_EQ("screen faded to black", rgb_of(g_top, 0, 0), 0u);
    BW_CHECK_EQ("captured red with alpha", bank16(bank_d, 0), 0x801Fu);
    BW_CHECK_EQ("captured last pixel", bank16(bank_d, (191u * 256u + 255u) * 2u),
                0x801Fu);
    BW_CHECK_EQ("past 256x192 untouched", bank16(bank_d, 192u * 256u * 2u), 0u);
    BW_CHECK_EQ("enable bit cleared", bw_io_read32(0x04000064u), 0x00330010u);

    /* The bit is clear now: the next frame captures nothing. */
    pal_set(0, 0, 0, GREEN);
    render(NULL);
    BW_CHECK_EQ("no capture once cleared", bank16(bank_d, 0), 0x801Fu);

    /* A destination bank not in LCDC mode is not written. */
    io8(0x04000243u, 0x84);
    io32(0x04000064u, 0x80330010u);
    render(NULL);
    io8(0x04000243u, 0x80);
    BW_CHECK_EQ("bank D mapped elsewhere: not captured", bank16(bank_d, 0),
                0x801Fu);

    /* The graphics screen while the display shows VRAM (mode 2, bank C). */
    io8(0x04000242u, 0x80);
    io32(0x04000000u, 0x20000u | (2u << 18));
    io16(0x0400006Cu, 0);
    io32(0x04000064u, 0x80330010u);
    render(NULL);
    BW_CHECK_EQ("graphics captured under a VRAM display", bank16(bank_d, 0),
                0x83E0u);

    /*
     * The game's 0x81330010 (func_021C2784): source A the 3D frame alone.
     * (0, 0) has alpha 0: colour 0, alpha bit 0.  Elsewhere 10, 20, 30 fold
     * to 6-bit 2, 5, 7 and halve to 1, 2, 3.
     */
    io32(0x04000000u, 0x10000u | 0x8u | 0x0100u);
    io32(0x04000064u, 0x81330010u);
    render(g3d);
    BW_CHECK_EQ("3D capture: alpha 0", bank16(bank_d, 0), 0u);
    BW_CHECK_EQ("3D capture: a pixel", bank16(bank_d, 2u),
                0x8000u | 1u | (2u << 5) | (3u << 10));

    /*
     * Blend, 128x128 into D at 32 KiB, source B bank C (DISPCNT bits 18-19)
     * from 32 KiB: (A * Aa * 8 + B * Ba * 8) / 16 per channel.
     *   pixel 0:   A red, B blue with alpha  -> 15, 0, 15, alpha
     *   pixel 1:   A red, B red without alpha -> 15, 0, 0, alpha
     *   pixel 128: A red, B green with alpha  -> 15, 15, 0 (row 1: the
     *              128-wide stream, at D + 32 KiB + 256)
     */
    io32(0x04000000u, 0x10000u | (2u << 18));
    pal_set(0, 0, 0, RED);
    bw_mem_wr16(bank_c + 0x8000u, 0x8000u | BLUE);
    bw_mem_wr16(bank_c + 0x8002u, RED);
    bw_mem_wr16(bank_c + 0x8000u + 256u, 0x8000u | GREEN);
    io32(0x04000064u, 0x80000000u | (2u << 29) | (1u << 26) | (0u << 20)
                      | (3u << 16) | (1u << 18) | (8u << 8) | 8u);
    render(NULL);
    BW_CHECK_EQ("blend pixel 0", bank16(bank_d, 0x8000u),
                0x8000u | 15u | (15u << 10));
    BW_CHECK_EQ("blend pixel 1, B alpha 0", bank16(bank_d, 0x8002u),
                0x8000u | 15u);
    BW_CHECK_EQ("blend pixel 128", bank16(bank_d, 0x8000u + 256u),
                0x8000u | 15u | (15u << 5));

    /* Source B alone is copied as it is, alpha bit included. */
    io32(0x04000064u, 0x80000000u | (1u << 29) | (1u << 26) | (3u << 16));
    render(NULL);
    BW_CHECK_EQ("source B copied", bank16(bank_d, 2u), RED);

    /* Source B from the main memory FIFO: flagged, read as 0. */
    io32(0x04000064u, 0x80000000u | (1u << 29) | (1u << 25) | (3u << 16));
    render(NULL);
    BW_CHECK_EQ("FIFO source flagged", g_flags[0] & G2D_HW_UNSUP_CAPTURE_FIFO,
                G2D_HW_UNSUP_CAPTURE_FIFO);
    BW_CHECK_EQ("FIFO source reads 0", bank16(bank_d, 2u), 0u);
}

/* ================================================================== */
/* Against the asset path, with game data                              */
/* ================================================================== */

#define POSE_X 100
#define POSE_Y 60

static int cmp_screens(const char *label, const u8 *a, const u8 *b)
{
    u32 i;

    for (i = 0; i < W * H; i++) {
        if (memcmp(a + i * 4u, b + i * 4u, 4) != 0) {
            printf("  %s: first difference at (%u, %u): hw %u,%u,%u asset %u,%u,%u\n",
                   label, i % W, i / W, a[i * 4u], a[i * 4u + 1u], a[i * 4u + 2u],
                   b[i * 4u], b[i * 4u + 1u], b[i * 4u + 2u]);
            return 1;
        }
    }
    return 0;
}

/* How many pixels differ from pixel (0, 0): a comparison of two blank
 * screens would pass, so every comparison also asserts content. */
static u32 non_flat(const u8 *a)
{
    u32 i, n = 0;

    for (i = 1; i < W * H; i++) {
        if (memcmp(a + i * 4u, a, 3) != 0) {
            n++;
        }
    }
    return n;
}

static u16 to555(G2dColor c)
{
    u32 r = ((u32)c.r * 31u + 127u) / 255u;
    u32 g = ((u32)c.g * 31u + 127u) / 255u;
    u32 b = ((u32)c.b * 31u + 127u) / 255u;

    return (u16)(r | (g << 5) | (b << 10));
}

/* The title BG into main BG0: char base 64 KB + 32 KB, map at 0. */
static void load_title(const G2dNscr *nscr, const G2dNcgr *ncgr,
                       const G2dPalette *pal, s32 hofs, s32 vofs, u32 prio)
{
    u32 i;
    u32 n = ncgr->tile_data_size;

    for (i = 0; i < pal->count && i < 256u; i++) {
        pal_set(0, 0, i, to555(pal->colors[i]));
    }
    if (n > 0x10000u) {
        n = 0x10000u;
    }
    vram_put(0x06000000u + 0x18000u, ncgr->tile_data, n);
    for (i = 0; i < nscr->count && i < 1024u; i++) {
        vram16(0x06000000u + i * 2u, nscr->entries[i]);
    }
    io16(0x04000008u, (u16)(0x80u | (2u << 2) | prio));   /* 8bpp, char 2, map 0 */
    io16(0x04000010u, (u16)hofs);
    io16(0x04000012u, (u16)vofs);
}

/*
 * The pose's OAMs into one engine's OAM and OBJ VRAM, 1D mapping, the
 * same walk g2d_compose_pose makes.  Returns the number of entries, or -1
 * when the pose needs something this loader does not do (a non-identity
 * frame, more than 128 entries).
 */
static int load_pose(int engine, const G2dRig *rig, u32 obj_vram)
{
    const G2dMcGroup *group = &rig->nmcr.groups[0];
    s32 minx = 0x7FFFFFFF, miny = 0x7FFFFFFF;
    u32 pass, i, j, slot = 0, tile = 0;

    for (i = 0; i < 256u && i < rig->palette.count; i++) {
        pal_set(engine, 1, i, to555(rig->palette.colors[i]));
    }
    for (pass = 0; pass < 2u; pass++) {
        for (i = 0; i < group->num_nodes; i++) {
            const G2dMcNode *node = &group->nodes[i];
            G2dFrame frame;
            const G2dCell *cell;
            u32 fi;

            if (!rig->has_nanr) {
                return -1;
            }
            fi = g2d_nanr_frame_at_time(&rig->nanr, node->anim_seq_idx, 0);
            if (g2d_nanr_frame_data(&rig->nanr, node->anim_seq_idx, fi, &frame) != 0
                || frame.cell_idx >= rig->ncer.num_cells) {
                continue;
            }
            cell = &rig->ncer.cells[frame.cell_idx];
            if (!g2d_affine_is_identity(frame.sx, frame.sy, frame.rot_z)) {
                return -1;
            }
            for (j = 0; j < cell->num_oam; j++) {
                const G2dOam *o = &cell->oams[j];
                s32 ax = (s32)node->x + frame.px + o->x;
                s32 ay = (s32)node->y + frame.py + o->y;
                u32 tw = o->w / 8u, th = o->h / 8u;
                u32 stride = (rig->ncgr.tile_num_x == G2D_NCGR_DIM_SENTINEL)
                             ? tw : (u32)rig->ncgr.tile_num_x;
                int bpp8 = (rig->ncgr.bytes_per_tile == 64u);
                u32 palrow = ((u32)o->pal * 16u + 16u <= rig->palette.count)
                             ? o->pal : 0u;
                s32 sx, sy;
                u16 a0, a1;
                u32 ty, tx;

                if (pass == 0u) {
                    if (ax < minx) { minx = ax; }
                    if (ay < miny) { miny = ay; }
                    continue;
                }
                if (slot >= 128u) {
                    return -1;
                }
                sx = POSE_X + ax - minx + 2;
                sy = POSE_Y + ay - miny + 2;
                for (ty = 0; ty < th; ty++) {
                    for (tx = 0; tx < tw; tx++) {
                        u8 block[64], packed[64];
                        u32 k;

                        g2d_ncgr_block(&rig->ncgr, (u32)o->tile_idx + ty * stride + tx,
                                       block);
                        if (bpp8) {
                            vram_put(obj_vram + tile * 32u, block, 64);
                            tile += 2u;
                        } else {
                            for (k = 0; k < 32u; k++) {
                                packed[k] = (u8)(block[k * 2u]
                                                 | (block[k * 2u + 1u] << 4));
                            }
                            vram_put(obj_vram + tile * 32u, packed, 32);
                            tile += 1u;
                        }
                    }
                }
                a0 = (u16)(((u32)o->shape << 14) | (bpp8 ? 0x2000u : 0u));
                a1 = (u16)((u32)o->size << 14);
                if (o->affine_double && !o->hflip && !o->vflip) {
                    /* As the hardware would hold it: affine, double size,
                     * placed half a size up and left, identity matrix. */
                    sx -= (s32)(o->w / 2u);
                    sy -= (s32)(o->h / 2u);
                    a0 |= 0x0300u;
                } else {
                    a1 |= (u16)((o->hflip ? 0x1000u : 0u) | (o->vflip ? 0x2000u : 0u));
                }
                a0 |= (u16)((u32)sy & 0xFFu);
                a1 |= (u16)((u32)sx & 0x1FFu);
                oam_set(engine, slot,
                        a0, a1,
                        (u16)((tile - tw * th * (bpp8 ? 2u : 1u))
                              | ((u32)o->priority << 10) | (palrow << 12)));
                slot++;
            }
        }
    }
    oam_affine(engine, 0, 256, 0, 0, 256);
    return (int)slot;
}

static int test_against_assets(void)
{
    static u8 asset[W * H * 4];
    BwArcFile title_arc, rig_arc;
    u8 *nscr_buf, *ncgr_buf, *nclr_buf;
    u32 ns = 0, gs = 0, ps = 0;
    G2dNscr nscr;
    G2dNcgr ncgr;
    G2dPalette pal;
    G2dRig rig;
    G2dPose pose;
    G2dBgLayer layer;
    G2dObjEntry obj;
    G2dFrameDesc desc;
    G2dWindows win;
    G2dBlend blend;
    int n_oam;

    if (bw_arc_open(&title_arc, "titledemo.narc") != 0) {
        printf("  skipped: no game data (titledemo.narc) under %s\n", bw_data_root());
        return 0;
    }
    memset(&nscr, 0, sizeof(nscr));
    memset(&ncgr, 0, sizeof(ncgr));
    memset(&pal, 0, sizeof(pal));
    nscr_buf = g2d_arc_member(&title_arc, 0, &ns);
    ncgr_buf = g2d_arc_member(&title_arc, 1, &gs);
    nclr_buf = g2d_arc_member(&title_arc, 2, &ps);
    BW_REQUIRE("title members", nscr_buf != NULL && ncgr_buf != NULL && nclr_buf != NULL);
    BW_REQUIRE("title parses", g2d_nscr_parse(&nscr, nscr_buf, ns) == 0
               && g2d_ncgr_parse(&ncgr, ncgr_buf, gs) == 0
               && g2d_palette_append_nclr(&pal, nclr_buf, ps) == 0);
    BW_REQUIRE("title is a 256x256 map", nscr.tiles_wide == 32 && nscr.tiles_high == 32);

    /* 1. The title screen alone, scrolled, on the main engine. */
    memset(&layer, 0, sizeof(layer));
    layer.nscr = &nscr;
    layer.ncgr = &ncgr;
    layer.pal = &pal;
    layer.screen_size = G2D_BG_SIZE_FLAT;
    layer.hofs = 13;
    layer.vofs = 37;
    layer.wrap = 1;
    layer.enabled = 1;
    memset(&desc, 0, sizeof(desc));
    desc.bgs = &layer;
    desc.n_bgs = 1;
    desc.backdrop_pal = &pal;
    g2d_frame_compose_desc(&desc, W, H, asset);

    reset();
    load_title(&nscr, &ncgr, &pal, 13, 37, 0);
    io32(0x04000000u, 0x10000u | 0x0100u | (1u << 24));
    render(NULL);
    BW_CHECK_EQ("title: hw == asset", cmp_screens("title", g_top, asset), 0);
    BW_CHECK_EQ("title has content", non_flat(g_top) > 20000u, 1);
    /* The comparison can fail: one pixel more of scroll does. */
    io16(0x04000010u, 14);
    render(NULL);
    printf("  (expected) ");
    BW_CHECK_EQ("title scrolled one more differs", cmp_screens("title+1", g_top, asset), 1);

    /* 2. The rig pose alone on the sub engine. */
    if (bw_arc_open(&rig_arc, "a/0/0/4") != 0) {
        printf("  skipped: no a/0/0/4\n");
        goto out_title;
    }
    memset(&rig, 0, sizeof(rig));
    memset(&pose, 0, sizeof(pose));
    BW_CHECK_EQ("rig loads", g2d_rig_load(&rig, &rig_arc, 25u * 20u, 20u), 0);
    BW_CHECK_EQ("pose composes",
                g2d_compose_pose(&rig.ncgr, &rig.ncer, rig.has_nanr ? &rig.nanr : NULL,
                                 &rig.nmcr.groups[0], 0, rig.palette.count, &pose), 0);

    memset(&obj, 0, sizeof(obj));
    obj.pose = &pose;
    obj.pal = &rig.palette;
    obj.x = POSE_X;
    obj.y = POSE_Y;
    memset(&desc, 0, sizeof(desc));
    desc.objs = &obj;
    desc.n_objs = 1;
    desc.backdrop_pal = &rig.palette;
    g2d_frame_compose_desc(&desc, W, H, asset);

    reset();
    pal_set(1, 0, 0, to555(rig.palette.colors[0]));
    n_oam = load_pose(1, &rig, 0x06600000u);
    BW_CHECK_EQ("pose fits OAM", n_oam > 0, 1);
    io32(0x04001000u, 0x10000u | 0x1000u | 0x10u);
    render(NULL);
    printf("  pose: %d OAM entries, %ux%u\n", n_oam, pose.width, pose.height);
    BW_CHECK_EQ("pose: hw == asset", cmp_screens("pose", g_bottom, asset), 0);
    BW_CHECK_EQ("pose has content", non_flat(g_bottom) > 1000u, 1);

    /*
     * 3. Both on the main engine, under WIN0 and an OBJ-over-BG0 blend.
     * BG0 at priority 3 so every OAM of the pose, whatever its own attr2
     * priority, is in front of it, as the one asset entry is.
     */
    layer.priority = 3;
    memset(&win, 0, sizeof(win));
    win.win0.enabled = 1;
    win.win0.x1 = 40;
    win.win0.x2 = 200;
    win.win0.y1 = 30;
    win.win0.y2 = 150;
    win.win0.layers = G2D_LAYER_BG0 | G2D_LAYER_OBJ | G2D_LAYER_EFFECT;
    win.outside_layers = G2D_LAYER_BG0 | G2D_LAYER_OBJ;
    memset(&blend, 0, sizeof(blend));
    blend.effect = G2D_BLEND_ALPHA;
    blend.first = G2D_LAYER_OBJ;
    blend.second = G2D_LAYER_BG0;
    blend.eva = 10;
    blend.evb = 6;
    memset(&desc, 0, sizeof(desc));
    desc.bgs = &layer;
    desc.n_bgs = 1;
    desc.objs = &obj;
    desc.n_objs = 1;
    desc.backdrop_pal = &pal;
    desc.windows = &win;
    desc.blend = &blend;
    g2d_frame_compose_desc(&desc, W, H, asset);

    reset();
    load_title(&nscr, &ncgr, &pal, 13, 37, 3);
    load_pose(0, &rig, 0x06400000u);
    io16(0x04000040u, (u16)((40u << 8) | 200u));
    io16(0x04000044u, (u16)((30u << 8) | 150u));
    io16(0x04000048u, 0x31);
    io16(0x0400004Au, 0x11);
    io16(0x04000050u, (u16)(0x10u | (1u << 6) | (0x01u << 8)));
    io16(0x04000052u, (u16)(10u | (6u << 8)));
    io32(0x04000000u, 0x10000u | 0x0100u | 0x1000u | 0x10u | 0x2000u | (1u << 24));
    render(NULL);
    BW_CHECK_EQ("title + pose + window + blend: hw == asset",
                cmp_screens("combined", g_top, asset), 0);

    g2d_pose_free(&pose);
    g2d_rig_free(&rig);
    bw_arc_close(&rig_arc);
out_title:
    g2d_nscr_free(&nscr);
    g2d_ncgr_free(&ncgr);
    g2d_palette_free(&pal);
    free(nscr_buf);
    free(ncgr_buf);
    free(nclr_buf);
    bw_arc_close(&title_arc);
    return 0;
}

int main(int argc, char **argv)
{
    BW_TEST_SETUP(argc, argv);

    test_display();
    test_text_priority();
    test_objs();
    test_windows();
    test_blend();
    test_affine_bg();
    test_extpal();
    test_3d();
    test_3d_alpha();
    test_mosaic();
    test_capture();
    if (test_against_assets() != 0) {
        BW_REPORT("test_hwcompose");
    }

    BW_REPORT("test_hwcompose");
}
