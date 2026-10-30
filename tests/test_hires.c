/*
 * tests/test_hires.c -- internal resolution: the 3D rasteriser at N times
 * the DS size and the 2D composition at N.
 *
 *   - N = 1 through the scaled entry points equals the 1x wrappers.
 *   - A pure-2D scene at N = 2, 3 is the nearest-upscaled 1x frame, both
 *     screens, exactly.
 *   - A 3D triangle rendered at N = 2 and box-downsampled is close to the
 *     1x render and has more distinct (partial-coverage) edge pixels.
 *   - The display capture writes the same bytes into VRAM at every N for a
 *     2D scene, and a 3D capture at N = 2 stays close to the 1x capture.
 *   - A micro benchmark prints the cost per frame at N = 1..4 (not
 *     asserted).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "dsmem.h"
#include "g2d.h"
#include "g3d.h"
#include "hw3d.h"
#include "port.h"
#include "port_test.h"

#define W 256u
#define H 192u

static void w32(u32 addr, u32 v) { *(volatile u32 *)bw_ds_ptr(addr) = v; }
static void w16(u32 addr, u16 v) { *(volatile u16 *)bw_ds_ptr_w(addr, sizeof(volatile u16)) = v; }
static void w8(u32 addr, u8 v)   { *(volatile u8 *)bw_ds_ptr_w(addr, sizeof(volatile u8)) = v; }

static void vram_fill(u32 addr, u8 v, u32 n)
{
    u32 i;

    for (i = 0; i < n; i++) {
        u8 *p = (u8 *)bw_ds_try_ptr(addr + i);

        if (p != NULL) {
            *p = v;
        }
    }
}

static void boot(void)
{
    bw_ds_reset();
    g3d_hw_reset();
    w8(0x04000240u, 0x81);            /* A: main BG */
    w8(0x04000243u, 0x80);            /* D: LCDC (capture target) */
    w16(0x04000304u, 0x8000);         /* POWCNT1: main on top */
}

/* A 2D scene: backdrop blue, BG0 a checkered text map of a 2-colour tile. */
static void scene_2d(void)
{
    u32 i;

    w16(0x05000000u, 0x7C00);         /* backdrop blue */
    w16(0x05000002u, 0x001F);         /* colour 1 red */
    w16(0x05000004u, 0x03E0);         /* colour 2 green */
    for (i = 0; i < 32u; i += 4u) {
        vram_fill(0x06000020u + i, 0x21, 4);   /* tile 1: 1,2 stripes */
    }
    vram_fill(0x06000040u, 0x11, 32);          /* tile 2: solid red */
    for (i = 0; i < 1024u; i++) {
        u32 e = (i & 1u) ? 1u : (((i >> 5) & 1u) ? 2u : 0u);

        *(volatile u16 *)bw_ds_ptr(0x06004800u + i * 2u) = (u16)e;
    }
    w16(0x04000008u, (u16)(9u << 8));          /* BG0, map at 0x4800 */
    w32(0x04000000u, 0x10000u | 0x0100u);
}

static u8 *frame(u32 n)
{
    return (u8 *)calloc((size_t)W * n * H * n, 4u);
}

static int render(u32 n, const u8 *f3d, u8 *top, u8 *bot)
{
    u32 fl[2];

    return g2d_hw_render_dsmem_scaled(n, f3d, top, bot, fl);
}

static u32 nearest_diff(const u8 *big, u32 n, const u8 *one)
{
    u32 x, y, bad = 0;

    for (y = 0; y < H * n; y++) {
        for (x = 0; x < W * n; x++) {
            if (memcmp(big + ((size_t)y * W * n + x) * 4u,
                       one + ((size_t)(y / n) * W + x / n) * 4u, 4u) != 0) {
                bad++;
            }
        }
    }
    return bad;
}

static void test_2d(void)
{
    u8 *t1 = frame(1), *b1 = frame(1);
    u32 n;

    boot();
    scene_2d();
    BW_CHECK_EQ("render 1x", render(1, NULL, t1, b1), 0);
    {
        u8 *t0 = frame(1), *b0 = frame(1);
        u32 fl[2];

        BW_CHECK_EQ("render wrapper", g2d_hw_render_dsmem(NULL, t0, b0, fl), 0);
        BW_CHECK_EQ("scaled(1) == wrapper top", memcmp(t0, t1, (size_t)W * H * 4u), 0);
        BW_CHECK_EQ("scaled(1) == wrapper bottom", memcmp(b0, b1, (size_t)W * H * 4u), 0);
        free(t0);
        free(b0);
    }
    BW_CHECK_EQ("scene is not flat", memcmp(t1, t1 + 8 * 4, 4) != 0, 1);
    for (n = 2; n <= 3u; n++) {
        u8 *t = frame(n), *b = frame(n);

        BW_CHECK_EQ("render scaled", render(n, NULL, t, b), 0);
        BW_CHECK_EQ(n == 2u ? "2D N=2 top is nearest" : "2D N=3 top is nearest",
                    nearest_diff(t, n, t1), 0);
        BW_CHECK_EQ(n == 2u ? "2D N=2 bottom is nearest" : "2D N=3 bottom is nearest",
                    nearest_diff(b, n, b1), 0);
        free(t);
        free(b);
    }
    free(t1);
    free(b1);
}

