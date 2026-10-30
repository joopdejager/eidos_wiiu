/*
 * tests/test_g3dhw.c -- engine/g3d/hw3d.c, the 3D hardware interface.
 *
 *     ./test_g3dhw
 *
 * Synthetic, no game data.  The address space is the real sdk/dsmem.c, and
 * every register store below goes through bw_ds_ptr exactly the way a
 * matched store does after tools/dsaddr.py's rewrite (one translation per
 * store), so the access-hook protocol hw3d.c relies on is what is tested.
 *
 *   1. One display list (every vertex encoding, the strips, push/pop, a
 *      translucent polygon, two textures out of VRAM through the texture
 *      slots, one with a palette base) rendered by the interpreter
 *      (g3d_gx_run + g3d_raster_geom) and by the hardware path, and the two
 *      frames required pixel-identical: packed into the FIFO word by word,
 *      through the direct command ports, through MIi_CpuSend32 and through
 *      MI_SendGXCommandAsync.
 *   2. The SDK's own shapes of FIFO traffic: an opcode store followed by
 *      GX_SendFifo64B / MI_Copy36B / MIi_CpuSend32 to a fresh &FIFO, an
 *      integer FIFO address, GXi_NopClearFifo128_, and the clip and vector
 *      matrices read back through MI_Copy64B / MI_Copy36B from raw result
 *      addresses.
 *   3. GXSTAT: the position/vector and projection stack levels, the stack
 *      error and its acknowledgement through `GXSTAT |= 0x8000`, the FIFO
 *      interrupt field, FIFO empty; RAM_COUNT; POS_TEST, VEC_TEST and
 *      BOX_TEST results.
 *   4. SWAP_BUFFERS: the frame count, the clear colour and alpha, texturing
 *      off, the viewport, toon shading, the alpha test, an unterminated
 *      vertex list, the unsupported-feature flags, and the frame through
 *      engine/g2d/hwcompose.c as BG0.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsmem.h"
#include "dsmem_ext.h"
#include "g2d.h"
#include "g3d.h"
#include "hw3d.h"
#include "port.h"
#include "port_test.h"

/* The shims under test (sdk/g3_fifo.c, sdk/mi_gx.c). */
void GX_SendFifo64B(const void *src);
void GXi_NopClearFifo128_(void *dst);
void MIi_CpuSend32(const void *src, volatile void *dst, u32 size);
void MI_Copy36B(const void *src, void *dst);
void MI_Copy64B(const void *src, void *dst);
void MI_SendGXCommandAsync(u32 dma, const void *src, u32 size,
                           void (*cb)(void *), void *arg);

/* ------------------------------------------------------------------ */
/* Matched-style register access                                       */
/* ------------------------------------------------------------------ */

static void w32(u32 addr, u32 v)
{
    *(volatile u32 *)bw_ds_ptr(addr) = v;
}

static void w16(u32 addr, u16 v)
{
    *(volatile u16 *)bw_ds_ptr_w(addr, sizeof(volatile u16)) = v;
}

static u32 r32(u32 addr)
{
    return *(volatile u32 *)bw_ds_ptr(addr);
}

static void boot(void)
{
    bw_ds_reset();
    g3d_hw_reset();
}

/* ------------------------------------------------------------------ */
/* A command list builder                                              */
/* ------------------------------------------------------------------ */

typedef struct Cmd {
    u32 op, n;
    u32 p[32];
} Cmd;

static Cmd g_cmds[512];
static u32 g_ncmd;

static void cmd(u32 op, u32 n, ...)
{
    va_list ap;
    u32 i;
    Cmd *c = &g_cmds[g_ncmd++];

    c->op = op;
    c->n = n;
    va_start(ap, n);
    for (i = 0; i < n; i++) {
        c->p[i] = va_arg(ap, u32);
    }
    va_end(ap);
}

static void cmd_mtx(u32 op, const G3dMtx *m, u32 rows, u32 cols)
{
    Cmd *c = &g_cmds[g_ncmd++];
    u32 r, k;

    c->op = op;
    c->n = rows * cols;
    for (r = 0; r < rows; r++) {
        for (k = 0; k < cols; k++) {
            c->p[r * cols + k] = (u32)m->m[r * 4 + k];
        }
    }
}

static u32 v16(s32 x, s32 y)
{
    return ((u32)x & 0xFFFFu) | (((u32)y & 0xFFFFu) << 16);
}

static u32 v10(s32 x, s32 y, s32 z)
{
    return ((u32)x & 0x3FFu) | (((u32)y & 0x3FFu) << 10) |
           (((u32)z & 0x3FFu) << 20);
}

/* Packed: four commands per command word, their parameters after it.
 * `ram` 0 lays the words out as a file does (little-endian, what
 * g3d_gx_run reads); 1 as the game's own stores leave them in DS memory
 * (bw_mem_wr32, what the FIFO path reads). */
static u32 pack_as(u8 *out, const Cmd *cmds, u32 n, int ram)
{
    u32 len = 0, i, k;

    for (i = 0; i < n; i += 4u) {
        u32 word = 0;
        for (k = 0; k < 4u && i + k < n; k++) {
            word |= cmds[i + k].op << (k * 8u);
        }
        (ram ? bw_mem_wr32 : bw_wr32)(out + len, word);
        len += 4u;
        for (k = 0; k < 4u && i + k < n; k++) {
            u32 j;
            for (j = 0; j < cmds[i + k].n; j++) {
                (ram ? bw_mem_wr32 : bw_wr32)(out + len, cmds[i + k].p[j]);
                len += 4u;
            }
        }
    }
    return len;
}

static u32 pack(u8 *out, const Cmd *cmds, u32 n)
{
    return pack_as(out, cmds, n, 0);
}

/* ------------------------------------------------------------------ */
/* The scene                                                           */
/* ------------------------------------------------------------------ */

#define TEX0_ADDR   0x0000u    /* PAL16 8x8 at texture slot 0 + 0      */
#define TEX1_ADDR   0x0100u    /* A3I5 8x8 at texture slot 0 + 0x100   */
#define PAL0_BASE   0u         /* 16-byte units                        */
#define PAL1_BASE   2u

static u8  g_tex0[32], g_tex1[64];
static u8  g_pal[64 + 64];
static u32 g_teximage0, g_teximage1;

static void make_textures(void)
{
    u32 i;

    for (i = 0; i < sizeof(g_tex0); i++) {
        g_tex0[i] = (u8)(i * 37u + 11u);
    }
    for (i = 0; i < sizeof(g_tex1); i++) {
        /* A3I5: index 0..31, alpha 0..7, both varying */
        g_tex1[i] = (u8)((i % 32u) | (((i / 8u) & 7u) << 5));
    }
    for (i = 0; i < sizeof(g_pal) / 2u; i++) {
        u16 c = (u16)((i * 2u) & 31u) | (u16)(((31u - i) & 31u) << 5) |
                (u16)(((i * 5u) & 31u) << 10);
        bw_mem_wr16(&g_pal[i * 2u], c);
    }
    /* repeat S and T, 8x8, format, address in 8-byte units */
    g_teximage0 = (TEX0_ADDR >> 3) | (3u << 16) | (3u << 26);
    g_teximage1 = (TEX1_ADDR >> 3) | (1u << 16) | (1u << 18) | (1u << 26);
}

/* VRAM A as texture slot 0, VRAM E as texture palette, and the bytes. */
static void load_vram(void)
{
    u32 sz;
    u8 *a, *e;

    w16(0x04000240u, 0x83u);      /* VRAMCNT_A: MST 3, slot 0 */
    *(volatile u8 *)bw_ds_ptr_w(0x04000244u, sizeof(*(volatile u8 *)0)) = 0x83u;   /* VRAMCNT_E: MST 3 */
    bw_vram_remap();
    a = bw_vram_bank(BW_VRAM_A, &sz);
    e = bw_vram_bank(BW_VRAM_E, &sz);
    memcpy(a + TEX0_ADDR, g_tex0, sizeof(g_tex0));
    memcpy(a + TEX1_ADDR, g_tex1, sizeof(g_tex1));
    memcpy(e, g_pal, sizeof(g_pal));
}

