/*
 * sdk/asm/CPi_RestoreContext.c -- CPi_RestoreContext at 0x020775F8.
 *
 * Replaces src/matched/arm9_sdk/CPi_RestoreContext.c (TwlSDK libcp
 * cp_context.o, an asm function in the vendor source).
 * void CPi_RestoreContext(const CPContext *context), the inverse of
 * CP_SaveContext.c, in the asm's store order:
 *
 *     context +0x00, 4 words  -> 0x04000290 (DIV_NUMER, DIV_DENOM)
 *     context +0x18 halfword  -> 0x04000280 (DIVCNT)
 *     context +0x1A halfword  -> 0x040002B0 (SQRTCNT)
 *     context +0x10, 2 words  -> 0x040002B8 (SQRT_PARAM)
 *
 * Plain stores into the register file, as the matched code's own register
 * stores are: a write hook on these registers runs at the next sync point.
 */

#include <string.h>

#include "asmsdk.h"
#include "../dsmem.h"

/* One word of the unit's registers: through bw_ds_ptr_w, which knows where
 * a word of a 64-bit register lives on a big-endian host (sdk/dsmem.h). */
static void reg32(u32 addr, u32 v)
{
    *(volatile u32 *)bw_ds_ptr_w(addr, 4) = v;
}

void CPi_RestoreContext(const void *context)
{
    const u8 *ctx = (const u8 *)context;
    u32 w[4];
    u16 divmode;
    u16 sqrtmode;

    memcpy(w, ctx, 16);
    reg32(0x04000290u, w[0]);
    reg32(0x04000294u, w[1]);
    reg32(0x04000298u, w[2]);
    reg32(0x0400029Cu, w[3]);
    memcpy(&divmode, ctx + 0x18, 2);
    memcpy(&sqrtmode, ctx + 0x1A, 2);
    *(volatile u16 *)bw_ds_ptr_w(0x04000280u, sizeof(*(volatile u16 *)0)) = divmode;
    *(volatile u16 *)bw_ds_ptr_w(0x040002B0u, sizeof(*(volatile u16 *)0)) = sqrtmode;
    memcpy(w, ctx + 0x10, 8);
    reg32(0x040002B8u, w[0]);
    reg32(0x040002BCu, w[1]);
}
