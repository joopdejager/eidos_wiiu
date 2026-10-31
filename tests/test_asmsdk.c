/*
 * tests/test_asmsdk.c -- sdk/asm/, the C that stands for the boot closure's
 * mwcc `asm` functions (the BW_ASMSDK_SOURCES half; the forwarders that call
 * game code are tests/test_asmsdk_glue.c).
 *
 * The expected values come from the asm, not from the C under test:
 *
 *   - MI_CpuCopy8 is checked against an instruction-level model of the
 *     vendor asm (halfword and word loads and stores on a flat byte array,
 *     the neighbour byte read and written back), for every source offset,
 *     destination offset and size in a window, overlapping or not.
 *   - The division kernel is checked against C division; the SDK entries
 *     on the edge inputs the asm defines (division by zero, 0x80000000).
 *   - MI_UncompressLZ8 decodes hand-built streams with known output, round
 *     trips a small encoder written here in both formats, is cross-checked
 *     against the engine's independent decoder (g2d_decompress), and, with
 *     game data, decodes every LZ member of a few real archives to the same
 *     bytes g2d_decompress produces.
 *   - The register and shared-memory functions run on the real address
 *     space (sdk/dsmem.c) after bw_ds_reset.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port.h"
#include "dsmem.h"
#include "g2d.h"
#include "asm/asmsdk.h"
#include "port_test.h"

/* ------------------------------------------------------------------ */
/* An instruction-level model of the MI_CpuCopy8 asm                   */
/* ------------------------------------------------------------------ */

#define WIN 96

static u8 g_model[WIN];

static u32 m_ldrh(u32 a)
{
    a &= ~1u;
    return (u32)g_model[a] | ((u32)g_model[a + 1] << 8);
}

static void m_strh(u32 a, u32 v)
{
    a &= ~1u;
    g_model[a] = (u8)v;
    g_model[a + 1] = (u8)(v >> 8);
}

static u32 m_ldr(u32 a)
{
    return (u32)g_model[a] | ((u32)g_model[a + 1] << 8)
         | ((u32)g_model[a + 2] << 16) | ((u32)g_model[a + 3] << 24);
}

static void m_str(u32 a, u32 v)
{
    g_model[a] = (u8)v;
    g_model[a + 1] = (u8)(v >> 8);
    g_model[a + 2] = (u8)(v >> 16);
    g_model[a + 3] = (u8)(v >> 24);
}

/* r0 = src, r1 = dst, r2 = size, as offsets into g_model. */
static void model_copy8(u32 r0, u32 r1, u32 r2)
{
    u32 ip;
    u32 r3;

    if (r2 == 0) {
        return;
    }
    if ((r1 & 1) != 0) {
        ip = m_ldrh(r1 - 1) & 0xFF;
        r3 = (r0 & 1) ? (m_ldrh(r0 - 1) >> 8) : m_ldrh(r0);
        r3 = ip | (r3 << 8);
        m_strh(r1 - 1, r3);
        r0++;
        r1++;
        if (--r2 == 0) {
            return;
        }
    }
    ip = r1 ^ r0;
    if ((ip & 1) != 0) {
        r0 &= ~1u;
        ip = m_ldrh(r0);
        r0 += 2;
        r3 = ip >> 8;
        if (r2 < 2) {
            r2 -= 2;
        } else {
            r2 -= 2;
            for (;;) {
                ip = m_ldrh(r0);
                r0 += 2;
                ip = r3 | (ip << 8);
                m_strh(r1, ip);
                r1 += 2;
                r3 = ip >> 16;
                if (r2 < 2) {
                    r2 -= 2;
                    break;
                }
                r2 -= 2;
            }
        }
        if ((r2 & 1) != 0) {
            ip = m_ldrh(r1) & 0xFF00;
            m_strh(r1, ip | r3);
        }
        return;
    }
    if ((ip & 2) != 0) {
        r3 = r2 & ~1u;
        if (r3 != 0) {
            r2 -= r3;
            ip = r3 + r1;
            do {
                r3 = m_ldrh(r0);
                r0 += 2;
                m_strh(r1, r3);
                r1 += 2;
            } while (r1 < ip);
        }
    } else if (r2 >= 2) {
        if ((r1 & 2) != 0) {
            r3 = m_ldrh(r0);
            r0 += 2;
            m_strh(r1, r3);
            r1 += 2;
            r2 -= 2;
            if (r2 == 0) {
                return;
            }
        }
        r3 = r2 & ~3u;
        if (r3 != 0) {
            r2 -= r3;
            ip = r3 + r1;
            do {
                r3 = m_ldr(r0);
                r0 += 4;
                m_str(r1, r3);
                r1 += 4;
            } while (r1 < ip);
        }
        if ((r2 & 2) != 0) {
            r3 = m_ldrh(r0);
            r0 += 2;
            m_strh(r1, r3);
            r1 += 2;
        }
    }
    if ((r2 & 1) != 0) {
        u32 d = m_ldrh(r1) & 0xFF00;
        u32 s = m_ldrh(r0) & 0xFF;

        m_strh(r1, d | s);
    }
}