static void build_scene(void)
{
    G3dMtx proj;
    const u32 attr_opaque = 0x001F00C0u;   /* both faces, alpha 31 */
    const u32 attr_trans = 0x001000C0u;    /* alpha 16 */

    g_ncmd = 0;
    g3d_mtx_perspective(&proj, 0x2000u, 5461, FX32_ONE / 4, FX32_ONE * 64);
    cmd(0x10, 1, 0u);                       /* MTX_MODE projection */
    cmd_mtx(0x16, &proj, 4, 4);             /* MTX_LOAD_4x4 */
    cmd(0x10, 1, 2u);                       /* MTX_MODE position+vector */
    cmd(0x15, 0);                           /* MTX_IDENTITY */
    cmd(0x1C, 3, 0u, 0u, (u32)(-4 * FX32_ONE));   /* MTX_TRANS */

    /* a red triangle, VTX_16 */
    cmd(0x29, 1, attr_opaque);
    cmd(0x20, 1, 31u);
    cmd(0x40, 1, 0u);
    cmd(0x23, 2, v16(-FX32_ONE, -FX32_ONE), 0u);
    cmd(0x23, 2, v16(FX32_ONE, -FX32_ONE), 0u);
    cmd(0x23, 2, v16(0, FX32_ONE), 0u);
    cmd(0x41, 0);

    /* a quad strip under a pushed translation and scale, VTX_10 + DIFF */
    cmd(0x11, 0);
    cmd(0x1C, 3, (u32)(FX32_ONE + FX32_ONE / 2), (u32)(FX32_ONE / 2), 0u);
    cmd(0x1B, 3, (u32)(FX32_ONE / 2), (u32)FX32_ONE, (u32)FX32_ONE);
    cmd(0x20, 1, 31u << 5);
    cmd(0x40, 1, 3u);
    cmd(0x24, 1, v10(-64, -64, 0));
    cmd(0x24, 1, v10(64, -64, 0));
    cmd(0x24, 1, v10(-64, 0, 0));
    cmd(0x24, 1, v10(64, 0, 0));
    cmd(0x28, 1, v10(-256, 256, 0));        /* VTX_DIFF: (-1/16, +1/16, 0) */
    cmd(0x28, 1, v10(511, 0, 0));
    cmd(0x41, 0);
    cmd(0x12, 1, 1u);                       /* MTX_POP 1 */

    /* a translucent triangle strip, VTX_XY / XZ / YZ */
    cmd(0x29, 1, attr_trans);
    cmd(0x20, 1, 31u << 10);
    cmd(0x40, 1, 2u);
    cmd(0x23, 2, v16(-2 * FX32_ONE, 0), (u32)(FX32_ONE / 2));
    cmd(0x25, 1, v16(-FX32_ONE, FX32_ONE));
    cmd(0x26, 1, v16(0, FX32_ONE / 2));
    cmd(0x27, 1, v16(-FX32_ONE, 0));
    cmd(0x41, 0);

    /* a textured quad, PAL16 */
    cmd(0x29, 1, attr_opaque);
    cmd(0x2A, 1, g_teximage0);
    cmd(0x2B, 1, PAL0_BASE);
    cmd(0x20, 1, 0x7FFFu);
    cmd(0x40, 1, 1u);
    cmd(0x22, 1, v16(0, 0));
    cmd(0x23, 2, v16(-2 * FX32_ONE, -2 * FX32_ONE), 0u);
    cmd(0x22, 1, v16(16 * 16, 0));
    cmd(0x23, 2, v16(0, -2 * FX32_ONE), 0u);
    cmd(0x22, 1, v16(16 * 16, 16 * 16));
    cmd(0x23, 2, v16(0, -FX32_ONE / 2), 0u);
    cmd(0x22, 1, v16(0, 16 * 16));
    cmd(0x23, 2, v16(-2 * FX32_ONE, -FX32_ONE / 2), 0u);
    cmd(0x41, 0);

    /* a textured triangle, A3I5 with a palette base and a flip */
    cmd(0x2A, 1, g_teximage1);
    cmd(0x2B, 1, PAL1_BASE);
    cmd(0x20, 1, 0x3DEFu);
    cmd(0x40, 1, 0u);
    cmd(0x22, 1, v16(0, 0));
    cmd(0x23, 2, v16(FX32_ONE / 2, -2 * FX32_ONE), (u32)FX32_ONE);
    cmd(0x22, 1, v16(24 * 16, 0));
    cmd(0x23, 2, v16(2 * FX32_ONE, -2 * FX32_ONE), (u32)FX32_ONE);
    cmd(0x22, 1, v16(12 * 16, 20 * 16));
    cmd(0x23, 2, v16(FX32_ONE, 0), (u32)FX32_ONE);
    cmd(0x41, 0);
}

/* The interpreter's texture lookup: decode from the test's own arrays,
 * with the addressing written out independently of hw3d.c. */
typedef struct RefTex {
    G3dGeom *geom;
    G3dTexture t0, t1;
} RefTex;

static const G3dTexture *ref_lookup(void *user, u32 i)
{
    RefTex *r = (RefTex *)user;
    u32 tp = r->geom->polys[i].teximage;

    if (G3D_TEX_FORMAT(tp) == 0) {
        return NULL;
    }
    return (tp & 0xFFFFu) == (TEX0_ADDR >> 3) ? &r->t0 : &r->t1;
}

static void clear_rgb(u32 cc, u8 out[4])
{
    out[0] = (u8)((cc & 31u) * 255u / 31u);
    out[1] = (u8)(((cc >> 5) & 31u) * 255u / 31u);
    out[2] = (u8)(((cc >> 10) & 31u) * 255u / 31u);
    out[3] = (u8)(((cc >> 16) & 31u) * 255u / 31u);
}

#define CLEAR_COLOR_VALUE (4u | (8u << 5) | (12u << 10) | (31u << 16))

static G3dTarget g_ref;

static void render_reference(int textured)
{
    u8 bytes[16384];
    u32 len = pack(bytes, g_cmds, g_ncmd);
    G3dGeom g;
    G3dGx gx;
    RefTex rt;
    u8 c[4];

    g3d_geom_init(&g);
    g3d_gx_init(&gx, &g);
    (void)g3d_gx_run(&gx, bytes, len, 0, len, len);
    if (g_ref.rgba == NULL) {
        (void)g3d_target_init(&g_ref, G3D_SCREEN_W, G3D_SCREEN_H);
    }
    clear_rgb(CLEAR_COLOR_VALUE, c);
    g3d_target_clear(&g_ref, c[0], c[1], c[2], c[3]);
    memset(&rt, 0, sizeof(rt));
    rt.geom = &g;
    (void)g3d_texture_decode(&rt.t0, 3, 8, 8, g_tex0, sizeof(g_tex0),
                             g_pal + PAL0_BASE * 16u, 32u, 0);
    (void)g3d_texture_decode(&rt.t1, 1, 8, 8, g_tex1, sizeof(g_tex1),
                             g_pal + PAL1_BASE * 16u, 64u, 0);
    g3d_raster_geom(&g_ref, &g, textured ? ref_lookup : NULL, &rt);
    g3d_texture_free(&rt.t0);
    g3d_texture_free(&rt.t1);
    g3d_geom_free(&g);
}

static void hw_setup_frame(u32 disp3dcnt)
{
    w16(G3D_HW_DISP3DCNT, (u16)disp3dcnt);
    w32(G3D_HW_CLEAR_COLOR, CLEAR_COLOR_VALUE);
    w16(G3D_HW_CLEAR_DEPTH, 0x7FFFu);
}

static u32 frame_diff(const u8 *a, const u8 *b)
{
    u32 i, n = 0;

    if (a == NULL || b == NULL) {
        return 0xFFFFFFFFu;
    }
    for (i = 0; i < G3D_SCREEN_W * G3D_SCREEN_H; i++) {
        if (memcmp(a + i * 4u, b + i * 4u, 4) != 0) {
            n++;
        }
    }
    return n;
}

static u32 covered(const u8 *f)
{
    u8 c[4];
    u32 i, n = 0;

    clear_rgb(CLEAR_COLOR_VALUE, c);
    for (i = 0; i < G3D_SCREEN_W * G3D_SCREEN_H; i++) {
        if (memcmp(f + i * 4u, c, 3) != 0) {
            n++;
        }
    }
    return n;
}

static int g_cb_calls;

static void count_cb(void *arg)
{
    (void)arg;
    g_cb_calls++;
}

/* ------------------------------------------------------------------ */
/* 1. the same list, interpreter against hardware                      */
/* ------------------------------------------------------------------ */