static u32 v16(s32 x, s32 y)
{
    return ((u32)x & 0xFFFFu) | (((u32)y & 0xFFFFu) << 16);
}

static void triangle(void)
{
    w32(0x040004A4u, 0x001F00C0u);    /* alpha 31, both faces */
    w32(0x04000480u, 0x001Fu);        /* red */
    w32(0x04000500u, 0u);             /* BEGIN_VTXS triangles */
    w32(0x0400048Cu, v16(-2400, -2000)); w32(0x0400048Cu, 0u);
    w32(0x0400048Cu, v16(2900, -1200));  w32(0x0400048Cu, 0u);
    w32(0x0400048Cu, v16(400, 2800));    w32(0x0400048Cu, 0u);
    w32(0x04000504u, 0u);
}

/* Render the triangle at scale n into a malloc'd copy of the frame. */
static u8 *render3d(u32 n)
{
    size_t bytes;
    u8 *copy;

    g3d_hw_set_scale(n);
    w16(G3D_HW_DISP3DCNT, 0x0008u);
    w32(G3D_HW_CLEAR_COLOR, 0x0000001Fu & 0u);   /* clear alpha 0 */
    w16(G3D_HW_CLEAR_DEPTH, 0x7FFFu);
    triangle();
    w32(G3D_HW_SWAP_BUFFERS, 0u);
    g3d_hw_flush();
    bytes = (size_t)W * n * H * n * 4u;
    if (g3d_hw_frame() == NULL) {
        return NULL;
    }
    copy = (u8 *)malloc(bytes);
    if (copy != NULL) {
        memcpy(copy, g3d_hw_frame(), bytes);
    }
    return copy;
}

static u32 partial_alpha(const u8 *f)
{
    u32 i, c = 0;

    for (i = 0; i < W * H; i++) {
        if (f[i * 4u + 3u] != 0 && f[i * 4u + 3u] != 255) {
            c++;
        }
    }
    return c;
}

static u32 far_pixels(const u8 *a, const u8 *b)
{
    u32 i, bad = 0;

    for (i = 0; i < W * H; i++) {
        u32 k, d = 0;

        for (k = 0; k < 4u; k++) {
            u32 x = a[i * 4u + k], y = b[i * 4u + k];

            d = d > (x > y ? x - y : y - x) ? d : (x > y ? x - y : y - x);
        }
        if (d > 128u) {
            bad++;
        }
    }
    return bad;
}

static u32 covered_px(const u8 *f)
{
    u32 i, c = 0;

    for (i = 0; i < W * H; i++) {
        c += f[i * 4u + 3u] != 0;
    }
    return c;
}

static int test_3d(void)
{
    u8 *f1, *f2, *d2, *f3, *d3;
    u32 c1, c2;

    boot();
    f1 = render3d(1);
    BW_REQUIRE("1x frame", f1 != NULL);
    BW_CHECK_EQ("frame scale 1", g3d_hw_frame_scale(), 1);
    f2 = render3d(2);
    BW_REQUIRE("2x frame", f2 != NULL);
    BW_CHECK_EQ("frame scale 2", g3d_hw_frame_scale(), 2);
    f3 = render3d(3);
    BW_REQUIRE("3x frame", f3 != NULL);
    d2 = (u8 *)malloc((size_t)W * H * 4u);
    d3 = (u8 *)malloc((size_t)W * H * 4u);
    g2d_hw_box_downsample(f2, 2, d2);
    g2d_hw_box_downsample(f3, 3, d3);
    c1 = covered_px(f1);
    c2 = covered_px(d2);
    printf("  triangle coverage: 1x %u, 2x down %u px, 3x down %u px\n",
           c1, c2, covered_px(d3));
    BW_REQUIRE("triangle drawn", c1 > 1000);
    BW_CHECK_EQ("2x close to 1x (far pixels < 1%)",
                far_pixels(f1, d2) < (W * H) / 100u, 1);
    BW_CHECK_EQ("3x close to 1x (far pixels < 1%)",
                far_pixels(f1, d3) < (W * H) / 100u, 1);
    BW_CHECK_EQ("1x has no partial edge pixels", partial_alpha(f1), 0);
    BW_CHECK_EQ("2x has more distinct edge pixels", partial_alpha(d2) > 100u, 1);

    /* Composition at N = 2: the opaque interior is the triangle colour. */
    {
        u8 *t = frame(2), *b = frame(2);

        w16(0x05000000u, 0x7C00);
        w32(0x04000000u, 0x10000u | 0x8u | 0x0100u);
        g3d_hw_set_scale(2);
        BW_CHECK_EQ("compose 2x", render(2, f2, t, b), 0);
        BW_CHECK_EQ("interior is red", t[((size_t)(2 * 96) * 512u + 2 * 128u) * 4u], 255);
        BW_CHECK_EQ("outside is backdrop blue",
                    t[((size_t)(2 * 5) * 512u + 2 * 5u) * 4u + 2], 255);
        free(t);
        free(b);
    }
    free(f1);
    free(f2);
    free(f3);
    free(d2);
    free(d3);
    g3d_hw_set_scale(1);
    return 0;
}