static void fill_pattern(u8 *p, u32 n, u32 seed)
{
    u32 i;

    for (i = 0; i < n; i++) {
        p[i] = (u8)(i * 37u + seed * 11u + 5u);
    }
}

static void test_copy8(void)
{
    static u32 host_words[WIN / 4];
    u8 *host = (u8 *)host_words;
    u32 so;
    u32 dof;
    u32 n;
    int mismatches = 0;
    int cases = 0;

    /* Offsets from 4 so the model's reads below a range stay in the array;
     * the window is wide enough that ranges overlap in every relation. */
    for (so = 4; so < 24; so++) {
        for (dof = 4; dof < 24; dof++) {
            for (n = 0; n <= 40; n++) {
                fill_pattern(g_model, WIN, so * 31u + dof);
                memcpy(host, g_model, WIN);
                model_copy8(so, dof, n);
                MI_CpuCopy8(host + so, host + dof, n);
                cases++;
                if (memcmp(host, g_model, WIN) != 0) {
                    if (mismatches < 5) {
                        printf("  copy8 src+%u dst+%u size %u differs\n",
                               (unsigned)so, (unsigned)dof, (unsigned)n);
                    }
                    mismatches++;
                }
            }
        }
    }
    printf("  MI_CpuCopy8: %d cases against the asm model\n", cases);
    BW_CHECK_EQ("MI_CpuCopy8 matches the asm model", mismatches, 0);

    /* The two address spellings reach the same copy. */
    memset(host, 0, WIN);
    func_02082D44("abcdef", host + 1, 6);
    BW_CHECK_EQ("func_02082D44 copies", memcmp(host + 1, "abcdef", 6), 0);
    sub_02082D44("xyz", host + 7, 3);
    BW_CHECK_EQ("sub_02082D44 copies", memcmp(host + 7, "xyz", 3), 0);
    BW_CHECK_EQ("byte after copy untouched", host[10], 0);
}

static void test_fill8(void)
{
    static u32 words[16];
    u8 *buf = (u8 *)words;
    u32 off;
    u32 n;
    int bad = 0;

    for (off = 0; off < 8; off++) {
        for (n = 0; n <= 24; n++) {
            u32 i;

            memset(buf, 0xEE, sizeof words);
            MI_CpuFill8(buf + off, 0x5A, n);
            for (i = 0; i < sizeof words; i++) {
                u8 want = (i >= off && i < off + n) ? 0x5A : 0xEE;

                if (buf[i] != want) {
                    bad++;
                }
            }
        }
    }
    BW_CHECK_EQ("MI_CpuFill8 fills exactly [dst, dst + size)", bad, 0);
    func_02082BCC(buf + 3, 0x00, 2);
    BW_CHECK_EQ("func_02082BCC fills", buf[3] | buf[4], 0);
}