static void test_identity(void)
{
    u8 bytes[16384];
    u32 len, i, j, base_frames;
    u32 cov;

    make_textures();
    build_scene();
    render_reference(1);
    cov = covered(g_ref.rgba);
    printf("reference frame: %lu of %lu pixels covered\n",
           (unsigned long)cov, (unsigned long)(G3D_SCREEN_W * G3D_SCREEN_H));
    BW_CHECK_EQ("reference covers something", cov > 4000u, 1);

    len = pack_as(bytes, g_cmds, g_ncmd, 1);

    /* a. packed, one translated store per word, swap through its port */
    boot();
    load_vram();
    hw_setup_frame(0x0009u);
    for (i = 0; i < len; i += 4u) {
        w32(G3D_HW_GXFIFO, bw_mem_rd32(bytes + i));
    }
    w32(G3D_HW_SWAP_BUFFERS, 0u);
    bw_io_sync();
    BW_CHECK_EQ("packed: one frame", g3d_hw_frame_count(), 1);
    BW_CHECK_EQ("packed: pixels differing", frame_diff(g3d_hw_frame(), g_ref.rgba), 0);
    BW_CHECK_EQ("packed: commands run", g3d_hw_gx()->commands, g_ncmd);

    /* b. the direct command ports, swap packed */
    boot();
    load_vram();
    hw_setup_frame(0x0009u);
    for (i = 0; i < g_ncmd; i++) {
        u32 port = G3D_HW_GXFIFO + g_cmds[i].op * 4u;
        if (g_cmds[i].n == 0) {
            w32(port, 0u);
        }
        for (j = 0; j < g_cmds[i].n; j++) {
            w32(port, g_cmds[i].p[j]);
        }
    }
    w32(G3D_HW_GXFIFO, 0x50u);
    w32(G3D_HW_GXFIFO, 0u);
    g3d_hw_flush();
    BW_CHECK_EQ("ports: pixels differing", frame_diff(g3d_hw_frame(), g_ref.rgba), 0);

    /* c. MIi_CpuSend32 to a translated &FIFO, then MI_SendGXCommandAsync
     *    twice (two frames, the second identical to the first) */
    boot();
    load_vram();
    hw_setup_frame(0x0009u);
    MIi_CpuSend32(bytes, bw_ds_ptr(G3D_HW_GXFIFO), len);
    w32(G3D_HW_SWAP_BUFFERS, 0u);
    bw_io_sync();
    BW_CHECK_EQ("CpuSend32: pixels differing", frame_diff(g3d_hw_frame(), g_ref.rgba), 0);
    base_frames = g3d_hw_frame_count();
    g_cb_calls = 0;
    {
        /* TEXIMAGE_PARAM persists across a swap, as on hardware, and the
         * list's first polygons assume the power-on 0: reset it first. */
        u8 with_swap[16384 + 16];
        bw_mem_wr32(with_swap, 0x2Au);
        bw_mem_wr32(with_swap + 4u, 0u);
        memcpy(with_swap + 8u, bytes, len);
        bw_mem_wr32(with_swap + 8u + len, 0x50u);
        bw_mem_wr32(with_swap + 12u + len, 0u);
        MI_SendGXCommandAsync(0, with_swap, len + 16u, count_cb, NULL);
    }
    BW_CHECK_EQ("SendGXCommandAsync: callback ran", g_cb_calls, 1);
    BW_CHECK_EQ("SendGXCommandAsync: a frame", g3d_hw_frame_count(), base_frames + 1u);
    BW_CHECK_EQ("SendGXCommandAsync: pixels differing",
                frame_diff(g3d_hw_frame(), g_ref.rgba), 0);

    /* d. texturing off in DISP3DCNT against the interpreter untextured */
    render_reference(0);
    boot();
    load_vram();
    hw_setup_frame(0x0008u);
    for (i = 0; i < len; i += 4u) {
        w32(G3D_HW_GXFIFO, bw_mem_rd32(bytes + i));
    }
    w32(G3D_HW_SWAP_BUFFERS, 0u);
    g3d_hw_flush();
    BW_CHECK_EQ("texturing off: pixels differing",
                frame_diff(g3d_hw_frame(), g_ref.rgba), 0);
}

/* ------------------------------------------------------------------ */
/* 2. the SDK's shapes of FIFO traffic                                 */
/* ------------------------------------------------------------------ */

static void expect_clip(const char *what, const G3dMtx *want)
{
    s32 got[16];
    u32 i, bad = 0;

    /* G3X_GetClipMtx: MI_Copy64B((const void *)REG_CLIPMTX_RESULT_0_ADDR, m),
     * the address a macro, so the raw DS value reaches the shim. */
    MI_Copy64B((const void *)(uintptr_t)G3D_HW_CLIPMTX_RESULT, got);
    for (i = 0; i < 16u; i++) {
        if (got[i] != want->m[i]) {
            bad++;
        }
    }
    BW_CHECK_EQ(what, bad, 0);
}

static void test_sdk_paths(void)
{
    G3dMtx a, b, want, t, id;
    u32 words[16], i;
    u32 m33[9];
    s32 vm[9];

    boot();
    for (i = 0; i < 16u; i++) {
        a.m[i] = (fx32)(i * 300 + 7) - 2000;
    }
    a.m[3] = a.m[7] = a.m[11] = 0;
    a.m[15] = FX32_ONE;

    /* G3_LoadMtx44 in projection mode: the opcode store, then
     * GX_SendFifo64B with the destination left implicit. */
    w32(0x04000440u, 0u);                   /* reg_G3_MTX_MODE = 0 */
    w32(G3D_HW_GXFIFO, 0x16u);
    for (i = 0; i < 16u; i++) {
        words[i] = (u32)a.m[i];
    }
    GX_SendFifo64B(words);

    /* G3_MultTransMtx33 in position mode: 0x19, MI_Copy36B to a fresh
     * &FIFO, then the translation as three stores. */
    w32(0x04000440u, 1u);
    for (i = 0; i < 9u; i++) {
        m33[i] = (u32)(FX32_ONE * ((i % 4u) == 0 ? 1 : 0)) + i * 5u;
    }
    w32(G3D_HW_GXFIFO, 0x19u);
    MI_Copy36B(m33, bw_ds_ptr(G3D_HW_GXFIFO));
    w32(G3D_HW_GXFIFO, (u32)FX32_ONE);
    w32(G3D_HW_GXFIFO, (u32)(2 * FX32_ONE));
    w32(G3D_HW_GXFIFO, (u32)(-3 * FX32_ONE));

    g3d_mtx_identity(&b);
    for (i = 0; i < 9u; i++) {
        b.m[(i / 3u) * 4u + (i % 3u)] = (fx32)m33[i];
    }
    b.m[12] = FX32_ONE;
    b.m[13] = 2 * FX32_ONE;
    b.m[14] = -3 * FX32_ONE;
    g3d_mtx_identity(&id);
    g3d_mtx_mul(&t, &b, &id);              /* pos = param * identity */
    g3d_mtx_mul(&want, &t, &a);            /* clip = pos * proj */
    expect_clip("LoadMtx44 + MultTransMtx33: clip matrix", &want);

    /* func_0206DDA4's shape: the opcode, then MIi_CpuSend32 to a fresh
     * (void *)0x04000400; and func_0206DB80's: an integer destination. */
    w32(G3D_HW_GXFIFO, 0x1Cu);              /* MTX_TRANS */
    {
        u32 tr[3] = { (u32)FX32_ONE, 0u, 0u };
        MIi_CpuSend32(tr, bw_ds_ptr(G3D_HW_GXFIFO), 12u);
    }
    {
        u32 list[4] = { 0x1Cu, 0u, (u32)FX32_ONE, 0u };
        MIi_CpuSend32(list, (volatile void *)(uintptr_t)G3D_HW_GXFIFO, 16u);
    }
    g3d_mtx_translate(&b, FX32_ONE, FX32_ONE, 0);
    g3d_mtx_mul(&t, &b, &want);
    expect_clip("CpuSend32 (pointer and integer): clip matrix", &t);

    /* An access with no store must not inject a word: take &FIFO, store
     * nothing, then a complete command. */
    {
        u32 before = g3d_hw_gx()->commands;
        (void)bw_ds_ptr(G3D_HW_GXFIFO);
        w32(0x04000454u, 0u);               /* reg_G3_MTX_IDENTITY */
        g3d_hw_flush();
        BW_CHECK_EQ("no store, no command", g3d_hw_gx()->commands, before + 1u);
    }

    /* GXi_NopClearFifo128_ completes a half-sent command with zeros. */
    w32(G3D_HW_GXFIFO, 0x1Cu);
    w32(G3D_HW_GXFIFO, (u32)FX32_ONE);      /* one of three parameters */
    GXi_NopClearFifo128_(bw_ds_ptr(G3D_HW_GXFIFO));
    g3d_mtx_translate(&b, FX32_ONE, 0, 0);
    g3d_mtx_mul(&t, &b, &a);                /* pos was identity */
    expect_clip("NopClearFifo128 completes the command", &t);

    /* G3X_GetVectorMtx: MI_Copy36B from the raw VECMTX_RESULT address. */
    w32(0x04000440u, 2u);
    w32(0x04000454u, 0u);
    w32(0x0400046Cu, (u32)(2 * FX32_ONE));  /* MTX_SCALE: pos only */
    w32(0x0400046Cu, (u32)(3 * FX32_ONE));
    w32(0x0400046Cu, (u32)(4 * FX32_ONE));
    MI_Copy36B((const void *)(uintptr_t)G3D_HW_VECMTX_RESULT, vm);
    BW_CHECK_EQ("vector matrix not scaled [0]", vm[0], FX32_ONE);
    BW_CHECK_EQ("vector matrix not scaled [4]", vm[4], FX32_ONE);
    BW_CHECK_EQ("vector matrix [1]", vm[1], 0);
    /* and the clip matrix word by word through literal reads */
    BW_CHECK_EQ("clip [0] by register read", (s32)r32(G3D_HW_CLIPMTX_RESULT),
                FX_MUL(2 * FX32_ONE, a.m[0]));
}

