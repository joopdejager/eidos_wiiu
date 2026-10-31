/*
 * sdk/asm/CP_SaveContext.c -- CP_SaveContext at 0x020775B8.
 *
 * Replaces src/matched/arm9_sdk/CP_SaveContext.c (TwlSDK libcp
 * cp_context.o, an asm function in the vendor source).
 * void CP_SaveContext(CPContext *context).
 *
 * The divider and square-root unit's state, read from the register file:
 *
 *     ldmia 0x04000290, {4 words}   -> context +0x00 (DIV_NUMER, DIV_DENOM)
 *     ldrh  0x04000280              -> & 3, context +0x18 (DIVCNT mode)
 *     ldmia 0x040002B8, {2 words}   -> context +0x10 (SQRT_PARAM)
 *     ldrh  0x040002B0              -> & 1, context +0x1A (SQRTCNT mode)
 *
 * Reads only: no register here has a read side effect.  The words are
 * copied as the register file holds them, so the context holds the
 * registers' bytes whatever the host byte order.
 */

#include <string.h>

#include "asmsdk.h"
#include "../dsmem.h"

/* One word of the unit's registers: through bw_ds_ptr_w, which knows where
 * a word of a 64-bit register lives on a big-endian host (sdk/dsmem.h). */
static u32 reg32(u32 addr)
{
    return *(const volatile u32 *)bw_ds_ptr_w(addr, 4);
}

void CP_SaveContext(void *context)
{
    u8 *ctx = (u8 *)context;
    u32 w[4];
    u16 mode;

    w[0] = reg32(0x04000290u);
    w[1] = reg32(0x04000294u);
    w[2] = reg32(0x04000298u);
    w[3] = reg32(0x0400029Cu);
    memcpy(ctx, w, 16);
    mode = (u16)(*(const volatile u16 *)bw_ds_ptr_w(0x04000280u, sizeof(*(const volatile u16 *)0)) & 3u);
    w[0] = reg32(0x040002B8u);
    w[1] = reg32(0x040002BCu);
    memcpy(ctx + 0x10, w, 8);
    memcpy(ctx + 0x18, &mode, 2);
    mode = (u16)(*(const volatile u16 *)bw_ds_ptr_w(0x040002B0u, sizeof(*(const volatile u16 *)0)) & 1u);
    memcpy(ctx + 0x1A, &mode, 2);
}