static void test_mii_units(void)
{
    static u32 words[16];
    static u32 src_words[16];
    u8 *b = (u8 *)words;
    u8 *s = (u8 *)src_words;
    u32 i;

    /* MIi_CpuClear16: halfwords while offset < size, size rounded up. */
    memset(b, 0xEE, sizeof words);
    MIi_CpuClear16(0x1234, b, 3);
    BW_CHECK_EQ("Clear16 size 3 [0]", bw_mem_rd16(b), 0x1234);
    BW_CHECK_EQ("Clear16 size 3 [2] (rounded up)", bw_mem_rd16(b + 2), 0x1234);
    BW_CHECK_EQ("Clear16 size 3 [4] untouched", b[4], 0xEE);
    memset(b, 0xEE, sizeof words);
    MIi_CpuClear16(0, b, 0);
    BW_CHECK_EQ("Clear16 size 0 writes nothing", b[0], 0xEE);
    MIi_CpuClear16(0, b, 0x80000000u);
    BW_CHECK_EQ("Clear16 negative size writes nothing", b[0], 0xEE);

    /* MIi_CpuClear32: rounded up to whole words. */
    memset(b, 0xEE, sizeof words);
    MIi_CpuClear32(0xA1B2C3D4u, b, 5);
    BW_CHECK_EQ("Clear32 size 5 word 0", bw_mem_rd32(b), 0xA1B2C3D4u);
    BW_CHECK_EQ("Clear32 size 5 word 1 (rounded up)", bw_mem_rd32(b + 4), 0xA1B2C3D4u);
    BW_CHECK_EQ("Clear32 size 5 byte 8 untouched", b[8], 0xEE);
    memset(b, 0xEE, sizeof words);
    MIi_CpuClear32(0, b, 0xFFFFFFFCu);
    BW_CHECK_EQ("Clear32 negative size writes nothing", b[0], 0xEE);
    func_02082A7C(0, b, 4);
    BW_CHECK_EQ("func_02082A7C clears", bw_mem_rd32(b), 0);

    /* MIi_CpuCopy16: forward halfwords, odd size rounded up. */
    for (i = 0; i < sizeof src_words; i++) {
        s[i] = (u8)(i + 1);
    }
    memset(b, 0, sizeof words);
    MIi_CpuCopy16(s, b, 5);
    BW_CHECK_EQ("Copy16 size 5 copies 6 bytes", memcmp(b, s, 6), 0);
    BW_CHECK_EQ("Copy16 size 5 byte 6 untouched", b[6], 0);
    /* Overlap, dst = src + 2: each halfword is loaded before it is stored,
     * so the first halfword repeats through the range. */
    memcpy(b, s, 16);
    MIi_CpuCopy16(b, b + 2, 8);
    BW_CHECK_EQ("Copy16 overlap repeats [2]", b[2], 1);
    BW_CHECK_EQ("Copy16 overlap repeats [3]", b[3], 2);
    BW_CHECK_EQ("Copy16 overlap repeats [8]", b[8], 1);
    BW_CHECK_EQ("Copy16 overlap repeats [9]", b[9], 2);
    BW_CHECK_EQ("Copy16 overlap stops at size [10]", b[10], 11);
    memset(b, 0, sizeof words);
    func_02082A60(s, b, 2);
    BW_CHECK_EQ("func_02082A60 copies", b[0] == 1 && b[1] == 2 && b[2] == 0, 1);

    /* MIi_CpuCopy32: forward words, size rounded up. */
    memset(b, 0, sizeof words);
    MIi_CpuCopy32(s, b, 6);
    BW_CHECK_EQ("Copy32 size 6 copies 8 bytes", memcmp(b, s, 8), 0);
    BW_CHECK_EQ("Copy32 size 6 byte 8 untouched", b[8], 0);
    memcpy(b, s, 32);
    MIi_CpuCopy32(b, b + 4, 12);
    BW_CHECK_EQ("Copy32 overlap repeats word 1", bw_rd32(b + 4), bw_rd32(s));
    BW_CHECK_EQ("Copy32 overlap repeats word 3", bw_rd32(b + 12), bw_rd32(s));
    BW_CHECK_EQ("Copy32 overlap stops at size", bw_rd32(b + 16), bw_rd32(s + 16));
    memset(b, 0xEE, sizeof words);
    MIi_CpuCopy32(s, b, 0x80000000u);
    BW_CHECK_EQ("Copy32 negative size copies nothing", b[0], 0xEE);
}

/* ------------------------------------------------------------------ */
/* Division                                                            */
/* ------------------------------------------------------------------ */

static u32 g_rng = 0x12345678u;