/* ------------------------------------------------------------------ */
/* 3. GXSTAT, RAM_COUNT, the tests                                     */
/* ------------------------------------------------------------------ */

static void test_status(void)
{
    u32 s, i;
    volatile u32 *p;

    boot();
    s = r32(G3D_HW_GXSTAT);
    BW_CHECK_EQ("fresh: FIFO empty", (s >> 26) & 1u, 1);
    BW_CHECK_EQ("fresh: less than half", (s >> 25) & 1u, 1);
    BW_CHECK_EQ("fresh: busy", (s >> 27) & 1u, 0);
    BW_CHECK_EQ("fresh: FIFO entries", (s >> 16) & 0x1FFu, 0);
    BW_CHECK_EQ("fresh: PV level", (s >> 8) & 0x1Fu, 0);

    w32(0x04000440u, 2u);
    w32(0x04000444u, 0u);
    w32(0x04000444u, 0u);
    w32(0x04000444u, 0u);
    BW_CHECK_EQ("three pushes: PV level", (r32(G3D_HW_GXSTAT) >> 8) & 0x1Fu, 3);
    w32(0x04000448u, 1u);
    BW_CHECK_EQ("pop 1: PV level", (r32(G3D_HW_GXSTAT) >> 8) & 0x1Fu, 2);
    w32(0x04000448u, 0u);                   /* pop 0: no move, no error */
    BW_CHECK_EQ("pop 0: PV level", (r32(G3D_HW_GXSTAT) >> 8) & 0x1Fu, 2);
    BW_CHECK_EQ("pop 0: no error", (g3d_hw_gxstat() >> 15) & 1u, 0);
    w32(0x04000448u, 0x3Fu);                /* pop -1: one level up */
    BW_CHECK_EQ("pop -1: PV level", (r32(G3D_HW_GXSTAT) >> 8) & 0x1Fu, 3);

    w32(0x04000440u, 0u);
    w32(0x04000444u, 0u);
    BW_CHECK_EQ("projection push: PJ level", (r32(G3D_HW_GXSTAT) >> 13) & 1u, 1);
    w32(0x04000444u, 0u);
    BW_CHECK_EQ("second projection push: error", (g3d_hw_gxstat() >> 15) & 1u, 1);
    BW_CHECK_EQ("error reads 0 in the register file", (r32(G3D_HW_GXSTAT) >> 15) & 1u, 0);
    w32(0x04000448u, 1u);
    BW_CHECK_EQ("projection pop: PJ level", (r32(G3D_HW_GXSTAT) >> 13) & 1u, 0);

    /* G3X_Reset's acknowledgement: one translation, a read-modify-write. */
    p = (volatile u32 *)bw_ds_ptr(G3D_HW_GXSTAT);
    *p |= 0x8000u;
    bw_io_sync();
    BW_CHECK_EQ("GXSTAT |= 0x8000 acknowledges", (g3d_hw_gxstat() >> 15) & 1u, 0);

    /* G3X_Init's FIFO interrupt field. */
    p = (volatile u32 *)bw_ds_ptr(G3D_HW_GXSTAT);
    *p = (*p & ~0xC0000000u) | 0x80000000u;
    bw_io_sync();
    BW_CHECK_EQ("FIFO interrupt condition", r32(G3D_HW_GXSTAT) >> 30, 2);

    /* overflow the position stack: level 3, 28 pushes to 31, the 29th
     * overflows (the pointer is 6 bits and GXSTAT shows its low 5) */
    w32(0x04000440u, 2u);
    for (i = 0; i < 28u; i++) {
        w32(0x04000444u, 0u);
    }
    BW_CHECK_EQ("level 31", (r32(G3D_HW_GXSTAT) >> 8) & 0x1Fu, 31);
    BW_CHECK_EQ("level 31: no error", (g3d_hw_gxstat() >> 15) & 1u, 0);
    w32(0x04000444u, 0u);
    g3d_hw_flush();
    BW_CHECK_EQ("position stack overflow: error", (g3d_hw_gxstat() >> 15) & 1u, 1);
    w32(0x04000448u, 1u);

    /* G3X_InitMtxStack: acknowledge, read both levels, pop them. */
    p = (volatile u32 *)bw_ds_ptr(G3D_HW_GXSTAT);
    *p |= 0x8000u;
    {
        u32 pv = (r32(G3D_HW_GXSTAT) & 0x1F00u) >> 8;
        u32 pj = (r32(G3D_HW_GXSTAT) & 0x2000u) >> 13;
        w32(0x04000440u, 3u);
        w32(0x04000454u, 0u);
        w32(0x04000440u, 0u);
        if (pj) {
            w32(0x04000448u, pj);
        }
        w32(0x04000454u, 0u);
        w32(0x04000440u, 2u);
        w32(0x04000448u, pv);
        w32(0x04000454u, 0u);
    }
    s = r32(G3D_HW_GXSTAT);
    BW_CHECK_EQ("InitMtxStack: PV level", (s >> 8) & 0x1Fu, 0);
    BW_CHECK_EQ("InitMtxStack: PJ level", (s >> 13) & 1u, 0);
    BW_CHECK_EQ("InitMtxStack: no error", (g3d_hw_gxstat() >> 15) & 1u, 0);

    /* RAM_COUNT: two triangles, six vertices; cleared by the swap. */
    w32(0x04000500u, 0u);                   /* BEGIN_VTXS triangles */
    for (i = 0; i < 6u; i++) {
        w32(0x0400048Cu, v16((s32)i * 100, 0));
        w32(0x0400048Cu, 0u);
    }
    w32(0x04000504u, 0u);
    s = r32(G3D_HW_RAM_COUNT);
    BW_CHECK_EQ("RAM_COUNT polygons", s & 0xFFFu, 2);
    BW_CHECK_EQ("RAM_COUNT vertices", (s >> 16) & 0x1FFFu, 6);
    w32(G3D_HW_SWAP_BUFFERS, 0u);
    BW_CHECK_EQ("RAM_COUNT after swap", r32(G3D_HW_RAM_COUNT), 0);

    /* POS_TEST through identity projection and a translation. */
    w32(0x04000440u, 0u);
    w32(0x04000454u, 0u);
    w32(0x04000440u, 1u);
    w32(0x04000454u, 0u);
    w32(0x04000470u, (u32)(10 * FX32_ONE));   /* MTX_TRANS x */
    w32(0x04000470u, 0u);
    w32(0x04000470u, 0u);
    w32(0x040005C4u, v16(FX32_ONE, 2 * FX32_ONE));
    w32(0x040005C4u, (u32)(3 * FX32_ONE));
    BW_CHECK_EQ("POS_RESULT x", (s32)r32(G3D_HW_POS_RESULT), 11 * FX32_ONE);
    BW_CHECK_EQ("POS_RESULT y", (s32)r32(G3D_HW_POS_RESULT + 4u), 2 * FX32_ONE);
    BW_CHECK_EQ("POS_RESULT z", (s32)r32(G3D_HW_POS_RESULT + 8u), 3 * FX32_ONE);
    BW_CHECK_EQ("POS_RESULT w", (s32)r32(G3D_HW_POS_RESULT + 12u), FX32_ONE);
    BW_CHECK_EQ("POS_TEST sets the current vertex", g3d_hw_gx()->cx, FX32_ONE);

    /* VEC_TEST through the identity vector matrix: 1.0.9 in, 12-bit out. */
    w32(0x04000440u, 2u);
    w32(0x04000454u, 0u);
    w32(0x040005C8u, v10(256, -256, 511));
    g3d_hw_flush();
    BW_CHECK_EQ("VEC_RESULT x", *(volatile u16 *)bw_ds_ptr_w(G3D_HW_VEC_RESULT, sizeof(*(volatile u16 *)0)), 0x0800);
    BW_CHECK_EQ("VEC_RESULT y", *(volatile u16 *)bw_ds_ptr_w(G3D_HW_VEC_RESULT + 2u, sizeof(*(volatile u16 *)0)), 0xF800);
    BW_CHECK_EQ("VEC_RESULT z", *(volatile u16 *)bw_ds_ptr_w(G3D_HW_VEC_RESULT + 4u, sizeof(*(volatile u16 *)0)), 0x0FF8);

    /* BOX_TEST with identity matrices: inside, outside, straddling. */
    w32(0x040005C0u, v16(-FX32_ONE / 2, -FX32_ONE / 2));
    w32(0x040005C0u, v16(-FX32_ONE / 2, FX32_ONE));
    w32(0x040005C0u, v16(FX32_ONE, FX32_ONE));
    BW_CHECK_EQ("BOX_TEST inside", (r32(G3D_HW_GXSTAT) >> 1) & 1u, 1);
    BW_CHECK_EQ("BOX_TEST busy", r32(G3D_HW_GXSTAT) & 1u, 0);
    w32(0x040005C0u, v16(2 * FX32_ONE, 0));
    w32(0x040005C0u, v16(0, FX32_ONE / 2));
    w32(0x040005C0u, v16(FX32_ONE / 2, FX32_ONE / 2));
    BW_CHECK_EQ("BOX_TEST outside", (r32(G3D_HW_GXSTAT) >> 1) & 1u, 0);
    w32(0x040005C0u, v16(FX32_ONE / 2, -3 * FX32_ONE));
    w32(0x040005C0u, v16(-FX32_ONE / 4, FX32_ONE));
    w32(0x040005C0u, v16(6 * FX32_ONE, FX32_ONE / 2));
    BW_CHECK_EQ("BOX_TEST straddling", (r32(G3D_HW_GXSTAT) >> 1) & 1u, 1);
}