static u16 cap16(u32 off)
{
    return *(volatile u16 *)bw_ds_ptr(0x06800000u + 3u * 0x20000u + off);
}

static int test_capture(void)
{
    u16 ref[W * H];
    u32 n, x, y, bad;
    u8 *f1, *f2, *t, *b;

    /* 2D scene: identical bytes at every N. */
    for (n = 1; n <= 3u; n++) {
        boot();
        scene_2d();
        t = frame(n);
        b = frame(n);
        w16(0x0400006Cu, 0);
        w32(0x04000064u, 0x80330010u | (3u << 16) | (0u << 29));
        render(n, NULL, t, b);
        if (n == 1u) {
            for (y = 0; y < H; y++) {
                for (x = 0; x < W; x++) {
                    ref[y * W + x] = cap16((y * W + x) * 2u);
                }
            }
            BW_CHECK_EQ("capture wrote data", ref[0] != 0 || ref[1] != 0, 1);
        } else {
            bad = 0;
            for (y = 0; y < H; y++) {
                for (x = 0; x < W; x++) {
                    bad += cap16((y * W + x) * 2u) != ref[y * W + x];
                }
            }
            BW_CHECK_EQ(n == 2u ? "capture identical at N=2" : "capture identical at N=3",
                        bad, 0);
        }
        free(t);
        free(b);
    }

    /* 3D source (bit 24): N = 2 stays close to N = 1. */
    boot();
    f1 = render3d(1);
    BW_REQUIRE("3d 1x", f1 != NULL);
    w32(0x04000000u, 0x10000u | 0x8u | 0x0100u);
    t = frame(1);
    b = frame(1);
    w32(0x04000064u, 0x80330010u | (3u << 16) | (1u << 24));
    render(1, f1, t, b);
    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            ref[y * W + x] = cap16((y * W + x) * 2u);
        }
    }
    free(t);
    free(b);
    f2 = render3d(2);
    BW_REQUIRE("3d 2x", f2 != NULL);
    t = frame(2);
    b = frame(2);
    w32(0x04000064u, 0x80330010u | (3u << 16) | (1u << 24));
    render(2, f2, t, b);
    bad = 0;
    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            u16 a = cap16((y * W + x) * 2u), r = ref[y * W + x];

            bad += (a & 0x8000u) != (r & 0x8000u);
        }
    }
    BW_CHECK_EQ("3D capture alpha bits close (< 1%)", bad < (W * H) / 100u, 1);
    free(t);
    free(b);
    free(f1);
    free(f2);
    g3d_hw_set_scale(1);
    return 0;
}

static void bench(void)
{
    u32 n;

    boot();
    scene_2d();
    for (n = 1; n <= 4u; n++) {
        u8 *f = render3d(n), *t = frame(n), *b = frame(n);
        clock_t c0;
        u32 i, reps = 5;
        double ms3d, ms2d;

        w32(0x04000000u, 0x10000u | 0x8u | 0x0100u);
        c0 = clock();
        for (i = 0; i < reps; i++) {
            (void)render(n, f, t, b);
        }
        ms2d = 1000.0 * (double)(clock() - c0) / CLOCKS_PER_SEC / reps;
        c0 = clock();
        for (i = 0; i < reps; i++) {
            free(render3d(n));
        }
        ms3d = 1000.0 * (double)(clock() - c0) / CLOCKS_PER_SEC / reps;
        printf("  bench N=%u: compose %.2f ms/frame, 3D raster %.2f ms/frame\n",
               n, ms2d, ms3d);
        free(f);
        free(t);
        free(b);
    }
    g3d_hw_set_scale(1);
}

int main(void)
{
    test_2d();
    (void)test_3d();
    (void)test_capture();
    bench();
    BW_REPORT("test_hires");
}