static u32 rng(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

static void test_division(void)
{
    static const u32 edges[] = {
        0, 1, 2, 3, 7, 9, 10, 15, 16, 17, 255, 256, 4095, 4096, 65535, 65536,
        0x00FFFFFFu, 0x01000000u, 0x7FFFFFFEu, 0x7FFFFFFFu, 0x80000000u,
        0x80000001u, 0xFFFFFFFEu, 0xFFFFFFFFu
    };
    const u32 ne = (u32)(sizeof edges / sizeof edges[0]);
    u32 i;
    u32 j;
    int bad = 0;
    u32 r;
    s32 sr;
    s32 q;

    /* The kernel against C division, wherever the asm runs it (n >= d). */
    for (i = 0; i < ne; i++) {
        for (j = 0; j < ne; j++) {
            u32 n = edges[i];
            u32 d = edges[j];

            if (d == 0 || n < d) {
                continue;
            }
            if (bw_rt_div_kernel(n, d, &r) != n / d || r != n % d) {
                bad++;
            }
        }
    }
    for (i = 0; i < 200000; i++) {
        u32 n = rng();
        u32 d = rng() >> (rng() & 31u);

        if (d == 0) {
            d = 1;
        }
        if (n < d) {
            u32 t = n;

            n = d;
            d = t == 0 ? 1 : t;
        }
        if (bw_rt_div_kernel(n, d, &r) != n / d || r != n % d) {
            bad++;
        }
    }
    BW_CHECK_EQ("the unrolled kernel equals C division", bad, 0);

    /* _s32_div_f: C semantics where C defines them (truncation, remainder
     * takes the dividend's sign). */
    bad = 0;
    for (i = 0; i < 200000; i++) {
        s32 a = (s32)rng();
        s32 b = (s32)(rng() >> (rng() & 31u));

        if (rng() & 1u) {
            b = -b;
        }
        if (b == 0 || (a == (s32)0x80000000u && b == -1)) {
            continue;
        }
        q = bw_rt_s32_divmod(a, b, &sr);
        if (q != a / b || sr != a % b) {
            bad++;
        }
    }
    BW_CHECK_EQ("_s32_div_f equals C on random operands", bad, 0);
    BW_CHECK_EQ("-7 / 2", _s32_div_f(-7, 2), -3);
    q = bw_rt_s32_divmod(-7, 2, &sr);
    BW_CHECK_EQ("-7 % 2", sr, -1);
    q = bw_rt_s32_divmod(7, -2, &sr);
    BW_CHECK_EQ("7 / -2", q, -3);
    BW_CHECK_EQ("7 % -2", sr, 1);
    q = bw_rt_s32_divmod(-7, -2, &sr);
    BW_CHECK_EQ("-7 / -2", q, 3);
    BW_CHECK_EQ("-7 % -2", sr, -1);

    /* What the asm does where C does not say. */
    q = bw_rt_s32_divmod(1234, 0, &sr);
    BW_CHECK_EQ("1234 / 0 returns the dividend", q, 1234);
    BW_CHECK_EQ("1234 / 0 remainder", sr, 0);
    q = bw_rt_s32_divmod(-1234, 0, &sr);
    BW_CHECK_EQ("-1234 / 0 returns the dividend", q, -1234);
    BW_CHECK_EQ("-1234 / 0 remainder", sr, 0);
    q = bw_rt_s32_divmod((s32)0x80000000u, -1, &sr);
    BW_CHECK_EQ("0x80000000 / -1 wraps", (u32)q, 0x80000000u);
    BW_CHECK_EQ("0x80000000 / -1 remainder", sr, 0);
    q = bw_rt_s32_divmod((s32)0x80000000u, 1, &sr);
    BW_CHECK_EQ("0x80000000 / 1", (u32)q, 0x80000000u);
    q = bw_rt_s32_divmod((s32)0x80000000u, (s32)0x80000000u, &sr);
    BW_CHECK_EQ("0x80000000 / 0x80000000", q, 1);
    BW_CHECK_EQ("0x80000000 % 0x80000000", sr, 0);
    q = bw_rt_s32_divmod(5, (s32)0x80000000u, &sr);
    BW_CHECK_EQ("5 / 0x80000000", q, 0);
    BW_CHECK_EQ("5 % 0x80000000", sr, 5);
    BW_CHECK_EQ("func_0209C0A4 forwards", func_0209C0A4(100, 7), 14);

    BW_CHECK_EQ("_u32_div_f 0xFFFFFFFF / 3", _u32_div_f(0xFFFFFFFFu, 3), 0x55555555u);
    BW_CHECK_EQ("_u32_div_f 7 / 0 returns the dividend", _u32_div_f(7, 0), 7);
    BW_CHECK_EQ("_div_by_zero_check 9 / 0", _div_by_zero_check(9, 0), 9);
    BW_CHECK_EQ("bw_rt_u32_divmod 7 / 0 rem",
                (bw_rt_u32_divmod(7, 0, &r), r), 0);
    BW_CHECK_EQ("_u32_div_not_0_f 3 / 9", _u32_div_not_0_f(3, 9), 0);
    BW_CHECK_EQ("_u32_div_not_0_f 0xFFFFFFFF / 0xFFFFFFFF",
                _u32_div_not_0_f(0xFFFFFFFFu, 0xFFFFFFFFu), 1);
    /* Entered past the zero check with a zero divisor, the kernel shifts
     * the whole dividend into the remainder and every quotient bit is 0. */
    bad = 0;
    for (i = 0; i < ne; i++) {
        if (bw_rt_u32_divmod_not_0(edges[i], 0, &r) != 0 || r != edges[i]) {
            bad++;
        }
    }
    BW_CHECK_EQ("_u32_div_not_0_f by zero: quotient 0, remainder n", bad, 0);
}

/* ------------------------------------------------------------------ */
/* LZ                                                                  */
/* ------------------------------------------------------------------ */

/*
 * A greedy encoder for both formats MI_UncompressLZ8 reads, written from
 * the format (not from the decoder): plain references carry 3..18 bytes,
 * extended ones 1-nibble 3..16, 0-form 0x11..0x110, 1-form 0x111..0x10110;
 * displacements are 1..4096.  Returns the stream length.
 */
static u32 lz_encode(const u8 *in, u32 n, u8 *out, int extended)
{
    u32 pos = 0;
    u32 o = 4;
    u32 maxlen = extended ? 0x10110u : 18u;

    bw_wr32(out, (n << 8) | (extended ? 0x11u : 0x10u));
    while (pos < n) {
        u32 flag_at = o++;
        u8 flags = 0;
        int k;

        for (k = 0; k < 8 && pos < n; k++) {
            u32 best_len = 0;
            u32 best_disp = 0;
            u32 disp;

            for (disp = 1; disp <= 4096 && disp <= pos; disp++) {
                u32 len = 0;

                while (len < maxlen && pos + len < n
                       && in[pos + len] == in[pos + len - disp]) {
                    len++;
                }
                if (len > best_len) {
                    best_len = len;
                    best_disp = disp;
                    if (len == maxlen) {
                        break;
                    }
                }
            }
            if (best_len >= 3) {
                u32 d = best_disp - 1;

                flags |= (u8)(0x80u >> k);
                if (!extended) {
                    out[o++] = (u8)(((best_len - 3) << 4) | (d >> 8));
                } else if (best_len <= 16) {
                    out[o++] = (u8)(((best_len - 1) << 4) | (d >> 8));
                } else if (best_len <= 0x110) {
                    u32 l = best_len - 0x11;

                    out[o++] = (u8)(l >> 4);
                    out[o++] = (u8)(((l & 0xF) << 4) | (d >> 8));
                } else {
                    u32 l = best_len - 0x111;

                    out[o++] = (u8)(0x10 | (l >> 12));
                    out[o++] = (u8)(l >> 4);
                    out[o++] = (u8)(((l & 0xF) << 4) | (d >> 8));
                }
                out[o++] = (u8)d;
                pos += best_len;
            } else {
                out[o++] = in[pos++];
            }
        }
        out[flag_at] = flags;
    }
    return o;
}

static void test_lz_hand(void)
{
    /* Plain: "ab", then a reference of length 6 at displacement 2 (an
     * overlapping copy repeating "ab"), then "c". */
    static const u8 plain[] = {
        0x10, 0x09, 0x00, 0x00,
        0x20,                  /* flags: literal, literal, ref, literal */
        'a', 'b',
        0x30, 0x01,            /* length 3 + 3, displacement 0x001 + 1 */
        'c'
    };
    /* Extended: literal 'x', a 1-nibble reference (b0 >> 4 = 3: length 4,
     * displacement 1), a 0-form reference (length 0x11 + 0x01 = 0x12) and a
     * 1-form reference (length 0x111 + 0x002 = 0x113), all displacement 1. */
    static const u8 ext[] = {
        0x11, 0x2A, 0x01, 0x00,  /* size 1 + 4 + 0x12 + 0x113 = 0x12A */
        0x70,                    /* literal, ref, ref, ref */
        'x',
        0x30, 0x00,
        0x00, 0x10, 0x00,
        0x10, 0x00, 0x20, 0x00
    };
    static u8 out[0x200];
    u32 i;
    int bad = 0;

    memset(out, 0xEE, sizeof out);
    MI_UncompressLZ8(plain, out);
    BW_CHECK_EQ("plain stream decodes", memcmp(out, "ababababc", 9), 0);
    BW_CHECK_EQ("plain stream stops at its size", out[9], 0xEE);

    memset(out, 0xEE, sizeof out);
    func_020834C0(ext, out);
    for (i = 0; i < 0x12A; i++) {
        if (out[i] != 'x') {
            bad++;
        }
    }
    BW_CHECK_EQ("extended stream: all three length forms", bad, 0);
    BW_CHECK_EQ("extended stream stops at its size", out[0x12A], 0xEE);

    /* Size 0: nothing after the header is read or written. */
    {
        static const u8 empty[] = { 0x10, 0x00, 0x00, 0x00 };

        memset(out, 0xEE, 16);
        MI_UncompressLZ8(empty, out);
        BW_CHECK_EQ("size 0 writes nothing", out[0], 0xEE);
    }

    /* A last reference that runs past the declared size is copied whole,
     * as the asm does: size 3, literal 'q', then a reference of length 5. */
    {
        static const u8 over[] = {
            0x10, 0x03, 0x00, 0x00,
            0x40, 'q', 0x20, 0x00
        };

        memset(out, 0xEE, 16);
        MI_UncompressLZ8(over, out);
        BW_CHECK_EQ("overrunning reference: byte 4 written", out[4], 'q');
        BW_CHECK_EQ("overrunning reference: byte 5 written", out[5], 'q');
        BW_CHECK_EQ("overrunning reference: byte 6 not", out[6], 0xEE);
    }
}

static void test_lz_roundtrip(void)
{
    enum { N = 0x14000 };
    u8 *src = (u8 *)malloc(N);
    u8 *enc = (u8 *)malloc(N * 2 + 64);
    u8 *dec = (u8 *)malloc(N + 0x10200);
    int ext;
    u32 i;

    BW_CHECK_EQ("allocate the LZ buffers", src != NULL && enc != NULL && dec != NULL, 1);
    if (src == NULL || enc == NULL || dec == NULL) {
        free(src);
        free(enc);
        free(dec);
        return;
    }
    /* Runs (long matches, the 1-form), repeats at varied distances, and
     * noise that stays literal. */
    for (i = 0; i < N; i++) {
        if (i < 0x11000) {
            src[i] = (u8)'r';
        } else if (i < 0x12000) {
            src[i] = (u8)((i * 7u) % 13u);
        } else {
            src[i] = (u8)rng();
        }
    }
    for (ext = 0; ext <= 1; ext++) {
        u32 len = lz_encode(src, N, enc, ext);
        u32 dec_size = 0;
        u8 *eng;

        memset(dec, 0xEE, N + 0x10200);
        MI_UncompressLZ8(enc, dec);
        BW_CHECK_EQ(ext ? "LZ11 round trip" : "LZ77 round trip",
                    memcmp(dec, src, N), 0);
        BW_CHECK_EQ(ext ? "LZ11 stops at size" : "LZ77 stops at size", dec[N], 0xEE);
        eng = g2d_decompress(enc, len, &dec_size);
        BW_CHECK_EQ("g2d_decompress takes the stream", eng != NULL, 1);
        if (eng == NULL) {
            continue;
        }
        BW_CHECK_EQ("g2d_decompress agrees on the size", dec_size, N);
        BW_CHECK_EQ("g2d_decompress agrees on the bytes", memcmp(eng, dec, N), 0);
        free(eng);
    }
    free(src);
    free(enc);
    free(dec);
}

/* Every LZ member g2d_decompress accepts in a few real archives. */
static void test_lz_game_data(void)
{
    static const char *const archives[] = { "a/0/0/4", "a/0/0/8", "a/0/1/1" };
    u32 a;
    int checked = 0;
    int bad = 0;

    for (a = 0; a < sizeof archives / sizeof archives[0]; a++) {
        BwArcFile arc;
        u32 m;

        if (bw_arc_open(&arc, archives[a]) != 0) {
            continue;
        }
        for (m = 0; m < arc.count && checked < 400; m++) {
            long raw_size = bw_arc_member_size(&arc, m);
            u8 *raw;
            u8 *ref;
            u8 *mine;
            u32 ref_size = 0;

            if (raw_size < 4) {
                continue;
            }
            raw = (u8 *)malloc((size_t)raw_size);
            if (raw == NULL) {
                continue;
            }
            if (bw_arc_read_member(&arc, m, raw, (u32)raw_size) != raw_size
                || (raw[0] != 0x10 && raw[0] != 0x11)
                || (bw_rd32(raw) >> 8) == 0) {
                free(raw);
                continue;
            }
            ref = g2d_decompress(raw, (u32)raw_size, &ref_size);
            if (ref == NULL) {
                free(raw);
                continue;
            }
            mine = (u8 *)malloc((size_t)ref_size + 0x10200);
            if (mine != NULL) {
                MI_UncompressLZ8(raw, mine);
                checked++;
                if (memcmp(mine, ref, ref_size) != 0) {
                    if (bad < 5) {
                        printf("  %s member %u differs\n", archives[a], (unsigned)m);
                    }
                    bad++;
                }
                free(mine);
            }
            free(ref);
            free(raw);
        }
        bw_arc_close(&arc);
    }
    if (checked == 0) {
        printf("  LZ game data: skipped (no archive under the data root)\n");
        return;
    }
    printf("  LZ game data: %d compressed members decoded\n", checked);
    BW_CHECK_EQ("real LZ members decode as g2d_decompress does", bad, 0);
}

/* ------------------------------------------------------------------ */
/* FX, OS, CP, CTRDG and the game leaf                                 */
/* ------------------------------------------------------------------ */

static void test_mtx(void)
{
    BwAsmMtxFx22 m;

    MTX_Rot22_(&m, 0x0B50, 0x0B50);
    BW_CHECK_EQ("Rot22 _00 = cos", m._00, 0x0B50);
    BW_CHECK_EQ("Rot22 _01 = sin", m._01, 0x0B50);
    BW_CHECK_EQ("Rot22 _10 = -sin", m._10, -0x0B50);
    BW_CHECK_EQ("Rot22 _11 = cos", m._11, 0x0B50);
    MTX_Rot22_(&m, (fx32)0x80000000u, -4096);
    BW_CHECK_EQ("Rot22 -0x80000000 wraps", (u32)m._10, 0x80000000u);
    BW_CHECK_EQ("Rot22 negative cos", m._00, -4096);
}

static volatile u32 *shared32(u32 addr)
{
    return (volatile u32 *)bw_ds_ptr_w(addr, 4);   /* a register word, as the SDK's */
}

static void test_lock_ids(void)
{
    *shared32(0x02FFFFB0u) = 0xFFFFFFFFu;
    *shared32(0x02FFFFB4u) = 0xFFFFFFFFu;
    BW_CHECK_EQ("first lock id", OS_GetLockID(), 0x40);
    BW_CHECK_EQ("second lock id", func_02084EB8(), 0x41);
    BW_CHECK_EQ("ids clear their bits", *shared32(0x02FFFFB0u), 0x3FFFFFFFu);
    *shared32(0x02FFFFB0u) = 0x00010000u;
    BW_CHECK_EQ("lowest free id in word 0", OS_GetLockID(), 0x4F);
    BW_CHECK_EQ("word 0 empty", *shared32(0x02FFFFB0u), 0);
    BW_CHECK_EQ("word 1 next", OS_GetLockID(), 0x60);
    *shared32(0x02FFFFB4u) = 0;
    BW_CHECK_EQ("no ids left: OS_LOCK_ID_ERROR", OS_GetLockID(), -3);

    OS_ReleaseLockID(0x45);
    BW_CHECK_EQ("release 0x45", *shared32(0x02FFFFB0u), 0x80000000u >> 5);
    func_02084F10(0x7F);
    BW_CHECK_EQ("release 0x7F", *shared32(0x02FFFFB4u), 1);
    OS_ReleaseLockID(0x3F);
    BW_CHECK_EQ("release below 0x40 changes word 0 not",
                *shared32(0x02FFFFB0u), 0x80000000u >> 5);
    OS_ReleaseLockID(0x80);
    BW_CHECK_EQ("release 0x80 (shift 32) changes nothing", *shared32(0x02FFFFB4u), 1);
    OS_ReleaseLockID(0x160);
    BW_CHECK_EQ("release 0x160 wraps to bit 31", *shared32(0x02FFFFB4u), 0x80000001u);
    BW_CHECK_EQ("the released id comes back first", OS_GetLockID(), 0x45);
}

static void test_cp_context(void)
{
    u8 ctx[0x1C];
    u8 back[0x1C];
    u32 i;

    for (i = 0; i < 4; i++) {
        *shared32(0x04000290u + 4 * i) = 0x11111111u * (i + 1);
    }
    *(volatile u16 *)bw_ds_ptr_w(0x04000280u, sizeof(*(volatile u16 *)0)) = 0x8002;     /* busy + mode 2 */
    *shared32(0x040002B8u) = 0xCAFEF00Du;
    *shared32(0x040002BCu) = 0x01234567u;
    *(volatile u16 *)bw_ds_ptr_w(0x040002B0u, sizeof(*(volatile u16 *)0)) = 0x8001;
    memset(ctx, 0xEE, sizeof ctx);
    CP_SaveContext(ctx);
    BW_CHECK_EQ("save: DIV_NUMER low", bw_mem_rd32(ctx), 0x11111111u);
    BW_CHECK_EQ("save: DIV_DENOM high", bw_mem_rd32(ctx + 12), 0x44444444u);
    BW_CHECK_EQ("save: SQRT_PARAM", bw_mem_rd32(ctx + 0x10), 0xCAFEF00Du);
    BW_CHECK_EQ("save: SQRT_PARAM high", bw_mem_rd32(ctx + 0x14), 0x01234567u);
    BW_CHECK_EQ("save: DIVCNT & 3", bw_mem_rd16(ctx + 0x18), 2);
    BW_CHECK_EQ("save: SQRTCNT & 1", bw_mem_rd16(ctx + 0x1A), 1);

    for (i = 0; i < 4; i++) {
        *shared32(0x04000290u + 4 * i) = 0;
    }
    *shared32(0x040002B8u) = 0;
    *shared32(0x040002BCu) = 0;
    *(volatile u16 *)bw_ds_ptr_w(0x04000280u, sizeof(*(volatile u16 *)0)) = 0;
    *(volatile u16 *)bw_ds_ptr_w(0x040002B0u, sizeof(*(volatile u16 *)0)) = 0;
    CPi_RestoreContext(ctx);
    BW_CHECK_EQ("restore: DIV_NUMER", *shared32(0x04000294u), 0x22222222u);
    BW_CHECK_EQ("restore: SQRT_PARAM", *shared32(0x040002B8u), 0xCAFEF00Du);
    BW_CHECK_EQ("restore: DIVCNT", *(volatile u16 *)bw_ds_ptr_w(0x04000280u, sizeof(*(volatile u16 *)0)), 2);
    BW_CHECK_EQ("restore: SQRTCNT", *(volatile u16 *)bw_ds_ptr_w(0x040002B0u, sizeof(*(volatile u16 *)0)), 1);
    CP_SaveContext(back);
    BW_CHECK_EQ("save after restore is the same context", memcmp(ctx, back, sizeof ctx), 0);
}

static void test_shared_reads(void)
{
    static const u8 mac[6] = { 0x00, 0x09, 0xBF, 0x12, 0x34, 0x56 };
    u8 got[8];

    *(volatile u16 *)bw_ds_ptr(0x02FFFC30u) = 0xFFFF;
    BW_CHECK_EQ("no cartridge at init", CTRDG_IsExistedAtInit(), 0);
    *(volatile u16 *)bw_ds_ptr(0x02FFFC30u) = 0x0000;
    BW_CHECK_EQ("cartridge id 0 is a cartridge", CTRDG_IsExistedAtInit(), 1);

    *shared32(0x0214617Cu) = 0;
    BW_CHECK_EQ("task not available", CTRDGi_IsTaskAvailable_02010054(), 0);
    *shared32(0x0214617Cu) = 0x80000000u;
    BW_CHECK_EQ("task available", func_02010054(), 1);
    BW_CHECK_EQ("sub_ spelling", sub_02010054(), 1);
    *shared32(0x0214617Cu) = 0;

    memcpy(bw_ds_ptr(0x02FFFCF4u), mac, 6);
    memset(got, 0xEE, sizeof got);
    OS_GetMacAddress(got);
    BW_CHECK_EQ("MAC address", memcmp(got, mac, 6), 0);
    BW_CHECK_EQ("MAC copy is six bytes", got[6], 0xEE);
    memset(got, 0, sizeof got);
    func_02087C6C(got);
    BW_CHECK_EQ("func_02087C6C", got[5], 0x56);
}

static void test_leaf(void)
{
    u8 obj[0x40];

    memset(obj, 0, sizeof obj);
    obj[0x38] = 0x81;
    func_02034F74(obj, 0x06);
    BW_CHECK_EQ("byte +0x38 |= mask", obj[0x38], 0x87);
    func_02034F74(obj, 0x170);
    BW_CHECK_EQ("only the low byte is stored", obj[0x38], 0xF7);
    BW_CHECK_EQ("byte +0x39 untouched", obj[0x39], 0);
}

int main(int argc, char **argv)
{
    BW_TEST_SETUP(argc, argv);

    test_copy8();
    test_fill8();
    test_mii_units();
    test_division();
    test_lz_hand();
    test_lz_roundtrip();
    test_mtx();

    bw_ds_reset();
    test_lock_ids();
    test_cp_context();
    test_shared_reads();
    test_leaf();

    test_lz_game_data();

    BW_REPORT("test_asmsdk");
}