/* ------------------------------------------------------------------ */
/* 4. SWAP_BUFFERS and the frame                                       */
/* ------------------------------------------------------------------ */

static const u8 *px(u32 x, u32 y)
{
    return g3d_hw_frame() + ((size_t)y * G3D_SCREEN_W + x) * 4u;
}

/* A full-screen quad at depth 0 with identity matrices. */
static void fullscreen_quad(u32 attr, u32 color)
{
    w32(0x040004A4u, attr);
    w32(0x04000480u, color);
    w32(0x04000500u, 1u);
    w32(0x0400048Cu, v16(-FX32_ONE, -FX32_ONE)); w32(0x0400048Cu, 0u);
    w32(0x0400048Cu, v16(FX32_ONE, -FX32_ONE));  w32(0x0400048Cu, 0u);
    w32(0x0400048Cu, v16(FX32_ONE, FX32_ONE));   w32(0x0400048Cu, 0u);
    w32(0x0400048Cu, v16(-FX32_ONE, FX32_ONE));  w32(0x0400048Cu, 0u);
    w32(0x04000504u, 0u);
}

static void test_swap(void)
{
    static u8 top[256 * 192 * 4], bottom[256 * 192 * 4];
    const u8 *f;
    u32 unsup[2];

    boot();
    BW_CHECK_EQ("no frame before the first swap", g3d_hw_frame() == NULL, 1);

    /* clear alpha 0: transparent where nothing is drawn */
    w16(G3D_HW_DISP3DCNT, 0x0008u);
    w32(G3D_HW_CLEAR_COLOR, 31u);
    w16(G3D_HW_CLEAR_DEPTH, 0x7FFFu);
    w32(G3D_HW_SWAP_BUFFERS, 0u);
    bw_io_sync();
    BW_CHECK_EQ("first swap: one frame", g3d_hw_frame_count(), 1);
    f = px(10, 10);
    BW_CHECK_EQ("clear red", f[0], 255);
    BW_CHECK_EQ("clear alpha 0", f[3], 0);

    /* the viewport: bottom-left quarter, y counted from the bottom */
    w32(G3D_HW_VIEWPORT, 0u | (0u << 8) | (127u << 16) | (95u << 24));
    fullscreen_quad(0x001F00C0u, 31u << 5);
    w32(G3D_HW_SWAP_BUFFERS, 0u);
    g3d_hw_flush();
    BW_CHECK_EQ("viewport: inside (10,180)", px(10, 180)[1], 255);
    BW_CHECK_EQ("viewport: inside (127,96)", px(127, 96)[1], 255);
    BW_CHECK_EQ("viewport: outside (128,96)", px(128, 96)[1], 0);
    BW_CHECK_EQ("viewport: outside (127,95)", px(127, 95)[1], 0);
    BW_CHECK_EQ("viewport: outside (200,20)", px(200, 20)[3], 0);
    BW_CHECK_EQ("drawn pixel opaque", px(10, 180)[3], 255);
    w32(G3D_HW_VIEWPORT, 0xBFFF0000u);

    /* toon: mode 2, the vertex red indexes the table */
    w16(G3D_HW_TOON_TABLE + 31u * 2u, (u16)(31u << 10));    /* blue */
    fullscreen_quad(0x001F00E0u, 31u);
    w32(G3D_HW_SWAP_BUFFERS, 0u);
    g3d_hw_flush();
    BW_CHECK_EQ("toon: red", px(100, 100)[0], 0);
    BW_CHECK_EQ("toon: blue", px(100, 100)[2], 255);
    /* highlight: grey from red, plus the entry */
    w16(G3D_HW_TOON_TABLE + 16u * 2u, (u16)(8u << 5));
    w16(G3D_HW_DISP3DCNT, 0x000Au);
    fullscreen_quad(0x001F00E0u, 16u);
    w32(G3D_HW_SWAP_BUFFERS, 0u);
    g3d_hw_flush();
    BW_CHECK_EQ("highlight: red", px(100, 100)[0], 16u * 255u / 31u);
    BW_CHECK_EQ("highlight: green", px(100, 100)[1],
                16u * 255u / 31u + 8u * 255u / 31u);

    /* the alpha test */
    w16(G3D_HW_DISP3DCNT, 0x000Cu);
    w16(G3D_HW_ALPHA_TEST_REF, 12u);
    fullscreen_quad(0x000A00C0u, 31u);
    w32(G3D_HW_SWAP_BUFFERS, 0u);
    g3d_hw_flush();
    BW_CHECK_EQ("alpha test rejects alpha 10 at ref 12", px(100, 100)[3], 0);
    w16(G3D_HW_ALPHA_TEST_REF, 8u);
    fullscreen_quad(0x000A00C0u, 31u);
    w32(G3D_HW_SWAP_BUFFERS, 0u);
    g3d_hw_flush();
    /* Drawn over the clear plane's alpha 0: the pixel keeps its own
     * alpha, 10, for the 2D engine to blend with. */
    BW_CHECK_EQ("alpha test passes alpha 10 at ref 8", px(100, 100)[3],
                10u * 255u / 31u);
    /* blending off: a translucent polygon is drawn opaque */
    w16(G3D_HW_DISP3DCNT, 0x0000u);
    w32(G3D_HW_CLEAR_COLOR, (31u << 10) | (31u << 16));
    fullscreen_quad(0x000A00C0u, 31u);
    w32(G3D_HW_SWAP_BUFFERS, 0u);
    g3d_hw_flush();
    BW_CHECK_EQ("blending off: pure red", px(100, 100)[2], 0);

    /* END_VTXS is a no-op on hardware: an unterminated list draws */
    w16(G3D_HW_DISP3DCNT, 0x0008u);
    w32(G3D_HW_CLEAR_COLOR, 0u);
    w32(0x040004A4u, 0x001F00C0u);
    w32(0x04000480u, 31u);
    w32(0x04000500u, 0u);
    w32(0x0400048Cu, v16(-FX32_ONE, -FX32_ONE)); w32(0x0400048Cu, 0u);
    w32(0x0400048Cu, v16(FX32_ONE, -FX32_ONE));  w32(0x0400048Cu, 0u);
    w32(0x0400048Cu, v16(0, FX32_ONE));          w32(0x0400048Cu, 0u);
    w32(G3D_HW_SWAP_BUFFERS, 0u);
    g3d_hw_flush();
    BW_CHECK_EQ("unterminated list drawn", px(128, 96)[0], 255);

    /* fog, edge marking and the w-buffer are honoured now: no flags */
    w16(G3D_HW_DISP3DCNT, 0x00A8u);
    w32(G3D_HW_SWAP_BUFFERS, 2u);
    g3d_hw_flush();
    BW_CHECK_EQ("fog, edges, w-buffer: nothing flagged", g3d_hw_unsupported(), 0);

    /* the frame as BG0 of the main engine, on the top screen */
    w16(G3D_HW_DISP3DCNT, 0x0008u);
    w32(G3D_HW_CLEAR_COLOR, 0u | (31u << 16));
    fullscreen_quad(0x001F00C0u, 31u << 5);
    w32(G3D_HW_SWAP_BUFFERS, 0u);
    bw_io_sync();
    w32(0x04000000u, 0x00010108u);          /* DISPCNT: mode 1 display, BG0, 3D */
    w16(0x04000304u, 0x8203u);              /* POWCNT1: main on top */
    BW_CHECK_EQ("hwcompose renders",
                g2d_hw_render_dsmem(g3d_hw_frame(), top, bottom, unsup), 0);
    BW_CHECK_EQ("top screen shows the 3D green", top[(96u * 256u + 128u) * 4u + 1u], 255);
    BW_CHECK_EQ("top screen 3D red 0", top[(96u * 256u + 128u) * 4u], 0);
}

/* ------------------------------------------------------------------ */
/* 5. Lighting and the rear passes                                     */
/* ------------------------------------------------------------------ */

/* A quad over clip-space [x0, x1] x [y0, y1] at depth z, identity
 * matrices, under `attr`.  `pre` runs between BEGIN_VTXS and the first
 * vertex (a NORMAL), or is NULL; `color` is a COLOR sent before BEGIN. */
static void quad_at(u32 attr, u32 color, s32 x0, s32 y0, s32 x1, s32 y1,
                    s32 z, const u32 *normal)
{
    w32(0x040004A4u, attr);
    w32(0x04000480u, color);
    w32(0x04000500u, 1u);
    if (normal != NULL) {
        w32(0x04000484u, *normal);
    }
    w32(0x0400048Cu, v16(x0, y0)); w32(0x0400048Cu, (u32)z & 0xFFFFu);
    w32(0x0400048Cu, v16(x1, y0)); w32(0x0400048Cu, (u32)z & 0xFFFFu);
    w32(0x0400048Cu, v16(x1, y1)); w32(0x0400048Cu, (u32)z & 0xFFFFu);
    w32(0x0400048Cu, v16(x0, y1)); w32(0x0400048Cu, (u32)z & 0xFFFFu);
    w32(0x04000504u, 0u);
}

static void swap(u32 param)
{
    w32(G3D_HW_SWAP_BUFFERS, param);
    g3d_hw_flush();
}

static u32 rgb_at(u32 x, u32 y)
{
    const u8 *p = px(x, y);

    return ((u32)p[0] << 16) | ((u32)p[1] << 8) | p[2];
}

static u32 e5(u32 c5)
{
    return c5 * 255u / 31u;
}

static void test_lighting(void)
{
    const u32 n_front = v10(0, 0, 511);          /* (0, 0, 511/512) */
    u32 i;

    boot();
    w16(G3D_HW_DISP3DCNT, 0x0008u);
    w32(G3D_HW_CLEAR_COLOR, 31u << 16);
    w16(G3D_HW_CLEAR_DEPTH, 0x7FFFu);

    /*
     * Light 0 along (0, 0, -1), white; diffuse red, ambient blue 8,
     * specular 0, emission green 4.  N = (0, 0, 4088) in 12 bits.
     *   diffuse level  -(L . N) = 4088
     *   red    31 * 31 * 4088 >> 17                     = 29
     *   green  emission                                 = 4
     *   blue   (8 * 31 << 12) >> 17 = 248 / 32           = 7
     */
    w32(0x040004C8u, v10(0, 0, -512));
    w32(0x040004CCu, 0x7FFFu);
    w32(0x040004C0u, 31u | (8u << 26));
    w32(0x040004C4u, (4u << 21));
    quad_at(0x001F00C1u, 0x7FFFu, -FX32_ONE, -FX32_ONE, FX32_ONE, FX32_ONE, 0,
            &n_front);
    swap(0);
    BW_CHECK_EQ("lit: diffuse + ambient + emission", rgb_at(100, 100),
                (e5(29) << 16) | (e5(4) << 8) | e5(7));

    /* No light enabled in POLYGON_ATTR: NORMAL leaves COLOR alone. */
    quad_at(0x001F00C0u, 0x7FFFu, -FX32_ONE, -FX32_ONE, FX32_ONE, FX32_ONE, 0,
            &n_front);
    swap(0);
    BW_CHECK_EQ("no light enabled: COLOR", rgb_at(100, 100), 0xFFFFFFu);
    /* Only light 1, black and with no direction: the emission alone. */
    quad_at(0x001F00C2u, 0x7FFFu, -FX32_ONE, -FX32_ONE, FX32_ONE, FX32_ONE, 0,
            &n_front);
    swap(0);
    BW_CHECK_EQ("light 1 only: emission", rgb_at(100, 100), e5(4) << 8);

    /*
     * The normal goes through the vector matrix: a 180-degree turn about
     * y (diag(-1, 1, -1), loaded in position+vector mode) faces it away,
     * so the diffuse level is 0; the light vector was taken earlier, under
     * the identity, and stays.
     */
    w32(0x04000440u, 2u);
    w32(0x0400045Cu, (u32)-FX32_ONE); w32(0x0400045Cu, 0u); w32(0x0400045Cu, 0u);
    w32(0x0400045Cu, 0u); w32(0x0400045Cu, (u32)FX32_ONE); w32(0x0400045Cu, 0u);
    w32(0x0400045Cu, 0u); w32(0x0400045Cu, 0u); w32(0x0400045Cu, (u32)-FX32_ONE);
    w32(0x0400045Cu, 0u); w32(0x0400045Cu, 0u); w32(0x0400045Cu, 0u);
    quad_at(0x001F00C1u, 0x7FFFu, -FX32_ONE, -FX32_ONE, FX32_ONE, FX32_ONE, 0,
            &n_front);
    swap(0);
    BW_CHECK_EQ("normal turned away: no diffuse", rgb_at(100, 100),
                (e5(0) << 16) | (e5(4) << 8) | e5(7));
    w32(0x04000454u, 0u);

    /*
     * Specular only, white: the half vector of (0, 0, -1) is (0, 0, -1),
     * shine level 4088^2 >> 12 = 4080, 31 * 31 * 4080 >> 17 = 29.  With
     * the table on and every entry 0x80 (one half): 128 << 4 = 2048,
     * 31 * 31 * 2048 >> 17 = 15.
     */
    w32(0x040004C0u, 0u);
    w32(0x040004C4u, 0x7FFFu);
    quad_at(0x001F00C1u, 0x7FFFu, -FX32_ONE, -FX32_ONE, FX32_ONE, FX32_ONE, 0,
            &n_front);
    swap(0);
    BW_CHECK_EQ("specular", rgb_at(100, 100), e5(29) * 0x010101u);
    for (i = 0; i < 32u; i++) {
        w32(0x040004D0u, 0x80808080u);
    }
    w32(0x040004C4u, 0x7FFFu | 0x8000u);
    quad_at(0x001F00C1u, 0x7FFFu, -FX32_ONE, -FX32_ONE, FX32_ONE, FX32_ONE, 0,
            &n_front);
    swap(0);
    BW_CHECK_EQ("specular through the shininess table", rgb_at(100, 100),
                e5(15) * 0x010101u);

    /* DIF_AMB bit 15: the diffuse colour becomes the vertex colour. */
    w32(0x04000480u, 0x7FFFu);
    w32(0x040004C0u, (31u << 5) | 0x8000u);
    w32(0x040004A4u, 0x001F00C0u);
    w32(0x04000500u, 1u);
    w32(0x0400048Cu, v16(-FX32_ONE, -FX32_ONE)); w32(0x0400048Cu, 0u);
    w32(0x0400048Cu, v16(FX32_ONE, -FX32_ONE));  w32(0x0400048Cu, 0u);
    w32(0x0400048Cu, v16(FX32_ONE, FX32_ONE));   w32(0x0400048Cu, 0u);
    w32(0x0400048Cu, v16(-FX32_ONE, FX32_ONE));  w32(0x0400048Cu, 0u);
    w32(0x04000504u, 0u);
    swap(0);
    BW_CHECK_EQ("DIF_AMB sets the vertex colour", rgb_at(100, 100), 0x00FF00u);
}

static void test_rear(void)
{
    static const s32 half = FX32_ONE / 2;
    u8 *bank_c, *bank_d;
    u32 i;

    boot();

    /*
     * Edge marking: a red quad of polygon ID 8 over a black clear plane of
     * ID 0, EDGE_COLOR[1] blue.  The quad covers screen x 64..191 and y
     * 48..143; its outermost pixels have a clear neighbour, further away
     * and of another ID, and turn blue; its inside stays red.
     */
    w16(G3D_HW_DISP3DCNT, 0x0028u);
    w32(G3D_HW_CLEAR_COLOR, 31u << 16);
    w16(G3D_HW_CLEAR_DEPTH, 0x7FFFu);
    w16(G3D_HW_EDGE_COLOR + 2u, (u16)(31u << 10));
    quad_at(0x081F00C0u, 31u, -half, -half, half, half, 0, NULL);
    swap(0);
    BW_CHECK_EQ("edge: left column", rgb_at(64, 100), 0x0000FFu);
    BW_CHECK_EQ("edge: right column", rgb_at(191, 100), 0x0000FFu);
    BW_CHECK_EQ("edge: top row", rgb_at(100, 48), 0x0000FFu);
    BW_CHECK_EQ("edge: inside", rgb_at(65, 100), 0xFF0000u);
    BW_CHECK_EQ("edge: the clear plane is not marked", rgb_at(63, 100), 0u);
    /* The same ID as the clear plane: nothing to mark. */
    quad_at(0x001F00C0u, 31u, -half, -half, half, half, 0, NULL);
    swap(0);
    BW_CHECK_EQ("edge: same ID as the clear plane", rgb_at(64, 100), 0xFF0000u);
    /* The screen border is the clear plane. */
    quad_at(0x081F00C0u, 31u, -FX32_ONE, -FX32_ONE, FX32_ONE, FX32_ONE, 0, NULL);
    swap(0);
    BW_CHECK_EQ("edge: screen border", rgb_at(0, 100), 0x0000FFu);
    BW_CHECK_EQ("edge: one in from the border", rgb_at(1, 100), 0xFF0000u);

    /*
     * Fog: shift 0 (step 0x400), offset 0, table entry k = 4k, white fog
     * of alpha 31.  The quad at z 0 has the rasteriser's depth 0x3FFFFF,
     * 15-bit 0x3FFF: entries 14 and 15 (56, 60) at 1/1024 and 1023/1024,
     * (56 * 1 + 60 * 1023) / 1024 = 59.  Red: (255 * 59 + 255 * 69) >> 7
     * = 255, green and blue 255 * 59 >> 7 = 117.  The clear plane, with
     * CLEAR_COLOR bit 15, is fogged too: depth 0x7FFF, entries 30 and 31,
     * density 123, colour 255 * 123 >> 7 = 245, alpha (31 * 123) >> 7 = 29.
     */
    w16(G3D_HW_DISP3DCNT, 0x0088u);
    w32(G3D_HW_CLEAR_COLOR, 0x8000u);
    w32(G3D_HW_FOG_COLOR, 0x7FFFu | (31u << 16));
    w16(G3D_HW_FOG_OFFSET, 0u);
    for (i = 0; i < 8u; i++) {
        u32 k = i * 4u;

        w32(G3D_HW_FOG_TABLE + i * 4u, (k * 4u) | ((k + 1u) * 4u << 8)
                                       | ((k + 2u) * 4u << 16)
                                       | ((k + 3u) * 4u << 24));
    }
    quad_at(0x001F80C0u, 31u, -half, -half, half, half, 0, NULL);
    swap(0);
    BW_CHECK_EQ("fog on a fogged polygon", rgb_at(100, 100), 0xFF7575u);
    BW_CHECK_EQ("fog on the clear plane", rgb_at(10, 10), 0xF5F5F5u);
    BW_CHECK_EQ("fogged clear alpha", px(10, 10)[3], 29u * 255u / 31u);
    quad_at(0x001F00C0u, 31u, -half, -half, half, half, 0, NULL);
    swap(0);
    BW_CHECK_EQ("no fog without POLYGON_ATTR bit 15", rgb_at(100, 100),
                0xFF0000u);
    w16(G3D_HW_DISP3DCNT, 0x00C8u);                /* alpha only */
    quad_at(0x001F80C0u, 31u, -half, -half, half, half, 0, NULL);
    swap(0);
    BW_CHECK_EQ("alpha-only fog keeps the colour", rgb_at(100, 100), 0xFF0000u);

    /*
     * The w-buffer.  A projection whose w is the vertex z and whose z is
     * the constant 0.5: a red quad at z 1 (w 1, z/w 0.5) drawn before a
     * green one at z 2 (w 2, z/w 0.25).  By z the green one is nearer, by
     * w the red one.
     */
    w16(G3D_HW_DISP3DCNT, 0x0008u);
    w32(G3D_HW_CLEAR_COLOR, 31u << 16);
    w32(0x04000440u, 0u);
    {
        static const s32 m[16] = {
            FX32_ONE, 0, 0, 0,   0, FX32_ONE, 0, 0,
            0, 0, 0, FX32_ONE,   0, 0, FX32_ONE / 2, 0,
        };
        for (i = 0; i < 16u; i++) {
            w32(0x04000458u, (u32)m[i]);
        }
    }
    w32(0x04000440u, 1u);
    for (i = 0; i < 2u; i++) {
        quad_at(0x001F00C0u, 31u, -FX32_ONE, -FX32_ONE, FX32_ONE, FX32_ONE,
                FX32_ONE, NULL);
        quad_at(0x001F00C0u, 31u << 5, -2 * FX32_ONE, -2 * FX32_ONE,
                2 * FX32_ONE, 2 * FX32_ONE, 2 * FX32_ONE, NULL);
        swap(i == 0 ? 0u : 2u);
        BW_CHECK_EQ(i == 0 ? "z-buffer: the smaller z/w wins"
                           : "w-buffer: the smaller w wins",
                    rgb_at(100, 100), i == 0 ? 0x00FF00u : 0xFF0000u);
    }
    w32(0x04000440u, 0u);
    w32(0x04000454u, 0u);
    w32(0x04000440u, 1u);

    /*
     * The rear-plane bitmap: banks C and D as texture slots 2 and 3,
     * CLRIMAGE_OFFSET x 1, y 2, so screen (4, 4) shows image (5, 6).
     */
    w32(0x04000240u, 0x9B930000u);               /* C: slot 2, D: slot 3 */
    bw_io_sync();
    bank_c = bw_vram_bank(BW_VRAM_C, NULL);
    bank_d = bw_vram_bank(BW_VRAM_D, NULL);
    for (i = 0; i < 256u * 256u; i++) {
        bw_mem_wr16(bank_d + i * 2u, 0x7FFFu);       /* far everywhere */
    }
    bw_mem_wr16(bank_c + (6u * 256u + 5u) * 2u, (u16)(0x8000u | (31u << 10)));
    bw_mem_wr16(bank_d + (6u * 256u + 5u) * 2u, 0x7FFFu);
    w16(G3D_HW_CLRIMAGE_OFS, (u16)(1u | (2u << 8)));
    w16(G3D_HW_DISP3DCNT, 0x4008u);
    swap(0);
    BW_CHECK_EQ("rear plane: image (5,6) at (4,4)", rgb_at(4, 4), 0x0000FFu);
    BW_CHECK_EQ("rear plane: its alpha bit", px(4, 4)[3], 255);
    BW_CHECK_EQ("rear plane: an empty pixel is transparent", px(5, 4)[3], 0);
    /* Its depth: an image depth of 0 hides a polygon behind it. */
    bw_mem_wr16(bank_d + (6u * 256u + 5u) * 2u, 0u);
    quad_at(0x001F00C0u, 31u << 5, -FX32_ONE, -FX32_ONE, FX32_ONE, FX32_ONE,
            0, NULL);
    swap(0);
    BW_CHECK_EQ("rear plane depth 0 in front", rgb_at(4, 4), 0x0000FFu);
    BW_CHECK_EQ("polygon elsewhere", rgb_at(6, 4), 0x00FF00u);
    w32(0x04000240u, 0u);
    bw_io_sync();

    /*
     * Anti-aliasing: a red quad from screen x 10.5 to 20.25 (clip x
     * -3760 / 4096 and -3448 / 4096).  Pixel 10 is half covered, coverage
     * 16/32 less one = 15; it lies on the silhouette.  Over the clear
     * plane's alpha 0 its colour stays and its alpha is (15 + 1) * 255 /
     * 32 = 127; over an opaque black clear plane the colour is blended,
     * 255 * 16 >> 5 = 127.  Pixel 15 is inside, pixel 19 fully covered.
     */
    w16(G3D_HW_DISP3DCNT, 0x0018u);
    w32(G3D_HW_CLEAR_COLOR, 0u);
    quad_at(0x001F00C0u, 31u, -3760, -FX32_ONE, -3448, FX32_ONE, 0, NULL);
    swap(0);
    BW_CHECK_EQ("AA over alpha 0: colour kept", rgb_at(10, 100), 0xFF0000u);
    BW_CHECK_EQ("AA over alpha 0: alpha is the coverage", px(10, 100)[3], 127);
    BW_CHECK_EQ("AA: inside untouched", px(15, 100)[3], 255);
    BW_CHECK_EQ("AA: a fully covered end untouched", px(19, 100)[3], 255);
    w32(G3D_HW_CLEAR_COLOR, 31u << 16);
    quad_at(0x001F00C0u, 31u, -3760, -FX32_ONE, -3448, FX32_ONE, 0, NULL);
    swap(0);
    BW_CHECK_EQ("AA over black: blended", rgb_at(10, 100), 127u << 16);
    w16(G3D_HW_DISP3DCNT, 0x0008u);
    quad_at(0x001F00C0u, 31u, -3760, -FX32_ONE, -3448, FX32_ONE, 0, NULL);
    swap(0);
    BW_CHECK_EQ("AA off: the edge pixel is solid", rgb_at(10, 100), 0xFF0000u);

    /* A shadow polygon is still not drawn, and says so. */
    quad_at(0x001F00F0u, 31u, -FX32_ONE, -FX32_ONE, FX32_ONE, FX32_ONE, 0, NULL);
    swap(0);
    BW_CHECK_EQ("shadow flagged",
                (g3d_hw_unsupported() & G3D_HW_UNSUP_SHADOW) != 0, 1);
}

/* ------------------------------------------------------------------ */
/* 6. Rasteriser: perspective, cracks, the texture matrix             */
/* ------------------------------------------------------------------ */

#define ATTR_BOTH_SIDES (((u32)1 << 6) | ((u32)1 << 7) | ((u32)31 << 16))

static G3dVertex rv(s32 x, s32 y, s32 w, s32 s, s32 t)
{
    G3dVertex v;

    memset(&v, 0, sizeof v);
    v.x = x; v.y = y; v.z = 0; v.w = w;
    v.s = s; v.t = t;
    v.r = v.g = v.b = 255;
    return v;
}

/* A strip whose left edge is at w = 1.0 and right edge at w = 4.0, both
 * spanning the whole screen after the divide.  The texture ramps red by 4
 * per texel over 64 texels.  Halfway across the screen the
 * perspective-correct s is 12.8 texels, where an affine interpolation
 * gives 32. */
static void test_perspective(void)
{
    static u8 texels[64 * 8 * 4];
    G3dTexture tex;
    G3dTarget t;
    G3dVertex v[4];
    const u8 *p;
    u32 x, y;

    for (y = 0; y < 8u; y++) {
        for (x = 0; x < 64u; x++) {
            u8 *q = &texels[(y * 64u + x) * 4u];
            q[0] = (u8)(x * 4u); q[1] = 0; q[2] = 0; q[3] = 255;
        }
    }
    tex.width = 64; tex.height = 8; tex.format = G3D_TEXFMT_DIRECT;
    tex.rgba = texels;
    if (g3d_target_init(&t, 256, 192) != 0) {
        BW_CHECK_EQ("perspective: target", 0, 1);
        return;
    }
    g3d_target_clear(&t, 0, 0, 0, 255);
    v[0] = rv(-4096, 4096, 4096, 0, 0);
    v[1] = rv(16384, 16384, 16384, 64 * 16, 0);
    v[2] = rv(16384, -16384, 16384, 64 * 16, 8 * 16);
    v[3] = rv(-4096, -4096, 4096, 0, 8 * 16);
    g3d_raster_polygon(&t, v, 4u, ATTR_BOTH_SIDES, (7u << 26) | (3u << 16),
                       &tex);
    p = t.rgba + (96u * 256u + 128u) * 4u;
    BW_CHECK_EQ("perspective: mid-screen texel is the perspective one",
                p[0] >= 44u && p[0] <= 60u, 1);
    p = t.rgba + (96u * 256u + 8u) * 4u;
    BW_CHECK_EQ("perspective: the near edge is still texel 0..3",
                p[0] <= 12u, 1);
    g3d_target_free(&t);
}

/* A fan of triangles around one off-grid point, with off-grid rim
 * vertices: every pixel well inside the hexagon must be drawn once, so no
 * crack opens along a shared edge. */
static void test_no_cracks(void)
{
    G3dTarget t;
    const double cx = 100.3, cy = 80.7;
    s32 rx[6], ry[6];
    u32 k, x, y, holes = 0;

    if (g3d_target_init(&t, 256, 192) != 0) {
        BW_CHECK_EQ("cracks: target", 0, 1);
        return;
    }
    g3d_target_clear(&t, 0, 0, 0, 255);
    for (k = 0; k < 6u; k++) {
        double a = (13.0 + 60.0 * (double)k) * 3.14159265358979 / 180.0;
        double px = cx + 50.0 * __builtin_cos(a);
        double py = cy + 50.0 * __builtin_sin(a);

        rx[k] = (s32)((px - 128.0) / 128.0 * 4096.0);
        ry[k] = (s32)((96.0 - py) / 96.0 * 4096.0);
    }
    for (k = 0; k < 6u; k++) {
        G3dVertex v[3];

        v[0] = rv((s32)((cx - 128.0) / 128.0 * 4096.0),
                  (s32)((96.0 - cy) / 96.0 * 4096.0), 4096, 0, 0);
        v[1] = rv(rx[k], ry[k], 4096, 0, 0);
        v[2] = rv(rx[(k + 1u) % 6u], ry[(k + 1u) % 6u], 4096, 0, 0);
        g3d_raster_polygon(&t, v, 3u, ATTR_BOTH_SIDES, 0, NULL);
    }
    for (y = 0; y < 192u; y++) {
        for (x = 0; x < 256u; x++) {
            double dx = (double)x + 0.5 - cx, dy = (double)y + 0.5 - cy;

            if (dx * dx + dy * dy < 40.0 * 40.0 &&
                t.rgba[(y * 256u + x) * 4u] == 0) {
                holes++;
            }
        }
    }
    BW_CHECK_EQ("cracks: no hole inside the fan", holes, 0u);
    g3d_target_free(&t);
}

/* TEXIMAGE_PARAM bits 30-31 = 1: the texture matrix transforms the
 * texcoord: S' = (S*M0 + T*M4 + M8 + M12) >> 12. */
static void test_texgen_mode1(void)
{
    static u8 buf[512];
    G3dGx gx;
    G3dGeom geom;
    u32 len;

    memset(&geom, 0, sizeof geom);
    g3d_gx_init(&gx, &geom);
    g_ncmd = 0;
    cmd(0x10, 1, 3u);                                    /* MTX_MODE texture */
    cmd(0x1B, 3, (u32)(2 * FX32_ONE), (u32)(3 * FX32_ONE), (u32)FX32_ONE);
    cmd(0x2A, 1, 1u << 30);                              /* texgen mode 1    */
    cmd(0x22, 1, 32u | (16u << 16));
    cmd(0x40, 1, 0u);
    cmd(0x23, 2, v16(0, 0), 0u);
    cmd(0x23, 2, v16(16, 0), 0u);
    cmd(0x23, 2, v16(0, 16), 0u);
    cmd(0x41, 0);
    len = pack(buf, g_cmds, g_ncmd);
    (void)g3d_gx_run(&gx, buf, len, 0, len, len);
    BW_CHECK_EQ("texgen 1: three vertices", geom.n_verts, 3u);
    BW_CHECK_EQ("texgen 1: S scaled by the texture matrix", geom.verts[0].s,
                64);
    BW_CHECK_EQ("texgen 1: T scaled by the texture matrix", geom.verts[0].t,
                48);

    g3d_gx_init(&gx, &geom);
    geom.n_verts = geom.n_idx = geom.n_polys = 0;
    g_ncmd = 0;
    cmd(0x10, 1, 3u);
    cmd(0x1B, 3, (u32)(2 * FX32_ONE), (u32)(3 * FX32_ONE), (u32)FX32_ONE);
    cmd(0x22, 1, 32u | (16u << 16));
    cmd(0x40, 1, 0u);
    cmd(0x23, 2, v16(0, 0), 0u);
    len = pack(buf, g_cmds, g_ncmd);
    (void)g3d_gx_run(&gx, buf, len, 0, len, len);
    BW_CHECK_EQ("texgen 0: the texcoord is left alone", geom.verts[0].s, 32);
}

int main(void)
{
    test_identity();
    test_sdk_paths();
    test_status();
    test_swap();
    test_lighting();
    test_rear();
    test_perspective();
    test_no_cracks();
    test_texgen_mode1();
    g3d_target_free(&g_ref);
    BW_REPORT("test_g3dhw");
}
